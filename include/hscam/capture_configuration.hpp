#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "hscam/capture_request.hpp"

namespace hscam {

enum class AdjustmentSource { Libcamera, Sensor, Library };

struct Adjustment {
    std::string field;
    std::string requested;
    std::string negotiated;
    AdjustmentSource source{AdjustmentSource::Library};
};

struct PlaneConfiguration {
    std::uint32_t stride{};
    std::uint32_t size{};
};

struct StreamConfiguration {
    StreamKind kind{StreamKind::Raw};
    Size size;
    PixelFormat format;
    std::vector<PlaneConfiguration> planes;
    std::size_t frameBytes{};
    unsigned bufferCount{};
};

struct SensorConfiguration {
    std::optional<std::string> modeId;
    std::optional<Rect> crop;
};

struct CropNegotiation {
    Rect requested;
    Rect negotiated;
    bool exact{};
};

struct TimingConfiguration {
    std::optional<std::chrono::microseconds> requestedFrameDuration;
    std::optional<std::chrono::microseconds> negotiatedFrameDuration;
    std::optional<std::chrono::microseconds> exposure;
    std::optional<double> analogueGain;
};

struct CaptureConfiguration {
    SensorConfiguration sensor;
    StreamConfiguration stream;
    TimingConfiguration timing;
    std::vector<Adjustment> adjustments;
};

} // namespace hscam
