#include "hscam/context.hpp"
#include "hscam/error.hpp"
#include "hscam/version.hpp"
#include "internal/json.hpp"
#include "crop_geometry.hpp"
#include "visual_samples.hpp"
#include "promote.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef HSCAM_SOURCE_REVISION
#define HSCAM_SOURCE_REVISION "unknown"
#endif

namespace {
using hscam::internal::json::Value;
namespace fs = std::filesystem;

struct Policy {
    std::string campaign;
    std::uint64_t workerTimeoutMs{15000};
    bool requireZeroDrops{true};
    bool advertisedEnabled{true};
    std::uint64_t advertisedDurationMs{3000};
    bool cropsEnabled{};
    std::uint64_t cropDurationMs{3000};
    bool cropsExact{true};
    std::vector<hscam::Rect> crops;

    hscam::qualification::CropGeometryPolicy geometry;

    bool cropPerformanceEnabled{};
    std::string cropPerformanceStrategy{"all_heights_full_width"};
    std::uint64_t cropPerformanceDurationMs{5000};
    bool cropPerformanceExact{true};
    std::uint64_t cropPerformanceMaxCases{};
    bool cropPerformanceSamplePositions{};

    bool fpsSearchEnabled{};
    std::int64_t fpsMinFrameDurationUs{100};
    std::int64_t fpsMaxFrameDurationUs{100000};
    std::uint64_t fpsShortDurationMs{1500};
    std::uint64_t fpsFinalDurationMs{10000};
    std::uint64_t fpsMaxIterations{16};
    std::uint64_t fpsToleranceUs{2};
    double fpsAchievedRatio{0.99};
    double fpsMaxIntervalRatio{1.5};

    bool requireNoThrottling{true};
    bool visualSamplesEnabled{true};
    std::uint64_t visualSampleFrames{120};
    unsigned visualPlaybackFps{30};
};

struct TestCase {
    std::string id;
    std::string cameraId;
    std::optional<std::string> modeId;
    std::optional<hscam::Rect> crop;
    std::optional<std::int64_t> frameDurationUs;
    hscam::StreamKind streamKind{hscam::StreamKind::Raw};
    std::uint64_t durationMs{3000};
    bool exact{true};
    bool requireZeroDrops{true};
};

std::string readAll(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("failed to open " + path.string());
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void writeText(const fs::path &path, const std::string &text)
{
    const auto tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("failed to create " + tmp);
        out << text;
        if (!out) throw std::runtime_error("failed writing " + tmp);
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        ec.clear();
        fs::rename(tmp, path, ec);
        if (ec) throw std::runtime_error("failed to publish " + path.string() + ": " + ec.message());
    }
}

std::string trim(std::string value)
{
    while (!value.empty() && (value.back() == '\0' || value.back() == '\n' ||
                              value.back() == '\r' || value.back() == ' ' || value.back() == '\t'))
        value.pop_back();
    std::size_t first{};
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
    return value.substr(first);
}

std::string readOptional(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream out;
    out << in.rdbuf();
    return trim(out.str());
}

std::optional<std::string> runCommand(const std::vector<std::string> &args)
{
    if (args.empty()) return std::nullopt;
    int pipefd[2];
    if (::pipe(pipefd) < 0) return std::nullopt;

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]); ::close(pipefd[1]);
        return std::nullopt;
    }

    if (pid == 0) {
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[0]);
        ::close(pipefd[1]);

        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (const auto &arg : args) argv.push_back(const_cast<char *>(arg.c_str()));
        argv.push_back(nullptr);
        ::execvp(argv.front(), argv.data());
        _exit(127);
    }

    ::close(pipefd[1]);
    std::string output;
    char buffer[4096];
    for (;;) {
        const ssize_t n = ::read(pipefd[0], buffer, sizeof(buffer));
        if (n > 0) output.append(buffer, static_cast<std::size_t>(n));
        else if (n == 0) break;
        else if (errno != EINTR) break;
    }
    ::close(pipefd[0]);

    int status{};
    if (::waitpid(pid, &status, 0) < 0) return std::nullopt;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return std::nullopt;
    return trim(output);
}

Value rectValue(const std::optional<hscam::Rect> &rect)
{
    if (!rect) return nullptr;
    return Value::Array{
        static_cast<std::int64_t>(rect->x),
        static_cast<std::int64_t>(rect->y),
        static_cast<std::uint64_t>(rect->width),
        static_cast<std::uint64_t>(rect->height)
    };
}

Value cropProbeValue(const hscam::qualification::CropProbeRecord &record)
{
    Value::Object object{
        {"phase", record.phase},
        {"requested", rectValue(record.requested)},
        {"negotiated", record.negotiated ? rectValue(*record.negotiated) : Value(nullptr)},
        {"exact", record.exact},
        {"error", record.error ? Value(*record.error) : Value(nullptr)}
    };
    return object;
}

std::optional<hscam::Rect> parseRect(const Value &value)
{
    if (value.isNull()) return std::nullopt;
    const auto &a = value.asArray();
    if (a.size() != 4) throw std::runtime_error("crop must contain four values");
    const auto x = a[0].asInt64();
    const auto y = a[1].asInt64();
    const auto w = a[2].asUInt64();
    const auto h = a[3].asUInt64();
    if (x < 0 || y < 0 || w == 0 || h == 0 ||
        x > static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) ||
        y > static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) ||
        w > std::numeric_limits<std::uint32_t>::max() ||
        h > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("crop is out of range");
    return hscam::Rect{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y),
                       static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)};
}

bool boolOr(const Value &object, std::string_view key, bool fallback)
{
    if (const auto *v = object.find(key)) return v->asBool();
    return fallback;
}

std::uint64_t uintOr(const Value &object, std::string_view key, std::uint64_t fallback)
{
    if (const auto *v = object.find(key)) return v->asUInt64();
    return fallback;
}

Policy parsePolicy(const Value &root)
{
    if (root.at("schema_version").asUInt64() != 1)
        throw std::runtime_error("unsupported qualification manifest schema");

    Policy policy;
    policy.campaign = root.at("campaign").asString();
    policy.workerTimeoutMs = uintOr(root, "worker_timeout_ms", policy.workerTimeoutMs);
    policy.requireZeroDrops = boolOr(root, "require_zero_drops", true);

    if (const auto *advertised = root.find("advertised_modes")) {
        policy.advertisedEnabled = boolOr(*advertised, "enabled", true);
        policy.advertisedDurationMs = uintOr(*advertised, "duration_ms", 3000);
    }

    if (const auto *crops = root.find("explicit_crops")) {
        policy.cropsEnabled = boolOr(*crops, "enabled", false);
        policy.cropDurationMs = uintOr(*crops, "duration_ms", 3000);
        policy.cropsExact = boolOr(*crops, "exact", true);
        if (const auto *items = crops->find("crops")) {
            for (const auto &item : items->asArray()) {
                auto rect = parseRect(item);
                if (rect) policy.crops.push_back(*rect);
            }
        }
    }

    if (const auto *geometry = root.find("crop_geometry")) {
        policy.geometry.enabled = boolOr(*geometry, "enabled", true);
        policy.geometry.verifyCartesian = boolOr(*geometry, "verify_cartesian", true);
        policy.geometry.maxCartesianChecks = uintOr(*geometry, "max_cartesian_checks", 0);
    } else {
        policy.geometry.enabled = false;
    }

    if (const auto *performance = root.find("crop_performance")) {
        policy.cropPerformanceEnabled = boolOr(*performance, "enabled", false);
        policy.cropPerformanceDurationMs = uintOr(*performance, "duration_ms", 5000);
        policy.cropPerformanceExact = boolOr(*performance, "exact", true);
        policy.cropPerformanceMaxCases = uintOr(*performance, "max_cases", 0);
        policy.cropPerformanceSamplePositions =
            boolOr(*performance, "sample_positions", false);
        if (const auto *strategy = performance->find("strategy"))
            policy.cropPerformanceStrategy = strategy->asString();
    }

    if (const auto *fps = root.find("fps_search")) {
        policy.fpsSearchEnabled = boolOr(*fps, "enabled", false);
        policy.fpsMinFrameDurationUs = static_cast<std::int64_t>(
            uintOr(*fps, "min_frame_duration_us", 100));
        policy.fpsMaxFrameDurationUs = static_cast<std::int64_t>(
            uintOr(*fps, "max_frame_duration_us", 100000));
        policy.fpsShortDurationMs = uintOr(*fps, "short_duration_ms", 1500);
        policy.fpsFinalDurationMs = uintOr(*fps, "final_duration_ms", 10000);
        policy.fpsMaxIterations = uintOr(*fps, "max_iterations", 16);
        policy.fpsToleranceUs = uintOr(*fps, "tolerance_us", 2);
        if (const auto *ratio = fps->find("achieved_ratio"))
            policy.fpsAchievedRatio = ratio->asNumber();
        if (const auto *ratio = fps->find("max_interval_ratio"))
            policy.fpsMaxIntervalRatio = ratio->asNumber();

        if (policy.fpsMinFrameDurationUs <= 0 ||
            policy.fpsMaxFrameDurationUs < policy.fpsMinFrameDurationUs)
            throw std::runtime_error("invalid fps_search frame-duration range");
        if (!(policy.fpsAchievedRatio > 0.0 && policy.fpsAchievedRatio <= 1.0))
            throw std::runtime_error("fps_search achieved_ratio must be in (0,1]");
        if (policy.fpsMaxIntervalRatio < 1.0)
            throw std::runtime_error("fps_search max_interval_ratio must be >= 1");
    }

    if (const auto *environment = root.find("environment"))
        policy.requireNoThrottling = boolOr(*environment, "require_no_throttling", true);

    if (const auto *visual = root.find("visual_samples")) {
        policy.visualSamplesEnabled = boolOr(*visual, "enabled", true);
        policy.visualSampleFrames = uintOr(*visual, "fastest_frame_count", 120);
        policy.visualPlaybackFps = static_cast<unsigned>(
            uintOr(*visual, "playback_fps", 30));
        if (!policy.visualPlaybackFps)
            throw std::runtime_error("visual_samples playback_fps must be positive");
    }

    return policy;
}

