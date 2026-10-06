#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "hscam/camera_info.hpp"
#include "hscam/context.hpp"

namespace hscam {

class Context::Impl : public std::enable_shared_from_this<Context::Impl> {
public:
    virtual ~Impl() = default;
    virtual std::vector<CameraInfo> cameras() const = 0;
    virtual std::unique_ptr<Camera::Impl> open(std::string_view cameraId) = 0;
    virtual bool available() const noexcept = 0;
};

#ifdef HSCAM_HAS_LIBCAMERA
std::shared_ptr<Context::Impl> makeLibcameraContext();
#endif

} // namespace hscam
