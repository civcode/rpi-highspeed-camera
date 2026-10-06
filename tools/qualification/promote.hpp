#pragma once

#include "internal/json.hpp"

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

inline void requireSchemaVersion1(const internal::json::Value &value,
                                  std::string_view artifact)
{
    if (value.at("schema_version").asUInt64() != 1)
        throw std::runtime_error(
            "unsupported " + std::string(artifact) + " schema version");
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

    const auto campaignDocument =
        internal::json::parse(promotionReadText(run / "campaign.json"));
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

    if (const auto *visual =
            campaignDocument.find("visual_samples");
        visual && visual->isObject()) {
        if (const auto *enabled = visual->find("enabled");
            enabled && enabled->asBool() &&
            !fs::exists(run / "samples.json"))
            throw std::runtime_error(
                "visual_samples are enabled but samples.json is missing");
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
        {"crop_geometry.json", false},
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

        const auto samples = run / "samples";
        if (fs::exists(samples)) {
            for (fs::recursive_directory_iterator it(samples), end;
                 it != end; ++it) {
                const auto relative =
                    fs::relative(it->path(), samples);
                if (it->is_directory() &&
                    it->path().extension() == ".hscap") {
                    result.omittedRawBundles.push_back(relative);
                    it.disable_recursion_pending();
                    continue;
                }
                if (!it->is_regular_file() ||
                    insideRawBundle(relative))
                    continue;

                const auto destination =
                    result.destination / "samples" / relative;
                fs::create_directories(
                    destination.parent_path());
                fs::copy_file(
                    it->path(), destination,
                    fs::copy_options::overwrite_existing);
                result.copiedFiles.push_back(destination);
            }
        }

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

} // namespace hscam::qualification
