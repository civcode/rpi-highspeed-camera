#pragma once

#include "internal/json.hpp"

#include <algorithm>
#include <cctype>
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

inline void copyPromotionFile(const std::filesystem::path &source,
                              const std::filesystem::path &destination,
                              PromotionResult &result,
                              bool required)
{
    if (!std::filesystem::exists(source)) {
        if (required)
            throw std::runtime_error("required qualification artifact is missing: " + source.string());
        return;
    }

    std::filesystem::create_directories(destination.parent_path());
    std::filesystem::copy_file(source, destination,
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

inline PromotionResult promoteQualificationRun(const std::filesystem::path &run,
                                               const std::filesystem::path &resultsRoot)
{
    namespace fs = std::filesystem;
    using internal::json::Value;

    const auto status = internal::json::parse(
        promotionReadText(run / "campaign_status.json"));
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
    const auto camera =
        internal::json::parse(promotionReadText(run / "camera.json"));
    const auto environment =
        internal::json::parse(promotionReadText(run / "environment.json"));

    const std::string campaign = plan.at("campaign").asString();
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

    struct Artifact { const char *name; bool required; };
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
        {"samples.json", false},
        {"recovery_failure.json", false}
    };

    try {
        for (const auto &artifact : artifacts)
            copyPromotionFile(run / artifact.name,
                              result.destination / artifact.name,
                              result, artifact.required);

        const auto samples = run / "samples";
        if (fs::exists(samples)) {
            for (fs::recursive_directory_iterator it(samples), end;
                 it != end; ++it) {
                const auto relative = fs::relative(it->path(), samples);
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
                fs::create_directories(destination.parent_path());
                fs::copy_file(it->path(), destination,
                              fs::copy_options::overwrite_existing);
                result.copiedFiles.push_back(destination);
            }
        }

        Value::Array omitted;
        for (const auto &path : result.omittedRawBundles)
            omitted.emplace_back(path.string());

        const Value publication = Value::Object{
            {"schema_version", static_cast<std::uint64_t>(1)},
            {"campaign", campaign},
            {"plan_id", planId},
            {"camera_id", cameraId},
            {"camera_model", cameraModel},
            {"sensor", sensor},
            {"platform", platform},
            {"raw_sample_bundles_omitted", std::move(omitted)}
        };

        std::ofstream published(
            result.destination / "published.json",
            std::ios::binary | std::ios::trunc);
        if (!published)
            throw std::runtime_error("failed to create published.json");
        published << internal::json::stringify(publication, 2) << '\n';
        if (!published)
            throw std::runtime_error("failed to write published.json");
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
