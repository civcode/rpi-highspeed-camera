#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "hscam/camera_info.hpp"
#include "hscam/camera.hpp"

namespace hscam {

class Context {
public:
    class Impl;
    Context();
    ~Context();

    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;
    Context(Context &&) noexcept;
    Context &operator=(Context &&) noexcept;

    [[nodiscard]] std::vector<CameraInfo> cameras() const;
    [[nodiscard]] Camera open(std::string_view cameraId);
    [[nodiscard]] bool cameraBackendAvailable() const noexcept;

private:
    std::shared_ptr<Impl> impl_;
};

} // namespace hscam
