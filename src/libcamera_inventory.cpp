#include "context_impl.hpp"
#include "internal/backend.hpp"
#include "internal/bounded_queue.hpp"
#include "hscam/error.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include <linux/dma-buf.h>
#include <libcamera/libcamera.h>

namespace hscam {
namespace {

Size toSize(const libcamera::Size &value)
{
    return {value.width, value.height};
}

Rect toRect(const libcamera::Rectangle &value)
{
    return {value.x, value.y, value.width, value.height};
}

unsigned bitDepthFromFormat(const std::string &name)
{
    for (unsigned bits : {16U, 14U, 12U, 10U, 8U}) {
        if (name.find(std::to_string(bits)) != std::string::npos)
            return bits;
    }
    return 0;
}

std::vector<SensorMode> enumerateRawModes(const std::shared_ptr<libcamera::Camera> &camera)
{
    std::vector<SensorMode> modes;
    auto raw = camera->generateConfiguration({libcamera::StreamRole::Raw});
    if (!raw || raw->empty())
        return modes;

    const auto &stream = raw->at(0);
    for (const auto &pixelFormat : stream.formats().pixelformats()) {
        const std::string formatName = pixelFormat.toString();
        for (const auto &size : stream.formats().sizes(pixelFormat)) {
            SensorMode mode;
            mode.size = toSize(size);
            mode.format = {formatName};
            mode.bitDepth = bitDepthFromFormat(formatName);
            mode.id = stableModeId(mode);
            modes.push_back(std::move(mode));
        }
    }

    std::sort(modes.begin(), modes.end(), [](const SensorMode &a, const SensorMode &b) {
        if (a.size.area() != b.size.area())
            return a.size.area() < b.size.area();
        if (a.format.name != b.format.name)
            return a.format.name < b.format.name;
        return a.id < b.id;
    });
    modes.erase(std::unique(modes.begin(), modes.end(), [](const SensorMode &a, const SensorMode &b) {
        return a.id == b.id;
    }), modes.end());
    return modes;
}

std::string sizeString(const libcamera::Size &s)
{
    return std::to_string(s.width) + "x" + std::to_string(s.height);
}

void addAdjustment(std::vector<Adjustment> &out, std::string field, std::string requested,
                   std::string negotiated, AdjustmentSource source)
{
    if (requested != negotiated)
        out.push_back({std::move(field), std::move(requested), std::move(negotiated), source});
}

struct MappedPlane {
    void *mapping{};
    std::size_t mappingLength{};
    const std::byte *data{};
    std::size_t length{};
    int fd{-1};

    MappedPlane() = default;
    MappedPlane(const MappedPlane &) = delete;
    MappedPlane &operator=(const MappedPlane &) = delete;
    MappedPlane(MappedPlane &&other) noexcept
        : mapping(std::exchange(other.mapping, nullptr)),
          mappingLength(std::exchange(other.mappingLength, 0)),
          data(std::exchange(other.data, nullptr)),
          length(std::exchange(other.length, 0)),
          fd(std::exchange(other.fd, -1))
    {
    }
    MappedPlane &operator=(MappedPlane &&other) noexcept
    {
        if (this != &other) {
            if (mapping)
                ::munmap(mapping, mappingLength);
            mapping = std::exchange(other.mapping, nullptr);
            mappingLength = std::exchange(other.mappingLength, 0);
            data = std::exchange(other.data, nullptr);
            length = std::exchange(other.length, 0);
            fd = std::exchange(other.fd, -1);
        }
        return *this;
    }
    ~MappedPlane()
    {
        if (mapping)
            ::munmap(mapping, mappingLength);
    }
};

MappedPlane mapPlane(const libcamera::FrameBuffer::Plane &plane)
{
    const long page = ::sysconf(_SC_PAGESIZE);
    if (page <= 0)
        throw CaptureFailed("failed to obtain page size");

    const std::uint64_t mask = static_cast<std::uint64_t>(page - 1);
    const std::uint64_t alignedOffset = plane.offset & ~mask;
    const std::size_t delta = static_cast<std::size_t>(plane.offset - alignedOffset);
    const std::size_t mappingLength = delta + plane.length;

    void *mapping = ::mmap(nullptr, mappingLength, PROT_READ | PROT_WRITE, MAP_SHARED,
                           plane.fd.get(), static_cast<off_t>(alignedOffset));
    if (mapping == MAP_FAILED)
        throw CaptureFailed("mmap dma-buf failed: " + std::string(std::strerror(errno)));

    MappedPlane result;
    result.mapping = mapping;
    result.mappingLength = mappingLength;
    result.data = static_cast<const std::byte *>(mapping) + delta;
    result.length = plane.length;
    result.fd = plane.fd.get();
    return result;
}

void dmaSync(const libcamera::FrameBuffer *buffer, bool start)
{
    std::set<int> seen;
    for (const auto &plane : buffer->planes()) {
        const int fd = plane.fd.get();
        if (!seen.insert(fd).second)
            continue;
        dma_buf_sync sync{};
        sync.flags = (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) | DMA_BUF_SYNC_READ;
        (void)::ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    }
}

struct SessionState;

class LibcameraFrameLease final : public FrameLease::Impl {
public:
    LibcameraFrameLease(std::shared_ptr<SessionState> state, libcamera::Request *request,
                        FrameMetadata metadata, std::vector<PlaneView> planes)
        : state_(std::move(state)), request_(request), metadata_(std::move(metadata)), planes_(std::move(planes))
    {
    }

