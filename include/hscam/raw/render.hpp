#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "hscam/capture_bundle.hpp"
#include "hscam/geometry.hpp"

namespace hscam::raw {

enum class BayerOrder { None, RGGB, GRBG, GBRG, BGGR };
enum class RawKind { Bayer, Mono, Yuv420 };

struct RawLayout {
    RawKind kind{RawKind::Bayer};
    BayerOrder bayer{BayerOrder::None};
    unsigned bitDepth{};
    bool csi2Packed{};
};

struct RawFrame16 {
    Size size;
    unsigned bitDepth{};
    BayerOrder bayer{BayerOrder::None};
    std::vector<std::uint16_t> pixels;
};

struct RenderedImage {
    Size size;
    unsigned channels{3};
    std::vector<std::uint8_t> pixels;
};

RawLayout describePixelFormat(const std::string &pixelFormat);
RawFrame16 decodeRaw(const BundleManifest &manifest, std::span<const std::byte> payload);
RenderedImage renderPreview(const BundleManifest &manifest, std::span<const std::byte> payload);

} // namespace hscam::raw
