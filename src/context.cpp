#include "hscam/context.hpp"
#include "context_impl.hpp"

namespace hscam {

#ifndef HSCAM_HAS_LIBCAMERA
class UnavailableContext final : public Context::Impl {
public:
    std::vector<CameraInfo> cameras() const override { return {}; }
    bool available() const noexcept override { return false; }
};
#endif

Context::Context()
{
#ifdef HSCAM_HAS_LIBCAMERA
    impl_ = makeLibcameraContext();
#else
    impl_ = std::make_unique<UnavailableContext>();
#endif
}

Context::~Context() = default;
Context::Context(Context &&) noexcept = default;
Context &Context::operator=(Context &&) noexcept = default;

std::vector<CameraInfo> Context::cameras() const { return impl_->cameras(); }
bool Context::cameraBackendAvailable() const noexcept { return impl_->available(); }

} // namespace hscam