Value caseValue(const TestCase &test)
{
    Value::Object object{
        {"case_id", test.id},
        {"camera_id", test.cameraId},
        {"mode_id", test.modeId ? Value(*test.modeId) : Value(nullptr)},
        {"crop", rectValue(test.crop)},
        {"frame_duration_us", test.frameDurationUs ? Value(*test.frameDurationUs) : Value(nullptr)},
        {"stream_kind", test.streamKind == hscam::StreamKind::Raw ? "raw" : "processed"},
        {"duration_ms", test.durationMs},
        {"exact", test.exact},
        {"require_zero_drops", test.requireZeroDrops}
    };
    return object;
}

TestCase parseCase(const Value &root)
{
    TestCase test;
    test.id = root.at("case_id").asString();
    test.cameraId = root.at("camera_id").asString();
    if (!root.at("mode_id").isNull()) test.modeId = root.at("mode_id").asString();
    test.crop = parseRect(root.at("crop"));
    if (!root.at("frame_duration_us").isNull()) test.frameDurationUs = root.at("frame_duration_us").asInt64();
    if (const auto *kind = root.find("stream_kind")) {
        if (kind->asString() == "raw") test.streamKind = hscam::StreamKind::Raw;
        else if (kind->asString() == "processed") test.streamKind = hscam::StreamKind::Processed;
        else throw std::runtime_error("invalid qualification stream_kind");
    }
    test.durationMs = root.at("duration_ms").asUInt64();
    test.exact = root.at("exact").asBool();
    test.requireZeroDrops = root.at("require_zero_drops").asBool();
    return test;
}

std::string testCaseKey(const TestCase &test)
{
    std::ostringstream canonical;
    canonical << test.cameraId << '|';
    if (test.modeId) canonical << *test.modeId;
    canonical << '|';
    if (test.crop)
        canonical << test.crop->x << ',' << test.crop->y << ',' << test.crop->width << ',' << test.crop->height;
    canonical << '|';
    if (test.frameDurationUs) canonical << *test.frameDurationUs;
    canonical << '|' << (test.streamKind == hscam::StreamKind::Raw ? "raw" : "processed");
    canonical << '|' << test.durationMs << '|' << test.exact << '|' << test.requireZeroDrops;
    return fnvHex(canonical.str());
}

Value cameraValue(const hscam::CameraInfo &camera)
{
    Value::Array modes;
    for (const auto &mode : camera.sensorModes) {
        modes.emplace_back(Value::Object{
            {"id", mode.id},
            {"width", static_cast<std::uint64_t>(mode.size.width)},
            {"height", static_cast<std::uint64_t>(mode.size.height)},
            {"pixel_format", mode.format.name},
            {"bit_depth", static_cast<std::uint64_t>(mode.bitDepth)}
        });
    }

    Value::Object capabilities{
        {"raw_capture", camera.capabilities.rawCapture},
        {"processed_capture", camera.capabilities.processedCapture},
        {"sensor_crop_queryable", camera.capabilities.sensorCropQueryable},
        {"sensor_crop_tryable", camera.capabilities.sensorCropTryable},
        {"sensor_crop_settable", camera.capabilities.sensorCropSettable},
        {"sensor_crop_exact", camera.capabilities.sensorCropExact},
        {"frame_duration_control", camera.capabilities.frameDurationControl},
        {"exposure_control", camera.capabilities.exposureControl},
        {"analogue_gain_control", camera.capabilities.analogueGainControl}
    };

    return Value::Object{
        {"schema_version", static_cast<std::uint64_t>(1)},
        {"id", camera.id},
        {"model", camera.model},
        {"sensor_model", camera.sensorModel ? Value(*camera.sensorModel) : Value(nullptr)},
        {"pixel_array", Value::Array{
            static_cast<std::uint64_t>(camera.pixelArray.width),
            static_cast<std::uint64_t>(camera.pixelArray.height)
        }},
        {"sensor_subdevice", camera.sensorSubdevice ? Value(*camera.sensorSubdevice) : Value(nullptr)},
        {"capabilities", std::move(capabilities)},
        {"modes", std::move(modes)}
    };
}

Value environmentValue()
{
    struct utsname uts{};
    const bool haveUname = ::uname(&uts) == 0;

    std::optional<std::int64_t> tempMilliC;
    const auto temp = readOptional("/sys/class/thermal/thermal_zone0/temp");
    if (!temp.empty()) {
        try { tempMilliC = std::stoll(temp); } catch (...) {}
    }

    Value::Object environment{
        {"schema_version", static_cast<std::uint64_t>(1)},
        {"hscam_version", HSCAM_VERSION_STRING},
        {"source_revision", HSCAM_SOURCE_REVISION},
        {"board_model", readOptional("/proc/device-tree/model")},
        {"os_release", readOptional("/etc/os-release")},
        {"kernel", haveUname ? Value(std::string(uts.release)) : Value(nullptr)},
        {"machine", haveUname ? Value(std::string(uts.machine)) : Value(nullptr)},
        {"thermal_millic", tempMilliC ? Value(*tempMilliC) : Value(nullptr)}
    };

    if (auto version = runCommand({"pkg-config", "--modversion", "libcamera"}))
        environment["libcamera_version"] = *version;
    else
        environment["libcamera_version"] = nullptr;

    if (auto throttled = runCommand({"vcgencmd", "get_throttled"}))
        environment["vcgencmd_get_throttled"] = *throttled;
    else
        environment["vcgencmd_get_throttled"] = nullptr;

    return environment;
}

bool throttlingClean()
{
    const auto value = runCommand({"vcgencmd", "get_throttled"});
    return value && trim(*value) == "throttled=0x0";
}

void requireCleanThrottling(std::string_view phase)
{
    const auto value = runCommand({"vcgencmd", "get_throttled"});
    if (!value)
        throw std::runtime_error("cannot verify throttling state during " +
                                 std::string(phase) + ": vcgencmd unavailable");
    if (trim(*value) != "throttled=0x0")
        throw std::runtime_error("Pi throttling state is not clean during " +
                                 std::string(phase) + ": " + trim(*value));
}

