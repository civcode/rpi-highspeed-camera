#include "hscam/camera_info.hpp"
#include "hscam/error.hpp"
#include "hscam/geometry.hpp"
#include "hscam/pixel_format.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
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

namespace {

std::string upperName(const std::string &value)
{
    std::string s = value;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

} // namespace

RawEncoding classifyRawEncoding(const std::string &formatName)
{
    const std::string s = upperName(formatName);
    if (s.find("COMP1") != std::string::npos)
        return RawEncoding::PispComp1;
    if (s.find("CSI2P") != std::string::npos)
        return RawEncoding::Csi2PackedBayer;
    if (bayerBitDepth(s) != 0)
        return RawEncoding::UnpackedBayer;
    return RawEncoding::Unknown;
}

unsigned bayerBitDepth(const std::string &formatName)
{
    const std::string s = upperName(formatName);
    for (const char *prefix : {"SRGGB", "SGRBG", "SGBRG", "SBGGR"}) {
        const std::size_t pos = s.find(prefix);
        if (pos == std::string::npos)
            continue;
        std::size_t i = pos + std::strlen(prefix);
        if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i])))
            continue;
        unsigned bits = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])))
            bits = bits * 10 + static_cast<unsigned>(s[i++] - '0');
        return bits;
    }
    return 0;
}

std::string unpackedBayerFormatName(unsigned bits)
{
    for (unsigned candidate : {8U, 10U, 12U, 14U, 16U})
        if (bits == candidate)
            return "SBGGR" + std::to_string(candidate);
    throw NegotiationFailed("no unpacked Bayer format for a " + std::to_string(bits) +
                            "-bit sensor; supported bit depths are 8, 10, 12, 14 and 16");
}

} // namespace hscam
