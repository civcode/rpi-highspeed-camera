#include "sensor_subdevice.hpp"
#include "hscam/error.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

#include <linux/v4l2-subdev.h>

namespace hscam::media {
namespace {

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
    return value;
}

std::string readLine(const std::filesystem::path &path)
{
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return line;
}

Rect fromV4l2(const v4l2_rect &r)
{
    return {r.left, r.top, static_cast<std::uint32_t>(r.width), static_cast<std::uint32_t>(r.height)};
}

v4l2_rect toV4l2(Rect r)
{
    return {r.x, r.y, r.width, r.height};
}

std::optional<Rect> getSelection(int fd, unsigned pad, std::uint32_t target, std::uint32_t which)
{
    v4l2_subdev_selection selection{};
    selection.which = which;
    selection.pad = pad;
    selection.target = target;
    if (::ioctl(fd, VIDIOC_SUBDEV_G_SELECTION, &selection) < 0)
        return std::nullopt;
    return fromV4l2(selection.r);
}

struct Candidate {
    std::string node;
    std::string name;
    int score{};
};

std::vector<Candidate> candidatesFromSystemDevices(const std::string &model,
                                                    const std::vector<std::int64_t> &devices)
{
    std::vector<Candidate> out;
    const std::string wanted = lower(model);
    for (const auto raw : devices) {
        const auto dev = static_cast<dev_t>(raw);
        std::filesystem::path sys = "/sys/dev/char/" + std::to_string(major(dev)) + ":" + std::to_string(minor(dev));
        std::error_code ec;
        auto resolved = std::filesystem::canonical(sys, ec);
        if (ec)
            continue;
        const std::string base = resolved.filename().string();
        if (!base.starts_with("v4l-subdev"))
            continue;
        const std::string name = readLine(std::filesystem::path("/sys/class/video4linux") / base / "name");
        int score = 1;
        if (!wanted.empty() && lower(name).find(wanted) != std::string::npos)
            score = 100;
        out.push_back({"/dev/" + base, name, score});
    }
    std::sort(out.begin(), out.end(), [](const Candidate &a, const Candidate &b) { return a.score > b.score; });
    return out;
}

} // namespace

SensorSubdevice::SensorSubdevice(int fd, SensorSubdeviceInfo info) : fd_(fd), info_(std::move(info)) {}
SensorSubdevice::~SensorSubdevice() { if (fd_ >= 0) ::close(fd_); }
SensorSubdevice::SensorSubdevice(SensorSubdevice &&other) noexcept
    : fd_(std::exchange(other.fd_, -1)), info_(std::move(other.info_)) {}
SensorSubdevice &SensorSubdevice::operator=(SensorSubdevice &&other) noexcept
{
    if (this != &other) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = std::exchange(other.fd_, -1);
        info_ = std::move(other.info_);
    }
    return *this;
}

