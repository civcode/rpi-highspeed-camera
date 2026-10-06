#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "hscam/camera.hpp"
#include "hscam/context.hpp"

namespace hscam {

class FrameLease::Impl {
public:
    virtual ~Impl() = default;
    virtual const FrameMetadata &metadata() const = 0;
    virtual std::span<const PlaneView> planes() const = 0;
};

class CaptureSession::Impl {
public:
    virtual ~Impl() = default;
    virtual std::unique_ptr<FrameLease::Impl> nextFrameImpl(std::chrono::milliseconds timeout) = 0;
    virtual CaptureStats stats() const = 0;
    virtual void stop() = 0;
};

class Camera::Impl {
public:
    virtual ~Impl() = default;
    virtual const std::string &id() const = 0;
    virtual CaptureConfiguration configure(const CaptureRequest &request) = 0;
    virtual CropNegotiation trySensorCrop(Rect requested) const = 0;
    virtual std::optional<Rect> currentSensorCrop() const = 0;
    virtual const CaptureConfiguration &configuration() const = 0;
    virtual CaptureSession start() = 0;
};

} // namespace hscam
