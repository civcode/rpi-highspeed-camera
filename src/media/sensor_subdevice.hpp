#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "hscam/camera_info.hpp"
#include "hscam/geometry.hpp"

namespace hscam::media {

struct CropProbe {
    Rect requested;
    Rect negotiated;
    bool exact{};
};

struct SensorSubdeviceInfo {
    std::string deviceNode;
    std::string name;
    unsigned pad{};
    CameraCapabilities capabilities;
    std::optional<Rect> currentCrop;
};

class SensorSubdevice {
public:
    SensorSubdevice() = default;
    ~SensorSubdevice();
    SensorSubdevice(SensorSubdevice &&) noexcept;
    SensorSubdevice &operator=(SensorSubdevice &&) noexcept;
    SensorSubdevice(const SensorSubdevice &) = delete;
    SensorSubdevice &operator=(const SensorSubdevice &) = delete;

    static std::optional<SensorSubdevice> discover(const std::string &sensorModel,
                                                    const std::vector<std::int64_t> &systemDevices);

    [[nodiscard]] const SensorSubdeviceInfo &info() const noexcept { return info_; }
    [[nodiscard]] std::optional<Rect> currentCrop() const;
    [[nodiscard]] CropProbe tryCrop(Rect requested) const;
    [[nodiscard]] CropProbe setCrop(Rect requested);
    void setFormatSize(Size size);

private:
    SensorSubdevice(int fd, SensorSubdeviceInfo info);
    CropProbe setSelection(Rect requested, std::uint32_t which) const;

    int fd_{-1};
    SensorSubdeviceInfo info_;
};

} // namespace hscam::media
