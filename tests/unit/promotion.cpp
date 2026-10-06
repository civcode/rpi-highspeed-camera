#include "promote.hpp"

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

void createRun(const fs::path &run, std::string sourceRevision)
{
    write(run / "campaign_status.json",
          R"({"schema_version":1,"valid":true,"reason":null})");
    write(run / "campaign.json",
          R"({"schema_version":1,"campaign":"official-rpi-cameras-v1","visual_samples":{"enabled":true}})");
    write(run / "plan.json",
          std::string{
              R"({"schema_version":1,"plan_id":"abc123","campaign":"official-rpi-cameras-v1","source_revision":")"} +
              sourceRevision + R"("})");
    write(run / "camera.json",
          R"({"schema_version":1,"id":"camera0","model":"imx296","sensor_model":"imx296"})");
    write(run / "environment.json",
          R"({"schema_version":1,"board_model":"Raspberry Pi 5 Model B"})");
    write(run / "environment_end.json",
          R"({"schema_version":1,"board_model":"Raspberry Pi 5 Model B"})");
    write(run / "results.json",
          R"({"schema_version":1,"plan_id":"abc123","summary":{"pass":1},"results":[]})");
    write(run / "results.csv", "case_id,status\ncase0,pass\n");
    write(run / "report.md", "# Result\n");
    write(run / "samples.json",
          R"({"schema_version":1,"samples":[]})");

    write(run / "samples" / "frame.png", "png");
    write(run / "samples" / "raw.hscap" / "manifest.json",
          R"({"schema_version":1})");
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
        rejectRecoveryFailure();
        std::cout << "hscam promotion tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "promotion test failed: " << e.what() << '\n';
        return 1;
    }
}
