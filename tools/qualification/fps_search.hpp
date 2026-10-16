#pragma once

#include "internal/json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace hscam::qualification {

using internal::json::Value;

struct FpsProbeAssessment {
    std::vector<std::string> reasons;

    [[nodiscard]] bool stable() const noexcept { return reasons.empty(); }
};

// Keep every FPS rejection observable: a healthy capture worker can still
// fail an FPS threshold, maximum inter-frame gap or delivered-duration check.
inline FpsProbeAssessment assessFpsProbe(const Value &result,
                                        std::int64_t requestedUs,
                                        double achievedRatio,
                                        std::uint64_t toleranceUs,
                                        double maxIntervalRatio)
{
    FpsProbeAssessment assessment;
    if (result.at("status").asString() != "pass") {
        assessment.reasons.push_back("worker_status_" + result.at("status").asString());
        return assessment;
    }

    const auto *stats = result.find("stats");
    if (!stats || !stats->isObject()) {
        assessment.reasons.emplace_back("missing_capture_stats");
        return assessment;
    }

    const auto *fps = stats->find("measured_fps");
    if (!fps || fps->isNull()) {
        assessment.reasons.emplace_back("missing_measured_fps");
    } else {
        const double actual = fps->asNumber();
        const double required = 1000000.0 / static_cast<double>(requestedUs) * achievedRatio;
        if (!std::isfinite(actual) || actual < required)
            assessment.reasons.emplace_back("measured_fps_below_minimum");
    }

    if (const auto *maxInterval = stats->find("max_interval_ns");
        maxInterval && !maxInterval->isNull()) {
        const double allowedNs = static_cast<double>(requestedUs) * 1000.0 *
                                 maxIntervalRatio;
        if (static_cast<double>(maxInterval->asInt64()) > allowedNs)
            assessment.reasons.emplace_back("max_interval_exceeded");
    }

    if (const auto *duration = result.find("last_frame_duration_us");
        duration && !duration->isNull()) {
        const auto delivered = duration->asInt64();
        const auto allowed = requestedUs + static_cast<std::int64_t>(
            std::max<std::uint64_t>(
                toleranceUs,
                static_cast<std::uint64_t>(requestedUs * (1.0 - achievedRatio))));
        if (delivered > allowed)
            assessment.reasons.emplace_back("delivered_frame_duration_exceeded");
    }
    return assessment;
}

// Short probes can pass spuriously near the sensor's rate limit. Confirm
// progressively slower candidates with the *full* validation duration, in
// fastest-first order; never accept a short run as qualification evidence.
inline std::vector<std::int64_t> fpsFinalCandidates(
    const std::vector<std::pair<std::int64_t, bool>> &shortProbes)
{
    std::vector<std::int64_t> candidates;
    for (const auto &[duration, stable] : shortProbes) {
        if (stable)
            candidates.push_back(duration);
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());
    return candidates;
}

} // namespace hscam::qualification