    ~LibcameraFrameLease() override;
    const FrameMetadata &metadata() const override { return metadata_; }
    std::span<const PlaneView> planes() const override { return planes_; }

private:
    std::shared_ptr<SessionState> state_;
    libcamera::Request *request_{};
    FrameMetadata metadata_;
    std::vector<PlaneView> planes_;
};

struct SessionState : public std::enable_shared_from_this<SessionState> {
    explicit SessionState(std::shared_ptr<libcamera::Camera> camera, libcamera::Stream *stream,
                          CaptureConfiguration config)
        : camera(std::move(camera)), stream(stream), config(std::move(config)), completed(64)
    {
    }

    ~SessionState()
    {
        stop();
    }

    void initialize()
    {
        allocator = std::make_unique<libcamera::FrameBufferAllocator>(camera);
        const int allocated = allocator->allocate(stream);
        if (allocated < 0)
            throw CaptureFailed("FrameBufferAllocator::allocate failed: " + std::to_string(allocated));

        const auto &buffers = allocator->buffers(stream);
        if (buffers.empty())
            throw CaptureFailed("libcamera allocated zero buffers");

        config.stream.bufferCount = static_cast<unsigned>(buffers.size());
        mapped.reserve(buffers.size());
        requests.reserve(buffers.size());

        for (std::size_t i = 0; i < buffers.size(); ++i) {
            auto *buffer = buffers[i].get();
            std::vector<MappedPlane> planes;
            planes.reserve(buffer->planes().size());
            for (const auto &plane : buffer->planes())
                planes.push_back(mapPlane(plane));
            mapped.emplace(buffer, std::move(planes));

            auto request = camera->createRequest(i);
            if (!request)
                throw CaptureFailed("Camera::createRequest failed");
            const int rc = request->addBuffer(stream, buffer);
            if (rc < 0)
                throw CaptureFailed("Request::addBuffer failed: " + std::to_string(rc));
            requests.push_back(std::move(request));
        }

        camera->requestCompleted.connect(this, &SessionState::requestComplete);

        libcamera::ControlList controls(camera->controls());
        if (config.timing.requestedFrameDuration) {
            const std::int64_t us = config.timing.requestedFrameDuration->count();
            controls.set(libcamera::controls::FrameDurationLimits,
                         libcamera::Span<const std::int64_t, 2>({us, us}));
        }
        if (config.timing.exposure) {
            if (camera->controls().contains(libcamera::controls::ExposureTimeMode.id()))
                controls.set(libcamera::controls::ExposureTimeMode, libcamera::controls::ExposureTimeModeManual);
            controls.set(libcamera::controls::ExposureTime,
                         static_cast<std::int32_t>(config.timing.exposure->count()));
        }
        if (config.timing.analogueGain) {
            if (camera->controls().contains(libcamera::controls::AnalogueGainMode.id()))
                controls.set(libcamera::controls::AnalogueGainMode, libcamera::controls::AnalogueGainModeManual);
            controls.set(libcamera::controls::AnalogueGain, static_cast<float>(*config.timing.analogueGain));
        }

        const int startRc = controls.empty() ? camera->start() : camera->start(&controls);
        if (startRc < 0)
            throw CaptureFailed("Camera::start failed: " + std::to_string(startRc));
        running.store(true, std::memory_order_release);

        for (auto &request : requests) {
            const int rc = camera->queueRequest(request.get());
            if (rc < 0) {
                stop();
                throw CaptureFailed("Camera::queueRequest failed: " + std::to_string(rc));
            }
        }
    }