Value captureConfigurationValue(const hscam::CaptureConfiguration &config)
{
    Value::Array adjustments;
    for (const auto &a : config.adjustments) {
        adjustments.emplace_back(Value::Object{
            {"field", a.field},
            {"requested", a.requested},
            {"negotiated", a.negotiated}
        });
    }

    return Value::Object{
        {"mode_id", config.sensor.modeId ? Value(*config.sensor.modeId) : Value(nullptr)},
        {"crop", rectValue(config.sensor.crop)},
        {"stream", Value::Object{
            {"width", static_cast<std::uint64_t>(config.stream.size.width)},
            {"height", static_cast<std::uint64_t>(config.stream.size.height)},
            {"pixel_format", config.stream.format.name},
            {"frame_bytes", static_cast<std::uint64_t>(config.stream.frameBytes)},
            {"buffer_count", static_cast<std::uint64_t>(config.stream.bufferCount)}
        }},
        {"adjustments", std::move(adjustments)}
    };
}

struct TimingSample {
    std::uint64_t frame{};
    std::uint64_t sequence{};
    std::optional<std::int64_t> sensorTimestampNs;
    std::optional<std::int64_t> frameDurationUs;
    std::optional<std::int64_t> exposureUs;
    hscam::FrameStatus status{hscam::FrameStatus::Complete};
};

std::filesystem::path timingTracePath(const std::filesystem::path &resultPath)
{
    auto trace = resultPath;
    trace.replace_extension(".timing.jsonl");
    return trace;
}

void writeTimingTrace(const std::filesystem::path &resultPath,
                      const std::vector<TimingSample> &samples)
{
    const auto tracePath = timingTracePath(resultPath);
    std::ofstream out(tracePath, std::ios::binary | std::ios::trunc);
    if (!out)
        throw std::runtime_error("failed to create timing trace: " +
                                 tracePath.string());

    for (const auto &sample : samples) {
        Value::Object row{
            {"frame", sample.frame},
            {"sequence", sample.sequence},
            {"sensor_timestamp_ns",
             sample.sensorTimestampNs ? Value(*sample.sensorTimestampNs)
                                      : Value(nullptr)},
            {"frame_duration_us",
             sample.frameDurationUs ? Value(*sample.frameDurationUs)
                                    : Value(nullptr)},
            {"exposure_us",
             sample.exposureUs ? Value(*sample.exposureUs) : Value(nullptr)},
            {"status",
             sample.status == hscam::FrameStatus::Complete ? "complete" :
             (sample.status == hscam::FrameStatus::Cancelled ? "cancelled" :
                                                               "error")}
        };
        out << hscam::internal::json::stringify(Value(std::move(row))) << '\n';
    }
    if (!out)
        throw std::runtime_error("failed writing timing trace: " +
                                 tracePath.string());
}

Value statsValue(const hscam::CaptureStats &stats)
{
    Value::Object object{
        {"requests_completed", stats.requestsCompleted},
        {"requests_cancelled", stats.requestsCancelled},
        {"sequence_gaps", stats.sequenceGaps},
        {"consumer_starvations", stats.consumerStarvations},
        {"queue_overruns", stats.completedQueueOverruns},
        {"measured_fps", stats.measuredFps ? Value(*stats.measuredFps) : Value(nullptr)},
        {"min_interval_ns", stats.minInterval ? Value(static_cast<std::int64_t>(stats.minInterval->count())) : Value(nullptr)},
        {"max_interval_ns", stats.maxInterval ? Value(static_cast<std::int64_t>(stats.maxInterval->count())) : Value(nullptr)}
    };
    return object;
}

int workerMain(const fs::path &casePath, const fs::path &resultPath)
{
    Value::Object result;
    std::vector<TimingSample> timingSamples;
    try {
        const TestCase test = parseCase(hscam::internal::json::parse(readAll(casePath)));
        result["case_id"] = test.id;

        hscam::Context context;
        auto camera = context.open(test.cameraId);

        hscam::CaptureRequest request;
        request.stream.kind = test.streamKind;
        request.negotiation = test.exact ? hscam::NegotiationPolicy::Exact
                                         : hscam::NegotiationPolicy::AllowAdjustments;
        request.sensor.modeId = test.modeId;
        request.sensor.crop = test.crop;
        if (test.frameDurationUs)
            request.timing.frameDuration = std::chrono::microseconds(*test.frameDurationUs);

        const auto configuration = camera.configure(request);
        result["configuration"] = captureConfigurationValue(configuration);

        auto session = camera.start();
        const auto start = std::chrono::steady_clock::now();
        const auto end = start + std::chrono::milliseconds(test.durationMs);

        std::uint64_t consumed{};
        std::optional<std::int64_t> lastFrameDurationUs;
        std::optional<std::int64_t> firstSensorTimestampNs;
        std::optional<std::int64_t> lastSensorTimestampNs;

        while (std::chrono::steady_clock::now() < end || consumed == 0) {
            auto frame = session.nextFrame(std::chrono::milliseconds(2500));
            const auto &metadata = frame.metadata();
            ++consumed;
            TimingSample timing;
            timing.frame = consumed - 1;
            timing.sequence = metadata.sequence;
            if (metadata.sensorTimestamp)
                timing.sensorTimestampNs = metadata.sensorTimestamp->count();
            if (metadata.frameDuration)
                timing.frameDurationUs = metadata.frameDuration->count();
            if (metadata.exposure)
                timing.exposureUs = metadata.exposure->count();
            timing.status = metadata.status;
            timingSamples.push_back(timing);

            if (metadata.frameDuration) lastFrameDurationUs = metadata.frameDuration->count();
            if (metadata.sensorTimestamp) {
                if (!firstSensorTimestampNs) firstSensorTimestampNs = metadata.sensorTimestamp->count();
                lastSensorTimestampNs = metadata.sensorTimestamp->count();
            }
        }

        session.stop();
        writeTimingTrace(resultPath, timingSamples);
        result["timing_trace"] = timingTracePath(resultPath).filename().string();
        const auto stats = session.stats();
        result["stats"] = statsValue(stats);
        result["frames_consumed"] = consumed;
        result["last_frame_duration_us"] = lastFrameDurationUs ? Value(*lastFrameDurationUs) : Value(nullptr);
        result["first_sensor_timestamp_ns"] = firstSensorTimestampNs ? Value(*firstSensorTimestampNs) : Value(nullptr);
        result["last_sensor_timestamp_ns"] = lastSensorTimestampNs ? Value(*lastSensorTimestampNs) : Value(nullptr);

        const bool clean = stats.requestsCancelled == 0 &&
                           stats.sequenceGaps == 0 &&
                           stats.completedQueueOverruns == 0 &&
                           consumed > 0;
        result["status"] = clean || !test.requireZeroDrops ? "pass" : "unstable";
        result["error"] = nullptr;
    } catch (const std::exception &e) {
        if (!timingSamples.empty()) {
            try {
                writeTimingTrace(resultPath, timingSamples);
                result["timing_trace"] =
                    timingTracePath(resultPath).filename().string();
            } catch (...) {
            }
        }
        result["status"] = "error";
        result["error"] = e.what();
    }

    writeText(resultPath, hscam::internal::json::stringify(Value(std::move(result)), 2) + "\n");
    return 0;
}

