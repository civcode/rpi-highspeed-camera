#pragma once

#include "hscam/capture_bundle.hpp"
#include "hscam/context.hpp"
#include "hscam/error.hpp"
#include "hscam/raw/render.hpp"
#include "internal/json.hpp"
#include "raw_capture.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace hscam::qualification {

struct VisualSampleSpec {
    std::string label;
    std::string cameraId;
    std::string cameraModel;
    std::optional<std::string> modeId;
    std::optional<Rect> crop;
    std::optional<std::int64_t> frameDurationUs;
    std::optional<std::int64_t> manualExposureUs;
    std::optional<double> manualAnalogueGain;
    StreamKind streamKind{StreamKind::Raw};
    RawCaptureSelection rawSelection;
    std::uint64_t frameCount{120};
    bool exact{};
    unsigned playbackFps{30};
};

struct VisualSampleResult {
    std::string label;
    std::filesystem::path bundle;
    CaptureConfiguration configuration;
    std::vector<std::filesystem::path> pngs;
    std::optional<std::filesystem::path> dng;
    std::optional<std::filesystem::path> video;
    std::vector<std::string> warnings;
};

inline int runProgram(const std::filesystem::path &program,
                      const std::vector<std::string> &arguments)
{
    const pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        std::vector<std::string> storage;
        storage.reserve(arguments.size() + 1);
        storage.push_back(program.string());
        storage.insert(storage.end(), arguments.begin(), arguments.end());

        std::vector<char *> argv;
        argv.reserve(storage.size() + 1);
        for (auto &item : storage) argv.push_back(item.data());
        argv.push_back(nullptr);
        ::execv(program.c_str(), argv.data());
        _exit(127);
    }

    int status{};
    if (::waitpid(pid, &status, 0) < 0) return -1;
    if (!WIFEXITED(status)) return 128;
    return WEXITSTATUS(status);
}

