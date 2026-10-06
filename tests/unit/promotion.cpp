#include "promote.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

namespace fs = std::filesystem;

void require(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}

void write(const fs::path &path, const std::string &text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        throw std::runtime_error("failed to create fixture");
    out << text;
}

void createRun(const fs::path &run, std::string sourceRevision,
               std::string sensor = "imx296")
{
    const std::string campaign =
        R"({"schema_version":1,"campaign":"official-rpi-cameras-v1","visual_samples":{"enabled":true}})";
    const std::string version = "0.1.0-dev";
    const std::string planId =
        hscam::qualification::qualificationPlanId(
            campaign, "camera0", version, sourceRevision);

    write(run / "campaign_status.json",
          R"({"schema_version":1,"valid":true,"reason":null})");
    write(run / "campaign.json", campaign);
    write(run / "plan.json",
          std::string{
              R"({"schema_version":1,"plan_id":")"} + planId +
              R"(","campaign":"official-rpi-cameras-v1","hscam_version":")" +
              version + R"(","source_revision":")" + sourceRevision +
              R"(","cases":[{"case_id":"case0"}]})");
    write(run / "camera.json",
          std::string{R"({"schema_version":1,"id":"camera0","model":")"} +
          sensor + R"(","sensor_model":")" + sensor + R"("})");
    write(run / "mode_sensor_crops.json",
          R"({"schema_version":1,"camera_id":"camera0","modes":[]})");
    write(run / "environment.json",
          R"({"schema_version":1,"board_model":"Raspberry Pi 5 Model B"})");
    write(run / "environment_end.json",
          R"({"schema_version":1,"board_model":"Raspberry Pi 5 Model B"})");
    write(run / "results.json",
          std::string{
              R"({"schema_version":1,"plan_id":")"} + planId +
              R"(","summary":{"pass":1},"results":[{"case_id":"case0","status":"pass"}]})");
    write(run / "results.csv", "case_id,status\ncase0,pass\n");
    write(run / "report.md", "# Result\n");
    write(run / "samples.json",
          R"({"schema_version":1,"samples":[{"label":"fastest","pngs":["samples/frame.png"],"dng":null,"video":"samples/video.mp4","warnings":[]}]})");

    write(run / "samples" / "frame.png", "png");
    write(run / "samples" / "video.mp4", "video");
    write(run / "samples" / "raw.hscap" / "manifest.json",
          R"({"schema_version":1})");
    write(run / "cases" / "case0.json",
          R"({"case_id":"case0","status":"pass","timing_trace":"case0.timing.jsonl"})");
    write(run / "cases" / "case0.timing.jsonl",
          R"({"frame":0,"sequence":1,"sensor_timestamp_ns":123})");
    write(run / "search" / "case0" / "1000-1500.json",
          R"({"case_id":"case0-fd-1000","status":"pass"})");
}

void validPromotion()
{
    const auto base =
        fs::temp_directory_path() / "hscam-promotion-test-valid";
    const auto run = base / "run";
    const auto results = base / "results";

    std::error_code ec;
    fs::remove_all(base, ec);
    createRun(run, "0123456789abcdef");

    const auto promoted =
        hscam::qualification::promoteQualificationRun(run, results);

    require(fs::exists(promoted.destination / "published.json"),
            "published.json must exist");
    require(fs::exists(promoted.destination / "results.json"),
            "results.json must be promoted");
    require(fs::exists(promoted.destination / "samples" / "frame.png"),
            "derived sample must be promoted");
    require(fs::exists(promoted.destination / "cases" / "case0.json"),
            "per-case result must be promoted");
    require(fs::exists(promoted.destination / "cases" / "case0.timing.jsonl"),
            "per-frame timing trace must be promoted");
    require(fs::exists(promoted.destination / "search" / "case0" /
                       "1000-1500.json"),
            "FPS-search evidence must be promoted");
    require(!fs::exists(
                promoted.destination / "samples" /
                "raw.hscap" / "manifest.json"),
            "raw HSCAP bundle must be omitted");
    require(promoted.omittedRawBundles.size() == 1,
            "omitted raw bundle must be recorded");

    fs::remove_all(base, ec);
}

void rejectDirtyRevision()
{
    const auto base =
        fs::temp_directory_path() / "hscam-promotion-test-dirty";
    const auto run = base / "run";

    std::error_code ec;
    fs::remove_all(base, ec);
    createRun(run, "0123456789abcdef-dirty");

    bool rejected = false;
    try {
        (void)hscam::qualification::promoteQualificationRun(
            run, base / "results");
    } catch (const std::exception &) {
        rejected = true;
    }

    require(rejected, "dirty source revision must be rejected");
    fs::remove_all(base, ec);
}

