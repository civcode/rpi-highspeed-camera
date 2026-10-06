#pragma once

#include "internal/json.hpp"

#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace hscam::qualification {

struct PromotionResult {
    std::filesystem::path destination;
    std::vector<std::filesystem::path> copiedFiles;
    std::vector<std::filesystem::path> omittedRawBundles;
};

inline std::string promotionReadText(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("failed to open qualification artifact: " + path.string());
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

inline std::string pathComponent(std::string_view input)
{
    std::string out;
    out.reserve(input.size());
    bool dash = false;
    for (const unsigned char ch : input) {
        if (std::isalnum(ch)) {
            if (dash && !out.empty()) out.push_back('-');
            out.push_back(static_cast<char>(std::tolower(ch)));
            dash = false;
        } else {
            dash = true;
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "unknown" : out;
}

inline bool insideRawBundle(const std::filesystem::path &relative);

inline std::string qualificationPlanId(std::string_view campaignText,
                                       std::string_view cameraId,
                                       std::string_view hscamVersion,
                                       std::string_view sourceRevision)
{
    std::string canonical;
    canonical.reserve(campaignText.size() + cameraId.size() +
                      hscamVersion.size() + sourceRevision.size() + 4);
    canonical.append(campaignText);
    canonical.push_back('\n');
    canonical.append(cameraId);
    canonical.push_back('\n');
    canonical.append(hscamVersion);
    canonical.push_back('\n');
    canonical.append(sourceRevision);

    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char ch : canonical) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

inline void copyPromotionTree(const std::filesystem::path &source,
                              const std::filesystem::path &destination,
                              PromotionResult &result)
{
    namespace fs = std::filesystem;
    if (!fs::exists(source))
        return;

    for (fs::recursive_directory_iterator it(source), end; it != end; ++it) {
        const auto relative = fs::relative(it->path(), source);
        if (it->is_directory() && it->path().extension() == ".hscap") {
            result.omittedRawBundles.push_back(relative);
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file() || insideRawBundle(relative))
            continue;

        const auto target = destination / relative;
        fs::create_directories(target.parent_path());
        fs::copy_file(it->path(), target,
                      fs::copy_options::overwrite_existing);
        result.copiedFiles.push_back(target);
    }
}

inline void requireSchemaVersion1(const internal::json::Value &value,
                                  std::string_view artifact)
{
    if (value.at("schema_version").asUInt64() != 1)
        throw std::runtime_error(
            "unsupported " + std::string(artifact) + " schema version");
}

inline std::filesystem::path requireRunRelativeFile(
    const std::filesystem::path &run, std::string_view value,
    std::string_view description)
{
    namespace fs = std::filesystem;
    const fs::path relative(value);
    if (relative.empty() || relative.is_absolute()) {
        throw std::runtime_error(
            std::string(description) + " path is not run-relative");
    }
    for (const auto &part : relative) {
        if (part == "..")
            throw std::runtime_error(
                std::string(description) + " path escapes the run directory");
    }
    const auto path = run / relative;
    if (!fs::is_regular_file(path))
        throw std::runtime_error(
            std::string(description) + " file is missing: " + path.string());
    return path;
}

inline void validateQualificationEvidence(
    const std::filesystem::path &run,
    const internal::json::Value &plan,
    const internal::json::Value &results)
{
    namespace fs = std::filesystem;
    std::set<std::string> planned;
    for (const auto &entry : plan.at("cases").asArray())
        planned.insert(entry.at("case_id").asString());

    std::set<std::string> aggregated;
    for (const auto &entry : results.at("results").asArray())
        aggregated.insert(entry.at("case_id").asString());

    if (planned != aggregated)
        throw std::runtime_error(
            "results.json case set does not match plan.json");

    for (const auto &caseId : planned) {
        const auto casePath = run / "cases" / (caseId + ".json");
        if (!fs::is_regular_file(casePath))
            throw std::runtime_error(
                "missing per-case qualification result: " + casePath.string());

        const auto result =
            internal::json::parse(promotionReadText(casePath));
        if (result.at("status").asString() != "pass")
            continue;

        fs::path tracePath;
        if (const auto *trace = result.find("timing_trace");
            trace && !trace->isNull()) {
            tracePath = casePath.parent_path() / trace->asString();
        } else if (const auto *finalResult = result.find("final_result");
                   finalResult && finalResult->isObject()) {
            if (const auto *trace = finalResult->find("timing_trace");
                trace && !trace->isNull())
                tracePath = run / "search" / caseId / trace->asString();
        }

        if (tracePath.empty() || !fs::is_regular_file(tracePath) ||
            fs::file_size(tracePath) == 0)
            throw std::runtime_error(
                "passing case lacks per-frame timing evidence: " + caseId);
    }
}

inline void copyPromotionFile(const std::filesystem::path &source,
                              const std::filesystem::path &destination,
                              PromotionResult &result,
                              bool required)
{
    if (!std::filesystem::exists(source)) {
        if (required)
            throw std::runtime_error(
                "required qualification artifact is missing: " +
                source.string());
        return;
    }

    std::filesystem::create_directories(destination.parent_path());
    std::filesystem::copy_file(
        source, destination,
        std::filesystem::copy_options::overwrite_existing);
    result.copiedFiles.push_back(destination);
}

inline bool insideRawBundle(const std::filesystem::path &relative)
{
    for (const auto &part : relative) {
        if (part.extension() == ".hscap")
            return true;
    }
    return false;
}

inline PromotionResult promoteQualificationRun(
    const std::filesystem::path &run,
    const std::filesystem::path &resultsRoot)
{
    namespace fs = std::filesystem;
    using internal::json::Value;

    if (fs::exists(run / "recovery_failure.json"))
        throw std::runtime_error(
            "refusing to promote qualification run with "
            "recovery_failure.json");

    const auto status = internal::json::parse(
        promotionReadText(run / "campaign_status.json"));
    requireSchemaVersion1(status, "campaign_status");
    if (!status.at("valid").asBool()) {
        std::string reason = "campaign_status.valid is false";
        if (const auto *value = status.find("reason");
            value && !value->isNull())
            reason += ": " + value->asString();
        throw std::runtime_error(
            "refusing to promote invalid qualification run: " + reason);
    }

    const auto plan =
        internal::json::parse(promotionReadText(run / "plan.json"));
    requireSchemaVersion1(plan, "qualification plan");

    const std::string campaignText =
        promotionReadText(run / "campaign.json");
    const auto campaignDocument =
        internal::json::parse(campaignText);
    requireSchemaVersion1(campaignDocument, "campaign manifest");

    const auto camera =
        internal::json::parse(promotionReadText(run / "camera.json"));
    requireSchemaVersion1(camera, "camera snapshot");

    const auto environment =
        internal::json::parse(promotionReadText(run / "environment.json"));
    requireSchemaVersion1(environment, "environment snapshot");

    const auto environmentEnd =
        internal::json::parse(
            promotionReadText(run / "environment_end.json"));
    requireSchemaVersion1(
        environmentEnd, "end environment snapshot");

    const auto results =
        internal::json::parse(promotionReadText(run / "results.json"));
    requireSchemaVersion1(results, "results");

    const std::string campaign = plan.at("campaign").asString();
    if (campaignDocument.at("campaign").asString() != campaign)
        throw std::runtime_error(
            "campaign.json does not match plan.json campaign");

    const std::string sourceRevision =
        plan.at("source_revision").asString();
    if (sourceRevision.empty() ||
        sourceRevision == "unknown" ||
        sourceRevision.ends_with("-dirty"))
        throw std::runtime_error(
            "refusing to promote a run without a clean committed "
            "source revision");

    const std::string hscamVersion = plan.at("hscam_version").asString();
    const std::string expectedPlanId = qualificationPlanId(
        campaignText, camera.at("id").asString(), hscamVersion,
        sourceRevision);
    if (plan.at("plan_id").asString() != expectedPlanId)
        throw std::runtime_error(
            "qualification plan_id does not match campaign/camera/version/source provenance");
    if (results.at("plan_id").asString() != expectedPlanId)
        throw std::runtime_error(
            "results.json plan_id does not match qualification plan");

    validateQualificationEvidence(run, plan, results);

    if (const auto *visual =
            campaignDocument.find("visual_samples");
        visual && visual->isObject()) {
        if (const auto *enabled = visual->find("enabled");
            enabled && enabled->asBool()) {
            if (!fs::exists(run / "samples.json"))
                throw std::runtime_error(
                    "visual_samples are enabled but samples.json is missing");

            const auto samplesDocument =
                internal::json::parse(
                    promotionReadText(run / "samples.json"));
            requireSchemaVersion1(
                samplesDocument, "visual samples");

            const auto &samples =
                samplesDocument.at("samples").asArray();
            if (samples.empty())
                throw std::runtime_error(
                    "visual_samples are enabled but no visual evidence was generated");

            bool hasVideoEvidence = false;
            for (const auto &sample : samples) {
                if (const auto *error = sample.find("error");
                    error && !error->isNull())
                    throw std::runtime_error(
                        "refusing to promote a run with visual-sample "
                        "errors: " + error->asString());

                const auto *pngs = sample.find("pngs");
                if (!pngs || !pngs->isArray() || pngs->asArray().empty())
                    throw std::runtime_error(
                        "refusing to promote a visual sample without PNG evidence");
                for (const auto &png : pngs->asArray())
                    (void)requireRunRelativeFile(
                        run, png.asString(), "visual PNG");

                if (const auto *video = sample.find("video");
                    video && !video->isNull()) {
                    (void)requireRunRelativeFile(
                        run, video->asString(), "visual video");
                    hasVideoEvidence = true;
                }
            }

            if (!hasVideoEvidence)
                throw std::runtime_error(
                    "visual_samples are enabled but no video evidence was generated");
        }
    }

    const std::string planId = plan.at("plan_id").asString();
    const std::string cameraId = camera.at("id").asString();
    const std::string cameraModel = camera.at("model").asString();

    std::string sensor = cameraModel;
    if (const auto *value = camera.find("sensor_model");
        value && !value->isNull())
        sensor = value->asString();

    std::string platform = "unknown-platform";
    if (const auto *value = environment.find("board_model");
        value && !value->isNull() && value->isString())
        platform = value->asString();

    PromotionResult result;
    result.destination =
        resultsRoot /
        pathComponent(platform) /
        pathComponent(sensor) /
        (pathComponent(campaign) + "-" + pathComponent(planId));

    if (fs::exists(result.destination))
        throw std::runtime_error(
            "qualification destination already exists: " +
            result.destination.string());
    fs::create_directories(result.destination);

    struct Artifact {
        const char *name;
        bool required;
    };
    static constexpr Artifact artifacts[] = {
        {"campaign.json", true},
        {"plan.json", true},
        {"environment.json", true},
        {"environment_end.json", true},
        {"camera.json", true},
        {"mode_sensor_crops.json", true},
        {"crop_geometry.json", false},
        {"crop_geometry_probes.jsonl", false},
        {"campaign_status.json", true},
        {"results.json", true},
        {"results.csv", true},
        {"report.md", true},
        {"samples.json", false}
    };

    try {
        for (const auto &artifact : artifacts)
            copyPromotionFile(
                run / artifact.name,
                result.destination / artifact.name,
                result, artifact.required);

        // Preserve all per-case and FPS-search evidence, including timing
        // JSONL sidecars. These are the audit trail behind aggregate tables.
        copyPromotionTree(run / "cases", result.destination / "cases", result);
        copyPromotionTree(run / "search", result.destination / "search", result);

        const auto samples = run / "samples";
        copyPromotionTree(samples, result.destination / "samples", result);

        Value::Array omitted;
        for (const auto &path : result.omittedRawBundles)
            omitted.emplace_back(path.string());

        const Value publication = Value::Object{
            {"schema_version",
             static_cast<std::uint64_t>(1)},
            {"campaign", campaign},
            {"plan_id", planId},
            {"source_revision", sourceRevision},
            {"camera_id", cameraId},
            {"camera_model", cameraModel},
            {"sensor", sensor},
            {"platform", platform},
            {"raw_sample_bundles_omitted",
             std::move(omitted)}
        };

        std::ofstream published(
            result.destination / "published.json",
            std::ios::binary | std::ios::trunc);
        if (!published)
            throw std::runtime_error(
                "failed to create published.json");
        published <<
            internal::json::stringify(publication, 2) <<
            '\n';
        if (!published)
            throw std::runtime_error(
                "failed to write published.json");
        result.copiedFiles.push_back(
            result.destination / "published.json");
    } catch (...) {
        std::error_code ec;
        fs::remove_all(result.destination, ec);
        throw;
    }

    return result;
}

struct OfficialDatasetAudit {
    std::map<std::string, std::vector<std::filesystem::path>> resultsBySensor;
    std::map<std::string, std::vector<std::filesystem::path>> blockedBySensor;
    std::vector<std::string> missingSensors;
    std::vector<std::string> invalidReceipts;

    [[nodiscard]] bool complete() const noexcept {
        return missingSensors.empty() && invalidReceipts.empty();
    }
};

inline OfficialDatasetAudit auditOfficialDataset(
    const std::filesystem::path &resultsRoot,
    std::string_view campaign = "official-rpi-cameras-v1")
{
    namespace fs = std::filesystem;
    static constexpr std::array<std::string_view, 6> expected{
        "ov5647", "imx219", "imx708", "imx477", "imx296", "imx500"
    };

    OfficialDatasetAudit audit;
    for (const auto sensor : expected) {
        audit.resultsBySensor.emplace(std::string(sensor),
                                     std::vector<fs::path>{});
        audit.blockedBySensor.emplace(std::string(sensor),
                                     std::vector<fs::path>{});
    }

    if (fs::exists(resultsRoot)) {
        for (fs::recursive_directory_iterator it(resultsRoot), end;
             it != end; ++it) {
            if (!it->is_regular_file())
                continue;

            const auto filename = it->path().filename().string();
            if (filename != "published.json" && filename != "blocked.json")
                continue;

            try {
                const auto receipt = internal::json::parse(
                    promotionReadText(it->path()));
                requireSchemaVersion1(receipt, "published receipt");

                if (receipt.at("campaign").asString() != campaign)
                    continue;

                const std::string sensor =
                    pathComponent(receipt.at("sensor").asString());
                std::string matched;
                for (const auto expectedSensor : expected) {
                    if (sensor.find(expectedSensor) != std::string::npos) {
                        matched = std::string(expectedSensor);
                        break;
                    }
                }
                if (matched.empty())
                    continue;

                if (filename == "blocked.json") {
                    if (receipt.at("status").asString() != "blocked")
                        throw std::runtime_error(
                            "blocked.json status must be 'blocked'");
                    const auto reason = receipt.at("reason").asString();
                    if (reason.empty())
                        throw std::runtime_error(
                            "blocked.json reason must not be empty");
                    audit.blockedBySensor[matched].push_back(it->path());
                    continue;
                }

                const auto directory = it->path().parent_path();
                static constexpr std::array<std::string_view, 10> required{
                    "campaign.json", "plan.json", "environment.json",
                    "environment_end.json", "camera.json",
                    "mode_sensor_crops.json", "campaign_status.json",
                    "results.json", "results.csv", "report.md"
                };
                for (const auto file : required) {
                    if (!fs::is_regular_file(directory / file))
                        throw std::runtime_error(
                            "missing " + std::string(file));
                }

                const auto status = internal::json::parse(
                    promotionReadText(directory / "campaign_status.json"));
                requireSchemaVersion1(status, "campaign_status");
                if (!status.at("valid").asBool())
                    throw std::runtime_error(
                        "campaign_status.valid is false");

                audit.resultsBySensor[matched].push_back(directory);
            } catch (const std::exception &e) {
                audit.invalidReceipts.push_back(
                    it->path().string() + ": " + e.what());
            }
        }
    }

    for (const auto sensor : expected) {
        const auto key = std::string(sensor);
        if (audit.resultsBySensor[key].empty() &&
            audit.blockedBySensor[key].empty())
            audit.missingSensors.push_back(key);
    }
    return audit;
}

} // namespace hscam::qualification
