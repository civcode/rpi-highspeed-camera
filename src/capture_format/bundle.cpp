#include "hscam/capture_bundle.hpp"
#include "hscam/error.hpp"
#include "internal/json.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>

namespace hscam {
namespace {
using internal::json::Value;

std::string streamKindName(StreamKind kind) { return kind == StreamKind::Raw ? "raw" : "processed"; }
StreamKind parseStreamKind(const std::string &s)
{
    if (s == "raw") return StreamKind::Raw;
    if (s == "processed") return StreamKind::Processed;
    throw Error("invalid HSCAP stream kind: " + s);
}

Value rectValue(const std::optional<Rect> &rect)
{
    if (!rect) return Value(nullptr);
    return Value::Array{rect->x, rect->y, static_cast<std::uint64_t>(rect->width), static_cast<std::uint64_t>(rect->height)};
}

std::optional<Rect> parseRect(const Value &value)
{
    if (value.isNull()) return std::nullopt;
    const auto &a = value.asArray();
    if (a.size() != 4) throw Error("invalid HSCAP crop array");
    return Rect{static_cast<std::int32_t>(a[0].asInt64()), static_cast<std::int32_t>(a[1].asInt64()),
                static_cast<std::uint32_t>(a[2].asUInt64()), static_cast<std::uint32_t>(a[3].asUInt64())};
}

Value manifestValue(const BundleManifest &m)
{
    Value::Array planes;
    for (const auto &p : m.observedPlanes)
        planes.emplace_back(Value::Object{{"stride", static_cast<std::uint64_t>(p.stride)}, {"size", static_cast<std::uint64_t>(p.size)}});

    Value::Object timing;
    if (m.configuration.timing.requestedFrameDuration)
        timing["requested_frame_duration_us"] = static_cast<std::int64_t>(m.configuration.timing.requestedFrameDuration->count());
    else timing["requested_frame_duration_us"] = nullptr;
    if (m.configuration.timing.exposure)
        timing["exposure_us"] = static_cast<std::int64_t>(m.configuration.timing.exposure->count());
    else timing["exposure_us"] = nullptr;
    if (m.configuration.timing.analogueGain)
        timing["analogue_gain"] = *m.configuration.timing.analogueGain;
    else timing["analogue_gain"] = nullptr;

    return Value::Object{
        {"schema_version", static_cast<std::uint64_t>(m.schemaVersion)},
        {"complete", m.complete},
        {"frame_count", m.frameCount},
        {"camera", Value::Object{{"id", m.cameraId}, {"model", m.cameraModel}}},
        {"sensor", Value::Object{{"mode_id", m.configuration.sensor.modeId ? Value(*m.configuration.sensor.modeId) : Value(nullptr)},
                                 {"crop", rectValue(m.configuration.sensor.crop)}}},
        {"stream", Value::Object{{"kind", streamKindName(m.configuration.stream.kind)},
                                 {"pixel_format", m.configuration.stream.format.name},
                                 {"width", static_cast<std::uint64_t>(m.configuration.stream.size.width)},
                                 {"height", static_cast<std::uint64_t>(m.configuration.stream.size.height)},
                                 {"frame_bytes", static_cast<std::uint64_t>(m.configuration.stream.frameBytes)},
                                 {"planes", Value(std::move(planes))}}},
        {"timing", Value(std::move(timing))}
    };
}

BundleManifest parseManifest(const Value &root)
{
    BundleManifest m;
    m.schemaVersion = static_cast<unsigned>(root.at("schema_version").asUInt64());
    if (m.schemaVersion != 1) throw Error("unsupported HSCAP schema version");
    m.complete = root.at("complete").asBool();
    m.frameCount = root.at("frame_count").asUInt64();
    m.cameraId = root.at("camera").at("id").asString();
    m.cameraModel = root.at("camera").at("model").asString();

    const auto &sensor = root.at("sensor");
    if (!sensor.at("mode_id").isNull()) m.configuration.sensor.modeId = sensor.at("mode_id").asString();
    m.configuration.sensor.crop = parseRect(sensor.at("crop"));

    const auto &stream = root.at("stream");
    m.configuration.stream.kind = parseStreamKind(stream.at("kind").asString());
    m.configuration.stream.format.name = stream.at("pixel_format").asString();
    m.configuration.stream.size.width = static_cast<std::uint32_t>(stream.at("width").asUInt64());
    m.configuration.stream.size.height = static_cast<std::uint32_t>(stream.at("height").asUInt64());
    m.configuration.stream.frameBytes = static_cast<std::size_t>(stream.at("frame_bytes").asUInt64());
    for (const auto &p : stream.at("planes").asArray())
        m.observedPlanes.push_back({static_cast<std::uint32_t>(p.at("stride").asUInt64()),
                                    static_cast<std::uint32_t>(p.at("size").asUInt64())});

    const auto &timing = root.at("timing");
    if (!timing.at("requested_frame_duration_us").isNull())
        m.configuration.timing.requestedFrameDuration = std::chrono::microseconds(timing.at("requested_frame_duration_us").asInt64());
    if (!timing.at("exposure_us").isNull())
        m.configuration.timing.exposure = std::chrono::microseconds(timing.at("exposure_us").asInt64());
    if (!timing.at("analogue_gain").isNull())
        m.configuration.timing.analogueGain = timing.at("analogue_gain").asNumber();
    return m;
}

void writeU64(std::ostream &out, std::uint64_t value)
{
    std::array<unsigned char, 8> bytes{};
    for (unsigned i = 0; i < 8; ++i) bytes[i] = static_cast<unsigned char>((value >> (8 * i)) & 0xff);
    out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

std::uint64_t readU64(std::istream &in)
{
    std::array<unsigned char, 8> bytes{};
    if (!in.read(reinterpret_cast<char *>(bytes.data()), bytes.size())) throw Error("truncated HSCAP index");
    std::uint64_t value{};
    for (unsigned i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
    return value;
}

std::string metadataLine(const FrameMetadata &m, std::uint64_t frame)
{
    Value::Object obj{{"frame", frame}, {"sequence", m.sequence}, {"request_cookie", m.requestCookie}};
    obj["sensor_timestamp_ns"] = m.sensorTimestamp ? Value(static_cast<std::int64_t>(m.sensorTimestamp->count())) : Value(nullptr);
    obj["exposure_us"] = m.exposure ? Value(static_cast<std::int64_t>(m.exposure->count())) : Value(nullptr);
    obj["frame_duration_us"] = m.frameDuration ? Value(static_cast<std::int64_t>(m.frameDuration->count())) : Value(nullptr);
    obj["analogue_gain"] = m.analogueGain ? Value(*m.analogueGain) : Value(nullptr);
    obj["status"] = m.status == FrameStatus::Complete ? "complete" : (m.status == FrameStatus::Cancelled ? "cancelled" : "error");
    return internal::json::stringify(Value(std::move(obj)));
}

std::string readAll(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) throw Error("failed to open " + path.string());
    std::ostringstream out; out << in.rdbuf(); return out.str();
}
}

class BundleWriter::Impl {
public:
    Impl(const std::filesystem::path &path, std::string cameraId, std::string cameraModel,
         CaptureConfiguration configuration) : path(path)
    {
        if (std::filesystem::exists(path)) throw Error("HSCAP output already exists: " + path.string());
        std::filesystem::create_directories(path);
        frames.open(path / "frames.bin", std::ios::binary | std::ios::trunc);
        index.open(path / "frames.idx", std::ios::binary | std::ios::trunc);
        metadata.open(path / "metadata.jsonl", std::ios::trunc);
        if (!frames || !index || !metadata) throw Error("failed to create HSCAP files");
        index.write("HSCIDX01", 8);
        manifest.cameraId = std::move(cameraId);
        manifest.cameraModel = std::move(cameraModel);
        manifest.configuration = std::move(configuration);
        writeManifest(false);
    }

