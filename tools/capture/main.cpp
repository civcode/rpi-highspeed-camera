#include "hscam/context.hpp"
#include "hscam/error.hpp"

#include <charconv>
#include <chrono>
#include <iostream>
#include <string>

namespace {
std::uint64_t parseCount(const char *text)
{
    std::uint64_t value{};
    const std::string s(text);
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || ptr != s.data() + s.size())
        throw std::runtime_error("invalid frame count");
    return value;
}
}

int main(int argc, char **argv)
{
    std::string cameraId;
    std::string modeId;
    std::uint64_t frameCount = 1000;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--camera" && i + 1 < argc) cameraId = argv[++i];
        else if (arg == "--mode" && i + 1 < argc) modeId = argv[++i];
        else if (arg == "--frames" && i + 1 < argc) frameCount = parseCount(argv[++i]);
        else if (arg == "--discard-payload") {}
        else {
            std::cerr << "usage: hscam-capture --camera ID [--mode ID] [--frames N] --discard-payload\n";
            return 2;
        }
    }

    try {
        hscam::Context context;
        auto cameras = context.cameras();
        if (cameraId.empty()) {
            if (cameras.size() != 1)
                throw std::runtime_error("--camera is required unless exactly one camera is present");
            cameraId = cameras.front().id;
        }

        hscam::Camera camera = context.open(cameraId);
        hscam::CaptureRequest request;
        request.stream.kind = hscam::StreamKind::Raw;
        if (!modeId.empty()) request.sensor.modeId = modeId;

        const auto config = camera.configure(request);
        std::cout << "configured " << config.stream.size << " " << config.stream.format.name
                  << ", buffers=" << config.stream.bufferCount << "\n";

        auto session = camera.start();
        for (std::uint64_t i = 0; i < frameCount; ++i) {
            auto frame = session.nextFrame(std::chrono::milliseconds(2000));
            if (!frame)
                throw std::runtime_error("empty frame");
        }
        session.stop();

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
