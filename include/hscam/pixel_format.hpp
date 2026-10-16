#pragma once

#include <string>

namespace hscam {

struct PixelFormat {
    std::string name;

    [[nodiscard]] bool empty() const noexcept { return name.empty(); }
    friend bool operator==(const PixelFormat &, const PixelFormat &) = default;
};

// Encoding class of a raw stream pixel format name.
enum class RawEncoding {
    Unknown,         // not a recognized raw encoding (processed formats, ...)
    UnpackedBayer,   // uncompressed Bayer container (e.g. SGBRG16)
    Csi2PackedBayer, // CSI-2 packed Bayer (e.g. SGBRG10_CSI2P)
    PispComp1        // PiSP COMP1 compression (e.g. GBRG_PISP_COMP1)
};

[[nodiscard]] RawEncoding classifyRawEncoding(const std::string &formatName);

// Bit depth encoded in a Bayer pixel format name (0 when the name carries none).
[[nodiscard]] unsigned bayerBitDepth(const std::string &formatName);

// Unpacked Bayer format name for a sensor bit depth; throws hscam::NegotiationFailed
// for bit depths without an unpacked Bayer format (no silent fallback).
std::string unpackedBayerFormatName(unsigned bits);

} // namespace hscam