    ~Impl()
    {
        if (!finalized) {
            try { closeFiles(); writeManifest(false); } catch (...) {}
        }
    }

    void writeManifest(bool complete)
    {
        manifest.complete = complete;
        std::ofstream out(path / "manifest.json", std::ios::trunc);
        if (!out) throw Error("failed to write HSCAP manifest");
        out << internal::json::stringify(manifestValue(manifest), 2) << '\n';
    }

    void closeFiles()
    {
        if (frames.is_open()) frames.close();
        if (index.is_open()) index.close();
        if (metadata.is_open()) metadata.close();
    }

    std::filesystem::path path;
    BundleManifest manifest;
    std::ofstream frames, index, metadata;
    std::uint64_t payloadOffset{};
    bool finalized{};
};

BundleWriter::BundleWriter(const std::filesystem::path &path, std::string cameraId, std::string cameraModel,
                           CaptureConfiguration configuration)
    : impl_(std::make_unique<Impl>(path, std::move(cameraId), std::move(cameraModel), std::move(configuration))) {}
BundleWriter::~BundleWriter() = default;
BundleWriter::BundleWriter(BundleWriter &&) noexcept = default;
BundleWriter &BundleWriter::operator=(BundleWriter &&) noexcept = default;

void BundleWriter::append(const FrameMetadata &metadata, std::span<const PlaneView> planes)
{
    if (!impl_ || impl_->finalized) throw Error("cannot append to finalized HSCAP bundle");
    if (planes.empty()) throw Error("cannot append a frame with zero planes");

    if (impl_->manifest.observedPlanes.empty()) {
        for (const auto &plane : planes) impl_->manifest.observedPlanes.push_back({plane.stride, plane.length});
    } else if (impl_->manifest.observedPlanes.size() != planes.size()) {
        throw Error("HSCAP plane count changed during capture");
    }

    std::uint64_t length{};
    for (std::size_t i = 0; i < planes.size(); ++i) {
        if (impl_->manifest.observedPlanes[i].size != planes[i].length)
            throw Error("HSCAP plane length changed during capture");
        impl_->frames.write(reinterpret_cast<const char *>(planes[i].data.data()), planes[i].data.size());
        if (!impl_->frames) throw Error("failed writing HSCAP payload");
        length += planes[i].data.size();
    }

    writeU64(impl_->index, impl_->manifest.frameCount);
    writeU64(impl_->index, impl_->payloadOffset);
    writeU64(impl_->index, length);
    writeU64(impl_->index, impl_->manifest.frameCount);
    if (!impl_->index) throw Error("failed writing HSCAP index");

    impl_->metadata << metadataLine(metadata, impl_->manifest.frameCount) << '\n';
    if (!impl_->metadata) throw Error("failed writing HSCAP metadata");

    impl_->payloadOffset += length;
    ++impl_->manifest.frameCount;
}

void BundleWriter::finalize()
{
    if (!impl_ || impl_->finalized) return;
    impl_->frames.flush(); impl_->index.flush(); impl_->metadata.flush();
    if (!impl_->frames || !impl_->index || !impl_->metadata) throw Error("failed flushing HSCAP bundle");
    impl_->closeFiles();
    impl_->writeManifest(true);
    impl_->finalized = true;
}

const BundleManifest &BundleWriter::manifest() const noexcept { return impl_->manifest; }

BundleReader::BundleReader(const std::filesystem::path &path) : path_(path)
{
    manifest_ = parseManifest(internal::json::parse(readAll(path / "manifest.json")));
    std::ifstream in(path / "frames.idx", std::ios::binary);
    if (!in) throw Error("failed to open HSCAP index");
    char magic[8]{};
    if (!in.read(magic, sizeof(magic)) || std::memcmp(magic, "HSCIDX01", 8) != 0) throw Error("invalid HSCAP index magic");
    while (in.peek() != std::char_traits<char>::eof()) {
        IndexRecord record;
        record.frame = readU64(in); record.offset = readU64(in); record.length = readU64(in); record.metadataLine = readU64(in);
        index_.push_back(record);
    }
    if (index_.size() != manifest_.frameCount) throw Error("HSCAP manifest/index frame count mismatch");
}

std::vector<std::byte> BundleReader::readFrame(std::uint64_t frameNumber) const
{
    if (frameNumber >= index_.size()) throw Error("HSCAP frame out of range");
    const auto &record = index_[frameNumber];
    std::ifstream in(path_ / "frames.bin", std::ios::binary);
    if (!in) throw Error("failed to open HSCAP payload");
    in.seekg(static_cast<std::streamoff>(record.offset));
    std::vector<std::byte> data(record.length);
    if (!in.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size())))
        throw Error("truncated HSCAP payload");
    return data;
}

} // namespace hscam