Value characterizeModeSensorCrops(hscam::Context &context,
                                   const hscam::CameraInfo &camera)
{
    Value::Array modes;
    for (const auto &mode : camera.sensorModes) {
        Value::Object entry{
            {"mode_id", mode.id},
            {"width", static_cast<std::uint64_t>(mode.size.width)},
            {"height", static_cast<std::uint64_t>(mode.size.height)},
            {"pixel_format", mode.format.name}
        };

        try {
            auto handle = context.open(camera.id);
            hscam::CaptureRequest request;
            request.stream.kind = hscam::StreamKind::Raw;
            request.sensor.modeId = mode.id;
            request.negotiation = hscam::NegotiationPolicy::AllowAdjustments;
            const auto configuration = handle.configure(request);
            entry["configuration"] = captureConfigurationValue(configuration);
            entry["sensor_crop"] = rectValue(handle.currentSensorCrop());
            entry["error"] = nullptr;
        } catch (const std::exception &e) {
            entry["configuration"] = nullptr;
            entry["sensor_crop"] = nullptr;
            entry["error"] = e.what();
        }

        modes.emplace_back(std::move(entry));
    }

    return Value::Object{
        {"schema_version", static_cast<std::uint64_t>(1)},
        {"camera_id", camera.id},
        {"modes", std::move(modes)}
    };
}

std::vector<TestCase> buildCases(const Policy &policy, const hscam::CameraInfo &camera,
                                 const hscam::qualification::CropGeometry *geometry)
{
    std::vector<TestCase> cases;
    std::set<std::string> cropKeys;

    auto addCrop = [&](hscam::Rect crop, std::uint64_t durationMs, bool exact,
                       std::string_view prefix) {
        TestCase test;
        test.cameraId = camera.id;
        test.crop = crop;
        test.streamKind = hscam::StreamKind::Processed;
        test.durationMs = durationMs;
        test.exact = exact;
        test.requireZeroDrops = policy.requireZeroDrops;
        const auto key = testCaseKey(test);
        if (!cropKeys.insert(key).second)
            return false;
        test.id = std::string(prefix) + "-" + key;
        cases.push_back(std::move(test));
        return true;
    };

    if (policy.advertisedEnabled) {
        for (const auto &mode : camera.sensorModes) {
            TestCase test;
            test.cameraId = camera.id;
            test.modeId = mode.id;
            test.streamKind = hscam::StreamKind::Raw;
            test.durationMs = policy.advertisedDurationMs;
            test.exact = false;
            test.requireZeroDrops = policy.requireZeroDrops;
            test.id = "mode-" + testCaseKey(test);
            cases.push_back(std::move(test));
        }

        // Some libcamera cameras may expose processed capture without an
        // application-visible raw mode. They still need a baseline campaign
        // case instead of silently producing an empty qualification plan.
        if (camera.sensorModes.empty() && camera.capabilities.processedCapture) {
            TestCase test;
            test.cameraId = camera.id;
            test.streamKind = hscam::StreamKind::Processed;
            test.durationMs = policy.advertisedDurationMs;
            test.exact = false;
            test.requireZeroDrops = policy.requireZeroDrops;
            test.id = "processed-default-" + testCaseKey(test);
            cases.push_back(std::move(test));
        }
    }

    if (policy.cropsEnabled) {
        for (const auto &crop : policy.crops)
            (void)addCrop(crop, policy.cropDurationMs, policy.cropsExact, "crop");
    }

    if (policy.cropPerformanceEnabled && geometry && geometry->supported &&
        !geometry->widths.empty() && !geometry->heights.empty()) {
        std::uint64_t generated{};
        const auto canAdd = [&] {
            return !policy.cropPerformanceMaxCases ||
                   generated < policy.cropPerformanceMaxCases;
        };

        auto alignedCoordinate = [](std::int32_t begin,
                                    std::uint32_t span,
                                    std::uint32_t size,
                                    std::uint32_t step,
                                    bool endPosition) {
            const auto available = static_cast<std::int32_t>(span - size);
            auto delta = endPosition ? available : available / 2;
            if (step)
                delta = (delta / static_cast<std::int32_t>(step)) *
                        static_cast<std::int32_t>(step);
            return begin + delta;
        };

        auto addSizeCases = [&](std::uint32_t width, std::uint32_t height,
                                std::string_view prefix) {
            if (!canAdd()) return;

            const auto x0 = geometry->bounds.x;
            const auto y0 = geometry->bounds.y;
            const auto x1 = alignedCoordinate(
                x0, geometry->bounds.width, width, geometry->xStep, true);
            const auto y1 = alignedCoordinate(
                y0, geometry->bounds.height, height, geometry->yStep, true);
            const auto xc = alignedCoordinate(
                x0, geometry->bounds.width, width, geometry->xStep, false);
            const auto yc = alignedCoordinate(
                y0, geometry->bounds.height, height, geometry->yStep, false);

            const std::vector<hscam::Rect> positions =
                policy.cropPerformanceSamplePositions
                    ? std::vector<hscam::Rect>{
                          {x0, y0, width, height},
                          {x1, y0, width, height},
                          {x0, y1, width, height},
                          {x1, y1, width, height},
                          {xc, yc, width, height}}
                    : std::vector<hscam::Rect>{
                          {xc, yc, width, height}};

            for (const auto &rect : positions) {
                if (!canAdd()) break;
                if (addCrop(rect, policy.cropPerformanceDurationMs,
                            policy.cropPerformanceExact, prefix))
                    ++generated;
            }
        };

        if (policy.cropPerformanceStrategy == "all_heights_full_width") {
            const auto width = geometry->widths.back();
            for (const auto height : geometry->heights) {
                if (!canAdd()) break;
                addSizeCases(width, height, "crop-height");
            }
        } else if (policy.cropPerformanceStrategy == "all_heights_width_samples") {
            const auto &widths = geometry->widths;
            std::set<std::uint32_t> samples{
                widths.front(),
                widths[widths.size() / 4],
                widths[widths.size() / 2],
                widths[(widths.size() * 3) / 4],
                widths.back()
            };
            for (const auto height : geometry->heights) {
                for (const auto width : samples) {
                    if (!canAdd()) break;
                    addSizeCases(width, height, "crop-grid");
                }
                if (!canAdd()) break;
            }
        } else if (policy.cropPerformanceStrategy == "all_centered_sizes") {
            for (const auto width : geometry->widths) {
                for (const auto height : geometry->heights) {
                    if (!canAdd()) break;
                    addSizeCases(width, height, "crop-size");
                }
                if (!canAdd()) break;
            }
        } else {
            throw std::runtime_error("unknown crop_performance strategy: " +
                                     policy.cropPerformanceStrategy);
        }
    }

    return cases;
}