std::optional<SensorSubdevice> SensorSubdevice::discover(const std::string &sensorModel,
                                                         const std::vector<std::int64_t> &systemDevices)
{
    for (const auto &candidate : candidatesFromSystemDevices(sensorModel, systemDevices)) {
        const int fd = ::open(candidate.node.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;

        for (unsigned pad = 0; pad < 8; ++pad) {
            auto current = getSelection(fd, pad, V4L2_SEL_TGT_CROP, V4L2_SUBDEV_FORMAT_ACTIVE);
            auto bounds = getSelection(fd, pad, V4L2_SEL_TGT_CROP_BOUNDS, V4L2_SUBDEV_FORMAT_ACTIVE);
            auto native = getSelection(fd, pad, V4L2_SEL_TGT_NATIVE_SIZE, V4L2_SUBDEV_FORMAT_ACTIVE);
            auto def = getSelection(fd, pad, V4L2_SEL_TGT_CROP_DEFAULT, V4L2_SUBDEV_FORMAT_ACTIVE);
            if (!current && !bounds && !native)
                continue;

            SensorSubdeviceInfo info;
            info.deviceNode = candidate.node;
            info.name = candidate.name;
            info.pad = pad;
            info.currentCrop = current;
            info.capabilities.sensorCropQueryable = current.has_value();
            info.capabilities.sensorCropBounds = bounds;
            info.capabilities.sensorNativeSize = native;
            info.capabilities.sensorDefaultCrop = def;

            if (current) {
                v4l2_subdev_selection sel{};
                sel.which = V4L2_SUBDEV_FORMAT_TRY;
                sel.pad = pad;
                sel.target = V4L2_SEL_TGT_CROP;
                sel.r = toV4l2(*current);
                info.capabilities.sensorCropTryable = ::ioctl(fd, VIDIOC_SUBDEV_S_SELECTION, &sel) == 0;

                sel = {};
                sel.which = V4L2_SUBDEV_FORMAT_ACTIVE;
                sel.pad = pad;
                sel.target = V4L2_SEL_TGT_CROP;
                sel.r = toV4l2(*current);
                info.capabilities.sensorCropSettable = ::ioctl(fd, VIDIOC_SUBDEV_S_SELECTION, &sel) == 0;
                info.capabilities.sensorCropExact = info.capabilities.sensorCropSettable && fromV4l2(sel.r) == *current;
            }
            return SensorSubdevice(fd, std::move(info));
        }
        ::close(fd);
    }
    return std::nullopt;
}

std::optional<Rect> SensorSubdevice::currentCrop() const
{
    if (fd_ < 0)
        return std::nullopt;
    return getSelection(fd_, info_.pad, V4L2_SEL_TGT_CROP, V4L2_SUBDEV_FORMAT_ACTIVE);
}

CropProbe SensorSubdevice::setSelection(Rect requested, std::uint32_t which) const
{
    if (fd_ < 0)
        throw Unsupported("sensor subdevice is not open");
    v4l2_subdev_selection selection{};
    selection.which = which;
    selection.pad = info_.pad;
    selection.target = V4L2_SEL_TGT_CROP;
    selection.r = toV4l2(requested);
    if (::ioctl(fd_, VIDIOC_SUBDEV_S_SELECTION, &selection) < 0)
        throw Unsupported("VIDIOC_SUBDEV_S_SELECTION failed for " + info_.deviceNode + ": " + std::strerror(errno));
    const Rect negotiated = fromV4l2(selection.r);
    return {requested, negotiated, requested == negotiated};
}

CropProbe SensorSubdevice::tryCrop(Rect requested) const
{
    setFormatSize(requested.size(), V4L2_SUBDEV_FORMAT_TRY);
    return setSelection(requested, V4L2_SUBDEV_FORMAT_TRY);
}

CropProbe SensorSubdevice::setCrop(Rect requested)
{
    auto result = setSelection(requested, V4L2_SUBDEV_FORMAT_ACTIVE);
    info_.currentCrop = result.negotiated;
    return result;
}

void SensorSubdevice::setFormatSize(Size size, std::uint32_t which) const
{
    if (fd_ < 0)
        throw Unsupported("sensor subdevice is not open");
    v4l2_subdev_format format{};
    format.which = which;
    format.pad = info_.pad;
    if (::ioctl(fd_, VIDIOC_SUBDEV_G_FMT, &format) < 0)
        throw Unsupported("VIDIOC_SUBDEV_G_FMT failed for " + info_.deviceNode + ": " + std::strerror(errno));
    format.format.width = size.width;
    format.format.height = size.height;
    if (::ioctl(fd_, VIDIOC_SUBDEV_S_FMT, &format) < 0)
        throw Unsupported("VIDIOC_SUBDEV_S_FMT failed for " + info_.deviceNode + ": " + std::strerror(errno));
}

void SensorSubdevice::setFormatSize(Size size)
{
    setFormatSize(size, V4L2_SUBDEV_FORMAT_ACTIVE);
}

} // namespace hscam::media
