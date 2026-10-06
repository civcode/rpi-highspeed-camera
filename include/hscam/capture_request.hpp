#pragma once

#include <chrono>
#include <optional>
#include <string>

#include "hscam/geometry.hpp"
#include "hscam/pixel_format.hpp"

namespace hscam {

enum class NegotiationPolicy { Exact, AllowAdjustments };
enum class StreamKind { Raw, Processed };

struct SensorRequest {
    std::optional<std::string> modeId;
    std::optional<Rect> crop;
};

struct StreamRequest {
    StreamKind kind{StreamKind::Raw};
    std::optional<Size> size;
    std::optional<PixelFormat> format;
    unsigned bufferCount{};
};

struct TimingRequest {
    std::optional<std::chrono::microseconds> frameDuration;
    std::optional<std::chrono::microseconds> exposure;
    std::optional<double> analogueGain;
};

struct CaptureRequest {
    SensorRequest sensor;
    StreamRequest stream;
    TimingRequest timing;
    NegotiationPolicy negotiation{NegotiationPolicy::AllowAdjustments};
};

} // namespace hscam
