#include "hscam/capture_bundle.hpp"
#include "hscam/context.hpp"
#include "hscam/error.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

namespace {

std::uint64_t parseUnsigned(const char *text, const char *name)
{
    std::uint64_t value{};
    const std::string s(text);
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || ptr != s.data() + s.size())
        throw std::runtime_error(std::string("invalid ") + name);
    return value;
}

hscam::Rect parseCrop(const std::string &text)
{
    long long x{}, y{};
    unsigned long long w{}, h{};
    char c1{}, c2{}, c3{};
    std::istringstream in(text);
    if (!(in >> x >> c1 >> y >> c2 >> w >> c3 >> h) ||
        c1 != ',' || c2 != ',' || c3 != ',' || !in.eof())
        throw std::runtime_error("invalid crop; expected x,y,width,height");

    if (x < std::numeric_limits<std::int32_t>::min() || x > std::numeric_limits<std::int32_t>::max() ||
        y < std::numeric_limits<std::int32_t>::min() || y > std::numeric_limits<std::int32_t>::max() ||
        w > std::numeric_limits<std::uint32_t>::max() ||
        h > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("crop value out of range");

    return {static_cast<std::int32_t>(x), static_cast<std::int32_t>(y),
            static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)};
}

void usage()
{
    std::cerr
        << "usage: hscam-capture [--camera ID] [--mode ID] [--frames N]\n"
        << "                     [--sensor-crop x,y,w,h] [--frame-duration-us N]\n"
        << "                     [--exposure-us N] [--gain G] [--exact]\n"
        << "                     [--discard-payload | --output capture.hscap]\n";
}

} // namespace

int main(int argc, char **argv)
{
    std::string cameraId;
    std::string modeId;
    std::string output;
    std::uint64_t frameCount = 1000;
    bool discardPayload = false;
    bool exact = false;
    std::optional<hscam::Rect> crop;
    std::optional<std::chrono::microseconds> frameDuration;
    std::optional<std::chrono::microseconds> exposure;
    std::optional<double> gain;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--camera" && i + 1 < argc) cameraId = argv[++i];
        else if (arg == "--mode" && i + 1 < argc) modeId = argv[++i];
        else if (arg == "--frames" && i + 1 < argc)
            frameCount = parseUnsigned(argv[++i], "frame count");
        else if (arg == "--sensor-crop" && i + 1 < argc)
            crop = parseCrop(argv[++i]);
        else if (arg == "--frame-duration-us" && i + 1 < argc)
            frameDuration = std::chrono::microseconds(parseUnsigned(argv[++i], "frame duration"));
        else if (arg == "--exposure-us" && i + 1 < argc)
            exposure = std::chrono::microseconds(parseUnsigned(argv[++i], "exposure"));
        else if (arg == "--gain" && i + 1 < argc)
            gain = std::stod(argv[++i]);
        else if (arg == "--exact")
            exact = true;
        else if (arg == "--discard-payload")
            discardPayload = true;
        else if (arg == "--output" && i + 1 < argc)
            output = argv[++i];
        else {
            usage();
            return 2;
        }
    }

    if (!output.empty() && discardPayload) {
        std::cerr << "choose either --output or --discard-payload\n";
        return 2;
    }
    if (output.empty())
        discardPayload = true;

    try {
        hscam::Context context;
        const auto cameras = context.cameras();
        if (cameraId.empty()) {
            if (cameras.size() != 1)
                throw std::runtime_error("--camera is required unless exactly one camera is present");
            cameraId = cameras.front().id;
        }

        const auto cameraInfo = std::find_if(cameras.begin(), cameras.end(),
            [&](const hscam::CameraInfo &info) { return info.id == cameraId; });
        if (cameraInfo == cameras.end())
            throw std::runtime_error("camera not found in inventory: " + cameraId);

        hscam::Camera camera = context.open(cameraId);
        hscam::CaptureRequest request;
        request.stream.kind = hscam::StreamKind::Raw;
        request.negotiation = exact ? hscam::NegotiationPolicy::Exact
                                    : hscam::NegotiationPolicy::AllowAdjustments;
        if (!modeId.empty()) request.sensor.modeId = modeId;
        request.sensor.crop = crop;
        request.timing.frameDuration = frameDuration;
        request.timing.exposure = exposure;
        request.timing.analogueGain = gain;

        const auto config = camera.configure(request);
        std::cout << "configured " << config.stream.size << ' ' << config.stream.format.name
                  << ", buffers=" << config.stream.bufferCount;
        if (config.sensor.crop)
            std::cout << ", sensor_crop=" << *config.sensor.crop;
        std::cout << '\n';

        for (const auto &adjustment : config.adjustments)
            std::cout << "adjusted " << adjustment.field << ": "
                      << adjustment.requested << " -> " << adjustment.negotiated << '\n';

        std::unique_ptr<hscam::BundleWriter> writer;
        if (!output.empty())
            writer = std::make_unique<hscam::BundleWriter>(
                output, cameraInfo->id, cameraInfo->model, config);

        auto session = camera.start();
        for (std::uint64_t i = 0; i < frameCount; ++i) {
            auto frame = session.nextFrame(std::chrono::milliseconds(2000));
            if (!frame)
                throw std::runtime_error("empty frame");
            if (writer)
                writer->append(frame);
        }
        session.stop();

        if (writer)
            writer->finalize();

        const auto stats = session.stats();
        std::cout << "frames=" << stats.requestsCompleted
                  << " gaps=" << stats.sequenceGaps
                  << " overruns=" << stats.completedQueueOverruns;
        if (stats.measuredFps)
            std::cout << " fps=" << *stats.measuredFps;
        if (writer)
            std::cout << " stored=" << writer->manifest().frameCount;
        std::cout << '\n';

        return stats.completedQueueOverruns == 0 ? 0 : 3;
    } catch (const std::exception &e) {
        std::cerr << "hscam-capture: " << e.what() << '\n';
        return 1;
    }
}
