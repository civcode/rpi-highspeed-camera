#include "hscam/context.hpp"
#include "hscam/error.hpp"
#include "hscam/version.hpp"
#include "internal/json.hpp"
#include "crop_geometry.hpp"

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
#include <optional>
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
};

struct TestCase {
    std::string id;
    std::string cameraId;
    std::optional<std::string> modeId;
    std::optional<hscam::Rect> crop;
    std::optional<std::int64_t> frameDurationUs;
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

std::string fnvHex(std::string_view text)
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char ch : text) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
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
        x > INT32_MAX || y > INT32_MAX || w > UINT32_MAX || h > UINT32_MAX)
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
        if (const auto *strategy = performance->find("strategy"))
            policy.cropPerformanceStrategy = strategy->asString();
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
    try {
        const TestCase test = parseCase(hscam::internal::json::parse(readAll(casePath)));
        result["case_id"] = test.id;

        hscam::Context context;
        auto camera = context.open(test.cameraId);

        hscam::CaptureRequest request;
        request.stream.kind = hscam::StreamKind::Raw;
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
            if (metadata.frameDuration) lastFrameDurationUs = metadata.frameDuration->count();
            if (metadata.sensorTimestamp) {
                if (!firstSensorTimestampNs) firstSensorTimestampNs = metadata.sensorTimestamp->count();
                lastSensorTimestampNs = metadata.sensorTimestamp->count();
            }
        }

        session.stop();
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
        result["status"] = "error";
        result["error"] = e.what();
    }

    writeText(resultPath, hscam::internal::json::stringify(Value(std::move(result)), 2) + "\n");
    return 0;
}