int runIsolated(const fs::path &executable, const fs::path &casePath,
                const fs::path &resultPath, std::uint64_t timeoutMs)
{
    const pid_t pid = ::fork();
    if (pid < 0) throw std::runtime_error("fork failed");

    if (pid == 0) {
        ::execl(executable.c_str(), executable.c_str(), "--worker",
                casePath.c_str(), resultPath.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        int status{};
        const pid_t wait = ::waitpid(pid, &status, WNOHANG);
        if (wait == pid) {
            if (!WIFEXITED(status)) return 128;
            return WEXITSTATUS(status);
        }
        if (wait < 0) throw std::runtime_error("waitpid failed");

        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(pid, SIGKILL);
            int status{};
            (void)::waitpid(pid, &status, 0);
            return 124;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

bool verifyRecovery(const fs::path &executable, const fs::path &output,
                    const hscam::CameraInfo &camera,
                    std::uint64_t workerTimeoutMs,
                    std::string &error);

Value timeoutResult(const TestCase &test);

bool probeStable(const Value &result, std::int64_t requestedUs,
                 double achievedRatio, std::uint64_t toleranceUs,
                 double maxIntervalRatio)
{
    if (result.at("status").asString() != "pass")
        return false;
    const auto *stats = result.find("stats");
    if (!stats || stats->at("measured_fps").isNull())
        return false;

    const double requestedFps = 1000000.0 / static_cast<double>(requestedUs);
    if (stats->at("measured_fps").asNumber() < requestedFps * achievedRatio)
        return false;

    if (const auto *maxInterval = stats->find("max_interval_ns")) {
        if (!maxInterval->isNull()) {
            const double allowedNs =
                static_cast<double>(requestedUs) * 1000.0 * maxIntervalRatio;
            if (static_cast<double>(maxInterval->asInt64()) > allowedNs)
                return false;
        }
    }

    if (const auto *duration = result.find("last_frame_duration_us")) {
        if (!duration->isNull()) {
            const auto delivered = duration->asInt64();
            const auto allowed = requestedUs + static_cast<std::int64_t>(
                std::max<std::uint64_t>(toleranceUs,
                    static_cast<std::uint64_t>(requestedUs * (1.0 - achievedRatio))));
            if (delivered > allowed)
                return false;
        }
    }
    return true;
}

Value runProbe(const fs::path &executable, const fs::path &output,
               const TestCase &base, std::int64_t frameDurationUs,
               std::uint64_t durationMs, std::uint64_t workerTimeoutMs,
               bool rerun)
{
    TestCase probe = base;
    probe.frameDurationUs = frameDurationUs;
    probe.durationMs = durationMs;
    probe.id = base.id + "-fd-" + std::to_string(frameDurationUs);

    const auto directory = output / "search" / base.id;
    fs::create_directories(directory);
    const auto resultPath = directory / (std::to_string(frameDurationUs) + "-" +
                                          std::to_string(durationMs) + ".json");
    if (fs::exists(resultPath) && !rerun)
        return hscam::internal::json::parse(readAll(resultPath));

    const auto casePath = output / "work" / (probe.id + ".json");
    writeText(casePath, hscam::internal::json::stringify(caseValue(probe), 2) + "\n");

    const auto timeout = std::max<std::uint64_t>(workerTimeoutMs, durationMs + 10000);
    const int rc = runIsolated(executable, casePath, resultPath, timeout);
    if (rc == 124) {
        writeText(resultPath,
                  hscam::internal::json::stringify(timeoutResult(probe), 2) + "\n");
    } else if (rc != 0 || !fs::exists(resultPath)) {
        const Value failure = Value::Object{
            {"case_id", probe.id},
            {"status", "worker_error"},
            {"error", "qualification worker exited with code " + std::to_string(rc)}
        };
        writeText(resultPath, hscam::internal::json::stringify(failure, 2) + "\n");
    }
    return hscam::internal::json::parse(readAll(resultPath));
}

Value runFpsSearch(const fs::path &executable, const fs::path &output,
                   const TestCase &base, const Policy &policy, bool rerun)
{
    struct ProbeSummary {
        std::int64_t durationUs{};
        bool stable{};
        double measuredFps{};
        std::string status;
    };

    std::vector<ProbeSummary> probes;
    auto probe = [&](std::int64_t durationUs, std::uint64_t durationMs) {
        const Value result = runProbe(executable, output, base, durationUs, durationMs,
                                      policy.workerTimeoutMs, rerun);
        const bool stable = probeStable(result, durationUs, policy.fpsAchievedRatio,
                                        policy.fpsToleranceUs,
                                        policy.fpsMaxIntervalRatio);
        double fps{};
        if (const auto *stats = result.find("stats")) {
            if (!stats->at("measured_fps").isNull())
                fps = stats->at("measured_fps").asNumber();
        }
        probes.push_back({durationUs, stable, fps, result.at("status").asString()});
        return std::pair<Value, bool>{result, stable};
    };

    auto [slowResult, slowStable] =
        probe(policy.fpsMaxFrameDurationUs, policy.fpsShortDurationMs);

    Value::Object summary;
    summary["case_id"] = base.id;

    if (!slowStable) {
        summary["status"] = "unstable";
        summary["error"] = "no stable capture at fps_search max_frame_duration_us";
        summary["best_frame_duration_us"] = nullptr;
        summary["best_measured_fps"] = nullptr;
    } else {
        std::int64_t stableUs = policy.fpsMaxFrameDurationUs;
        std::int64_t unstableUs = policy.fpsMinFrameDurationUs - 1;

        auto [fastResult, fastStable] =
            probe(policy.fpsMinFrameDurationUs, policy.fpsShortDurationMs);
        if (fastStable) {
            stableUs = policy.fpsMinFrameDurationUs;
        } else {
            unstableUs = policy.fpsMinFrameDurationUs;
            for (std::uint64_t iteration = 0;
                 iteration < policy.fpsMaxIterations &&
                 stableUs - unstableUs > static_cast<std::int64_t>(policy.fpsToleranceUs);
                 ++iteration) {
                const auto mid = unstableUs + (stableUs - unstableUs) / 2;
                auto [midResult, midStable] = probe(mid, policy.fpsShortDurationMs);
                (void)midResult;
                if (midStable) stableUs = mid;
                else unstableUs = mid;
            }
        }

        auto [finalResult, finalStable] =
            probe(stableUs, policy.fpsFinalDurationMs);

        summary["status"] = finalStable ? "pass" : "unstable";
        summary["error"] = finalStable ? Value(nullptr)
                                        : Value("final validation failed at discovered boundary");
        summary["best_frame_duration_us"] = stableUs;
        const auto *finalStats = finalResult.find("stats");
        if (finalStats && !finalStats->at("measured_fps").isNull()) {
            summary["best_measured_fps"] = finalStats->at("measured_fps").asNumber();
            summary["stats"] = *finalStats;
        } else {
            summary["best_measured_fps"] = nullptr;
        }
        summary["final_result"] = finalResult;
    }

    Value::Array probeValues;
    for (const auto &p : probes) {
        probeValues.emplace_back(Value::Object{
            {"frame_duration_us", p.durationUs},
            {"stable", p.stable},
            {"measured_fps", p.measuredFps ? Value(p.measuredFps) : Value(nullptr)},
            {"worker_status", p.status}
        });
    }
    summary["probes"] = std::move(probeValues);
    return Value(std::move(summary));
}

Value timeoutResult(const TestCase &test)
{
    return Value::Object{
        {"case_id", test.id},
        {"status", "timeout"},
        {"error", "qualification worker exceeded hard timeout"}
    };
}

bool verifyRecovery(const fs::path &executable, const fs::path &output,
                    const hscam::CameraInfo &camera,
                    std::uint64_t workerTimeoutMs,
                    std::string &error)
{
    TestCase health;
    health.id = "recovery-health";
    health.cameraId = camera.id;
    health.durationMs = 750;
    health.exact = false;
    health.requireZeroDrops = false;

    if (!camera.sensorModes.empty()) {
        health.streamKind = hscam::StreamKind::Raw;
        health.modeId = camera.sensorModes.front().id;
    } else {
        health.streamKind = hscam::StreamKind::Processed;
    }

    const auto casePath = output / "work" / "recovery-health.json";
    const auto resultPath = output / "work" / "recovery-health-result.json";
    writeText(casePath,
              hscam::internal::json::stringify(caseValue(health), 2) + "\n");

    std::error_code ec;
    fs::remove(resultPath, ec);

    const int rc = runIsolated(executable, casePath, resultPath,
                               std::max<std::uint64_t>(workerTimeoutMs, 10000));
    if (rc != 0 || !fs::exists(resultPath)) {
        error = "recovery worker failed with exit code " + std::to_string(rc);
        return false;
    }

    try {
        const auto result = hscam::internal::json::parse(readAll(resultPath));
        if (result.at("status").asString() != "pass") {
            if (const auto *failure = result.find("error")) {
                if (!failure->isNull()) error = failure->asString();
            }
            if (error.empty()) error = "baseline health capture did not pass";
            return false;
        }
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }

    return true;
}

std::string csvEscape(std::string_view value)
{
    bool quote = false;
    for (const char ch : value)
        quote = quote || ch == ',' || ch == '"' || ch == '\n' || ch == '\r';
    if (!quote) return std::string(value);

    std::string out = "\"";
    for (const char ch : value) {
        if (ch == '"') out += "\"\"";
        else out += ch;
    }
    out += '"';
    return out;
}

std::string cropLabel(const TestCase &test)
{
    if (!test.crop) return "-";
    std::ostringstream out;
    out << test.crop->x << ',' << test.crop->y << ','
        << test.crop->width << ',' << test.crop->height;
    return out.str();
}

std::string caseType(const TestCase &test)
{
    if (test.crop) return test.streamKind == hscam::StreamKind::Processed
                              ? "sensor_crop_processed"
                              : "sensor_crop_raw";
    if (test.modeId) return "advertised_mode_raw";
    return "capture";
}

const Value *resultStats(const Value &result)
{
    if (const auto *stats = result.find("stats"))
        return stats;
    if (const auto *final = result.find("final_result")) {
        if (const auto *stats = final->find("stats"))
            return stats;
    }
    return nullptr;
}

std::optional<std::int64_t> resultBestDurationUs(const Value &result)
{
    if (const auto *value = result.find("best_frame_duration_us")) {
        if (!value->isNull()) return value->asInt64();
    }
    if (const auto *value = result.find("last_frame_duration_us")) {
        if (!value->isNull()) return value->asInt64();
    }
    if (const auto *final = result.find("final_result")) {
        if (const auto *value = final->find("last_frame_duration_us")) {
            if (!value->isNull()) return value->asInt64();
        }
    }
    return std::nullopt;
}

std::optional<double> resultBestFps(const Value &result)
{
    if (const auto *value = result.find("best_measured_fps")) {
        if (!value->isNull()) return value->asNumber();
    }
    if (const auto *stats = resultStats(result)) {
        const auto &value = stats->at("measured_fps");
        if (!value.isNull()) return value.asNumber();
    }
    return std::nullopt;
}

std::string resultError(const Value &result)
{
    if (const auto *error = result.find("error")) {
        if (!error->isNull()) return error->asString();
    }
    return {};
}

void generateAggregate(const fs::path &output, std::string_view planId,
                       const std::vector<TestCase> &cases)
{
    Value::Array results;
    std::ostringstream report;
    std::ostringstream csv;

    report << "# Qualification results\n\n";
    report << "Plan: `" << planId << "`\n\n";
    report << "| Case | Type | Mode/crop | Status | Max stable FPS | Frame duration (us) | Frames | Gaps | Overruns |\n";
    report << "| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: |\n";

    csv << "case_id,type,mode_id,crop_x,crop_y,crop_width,crop_height,status,"
           "best_frame_duration_us,best_measured_fps,requests_completed,sequence_gaps,"
           "queue_overruns,error\n";

    std::uint64_t passCount{};
    std::uint64_t unstableCount{};
    std::uint64_t errorCount{};

    for (const auto &test : cases) {
        const auto path = output / "cases" / (test.id + ".json");
        const Value result = hscam::internal::json::parse(readAll(path));
        results.push_back(result);

        const std::string status = result.at("status").asString();
        if (status == "pass") ++passCount;
        else if (status == "unstable") ++unstableCount;
        else ++errorCount;

        const auto *stats = resultStats(result);
        const auto bestDuration = resultBestDurationUs(result);
        const auto bestFps = resultBestFps(result);

        std::string frames = "-";
        std::string gaps = "-";
        std::string overruns = "-";
        if (stats) {
            frames = std::to_string(stats->at("requests_completed").asUInt64());
            gaps = std::to_string(stats->at("sequence_gaps").asUInt64());
            overruns = std::to_string(stats->at("queue_overruns").asUInt64());
        }

        std::string fps = "-";
        if (bestFps) {
            std::ostringstream value;
            value << std::fixed << std::setprecision(3) << *bestFps;
            fps = value.str();
        }

        const std::string geometry = test.crop ? cropLabel(test)
                                              : (test.modeId ? *test.modeId : "-");

        report << "| " << test.id
               << " | " << caseType(test)
               << " | " << geometry
               << " | " << status
               << " | " << fps
               << " | " << (bestDuration ? std::to_string(*bestDuration) : "-")
               << " | " << frames
               << " | " << gaps
               << " | " << overruns
               << " |\n";

        csv << csvEscape(test.id) << ','
            << caseType(test) << ','
            << csvEscape(test.modeId.value_or("")) << ',';
        if (test.crop) {
            csv << test.crop->x << ',' << test.crop->y << ','
                << test.crop->width << ',' << test.crop->height;
        } else {
            csv << ",,,";
        }
        csv << ',' << csvEscape(status) << ',';
        if (bestDuration) csv << *bestDuration;
        csv << ',';
        if (bestFps) csv << std::setprecision(12) << *bestFps;
        csv << ',';
        if (stats) csv << stats->at("requests_completed").asUInt64();
        csv << ',';
        if (stats) csv << stats->at("sequence_gaps").asUInt64();
        csv << ',';
        if (stats) csv << stats->at("queue_overruns").asUInt64();
        csv << ',' << csvEscape(resultError(result)) << '\n';
    }

    report << "\n## Summary\n\n";
    report << "- Passed: " << passCount << "\n";
    report << "- Unstable: " << unstableCount << "\n";
    report << "- Error/timeout/unsupported: " << errorCount << "\n";

    const Value aggregate = Value::Object{
        {"schema_version", static_cast<std::uint64_t>(1)},
        {"plan_id", std::string(planId)},
        {"summary", Value::Object{
            {"pass", passCount},
            {"unstable", unstableCount},
            {"error_or_other", errorCount}
        }},
        {"results", std::move(results)}
    };
    writeText(output / "results.json", hscam::internal::json::stringify(aggregate, 2) + "\n");
    writeText(output / "results.csv", csv.str());
    writeText(output / "report.md", report.str());
}

std::optional<std::size_t> findFastestCase(const fs::path &output,
                                           const std::vector<TestCase> &cases)
{
    std::optional<std::size_t> best;
    double bestFps{};
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto resultPath = output / "cases" / (cases[i].id + ".json");
        if (!fs::exists(resultPath)) continue;
        const auto result = hscam::internal::json::parse(readAll(resultPath));
        if (result.at("status").asString() != "pass") continue;
        const auto fps = resultBestFps(result);
        if (fps && (!best || *fps > bestFps)) {
            best = i;
            bestFps = *fps;
        }
    }
    return best;
}

std::optional<std::size_t> findBaselineCase(const fs::path &output,
                                            const std::vector<TestCase> &cases,
                                            const hscam::CameraInfo &camera)
{
    std::optional<std::size_t> best;
    std::uint64_t bestArea{};
    for (std::size_t i = 0; i < cases.size(); ++i) {
        if (!cases[i].modeId || cases[i].crop) continue;
        const auto resultPath = output / "cases" / (cases[i].id + ".json");
        if (!fs::exists(resultPath)) continue;
        const auto result = hscam::internal::json::parse(readAll(resultPath));
        if (result.at("status").asString() != "pass") continue;

        const auto mode = std::find_if(camera.sensorModes.begin(), camera.sensorModes.end(),
            [&](const auto &candidate) { return candidate.id == *cases[i].modeId; });
        if (mode == camera.sensorModes.end()) continue;
        const auto area = mode->size.area();
        if (!best || area > bestArea) {
            best = i;
            bestArea = area;
        }
    }
    return best;
}

void generateVisualSamples(hscam::Context &context,
                           const fs::path &output,
                           const hscam::CameraInfo &camera,
                           const std::vector<TestCase> &cases,
                           const Policy &policy)
{
    if (!policy.visualSamplesEnabled || cases.empty())
        return;

    const auto executable = fs::canonical("/proc/self/exe");
    const auto exporter = executable.parent_path() / "hscam-export";
    const auto samplesDirectory = output / "samples";
    fs::create_directories(samplesDirectory);

    Value::Array sampleResults;

    auto capture = [&](std::string label, std::size_t index, std::uint64_t frameCount) {
        const auto &test = cases[index];
        const auto resultPath = output / "cases" / (test.id + ".json");
        const auto result = hscam::internal::json::parse(readAll(resultPath));

        hscam::qualification::VisualSampleSpec spec;
        spec.label = std::move(label);
        spec.cameraId = camera.id;
        spec.cameraModel = camera.model;
        spec.modeId = test.modeId;
        spec.crop = test.crop;
        spec.frameDurationUs = resultBestDurationUs(result);
        spec.streamKind = test.streamKind;
        spec.frameCount = std::max<std::uint64_t>(1, frameCount);
        spec.exact = test.exact;
        spec.playbackFps = policy.visualPlaybackFps;

        try {
            const auto sample = hscam::qualification::captureVisualSample(
                context, spec, samplesDirectory, exporter);
            sampleResults.push_back(
                hscam::qualification::visualSampleValue(sample, output));
        } catch (const std::exception &e) {
            sampleResults.emplace_back(Value::Object{
                {"label", spec.label},
                {"error", e.what()}
            });
        }
    };

    if (const auto baseline = findBaselineCase(output, cases, camera))
        capture("baseline", *baseline, 1);

    if (const auto fastest = findFastestCase(output, cases))
        capture("fastest", *fastest, policy.visualSampleFrames);

    const Value samples = Value::Object{
        {"schema_version", static_cast<std::uint64_t>(1)},
        {"samples", std::move(sampleResults)}
    };
    writeText(output / "samples.json",
              hscam::internal::json::stringify(samples, 2) + "\n");
}

bool validatePassTimingEvidence(const fs::path &output,
                                const std::vector<TestCase> &cases,
                                std::string &reason)
{
    for (const auto &test : cases) {
        const auto resultPath = output / "cases" / (test.id + ".json");
        if (!fs::exists(resultPath)) {
            reason = "missing result for " + test.id;
            return false;
        }

        const auto result =
            hscam::internal::json::parse(readAll(resultPath));
        if (result.at("status").asString() != "pass")
            continue;

        fs::path trace;
        if (const auto *name = result.find("timing_trace");
            name && !name->isNull()) {
            trace = resultPath.parent_path() / name->asString();
        } else if (const auto *finalResult = result.find("final_result");
                   finalResult && finalResult->isObject()) {
            if (const auto *name = finalResult->find("timing_trace");
                name && !name->isNull())
                trace = output / "search" / test.id / name->asString();
        }

        if (trace.empty() || !fs::exists(trace) ||
            fs::file_size(trace) == 0) {
            reason = "missing per-frame timing evidence for pass case " +
                     test.id;
            return false;
        }
    }
    return true;
}

bool validateVisualEvidence(const fs::path &output, std::string &reason)
{
    const auto path = output / "samples.json";
    if (!fs::exists(path)) {
        reason = "visual sampling enabled but samples.json is missing";
        return false;
    }

    const auto document =
        hscam::internal::json::parse(readAll(path));
    const auto &samples = document.at("samples").asArray();
    if (samples.empty()) {
        reason = "visual sampling enabled but no samples were generated";
        return false;
    }

    bool hasVideo = false;
    for (const auto &sample : samples) {
        if (const auto *error = sample.find("error");
            error && !error->isNull()) {
            reason = "visual sample error: " + error->asString();
            return false;
        }

        const auto *pngs = sample.find("pngs");
        if (!pngs || !pngs->isArray() || pngs->asArray().empty()) {
            reason = "visual sample has no PNG evidence";
            return false;
        }

        for (const auto &png : pngs->asArray()) {
            if (!fs::exists(output / png.asString())) {
                reason = "visual PNG evidence file is missing: " +
                         png.asString();
                return false;
            }
        }

        if (const auto *video = sample.find("video");
            video && !video->isNull()) {
            if (!fs::exists(output / video->asString())) {
                reason = "visual video evidence file is missing: " +
                         video->asString();
                return false;
            }
            hasVideo = true;
        }
    }

    if (!hasVideo) {
        reason = "visual sampling enabled but no video evidence was generated";
        return false;
    }
    return true;
}

void usage()
{
    std::cerr
        << "usage:\n"
        << "  hscam-qualify run MANIFEST.json --output DIR [--camera ID] [--rerun]\n"
        << "  hscam-qualify promote RUN_DIR --results-root DIR\n"
        << "  hscam-qualify audit-results --results-root DIR\n"
        << "  hscam-qualify --worker CASE.json RESULT.json\n";
}

void resetQualificationOutput(const fs::path &output)
{
    std::error_code ec;
    for (const auto &directory : {"cases", "search", "work", "samples"}) {
        fs::remove_all(output / directory, ec);
        if (ec)
            throw std::runtime_error(
                "failed to clear qualification directory " +
                (output / directory).string() + ": " + ec.message());
    }

    for (const auto &file : {
             "campaign.json", "plan.json", "environment.json",
             "environment_end.json", "camera.json", "mode_sensor_crops.json",
             "crop_geometry.json", "crop_geometry_probes.jsonl",
             "campaign_status.json", "results.json", "results.csv",
             "report.md", "samples.json", "recovery_failure.json"}) {
        ec.clear();
        fs::remove(output / file, ec);
        if (ec)
            throw std::runtime_error(
                "failed to clear qualification artifact " +
                (output / file).string() + ": " + ec.message());
    }
}

void validateResumePlan(const fs::path &output, std::string_view planId,
                        bool rerun)
{
    if (rerun) {
        resetQualificationOutput(output);
        return;
    }

    const auto planPath = output / "plan.json";
    if (fs::exists(planPath)) {
        const auto existing =
            hscam::internal::json::parse(readAll(planPath));
        if (existing.at("plan_id").asString() != planId)
            throw std::runtime_error(
                "qualification output belongs to a different plan; "
                "use --rerun or a new output directory");
        return;
    }

    for (const auto &directory : {"cases", "search", "work", "samples"}) {
        if (fs::exists(output / directory))
            throw std::runtime_error(
                "qualification output contains generated state without "
                "plan.json; use --rerun or a new output directory");
    }
}

int runMain(const fs::path &manifestPath, const fs::path &output,
            const std::string &cameraIdArg, bool rerun)
{
    const std::string manifestText = readAll(manifestPath);
    const Value manifestJson = hscam::internal::json::parse(manifestText);
    const Policy policy = parsePolicy(manifestJson);

    hscam::Context context;
    const auto cameras = context.cameras();
    if (cameras.empty())
        throw std::runtime_error("no libcamera cameras detected");

    std::string cameraId = cameraIdArg;
    if (cameraId.empty()) {
        if (cameras.size() != 1)
            throw std::runtime_error("--camera is required unless exactly one camera is attached");
        cameraId = cameras.front().id;
    }

    const auto it = std::find_if(cameras.begin(), cameras.end(),
                                 [&](const auto &camera) { return camera.id == cameraId; });
    if (it == cameras.end())
        throw std::runtime_error("selected camera is not present: " + cameraId);
    const auto &camera = *it;

    const std::string planId =
        hscam::qualification::qualificationPlanId(
            manifestText, camera.id, HSCAM_VERSION_STRING,
            HSCAM_SOURCE_REVISION);

    fs::create_directories(output);
    validateResumePlan(output, planId, rerun);
    fs::create_directories(output / "cases");
    fs::create_directories(output / "work");

    if (policy.requireNoThrottling)
        requireCleanThrottling("campaign start");

    writeText(output / "campaign.json", manifestText);
    writeText(output / "environment.json",
              hscam::internal::json::stringify(environmentValue(), 2) + "\n");
    writeText(output / "camera.json",
              hscam::internal::json::stringify(cameraValue(camera), 2) + "\n");

    writeText(output / "mode_sensor_crops.json",
              hscam::internal::json::stringify(
                  characterizeModeSensorCrops(context, camera), 2) + "\n");

    std::optional<hscam::qualification::CropGeometry> geometry;
    if (policy.geometry.enabled) {
        {
            std::ofstream probeLog(
                output / "crop_geometry_probes.jsonl",
                std::ios::binary | std::ios::trunc);
            if (!probeLog)
                throw std::runtime_error(
                    "failed to create crop_geometry_probes.jsonl");

            auto cropCamera = context.open(camera.id);
            geometry = hscam::qualification::discoverCropGeometry(
                cropCamera, camera, policy.geometry,
                [&](const hscam::qualification::CropProbeRecord &probe) {
                    probeLog << hscam::internal::json::stringify(
                                    cropProbeValue(probe))
                             << '\n';
                    if (!probeLog)
                        throw std::runtime_error(
                            "failed writing crop geometry probe log");
                });
        }
        writeText(output / "crop_geometry.json",
                  hscam::internal::json::stringify(
                      hscam::qualification::geometryValue(*geometry), 2) + "\n");
        if (geometry->supported) {
            std::cout << "crop geometry: " << geometry->widths.size() << " widths x "
                      << geometry->heights.size() << " heights";
            if (policy.geometry.verifyCartesian)
                std::cout << ", Cartesian checks=" << geometry->cartesianChecks
                          << ", exceptions=" << geometry->cartesianExceptions;
            std::cout << "\n";
        } else {
            std::cout << "crop geometry: sensor crop TRY not supported\n";
        }
    }

    auto cases = buildCases(policy, camera, geometry ? &*geometry : nullptr);
    Value::Array casePlan;
    for (const auto &test : cases) casePlan.push_back(caseValue(test));
    const Value plan = Value::Object{
        {"schema_version", static_cast<std::uint64_t>(1)},
        {"plan_id", planId},
        {"campaign", policy.campaign},
        {"camera_id", camera.id},
        {"hscam_version", HSCAM_VERSION_STRING},
        {"source_revision", HSCAM_SOURCE_REVISION},
        {"cases", std::move(casePlan)}
    };
    writeText(output / "plan.json", hscam::internal::json::stringify(plan, 2) + "\n");

    const fs::path executable = fs::canonical("/proc/self/exe");
    std::size_t completed{};
    for (const auto &test : cases) {
        const auto resultPath = output / "cases" / (test.id + ".json");
        if (fs::exists(resultPath) && !rerun) {
            ++completed;
            std::cout << "[" << completed << "/" << cases.size() << "] "
                      << test.id << " resume\n";
            continue;
        }

        std::cout << "[" << (completed + 1) << "/" << cases.size() << "] "
                  << test.id << std::flush;

        if (policy.fpsSearchEnabled) {
            const auto result = runFpsSearch(executable, output, test, policy, rerun);
            writeText(resultPath,
                      hscam::internal::json::stringify(result, 2) + "\n");
            std::cout << " " << result.at("status").asString() << "\n";
        } else {
            const auto casePath = output / "work" / (test.id + ".json");
            writeText(casePath,
                      hscam::internal::json::stringify(caseValue(test), 2) + "\n");

            const int rc = runIsolated(executable, casePath, resultPath,
                                       policy.workerTimeoutMs);
            if (rc == 124) {
                writeText(resultPath,
                          hscam::internal::json::stringify(timeoutResult(test), 2) + "\n");
                std::cout << " timeout\n";
            } else if (rc != 0 || !fs::exists(resultPath)) {
                const Value failure = Value::Object{
                    {"case_id", test.id},
                    {"status", "worker_error"},
                    {"error", "qualification worker exited with code " +
                              std::to_string(rc)}
                };
                writeText(resultPath,
                          hscam::internal::json::stringify(failure, 2) + "\n");
                std::cout << " worker_error\n";
            } else {
                const auto result =
                    hscam::internal::json::parse(readAll(resultPath));
                std::cout << " " << result.at("status").asString() << "\n";
            }
        }

        const auto completedResultPath = output / "cases" / (test.id + ".json");
        if (fs::exists(completedResultPath)) {
            const auto completedResult =
                hscam::internal::json::parse(readAll(completedResultPath));
            if (completedResult.at("status").asString() != "pass") {
                std::string recoveryError;
                std::cout << "  recovery check" << std::flush;
                if (!verifyRecovery(executable, output, camera,
                                    policy.workerTimeoutMs, recoveryError)) {
                    std::cout << " failed\n";
                    const Value recoveryFailure = Value::Object{
                        {"schema_version", static_cast<std::uint64_t>(1)},
                        {"after_case", test.id},
                        {"error", recoveryError}
                    };
                    writeText(output / "recovery_failure.json",
                              hscam::internal::json::stringify(recoveryFailure, 2) + "\n");
                    throw std::runtime_error(
                        "camera failed recovery check after " + test.id +
                        ": " + recoveryError);
                }
                std::cout << " pass\n";
            }
        }

        ++completed;
    }

    generateAggregate(output, planId, cases);
    generateVisualSamples(context, output, camera, cases, policy);

    writeText(output / "environment_end.json",
              hscam::internal::json::stringify(environmentValue(), 2) + "\n");

    bool campaignValid = true;
    std::string invalidReason;

    if (!validatePassTimingEvidence(output, cases, invalidReason))
        campaignValid = false;

    if (campaignValid && policy.visualSamplesEnabled &&
        !validateVisualEvidence(output, invalidReason))
        campaignValid = false;

    if (campaignValid && policy.requireNoThrottling &&
        !throttlingClean()) {
        campaignValid = false;
        invalidReason = "Pi reported throttling by campaign end";
    }

    const Value campaignStatus = Value::Object{
        {"schema_version", static_cast<std::uint64_t>(1)},
        {"valid", campaignValid},
        {"reason", invalidReason.empty() ? Value(nullptr) : Value(invalidReason)}
    };
    writeText(output / "campaign_status.json",
              hscam::internal::json::stringify(campaignStatus, 2) + "\n");

    std::cout << "qualification output: " << output << "\n";
    if (!campaignValid)
        throw std::runtime_error(invalidReason);
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    try {
        if (argc == 4 && std::string_view(argv[1]) == "--worker")
            return workerMain(argv[2], argv[3]);

        if (argc >= 2 && std::string_view(argv[1]) == "audit-results") {
            fs::path resultsRoot;
            for (int i = 2; i < argc; ++i) {
                const std::string arg = argv[i];
                if (arg == "--results-root" && i + 1 < argc)
                    resultsRoot = argv[++i];
                else {
                    usage();
                    return 2;
                }
            }
            if (resultsRoot.empty())
                throw std::runtime_error("--results-root is required");

            const auto audit =
                hscam::qualification::auditOfficialDataset(resultsRoot);

            for (const auto &[sensor, results] : audit.resultsBySensor) {
                const auto blocked = audit.blockedBySensor.at(sensor).size();
                std::cout << sensor << ": ";
                if (!results.empty())
                    std::cout << "published (" << results.size() << ")";
                else if (blocked)
                    std::cout << "blocked (" << blocked << ")";
                else
                    std::cout << "missing";
                std::cout << "\n";
            }

            for (const auto &invalid : audit.invalidReceipts)
                std::cerr << "invalid receipt: " << invalid << "\n";

            if (audit.complete()) {
                std::cout << "official dataset gate: complete\n";
                return 0;
            }

            std::cerr << "official dataset gate: incomplete\n";
            return 3;
        }

        if (argc >= 2 && std::string_view(argv[1]) == "promote") {
            if (argc < 5) {
                usage();
                return 2;
            }

            const fs::path run = argv[2];
            fs::path resultsRoot;
            for (int i = 3; i < argc; ++i) {
                const std::string arg = argv[i];
                if (arg == "--results-root" && i + 1 < argc)
                    resultsRoot = argv[++i];
                else {
                    usage();
                    return 2;
                }
            }
            if (resultsRoot.empty())
                throw std::runtime_error("--results-root is required");

            const auto promoted =
                hscam::qualification::promoteQualificationRun(
                    run, resultsRoot);
            std::cout << "published qualification: "
                      << promoted.destination << "\n"
                      << "published files: "
                      << promoted.copiedFiles.size() << "\n"
                      << "omitted raw bundles: "
                      << promoted.omittedRawBundles.size() << "\n";
            return 0;
        }

        if (argc < 5 || std::string_view(argv[1]) != "run") {
            usage();
            return 2;
        }

        const fs::path manifest = argv[2];
        fs::path output;
        std::string cameraId;
        bool rerun = false;
        for (int i = 3; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--output" && i + 1 < argc) output = argv[++i];
            else if (arg == "--camera" && i + 1 < argc) cameraId = argv[++i];
            else if (arg == "--rerun") rerun = true;
            else { usage(); return 2; }
        }
        if (output.empty()) throw std::runtime_error("--output is required");
        return runMain(manifest, output, cameraId, rerun);
    } catch (const std::exception &e) {
        std::cerr << "hscam-qualify: " << e.what() << '\n';
        return 1;
    }
}
