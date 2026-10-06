#pragma once

#include <memory>
#include <vector>

#include "hscam/camera_info.hpp"
#include "hscam/context.hpp"

namespace hscam {

class Context::Impl {
public:
    virtual ~Impl() = default;
    virtual std::vector<CameraInfo> cameras() const = 0;
    virtual bool available() const noexcept = 0;
};

#ifdef HSCAM_HAS_LIBCAMERA
std::unique_ptr<Context::Impl> makeLibcameraContext();
#endif

} // namespace hscam