    void requestComplete(libcamera::Request *request)
    {
        if (request->status() == libcamera::Request::RequestCancelled) {
            std::lock_guard lock(statsMutex);
            ++stats.requestsCancelled;
            return;
        }

        for (const auto &[streamPtr, buffer] : request->buffers()) {
            (void)streamPtr;
            dmaSync(buffer, true);
        }

        {
            std::lock_guard lock(statsMutex);
            ++stats.requestsCompleted;
            const std::uint64_t seq = request->sequence();
            if (lastSequence && seq > *lastSequence + 1)
                stats.sequenceGaps += seq - *lastSequence - 1;
            lastSequence = seq;

            if (auto ts = request->metadata().get(libcamera::controls::SensorTimestamp)) {
                const auto timestamp = std::chrono::nanoseconds(*ts);
                if (!firstSensorTimestamp)
                    firstSensorTimestamp = timestamp;
                if (lastSensorTimestamp) {
                    const auto interval = timestamp - *lastSensorTimestamp;
                    if (!stats.minInterval || interval < *stats.minInterval) stats.minInterval = interval;
                    if (!stats.maxInterval || interval > *stats.maxInterval) stats.maxInterval = interval;
                }
                lastSensorTimestamp = timestamp;
                if (firstSensorTimestamp && stats.requestsCompleted > 1) {
                    const auto elapsed = timestamp - *firstSensorTimestamp;
                    if (elapsed.count() > 0)
                        stats.measuredFps = static_cast<double>(stats.requestsCompleted - 1) * 1e9 /
                                            static_cast<double>(elapsed.count());
                }
            }
        }

        if (!completed.tryPush(request)) {
            {
                std::lock_guard lock(statsMutex);
                ++stats.completedQueueOverruns;
            }
            recycle(request);
        }
    }

    std::unique_ptr<FrameLease::Impl> nextFrame(std::chrono::milliseconds timeout)
    {
        auto item = completed.popFor(timeout);
        if (!item)
            throw Timeout("timed out waiting for camera frame");
        libcamera::Request *request = *item;

        FrameMetadata metadata;
        metadata.sequence = request->sequence();
        metadata.requestCookie = request->cookie();
        metadata.completionTimestamp = std::chrono::steady_clock::now();
        metadata.status = request->status() == libcamera::Request::RequestComplete
                              ? FrameStatus::Complete
                              : FrameStatus::Error;
        if (auto value = request->metadata().get(libcamera::controls::SensorTimestamp))
            metadata.sensorTimestamp = std::chrono::nanoseconds(*value);
        if (auto value = request->metadata().get(libcamera::controls::ExposureTime))
            metadata.exposure = std::chrono::microseconds(*value);
        if (auto value = request->metadata().get(libcamera::controls::FrameDuration))
            metadata.frameDuration = std::chrono::microseconds(*value);
        if (auto value = request->metadata().get(libcamera::controls::AnalogueGain))
            metadata.analogueGain = *value;

        std::vector<PlaneView> views;
        auto *buffer = request->findBuffer(stream);
        if (!buffer)
            throw CaptureFailed("completed request has no buffer for configured stream");
        auto mappedIt = mapped.find(buffer);
        if (mappedIt == mapped.end())
            throw CaptureFailed("completed buffer is not mapped");
        views.reserve(mappedIt->second.size());
        for (std::size_t i = 0; i < mappedIt->second.size(); ++i) {
            const auto &p = mappedIt->second[i];
            const std::uint32_t stride = i == 0 && !config.stream.planes.empty() ? config.stream.planes.front().stride : 0;
            views.push_back({std::span<const std::byte>(p.data, p.length), stride,
                             static_cast<std::uint32_t>(p.length), p.fd});
        }

        return std::make_unique<LibcameraFrameLease>(shared_from_this(), request, std::move(metadata), std::move(views));
    }

    void recycle(libcamera::Request *request)
    {
        for (const auto &[streamPtr, buffer] : request->buffers()) {
            (void)streamPtr;
            dmaSync(buffer, false);
        }
        if (!running.load(std::memory_order_acquire))
            return;
        request->reuse(libcamera::Request::ReuseBuffers);
        const int rc = camera->queueRequest(request);
        if (rc < 0) {
            running.store(false, std::memory_order_release);
            completed.close();
        }
    }

    void stop()
    {
        bool expected = true;
        if (!running.compare_exchange_strong(expected, false, std::memory_order_acq_rel))
            return;
        (void)camera->stop();
        completed.close();
        camera->requestCompleted.disconnect(this, &SessionState::requestComplete);
    }