inline VisualSampleResult captureVisualSample(Context &context,
                                              const VisualSampleSpec &spec,
                                              const std::filesystem::path &directory,
                                              const std::filesystem::path &exporter)
{
    namespace fs = std::filesystem;
    fs::create_directories(directory);

    VisualSampleResult result;
    result.label = spec.label;
    result.bundle = directory / (spec.label + ".hscap");

    if (fs::exists(result.bundle))
        fs::remove_all(result.bundle);

    auto camera = context.open(spec.cameraId);

    CaptureRequest request;
    request.stream.kind = spec.streamKind;
    request.negotiation = spec.exact ? NegotiationPolicy::Exact
                                     : NegotiationPolicy::AllowAdjustments;
    request.sensor.modeId = spec.modeId;
    request.sensor.crop = spec.crop;
    applyRawCaptureSelection(request, spec.rawSelection);
    if (spec.frameDurationUs)
        request.timing.frameDuration = std::chrono::microseconds(*spec.frameDurationUs);
    if (spec.manualExposureUs)
        request.timing.exposure = std::chrono::microseconds(*spec.manualExposureUs);
    if (spec.manualAnalogueGain)
        request.timing.analogueGain = *spec.manualAnalogueGain;

    const auto configuration = camera.configure(request);
    verifyRawCaptureSelection(request, configuration, true);
    auto session = camera.start();
    result.configuration = camera.configuration();

    // Sample capture is outside the timing benchmark. Give auto-exposure time
    // to converge before recording evidence, particularly for one-frame
    // baseline samples where frame zero otherwise may be almost black.
    const auto warmupEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    while (std::chrono::steady_clock::now() < warmupEnd) {
        auto warmup = session.nextFrame(std::chrono::milliseconds(2500));
        (void)warmup;
    }

    BundleWriter writer(result.bundle, spec.cameraId, spec.cameraModel,
                        camera.configuration());
    for (std::uint64_t i = 0; i < spec.frameCount; ++i) {
        auto frame = session.nextFrame(std::chrono::milliseconds(2500));
        writer.append(frame);
    }
    session.stop();
    writer.finalize();

    BundleReader reader(result.bundle);
    if (!reader.manifest().frameCount)
        throw Error("visual sample capture produced zero frames");

    std::vector<std::uint64_t> previewFrames{0};
    if (reader.manifest().frameCount > 2)
        previewFrames.push_back(reader.manifest().frameCount / 2);
    if (reader.manifest().frameCount > 1)
        previewFrames.push_back(reader.manifest().frameCount - 1);

    for (const auto frameIndex : previewFrames) {
        try {
            const auto image = raw::renderPreview(reader.manifest(),
                                                  reader.readFrame(frameIndex));
            // A black/near-constant PNG can exist and pass the file-presence gate.
            // Reject such frames as visual evidence rather than publishing a
            // seemingly successful but uninformative image.
            std::array<std::uint64_t, 256> histogram{};
            for (const auto pixel : image.pixels) ++histogram[pixel];
            auto percentile = [&](std::uint64_t threshold) {
                std::uint64_t count{};
                for (unsigned v = 0; v < histogram.size(); ++v) {
                    count += histogram[v];
                    if (count > threshold) return v;
                }
                return 255U;
            };
            const auto p05 = percentile(image.pixels.size() * 5 / 100);
            const auto p95 = percentile(image.pixels.size() * 95 / 100);
            if (p95 < 8 || p95 - p05 < 6)
                throw Error("black or near-constant preview; inspect exposure, gain and Bayer decoding");
            const auto path = directory /
                (spec.label + "-frame-" + std::to_string(frameIndex) + ".png");
            raw::writePng(path, image);
            result.pngs.push_back(path);
        } catch (const std::exception &e) {
            result.warnings.push_back("PNG frame " + std::to_string(frameIndex) +
                                      ": " + e.what());
        }
    }

    if (result.pngs.empty())
        throw Error("visual sample has no usable PNG frames; check exposure and RAW format");

    if (fs::exists(exporter)) {
        const auto dng = directory / (spec.label + ".dng");
        if (runProgram(exporter, {"dng", result.bundle.string(), "--frame", "0",
                                  "--output", dng.string()}) == 0) {
            result.dng = dng;
        } else {
            result.warnings.push_back("DNG export unavailable or failed");
            std::error_code ec;
            fs::remove(dng, ec);
        }

        if (spec.frameCount > 1) {
            const auto video = directory / (spec.label + "-slow-motion.mp4");
            if (runProgram(exporter, {"video", result.bundle.string(),
                                      "--output", video.string(),
                                      "--playback-fps",
                                      std::to_string(spec.playbackFps)}) == 0) {
                result.video = video;
            } else {
                result.warnings.push_back("video export unavailable or failed");
                std::error_code ec;
                fs::remove(video, ec);
            }
        }
    } else {
        result.warnings.push_back("hscam-export executable not found beside hscam-qualify");
    }

    return result;
}

inline internal::json::Value visualSampleValue(const VisualSampleResult &sample,
                                               const std::filesystem::path &base)
{
    using internal::json::Value;
    auto relative = [&](const std::filesystem::path &path) {
        std::error_code ec;
        const auto value = std::filesystem::relative(path, base, ec);
        return ec ? path.string() : value.string();
    };

    Value::Array pngs;
    for (const auto &path : sample.pngs) pngs.emplace_back(relative(path));
    Value::Array warnings;
    for (const auto &warning : sample.warnings) warnings.emplace_back(warning);

    return Value::Object{
        {"label", sample.label},
        {"bundle", relative(sample.bundle)},
        {"negotiated_pixel_format", sample.configuration.stream.format.name},
        {"requested_pixel_format", sample.configuration.stream.requestedFormat
                                       ? Value(sample.configuration.stream.requestedFormat->name)
                                       : Value(nullptr)},
        {"raw_representation", std::string(representationName(sample.configuration.stream.representation))},
        {"data_bits", static_cast<std::uint64_t>(sample.configuration.stream.dataBits)},
        {"storage_bits", static_cast<std::uint64_t>(sample.configuration.stream.storageBits)},
        {"sample_alignment", std::string(alignmentName(sample.configuration.stream.sampleAlignment))},
        {"pngs", std::move(pngs)},
        {"dng", sample.dng ? Value(relative(*sample.dng)) : Value(nullptr)},
        {"video", sample.video ? Value(relative(*sample.video)) : Value(nullptr)},
        {"warnings", std::move(warnings)}
    };
}

} // namespace hscam::qualification
