#pragma once

#include <chrono>
#include <memory>
#include <string>

#include "hscam/capture_configuration.hpp"
#include "hscam/frame.hpp"

namespace hscam {

class CaptureSession {
public:
    class Impl;

    CaptureSession();
    ~CaptureSession();
    CaptureSession(CaptureSession &&) noexcept;
    CaptureSession &operator=(CaptureSession &&) noexcept;
    CaptureSession(const CaptureSession &) = delete;
    CaptureSession &operator=(const CaptureSession &) = delete;

    [[nodiscard]] FrameLease nextFrame(std::chrono::milliseconds timeout);
    [[nodiscard]] CaptureStats stats() const;
    void stop();
    [[nodiscard]] explicit operator bool() const noexcept;

private:
    explicit CaptureSession(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    friend class Camera;
};

class Camera {
public:
    class Impl;

    Camera();
    ~Camera();
    Camera(Camera &&) noexcept;
    Camera &operator=(Camera &&) noexcept;
    Camera(const Camera &) = delete;
    Camera &operator=(const Camera &) = delete;

    [[nodiscard]] const std::string &id() const;
    [[nodiscard]] CaptureConfiguration configure(const CaptureRequest &request);
    [[nodiscard]] CropNegotiation trySensorCrop(Rect requested) const;
    [[nodiscard]] const CaptureConfiguration &configuration() const;
    [[nodiscard]] CaptureSession start();
    [[nodiscard]] explicit operator bool() const noexcept;

private:
    explicit Camera(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    friend class Context;
};

} // namespace hscam
