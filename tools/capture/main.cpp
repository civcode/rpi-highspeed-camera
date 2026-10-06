#include "hscam/capture_bundle.hpp"
#include "hscam/context.hpp"
#include "hscam/error.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

namespace {
template <class T>
T parseNumber(const std::string &text, const char *what)
{
    T value{};
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size())
        throw std::runtime_error(std::string("invalid ") + what + ": " + text);
    return value;
}

hscam::Size parseSize(const std::string &text)
{
    const auto x = text.find('x');
    if (x == std::string::npos) throw std::runtime_error("size must be WIDTHxHEIGHT");
    return {parseNumber<std::uint32_t>(text.substr(0, x), "width"),
            parseNumber<std::uint32_t>(text.substr(x + 1), "height")};
}

hscam::Rect parseCrop(const std::string &text)
{
    std::stringstream ss(text);
    std::string item;
    long long values[4]{};
    for (int i = 0; i < 4; ++i) {
        if (!std::getline(ss, item, ',')) throw std::runtime_error("crop must be X,Y,W,H");
        values[i] = parseNumber<long long>(item, "crop value");
    }
    if (std::getline(ss, item, ',')) throw std::runtime_error("crop must be X,Y,W,H");
    if (values[0] < 0 || values[1] < 0 || values[2] <= 0 || values[3] <= 0)
        throw std::runtime_error("crop values must be non-negative with positive width/height");
    return {static_cast<std::int32_t>(values[0]), static_cast<std::int32_t>(values[1]),
            static_cast<std::uint32_t>(values[2]), static_cast<std::uint32_t>(values[3])};
}

void usage()
{
    std::cerr << "usage: hscam-capture [--camera ID] [--mode ID] [--frames N] [--raw|--processed]\n"
                 "       [--size WxH] [--format PIXELFORMAT] [--crop X,Y,W,H]\n"
                 "       [--frame-duration-us N] [--exposure-us N] [--gain X]\n"
                 "       [--exact] (--discard-payload | --output PATH.hscap)\n";
}
}

int main(int argc, char **argv)
{
    std::string cameraId;
    std::string modeId;
    std::string output;
    std::string format;
    std::uint64_t frameCount = 1000;
    bool discard = false;
    bool exact = false;
    hscam::StreamKind streamKind = hscam::StreamKind::Raw;
    std::optional<hscam::Size> size;
    std::optional<hscam::Rect> crop;
    std::optional<std::chrono::microseconds> frameDuration;
    std::optional<std::chrono::microseconds> exposure;
    std::optional<double> gain;

    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto value = [&](const char *name) -> std::string {
                if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + name);
                return argv[++i];
            };
            if (arg == "--camera") cameraId = value("--camera");
            else if (arg == "--mode") modeId = value("--mode");
            else if (arg == "--frames") frameCount = parseNumber<std::uint64_t>(value("--frames"), "frame count");
            else if (arg == "--size") size = parseSize(value("--size"));
            else if (arg == "--format") format = value("--format");
            else if (arg == "--crop") crop = parseCrop(value("--crop"));
            else if (arg == "--frame-duration-us") frameDuration = std::chrono::microseconds(parseNumber<std::int64_t>(value("--frame-duration-us"), "frame duration"));
            else if (arg == "--exposure-us") exposure = std::chrono::microseconds(parseNumber<std::int64_t>(value("--exposure-us"), "exposure"));
            else if (arg == "--gain") gain = std::stod(value("--gain"));
            else if (arg == "--output") output = value("--output");
            else if (arg == "--discard-payload") discard = true;
            else if (arg == "--exact") exact = true;
            else if (arg == "--raw") streamKind = hscam::StreamKind::Raw;
            else if (arg == "--processed") streamKind = hscam::StreamKind::Processed;
            else { usage(); return 2; }
        }

        if (discard == !output.empty()) {
            usage();
            throw std::runtime_error("select exactly one of --discard-payload or --output");
        }

        hscam::Context context;
        auto cameras = context.cameras();
        if (cameraId.empty()) {
            if (cameras.size() != 1)
                throw std::runtime_error("--camera is required unless exactly one camera is present");
            cameraId = cameras.front().id;
        }
        auto infoIt = std::find_if(cameras.begin(), cameras.end(), [&](const auto &x) { return x.id == cameraId; });
        if (infoIt == cameras.end()) throw std::runtime_error("camera is not present: " + cameraId);

        hscam::Camera camera = context.open(cameraId);
        hscam::CaptureRequest request;
        request.stream.kind = streamKind;
        request.negotiation = exact ? hscam::NegotiationPolicy::Exact : hscam::NegotiationPolicy::AllowAdjustments;
        if (!modeId.empty()) request.sensor.modeId = modeId;
        if (size) request.stream.size = size;
        if (!format.empty()) request.stream.format = hscam::PixelFormat{format};
        request.sensor.crop = crop;
        request.timing.frameDuration = frameDuration;
        request.timing.exposure = exposure;
        request.timing.analogueGain = gain;

        const auto config = camera.configure(request);
        std::cout << "configured " << config.stream.size << " " << config.stream.format.name
                  << ", buffers=" << config.stream.bufferCount;
        if (config.sensor.crop) std::cout << ", sensor-crop=" << *config.sensor.crop;
        std::cout << "\n";
        for (const auto &a : config.adjustments)
            std::cout << "adjusted " << a.field << ": " << a.requested << " -> " << a.negotiated << "\n";

        auto session = camera.start();
        std::unique_ptr<hscam::BundleWriter> writer;
        if (!output.empty())
            writer = std::make_unique<hscam::BundleWriter>(output, infoIt->id, infoIt->model, camera.configuration());

        for (std::uint64_t i = 0; i < frameCount; ++i) {
            auto frame = session.nextFrame(std::chrono::milliseconds(2000));
            if (!frame) throw std::runtime_error("empty frame");
            if (writer) writer->append(frame);
        }
        session.stop();
        if (writer) writer->finalize();

        const auto stats = session.stats();
        std::cout << "frames=" << stats.requestsCompleted
                  << " gaps=" << stats.sequenceGaps
                  << " overruns=" << stats.completedQueueOverruns;
        if (stats.measuredFps) std::cout << " fps=" << *stats.measuredFps;
        std::cout << '\n';
        return stats.completedQueueOverruns == 0 ? 0 : 3;
    } catch (const std::exception &e) {
        std::cerr << "hscam-capture: " << e.what() << '\n';
        return 1;
    }
}
