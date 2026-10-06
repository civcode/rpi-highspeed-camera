#pragma once

#include "hscam/camera.hpp"
#include "hscam/camera_info.hpp"
#include "internal/json.hpp"

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace hscam::qualification {

struct CropGeometryPolicy {
    bool enabled{true};
    bool verifyCartesian{true};
    std::uint64_t maxCartesianChecks{};
};

struct CropGeometry {
    bool supported{};
    Rect bounds;
    std::vector<std::uint32_t> widths;
    std::vector<std::uint32_t> heights;
    std::vector<std::int32_t> xOriginsForMinimumSize;
    std::vector<std::int32_t> yOriginsForMinimumSize;
    std::uint32_t xStep{};
    std::uint32_t yStep{};
    bool cartesianVerified{};
    std::uint64_t cartesianChecks{};
    std::uint64_t cartesianExceptions{};
    std::vector<Rect> exceptionSamples;
};

inline std::uint32_t stepFromOrigins(const std::vector<std::int32_t> &values)
{
    if (values.size() < 2) return 0;
    std::uint32_t step{};
    for (std::size_t i = 1; i < values.size(); ++i) {
        const auto delta = static_cast<std::uint32_t>(values[i] - values[i - 1]);
        if (delta) step = step ? std::gcd(step, delta) : delta;
    }
    return step;
}

inline std::vector<std::uint32_t> probeWidths(Camera &camera, Rect bounds, std::uint32_t height)
{
    std::set<std::uint32_t> accepted;
    for (std::uint32_t width = 1; width <= bounds.width; ++width) {
        const auto x = bounds.x + static_cast<std::int32_t>((bounds.width - width) / 2);
        const auto y = bounds.y + static_cast<std::int32_t>((bounds.height - height) / 2);
        try {
            const auto result = camera.trySensorCrop({x, y, width, height});
            if (result.negotiated.width && result.negotiated.height)
                accepted.insert(result.negotiated.width);
        } catch (const Unsupported &) {
            throw;
        } catch (...) {
            // An individual TRY request may be rejected. Continue probing the axis.
        }
    }
    return {accepted.begin(), accepted.end()};
}

inline std::vector<std::uint32_t> probeHeights(Camera &camera, Rect bounds, std::uint32_t width)
{
    std::set<std::uint32_t> accepted;
    for (std::uint32_t height = 1; height <= bounds.height; ++height) {
        const auto x = bounds.x + static_cast<std::int32_t>((bounds.width - width) / 2);
        const auto y = bounds.y + static_cast<std::int32_t>((bounds.height - height) / 2);
        try {
            const auto result = camera.trySensorCrop({x, y, width, height});
            if (result.negotiated.width && result.negotiated.height)
                accepted.insert(result.negotiated.height);
        } catch (const Unsupported &) {
            throw;
        } catch (...) {
        }
    }
    return {accepted.begin(), accepted.end()};
}

inline void mergeUnique(std::vector<std::uint32_t> &into, const std::vector<std::uint32_t> &values)
{
    std::set<std::uint32_t> merged(into.begin(), into.end());
    merged.insert(values.begin(), values.end());
    into.assign(merged.begin(), merged.end());
}

inline CropGeometry discoverCropGeometry(Camera &camera, const CameraInfo &info,
                                         const CropGeometryPolicy &policy)
{
    CropGeometry geometry;
    if (!policy.enabled || !info.capabilities.sensorCropTryable ||
        !info.capabilities.sensorCropBounds)
        return geometry;

    geometry.supported = true;
    geometry.bounds = *info.capabilities.sensorCropBounds;
    if (geometry.bounds.empty())
        throw std::runtime_error("sensor reports an empty crop bound");

    geometry.widths = probeWidths(camera, geometry.bounds, geometry.bounds.height);
    geometry.heights = probeHeights(camera, geometry.bounds, geometry.bounds.width);

    if (geometry.widths.empty() || geometry.heights.empty())
        throw std::runtime_error("TRY crop probing returned no valid dimensions");

    // Repeat at the discovered minima to catch dimensions that are only valid
    // when the other axis is also reduced.
    mergeUnique(geometry.widths, probeWidths(camera, geometry.bounds, geometry.heights.front()));
    mergeUnique(geometry.heights, probeHeights(camera, geometry.bounds, geometry.widths.front()));

    const auto minWidth = geometry.widths.front();
    const auto minHeight = geometry.heights.front();

    {
        std::set<std::int32_t> origins;
        const auto y = geometry.bounds.y +
                       static_cast<std::int32_t>((geometry.bounds.height - minHeight) / 2);
        const auto last = geometry.bounds.x +
                          static_cast<std::int32_t>(geometry.bounds.width - minWidth);
        for (std::int32_t x = geometry.bounds.x; x <= last; ++x) {
            try {
                const auto result = camera.trySensorCrop({x, y, minWidth, minHeight});
                if (result.negotiated.width == minWidth && result.negotiated.height == minHeight)
                    origins.insert(result.negotiated.x);
            } catch (...) {
            }
        }
        geometry.xOriginsForMinimumSize.assign(origins.begin(), origins.end());
        geometry.xStep = stepFromOrigins(geometry.xOriginsForMinimumSize);
    }

    {
        std::set<std::int32_t> origins;
        const auto x = geometry.bounds.x +
                       static_cast<std::int32_t>((geometry.bounds.width - minWidth) / 2);
        const auto last = geometry.bounds.y +
                          static_cast<std::int32_t>(geometry.bounds.height - minHeight);
        for (std::int32_t y = geometry.bounds.y; y <= last; ++y) {
            try {
                const auto result = camera.trySensorCrop({x, y, minWidth, minHeight});
                if (result.negotiated.width == minWidth && result.negotiated.height == minHeight)
                    origins.insert(result.negotiated.y);
            } catch (...) {
            }
        }
        geometry.yOriginsForMinimumSize.assign(origins.begin(), origins.end());
        geometry.yStep = stepFromOrigins(geometry.yOriginsForMinimumSize);
    }

    if (policy.verifyCartesian) {
        const std::uint64_t total = static_cast<std::uint64_t>(geometry.widths.size()) *
                                    static_cast<std::uint64_t>(geometry.heights.size());
        if (policy.maxCartesianChecks && total > policy.maxCartesianChecks)
            throw std::runtime_error("crop Cartesian verification would exceed max_cartesian_checks");

        for (const auto width : geometry.widths) {
            for (const auto height : geometry.heights) {
                const auto x = geometry.bounds.x +
                               static_cast<std::int32_t>((geometry.bounds.width - width) / 2);
                const auto y = geometry.bounds.y +
                               static_cast<std::int32_t>((geometry.bounds.height - height) / 2);
                ++geometry.cartesianChecks;
                bool exact = false;
                try {
                    const auto result = camera.trySensorCrop({x, y, width, height});
                    exact = result.negotiated.width == width &&
                            result.negotiated.height == height;
                } catch (...) {
                }

                if (!exact) {
                    ++geometry.cartesianExceptions;
                    if (geometry.exceptionSamples.size() < 100)
                        geometry.exceptionSamples.push_back({x, y, width, height});
                }
            }
        }
        geometry.cartesianVerified = geometry.cartesianExceptions == 0;
    }

    return geometry;
}

inline internal::json::Value geometryValue(const CropGeometry &geometry)
{
    using internal::json::Value;

    Value::Array widths;
    for (const auto value : geometry.widths) widths.emplace_back(static_cast<std::uint64_t>(value));
    Value::Array heights;
    for (const auto value : geometry.heights) heights.emplace_back(static_cast<std::uint64_t>(value));
    Value::Array xs;
    for (const auto value : geometry.xOriginsForMinimumSize) xs.emplace_back(static_cast<std::int64_t>(value));
    Value::Array ys;
    for (const auto value : geometry.yOriginsForMinimumSize) ys.emplace_back(static_cast<std::int64_t>(value));
    Value::Array exceptions;
    for (const auto &rect : geometry.exceptionSamples) {
        exceptions.emplace_back(Value::Array{
            static_cast<std::int64_t>(rect.x),
            static_cast<std::int64_t>(rect.y),
            static_cast<std::uint64_t>(rect.width),
            static_cast<std::uint64_t>(rect.height)
        });
    }

    return Value::Object{
        {"schema_version", static_cast<std::uint64_t>(1)},
        {"supported", geometry.supported},
        {"bounds", Value::Array{
            static_cast<std::int64_t>(geometry.bounds.x),
            static_cast<std::int64_t>(geometry.bounds.y),
            static_cast<std::uint64_t>(geometry.bounds.width),
            static_cast<std::uint64_t>(geometry.bounds.height)
        }},
        {"widths", std::move(widths)},
        {"heights", std::move(heights)},
        {"x_origins_for_minimum_size", std::move(xs)},
        {"y_origins_for_minimum_size", std::move(ys)},
        {"x_step", static_cast<std::uint64_t>(geometry.xStep)},
        {"y_step", static_cast<std::uint64_t>(geometry.yStep)},
        {"cartesian_verified", geometry.cartesianVerified},
        {"cartesian_checks", geometry.cartesianChecks},
        {"cartesian_exceptions", geometry.cartesianExceptions},
        {"exception_samples", std::move(exceptions)}
    };
}

} // namespace hscam::qualification
