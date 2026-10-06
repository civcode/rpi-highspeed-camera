#include "hscam/camera_info.hpp"
#include "hscam/geometry.hpp"

#include <iomanip>
#include <sstream>

namespace hscam {

std::ostream &operator<<(std::ostream &os, const Size &size)
{
    return os << size.width << 'x' << size.height;
}

std::ostream &operator<<(std::ostream &os, const Rect &rect)
{
    return os << rect.x << ',' << rect.y << '+' << rect.width << 'x' << rect.height;
}

std::string stableModeId(const SensorMode &mode)
{
    std::ostringstream canonical;
    canonical << mode.size.width << 'x' << mode.size.height << ':' << mode.format.name << ':' << mode.bitDepth;
    if (mode.sensorCrop)
        canonical << ':' << *mode.sensorCrop;

    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char ch : canonical.str()) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }

    std::ostringstream id;
    id << std::hex << std::setfill('0') << std::setw(16) << hash;
    return id.str();
}

} // namespace hscam