std::vector<TestCase> buildCases(const Policy &policy, const hscam::CameraInfo &camera,
                                 const hscam::qualification::CropGeometry *geometry)
{
    std::vector<TestCase> cases;

    auto addCrop = [&](hscam::Rect crop, std::uint64_t durationMs, bool exact, std::string_view prefix) {
        TestCase test;
        test.cameraId = camera.id;
        test.crop = crop;
        test.durationMs = durationMs;
        test.exact = exact;
        test.requireZeroDrops = policy.requireZeroDrops;
        test.id = std::string(prefix) + "-" + testCaseKey(test);
        cases.push_back(std::move(test));
    };

    if (policy.advertisedEnabled) {
        for (const auto &mode : camera.sensorModes) {
            TestCase test;
            test.cameraId = camera.id;
            test.modeId = mode.id;
            test.durationMs = policy.advertisedDurationMs;
            test.exact = false;
            test.requireZeroDrops = policy.requireZeroDrops;
            test.id = "mode-" + testCaseKey(test);
            cases.push_back(std::move(test));
        }
    }

    if (policy.cropsEnabled) {
        for (const auto &crop : policy.crops)
            addCrop(crop, policy.cropDurationMs, policy.cropsExact, "crop");
    }

    if (policy.cropPerformanceEnabled && geometry && geometry->supported) {
        std::uint64_t generated{};
        const auto canAdd = [&] {
            return !policy.cropPerformanceMaxCases || generated < policy.cropPerformanceMaxCases;
        };

        if (policy.cropPerformanceStrategy == "all_heights_full_width") {
            const auto width = geometry->widths.back();
            for (const auto height : geometry->heights) {
                if (!canAdd()) break;
                const auto x = geometry->bounds.x +
                               static_cast<std::int32_t>((geometry->bounds.width - width) / 2);
                const auto y = geometry->bounds.y +
                               static_cast<std::int32_t>((geometry->bounds.height - height) / 2);
                addCrop({x, y, width, height}, policy.cropPerformanceDurationMs,
                        policy.cropPerformanceExact, "crop-height");
                ++generated;
            }
        } else if (policy.cropPerformanceStrategy == "all_centered_sizes") {
            for (const auto width : geometry->widths) {
                for (const auto height : geometry->heights) {
                    if (!canAdd()) break;
                    const auto x = geometry->bounds.x +
                                   static_cast<std::int32_t>((geometry->bounds.width - width) / 2);
                    const auto y = geometry->bounds.y +
                                   static_cast<std::int32_t>((geometry->bounds.height - height) / 2);
                    addCrop({x, y, width, height}, policy.cropPerformanceDurationMs,
                            policy.cropPerformanceExact, "crop-size");
                    ++generated;
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

Value timeoutResult(const TestCase &test)
{
    return Value::Object{
        {"case_id", test.id},
        {"status", "timeout"},
        {"error", "qualification worker exceeded hard timeout"}
    };
}

void generateAggregate(const fs::path &output, std::string_view planId,
                       const std::vector<TestCase> &cases)
{
    Value::Array results;
    std::ostringstream report;
    report << "# Qualification results\n\n";
    report << "Plan: `" << planId << "`\n\n";
    report << "| Case | Status | Frames | FPS | Gaps | Overruns |\n";
    report << "| --- | --- | ---: | ---: | ---: | ---: |\n";

    for (const auto &test : cases) {
        const auto path = output / "cases" / (test.id + ".json");
        const Value result = hscam::internal::json::parse(readAll(path));
        results.push_back(result);

        const std::string status = result.at("status").asString();
        std::string frames = "-";
        std::string fps = "-";
        std::string gaps = "-";
        std::string overruns = "-";
        if (const auto *stats = result.find("stats")) {
            frames = std::to_string(stats->at("requests_completed").asUInt64());
            gaps = std::to_string(stats->at("sequence_gaps").asUInt64());
            overruns = std::to_string(stats->at("queue_overruns").asUInt64());
            if (!stats->at("measured_fps").isNull()) {
                std::ostringstream out;
                out << std::fixed << std::setprecision(3) << stats->at("measured_fps").asNumber();
                fps = out.str();
            }
        }
        report << "| " << test.id << " | " << status << " | " << frames << " | "
               << fps << " | " << gaps << " | " << overruns << " |\n";
    }

    const Value aggregate = Value::Object{
        {"schema_version", static_cast<std::uint64_t>(1)},
        {"plan_id", std::string(planId)},
        {"results", std::move(results)}
    };
    writeText(output / "results.json", hscam::internal::json::stringify(aggregate, 2) + "\n");
    writeText(output / "report.md", report.str());
}

void usage()
{
    std::cerr
        << "usage:\n"
        << "  hscam-qualify run MANIFEST.json --output DIR [--camera ID] [--rerun]\n"
        << "  hscam-qualify --worker CASE.json RESULT.json\n";
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

    const std::string planId = fnvHex(manifestText + "\n" + camera.id + "\n" +
                                      HSCAM_VERSION_STRING + "\n" + HSCAM_SOURCE_REVISION);

    fs::create_directories(output / "cases");
    fs::create_directories(output / "work");
    writeText(output / "campaign.json", manifestText);
    writeText(output / "environment.json",
              hscam::internal::json::stringify(environmentValue(), 2) + "\n");
    writeText(output / "camera.json",
              hscam::internal::json::stringify(cameraValue(camera), 2) + "\n");

    std::optional<hscam::qualification::CropGeometry> geometry;
    if (policy.geometry.enabled) {
        {
            auto cropCamera = context.open(camera.id);
            geometry = hscam::qualification::discoverCropGeometry(
                cropCamera, camera, policy.geometry);
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
            std::cout << "[" << completed << "/" << cases.size() << "] " << test.id << " resume\n";
            continue;
        }

        const auto casePath = output / "work" / (test.id + ".json");
        writeText(casePath, hscam::internal::json::stringify(caseValue(test), 2) + "\n");

        std::cout << "[" << (completed + 1) << "/" << cases.size() << "] " << test.id << std::flush;
        const int rc = runIsolated(executable, casePath, resultPath, policy.workerTimeoutMs);
        if (rc == 124) {
            writeText(resultPath, hscam::internal::json::stringify(timeoutResult(test), 2) + "\n");
            std::cout << " timeout\n";
        } else if (rc != 0 || !fs::exists(resultPath)) {
            const Value failure = Value::Object{
                {"case_id", test.id},
                {"status", "worker_error"},
                {"error", "qualification worker exited with code " + std::to_string(rc)}
            };
            writeText(resultPath, hscam::internal::json::stringify(failure, 2) + "\n");
            std::cout << " worker_error\n";
        } else {
            const auto result = hscam::internal::json::parse(readAll(resultPath));
            std::cout << " " << result.at("status").asString() << "\n";
        }
        ++completed;
    }

    generateAggregate(output, planId, cases);
    std::cout << "qualification output: " << output << "\n";
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    try {
        if (argc == 4 && std::string_view(argv[1]) == "--worker")
            return workerMain(argv[2], argv[3]);

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