void rejectVisualSampleError()
{
    const auto base =
        fs::temp_directory_path() / "hscam-promotion-test-visual";
    const auto run = base / "run";

    std::error_code ec;
    fs::remove_all(base, ec);
    createRun(run, "0123456789abcdef");
    write(run / "samples.json",
          R"({"schema_version":1,"samples":[{"label":"fastest","error":"ffmpeg failed"}]})");

    bool rejected = false;
    try {
        (void)hscam::qualification::promoteQualificationRun(
            run, base / "results");
    } catch (const std::exception &) {
        rejected = true;
    }

    require(rejected, "visual sample errors must block promotion");
    fs::remove_all(base, ec);
}

void rejectTamperedPlanId()
{
    const auto base =
        fs::temp_directory_path() / "hscam-promotion-test-plan";
    const auto run = base / "run";

    std::error_code ec;
    fs::remove_all(base, ec);
    createRun(run, "0123456789abcdef");
    write(run / "results.json",
          R"({"schema_version":1,"plan_id":"tampered","summary":{"pass":1},"results":[]})");

    bool rejected = false;
    try {
        (void)hscam::qualification::promoteQualificationRun(
            run, base / "results");
    } catch (const std::exception &) {
        rejected = true;
    }

    require(rejected, "tampered plan provenance must block promotion");
    fs::remove_all(base, ec);
}

void rejectMissingVisualVideo()
{
    const auto base =
        fs::temp_directory_path() / "hscam-promotion-test-no-video";
    const auto run = base / "run";

    std::error_code ec;
    fs::remove_all(base, ec);
    createRun(run, "0123456789abcdef");
    write(run / "samples.json",
          R"({"schema_version":1,"samples":[{"label":"fastest","pngs":["samples/frame.png"],"dng":null,"video":null,"warnings":["video export failed"]}]})");

    bool rejected = false;
    try {
        (void)hscam::qualification::promoteQualificationRun(
            run, base / "results");
    } catch (const std::exception &) {
        rejected = true;
    }

    require(rejected, "missing visual video must block promotion");
    fs::remove_all(base, ec);
}

void rejectMissingTimingEvidence()
{
    const auto base =
        fs::temp_directory_path() / "hscam-promotion-test-no-timing";
    const auto run = base / "run";

    std::error_code ec;
    fs::remove_all(base, ec);
    createRun(run, "0123456789abcdef");
    fs::remove(run / "cases" / "case0.timing.jsonl", ec);

    bool rejected = false;
    try {
        (void)hscam::qualification::promoteQualificationRun(
            run, base / "results");
    } catch (const std::exception &) {
        rejected = true;
    }

    require(rejected, "missing pass-case timing evidence must block promotion");
    fs::remove_all(base, ec);
}

void auditOfficialDatasetCoverage()
{
    const auto base =
        fs::temp_directory_path() / "hscam-promotion-test-audit";
    const auto results = base / "results";
    std::error_code ec;
    fs::remove_all(base, ec);

    createRun(base / "run-imx296", "0123456789abcdef", "imx296");
    (void)hscam::qualification::promoteQualificationRun(
        base / "run-imx296", results);

    const std::array<const char *, 5> blocked{
        "ov5647", "imx219", "imx708", "imx477", "imx500"
    };
    for (const auto *sensor : blocked) {
        write(results / "blocked" / sensor / "blocked.json",
              std::string{
                  R"({"schema_version":1,"campaign":"official-rpi-cameras-v1","sensor":")"} +
                  sensor +
                  R"(","status":"blocked","reason":"hardware unavailable for campaign"})");
    }

    const auto audit =
        hscam::qualification::auditOfficialDataset(results);
    require(audit.complete(),
            "published or explicitly blocked sensors must satisfy dataset gate");
    require(audit.resultsBySensor.at("imx296").size() == 1,
            "published IMX296 result must be counted");
    require(audit.blockedBySensor.at("imx500").size() == 1,
            "blocked IMX500 record must be counted");

    fs::remove(results / "blocked" / "imx500" / "blocked.json", ec);
    const auto incomplete =
        hscam::qualification::auditOfficialDataset(results);
    require(!incomplete.complete(),
            "missing official sensor must fail dataset gate");

    fs::remove_all(base, ec);
}

void rejectRecoveryFailure()
{
    const auto base =
        fs::temp_directory_path() / "hscam-promotion-test-recovery";
    const auto run = base / "run";

    std::error_code ec;
    fs::remove_all(base, ec);
    createRun(run, "0123456789abcdef");
    write(run / "recovery_failure.json",
          R"({"schema_version":1,"error":"camera did not recover"})");

    bool rejected = false;
    try {
        (void)hscam::qualification::promoteQualificationRun(
            run, base / "results");
    } catch (const std::exception &) {
        rejected = true;
    }

    require(rejected, "recovery failure must block promotion");
    fs::remove_all(base, ec);
}

} // namespace

int main()
{
    try {
        validPromotion();
        rejectDirtyRevision();
        rejectVisualSampleError();
        rejectTamperedPlanId();
        rejectMissingVisualVideo();
        rejectMissingTimingEvidence();
        auditOfficialDatasetCoverage();
        rejectRecoveryFailure();
        std::cout << "hscam promotion tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "promotion test failed: " << e.what() << '\n';
        return 1;
    }
}