    CaptureStats snapshotStats() const
    {
        std::lock_guard lock(statsMutex);
        return stats;
    }

    std::shared_ptr<libcamera::Camera> camera;
    libcamera::Stream *stream{};
    CaptureConfiguration config;
    std::unique_ptr<libcamera::FrameBufferAllocator> allocator;
    std::vector<std::unique_ptr<libcamera::Request>> requests;
    std::map<libcamera::FrameBuffer *, std::vector<MappedPlane>> mapped;
    internal::BoundedQueue<libcamera::Request *> completed;
    std::atomic_bool running{};
    mutable std::mutex statsMutex;
    CaptureStats stats;
    std::optional<std::uint64_t> lastSequence;
    std::optional<std::chrono::nanoseconds> firstSensorTimestamp;
    std::optional<std::chrono::nanoseconds> lastSensorTimestamp;
};

LibcameraFrameLease::~LibcameraFrameLease()
{
    if (state_ && request_)
        state_->recycle(request_);
}

class LibcameraSession final : public CaptureSession::Impl {
public:
    explicit LibcameraSession(std::shared_ptr<SessionState> state) : state_(std::move(state)) {}
    ~LibcameraSession() override { stop(); }

    std::unique_ptr<FrameLease::Impl> nextFrameImpl(std::chrono::milliseconds timeout) override
    {
        return state_->nextFrame(timeout);
    }
    CaptureStats stats() const override { return state_->snapshotStats(); }
    void stop() override { if (state_) state_->stop(); }

private:
    std::shared_ptr<SessionState> state_;
};

class LibcameraCamera final : public Camera::Impl {
public:
    LibcameraCamera(std::shared_ptr<Context::Impl> owner, std::shared_ptr<libcamera::Camera> camera)
        : owner_(std::move(owner)), camera_(std::move(camera)), id_(camera_->id())
    {
        const int rc = camera_->acquire();
        if (rc < 0)
            throw CameraBusy("failed to acquire camera " + id_ + ": " + std::to_string(rc));
    }

    ~LibcameraCamera() override
    {
        if (camera_)
            (void)camera_->release();
    }

    const std::string &id() const override { return id_; }

    CaptureConfiguration configure(const CaptureRequest &request) override
    {
        if (configured_)
            throw NegotiationFailed("camera is already configured; create a new Camera handle to reconfigure");

        const libcamera::StreamRole role = request.stream.kind == StreamKind::Raw
                                               ? libcamera::StreamRole::Raw
                                               : libcamera::StreamRole::VideoRecording;
        auto config = camera_->generateConfiguration({role});
        if (!config || config->empty())
            throw Unsupported("requested stream role is not supported by camera " + id_);

        auto &stream = config->at(0);
        if (request.sensor.modeId) {
            const auto modes = enumerateRawModes(camera_);
            auto it = std::find_if(modes.begin(), modes.end(), [&](const SensorMode &m) { return m.id == *request.sensor.modeId; });
            if (it == modes.end())
                throw NegotiationFailed("unknown sensor mode id: " + *request.sensor.modeId);
            stream.size = libcamera::Size(it->size.width, it->size.height);
            stream.pixelFormat = libcamera::PixelFormat::fromString(it->format.name);
        }
        if (request.stream.size)
            stream.size = libcamera::Size(request.stream.size->width, request.stream.size->height);
        if (request.stream.format) {
            const auto format = libcamera::PixelFormat::fromString(request.stream.format->name);
            if (!format.isValid())
                throw NegotiationFailed("invalid libcamera pixel format: " + request.stream.format->name);
            stream.pixelFormat = format;
        }
        if (request.stream.bufferCount)
            stream.bufferCount = request.stream.bufferCount;

        const auto requestedSize = stream.size;
        const auto requestedFormat = stream.pixelFormat;
        const auto requestedBuffers = stream.bufferCount;

        const auto status = config->validate();
        if (status == libcamera::CameraConfiguration::Invalid)
            throw NegotiationFailed("libcamera rejected camera configuration");

        CaptureConfiguration result;
        result.sensor.modeId = request.sensor.modeId;
        result.sensor.crop = request.sensor.crop;
        result.stream.kind = request.stream.kind;
        result.stream.size = toSize(config->at(0).size);
        result.stream.format = {config->at(0).pixelFormat.toString()};
        result.stream.frameBytes = config->at(0).frameSize;
        result.stream.bufferCount = config->at(0).bufferCount;
        result.stream.planes.push_back({config->at(0).stride, config->at(0).frameSize});
        result.timing.requestedFrameDuration = request.timing.frameDuration;
        result.timing.exposure = request.timing.exposure;
        result.timing.analogueGain = request.timing.analogueGain;

        addAdjustment(result.adjustments, "stream.size", sizeString(requestedSize), sizeString(config->at(0).size), AdjustmentSource::Libcamera);
        addAdjustment(result.adjustments, "stream.format", requestedFormat.toString(), config->at(0).pixelFormat.toString(), AdjustmentSource::Libcamera);
        addAdjustment(result.adjustments, "stream.bufferCount", std::to_string(requestedBuffers), std::to_string(config->at(0).bufferCount), AdjustmentSource::Libcamera);

        if (request.negotiation == NegotiationPolicy::Exact && !result.adjustments.empty())
            throw ExactConfigurationFailed("libcamera adjusted an exact capture request");

        const int rc = camera_->configure(config.get());
        if (rc < 0)
            throw NegotiationFailed("Camera::configure failed: " + std::to_string(rc));

        stream_ = config->at(0).stream();
        if (!stream_)
            throw NegotiationFailed("configured stream has no libcamera Stream object");
        config_ = std::move(config);
        actual_ = result;
        configured_ = true;
        return actual_;
    }

