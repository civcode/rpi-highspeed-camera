#pragma once

#include <memory>
#include <vector>

#include "hscam/camera_info.hpp"

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
    [[nodiscard]] bool cameraBackendAvailable() const noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace hscam
