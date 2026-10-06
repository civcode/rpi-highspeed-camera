#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "hscam/geometry.hpp"
#include "hscam/pixel_format.hpp"

namespace hscam {

struct DurationRange {
    std::chrono::microseconds min{};
    std::chrono::microseconds max{};
};

struct SensorMode {
    std::string id;
    Size size;
    PixelFormat format;
    unsigned bitDepth{};
    std::optional<DurationRange> frameDuration;
    std::optional<std::int64_t> pixelRate;
    std::vector<std::int64_t> linkFrequencies;
    std::optional<Rect> sensorCrop;
};

struct CameraCapabilities {
    bool rawCapture{};
    bool processedCapture{};
    bool sensorCropQueryable{};
    bool sensorCropTryable{};
    bool sensorCropSettable{};
    bool sensorCropExact{};
    bool frameDurationControl{};
    bool exposureControl{};
    bool analogueGainControl{};
    std::optional<Rect> sensorNativeSize;
    std::optional<Rect> sensorCropBounds;
    std::optional<Rect> sensorDefaultCrop;
};

struct CameraInfo {
    std::string id;
    std::string model;
    std::optional<std::string> sensorModel;
    Size pixelArray;
    std::vector<Rect> activeAreas;
    std::vector<SensorMode> sensorModes;
    CameraCapabilities capabilities;
    std::vector<std::int64_t> systemDevices;
    std::optional<std::string> sensorSubdevice;
};

std::string stableModeId(const SensorMode &mode);

} // namespace hscam
