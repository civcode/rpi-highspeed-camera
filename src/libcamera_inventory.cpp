#include "context_impl.hpp"
#include "hscam/error.hpp"

#include <algorithm>
#include <memory>
#include <string>

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

            auto raw = camera->generateConfiguration({libcamera::StreamRole::Raw});
            if (raw && !raw->empty()) {
                info.capabilities.rawCapture = true;
                const auto &stream = raw->at(0);
                for (const auto &pixelFormat : stream.formats().pixelformats()) {
                    const std::string formatName = pixelFormat.toString();
                    const auto sizes = stream.formats().sizes(pixelFormat);
                    for (const auto &size : sizes) {
                        SensorMode mode;
                        mode.size = toSize(size);
                        mode.format = {formatName};
                        mode.bitDepth = bitDepthFromFormat(formatName);
                        mode.id = stableModeId(mode);
                        info.sensorModes.push_back(std::move(mode));
                    }
                }
            }

            auto processed = camera->generateConfiguration({libcamera::StreamRole::VideoRecording});
            info.capabilities.processedCapture = processed && !processed->empty();

            std::sort(info.sensorModes.begin(), info.sensorModes.end(), [](const SensorMode &a, const SensorMode &b) {
                if (a.size.area() != b.size.area())
                    return a.size.area() < b.size.area();
                if (a.format.name != b.format.name)
                    return a.format.name < b.format.name;
                return a.id < b.id;
            });

            info.sensorModes.erase(std::unique(info.sensorModes.begin(), info.sensorModes.end(), [](const SensorMode &a, const SensorMode &b) {
                return a.id == b.id;
            }), info.sensorModes.end());

            result.push_back(std::move(info));
        }

        return result;
    }

private:
    std::unique_ptr<libcamera::CameraManager> manager_;
};

} // namespace

std::unique_ptr<Context::Impl> makeLibcameraContext()
{
    return std::make_unique<LibcameraContext>();
}

} // namespace hscam
