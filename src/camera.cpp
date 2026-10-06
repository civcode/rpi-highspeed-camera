#include "hscam/camera.hpp"
#include "hscam/error.hpp"
#include "internal/backend.hpp"

namespace hscam {

FrameLease::FrameLease() = default;
FrameLease::FrameLease(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FrameLease::~FrameLease() = default;
FrameLease::FrameLease(FrameLease &&) noexcept = default;
FrameLease &FrameLease::operator=(FrameLease &&) noexcept = default;
FrameLease::operator bool() const noexcept { return static_cast<bool>(impl_); }
const FrameMetadata &FrameLease::metadata() const {
    if (!impl_) throw CaptureFailed("empty FrameLease");
    return impl_->metadata();
}
std::span<const PlaneView> FrameLease::planes() const {
    if (!impl_) throw CaptureFailed("empty FrameLease");
    return impl_->planes();
}

CaptureSession::CaptureSession() = default;
CaptureSession::CaptureSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
CaptureSession::~CaptureSession() = default;
CaptureSession::CaptureSession(CaptureSession &&) noexcept = default;
CaptureSession &CaptureSession::operator=(CaptureSession &&) noexcept = default;
CaptureSession::operator bool() const noexcept { return static_cast<bool>(impl_); }
FrameLease CaptureSession::nextFrame(std::chrono::milliseconds timeout) {
    if (!impl_) throw CaptureFailed("empty CaptureSession");
    return FrameLease(impl_->nextFrameImpl(timeout));
}
CaptureStats CaptureSession::stats() const {
    if (!impl_) return {};
    return impl_->stats();
}
void CaptureSession::stop() {
    if (impl_) impl_->stop();
}

Camera::Camera() = default;
Camera::Camera(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Camera::~Camera() = default;
Camera::Camera(Camera &&) noexcept = default;
Camera &Camera::operator=(Camera &&) noexcept = default;
Camera::operator bool() const noexcept { return static_cast<bool>(impl_); }
const std::string &Camera::id() const {
    if (!impl_) throw CameraNotFound("empty Camera");
    return impl_->id();
}
CaptureConfiguration Camera::configure(const CaptureRequest &request) {
    if (!impl_) throw CameraNotFound("empty Camera");
    return impl_->configure(request);
}
CropNegotiation Camera::trySensorCrop(Rect requested) const {
    if (!impl_) throw CameraNotFound("empty Camera");
    return impl_->trySensorCrop(requested);
}
const CaptureConfiguration &Camera::configuration() const {
    if (!impl_) throw CameraNotFound("empty Camera");
    return impl_->configuration();
}
CaptureSession Camera::start() {
    if (!impl_) throw CameraNotFound("empty Camera");
    return impl_->start();
}

} // namespace hscam