    const CaptureConfiguration &configuration() const override
    {
        if (!configured_)
            throw NegotiationFailed("camera has not been configured");
        return actual_;
    }

    CaptureSession start() override
    {
        if (!configured_ || !stream_)
            throw CaptureFailed("camera must be configured before start");
        auto state = std::make_shared<SessionState>(camera_, stream_, actual_);
        state->initialize();
        actual_ = state->config;
        return CaptureSession(std::make_unique<LibcameraSession>(std::move(state)));
    }

private:
    std::shared_ptr<Context::Impl> owner_;
    std::shared_ptr<libcamera::Camera> camera_;
    std::string id_;
    std::unique_ptr<libcamera::CameraConfiguration> config_;
    libcamera::Stream *stream_{};
    CaptureConfiguration actual_;
    bool configured_{};
};

class LibcameraContext final : public Context::Impl {
public:
    LibcameraContext()
        : manager_(std::make_unique<libcamera::CameraManager>())
    {
        const int rc = manager_->start();
        if (rc < 0)
            throw Error("libcamera CameraManager::start failed: " + std::to_string(rc));
    }

    ~LibcameraContext() override
    {
        if (manager_)
            manager_->stop();
    }

    bool available() const noexcept override { return true; }

    std::vector<CameraInfo> cameras() const override
    {
        std::vector<CameraInfo> result;
        result.reserve(manager_->cameras().size());

        for (const auto &camera : manager_->cameras()) {
            CameraInfo info;
            info.id = camera->id();

            const auto &props = camera->properties();
            if (auto model = props.get(libcamera::properties::Model)) {
                info.model = std::string(*model);
                info.sensorModel = info.model;
            } else {
                info.model = camera->id();
            }
            if (auto array = props.get(libcamera::properties::PixelArraySize))
                info.pixelArray = toSize(*array);
            if (auto areas = props.get(libcamera::properties::PixelArrayActiveAreas)) {
                info.activeAreas.reserve(areas->size());
                for (const auto &area : *areas)
                    info.activeAreas.push_back(toRect(area));
            }

            info.capabilities.exposureControl = camera->controls().contains(libcamera::controls::ExposureTime.id());
            info.capabilities.analogueGainControl = camera->controls().contains(libcamera::controls::AnalogueGain.id());
            info.capabilities.frameDurationControl = camera->controls().contains(libcamera::controls::FrameDurationLimits.id());
            info.sensorModes = enumerateRawModes(camera);
            info.capabilities.rawCapture = !info.sensorModes.empty();
            auto processed = camera->generateConfiguration({libcamera::StreamRole::VideoRecording});
            info.capabilities.processedCapture = processed && !processed->empty();
            result.push_back(std::move(info));
        }
        return result;
    }

    std::unique_ptr<Camera::Impl> open(std::string_view cameraId) override
    {
        auto camera = manager_->get(std::string(cameraId));
        if (!camera)
            throw CameraNotFound("camera not found: " + std::string(cameraId));
        return std::make_unique<LibcameraCamera>(shared_from_this(), std::move(camera));
    }

private:
    std::unique_ptr<libcamera::CameraManager> manager_;
};

} // namespace

std::shared_ptr<Context::Impl> makeLibcameraContext()
{
    return std::make_shared<LibcameraContext>();
}

} // namespace hscam
