#include "hscam/context.hpp"
#include "hscam/error.hpp"
#include "context_impl.hpp"
#include "internal/backend.hpp"

namespace hscam {

#ifndef HSCAM_HAS_LIBCAMERA
class UnavailableContext final : public Context::Impl {
public:
    std::vector<CameraInfo> cameras() const override { return {}; }
    std::unique_ptr<Camera::Impl> open(std::string_view) override {
        throw Unsupported("hscam was built without libcamera support");
    }
    bool available() const noexcept override { return false; }
};
#endif

Context::Context()
{
#ifdef HSCAM_HAS_LIBCAMERA
    impl_ = makeLibcameraContext();
#else
    impl_ = std::make_shared<UnavailableContext>();
#endif
}

Context::~Context() = default;
Context::Context(Context &&) noexcept = default;
Context &Context::operator=(Context &&) noexcept = default;

std::vector<CameraInfo> Context::cameras() const { return impl_->cameras(); }
Camera Context::open(std::string_view cameraId) { return Camera(impl_->open(cameraId)); }
bool Context::cameraBackendAvailable() const noexcept { return impl_->available(); }

} // namespace hscam
