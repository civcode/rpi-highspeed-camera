#include "hscam/raw/render.hpp"
#include "hscam/error.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hscam::raw {
namespace {

std::string upper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

unsigned digitsAfter(const std::string &s, const std::string &token)
{
    const auto pos0 = s.find(token);
    if (pos0 == std::string::npos) return 0;
    std::size_t pos = pos0 + token.size();
    unsigned value{};
    bool any = false;
    while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
        any = true;
        value = value * 10 + static_cast<unsigned>(s[pos++] - '0');
    }
    return any ? value : 0;
}

std::size_t packedRowBytes(std::uint32_t width, unsigned bits)
{
    if (bits == 10)
        return static_cast<std::size_t>(width) + (width + 3U) / 4U;
    if (bits == 12)
        return static_cast<std::size_t>(width) + (width + 1U) / 2U;
    return (static_cast<std::size_t>(width) * bits + 7U) / 8U;
}

std::uint16_t readLe16(const std::byte *p)
{
    return static_cast<std::uint16_t>(std::to_integer<unsigned char>(p[0])) |
           (static_cast<std::uint16_t>(std::to_integer<unsigned char>(p[1])) << 8);
}

void unpack10(const std::byte *src, std::size_t bytes, std::uint16_t *dst, std::size_t pixels)
{
    std::size_t x = 0, off = 0;
    while (x < pixels) {
        const unsigned count = static_cast<unsigned>(std::min<std::size_t>(4, pixels - x));
        if (off + count + 1 > bytes) throw Error("truncated CSI-2 RAW10 row");
        const unsigned low = std::to_integer<unsigned char>(src[off + count]);
        for (unsigned i = 0; i < count; ++i) {
            const unsigned high = std::to_integer<unsigned char>(src[off + i]);
            dst[x + i] = static_cast<std::uint16_t>((high << 2) | ((low >> (2 * i)) & 0x03));
        }
        x += count;
        off += count + 1;
    }
}

void unpack12(const std::byte *src, std::size_t bytes, std::uint16_t *dst, std::size_t pixels)
{
    std::size_t x = 0, off = 0;
    while (x < pixels) {
        const unsigned count = static_cast<unsigned>(std::min<std::size_t>(2, pixels - x));
        if (off + count + 1 > bytes) throw Error("truncated CSI-2 RAW12 row");
        const unsigned low = std::to_integer<unsigned char>(src[off + count]);
        const unsigned high0 = std::to_integer<unsigned char>(src[off]);
        dst[x] = static_cast<std::uint16_t>((high0 << 4) | (low & 0x0f));
        if (count == 2) {
            const unsigned high1 = std::to_integer<unsigned char>(src[off + 1]);
            dst[x + 1] = static_cast<std::uint16_t>((high1 << 4) | ((low >> 4) & 0x0f));
        }
        x += count;
        off += count + 1;
    }
}

enum class Colour { R, G, B };
Colour colourAt(unsigned x, unsigned y, BayerOrder order)
{
    const bool xe = (x & 1U) == 0, ye = (y & 1U) == 0;
    switch (order) {
    case BayerOrder::RGGB: return ye ? (xe ? Colour::R : Colour::G) : (xe ? Colour::G : Colour::B);
    case BayerOrder::GRBG: return ye ? (xe ? Colour::G : Colour::R) : (xe ? Colour::B : Colour::G);
    case BayerOrder::GBRG: return ye ? (xe ? Colour::G : Colour::B) : (xe ? Colour::R : Colour::G);
    case BayerOrder::BGGR: return ye ? (xe ? Colour::B : Colour::G) : (xe ? Colour::G : Colour::R);
    case BayerOrder::None: return Colour::G;
    }
    return Colour::G;
}

std::uint16_t interpolate(const RawFrame16 &raw, int x, int y, Colour wanted)
{
    std::uint32_t sum{};
    unsigned count{};
    for (int dy = -1; dy <= 1; ++dy) {
        const int yy = y + dy;
        if (yy < 0 || yy >= static_cast<int>(raw.size.height)) continue;
        for (int dx = -1; dx <= 1; ++dx) {
            const int xx = x + dx;
            if (xx < 0 || xx >= static_cast<int>(raw.size.width)) continue;
            if (colourAt(static_cast<unsigned>(xx), static_cast<unsigned>(yy), raw.bayer) == wanted) {
                sum += raw.pixels[static_cast<std::size_t>(yy) * raw.size.width + static_cast<unsigned>(xx)];
                ++count;
            }
        }
    }
    return count ? static_cast<std::uint16_t>((sum + count / 2) / count)
                 : raw.pixels[static_cast<std::size_t>(y) * raw.size.width + static_cast<unsigned>(x)];
}

std::uint8_t to8(std::uint16_t value, unsigned bits)
{
    const unsigned max = bits >= 16 ? 65535U : ((1U << bits) - 1U);
    return static_cast<std::uint8_t>((static_cast<std::uint32_t>(value) * 255U + max / 2U) / max);
}

std::size_t planeOffset(const BundleManifest &manifest, std::size_t plane)
{
    std::size_t offset{};
    for (std::size_t i = 0; i < plane; ++i) offset += manifest.observedPlanes.at(i).size;
    return offset;
}

std::uint8_t clamp8(int value) { return static_cast<std::uint8_t>(std::clamp(value, 0, 255)); }

std::uint32_t crc32(std::span<const std::uint8_t> data)
{
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : data) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return crc ^ 0xffffffffU;
}

std::uint32_t adler32(std::span<const std::uint8_t> data)
{
    std::uint32_t a = 1, b = 0;
    for (const auto byte : data) {
        a = (a + byte) % 65521U;
        b = (b + a) % 65521U;
    }
    return (b << 16) | a;
}

void writeBe32(std::ostream &out, std::uint32_t value)
{
    const std::array<char, 4> bytes{
        static_cast<char>((value >> 24) & 0xff),
        static_cast<char>((value >> 16) & 0xff),
        static_cast<char>((value >> 8) & 0xff),
        static_cast<char>(value & 0xff)
    };
    out.write(bytes.data(), bytes.size());
}

void writePngChunk(std::ostream &out, std::string_view type, std::span<const std::uint8_t> data)
{
    writeBe32(out, static_cast<std::uint32_t>(data.size()));
    out.write(type.data(), static_cast<std::streamsize>(type.size()));
    out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));

    std::vector<std::uint8_t> crcInput(type.begin(), type.end());
    crcInput.insert(crcInput.end(), data.begin(), data.end());
    writeBe32(out, crc32(crcInput));
}

RenderedImage renderYuv420(const BundleManifest &manifest, std::span<const std::byte> payload)
{
    const auto w = manifest.configuration.stream.size.width;
    const auto h = manifest.configuration.stream.size.height;
    if (!w || !h) throw Error("invalid YUV420 dimensions");

    const std::size_t yStride =
        manifest.observedPlanes.empty() || !manifest.observedPlanes[0].stride
            ? w
            : manifest.observedPlanes[0].stride;
    const std::size_t inferredChromaStride = (yStride + 1) / 2;
    std::size_t uStride = inferredChromaStride;
    std::size_t vStride = inferredChromaStride;
    std::size_t yOff = 0;
    std::size_t uOff = yStride * h;
    std::size_t vOff = uOff + uStride * ((h + 1) / 2);

    if (manifest.observedPlanes.size() >= 3) {
        uStride = manifest.observedPlanes[1].stride
                      ? manifest.observedPlanes[1].stride
                      : inferredChromaStride;
        vStride = manifest.observedPlanes[2].stride
                      ? manifest.observedPlanes[2].stride
                      : inferredChromaStride;
        uOff = planeOffset(manifest, 1);
        vOff = planeOffset(manifest, 2);
    }

    const std::size_t needed = manifest.observedPlanes.size() >= 3
                                   ? vOff + manifest.observedPlanes[2].size
                                   : vOff + vStride * ((h + 1) / 2);
    if (payload.size() < needed) throw Error("truncated YUV420 payload");

    RenderedImage out{{w, h}, 3, std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 3)};
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const int Y = std::to_integer<unsigned char>(payload[yOff + static_cast<std::size_t>(y) * yStride + x]);
            const int U = std::to_integer<unsigned char>(payload[uOff + static_cast<std::size_t>(y / 2) * uStride + x / 2]);
            const int V = std::to_integer<unsigned char>(payload[vOff + static_cast<std::size_t>(y / 2) * vStride + x / 2]);
            const int c = std::max(0, Y - 16), d = U - 128, e = V - 128;
            const auto i = (static_cast<std::size_t>(y) * w + x) * 3;
            out.pixels[i + 0] = clamp8((298 * c + 409 * e + 128) >> 8);
            out.pixels[i + 1] = clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
            out.pixels[i + 2] = clamp8((298 * c + 516 * d + 128) >> 8);
        }
    }
    return out;
}

} // namespace

RawLayout describePixelFormat(const std::string &pixelFormat)
{
    const std::string s = upper(pixelFormat);
    if (s.find("YUV420") != std::string::npos)
        return {RawKind::Yuv420, BayerOrder::None, 8, false};

    struct Pattern { const char *name; BayerOrder order; };
    for (const auto &p : {Pattern{"SRGGB", BayerOrder::RGGB}, Pattern{"SGRBG", BayerOrder::GRBG},
                          Pattern{"SGBRG", BayerOrder::GBRG}, Pattern{"SBGGR", BayerOrder::BGGR}}) {
        if (s.find(p.name) != std::string::npos) {
            const unsigned bits = digitsAfter(s, p.name);
            if (!bits) throw Error("cannot determine Bayer bit depth from pixel format: " + pixelFormat);
            return {RawKind::Bayer, p.order, bits, s.find("CSI2P") != std::string::npos};
        }
    }

    if (!s.empty() && (s[0] == 'R' || s[0] == 'Y')) {
        unsigned bits{};
        std::size_t pos = 1;
        while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
            bits = bits * 10 + static_cast<unsigned>(s[pos++] - '0');
        }
        if (bits)
            return {RawKind::Mono, BayerOrder::None, bits, s.find("CSI2P") != std::string::npos};
    }
    throw Unsupported("unsupported preview pixel format: " + pixelFormat);
}

RawFrame16 decodeRaw(const BundleManifest &manifest, std::span<const std::byte> payload)
{
    const RawLayout layout = describePixelFormat(manifest.configuration.stream.format.name);
    if (layout.kind == RawKind::Yuv420) throw Unsupported("YUV420 is not a raw Bayer/mono frame");
    const auto w = manifest.configuration.stream.size.width;
    const auto h = manifest.configuration.stream.size.height;
    if (!w || !h) throw Error("invalid raw dimensions");
    if (manifest.observedPlanes.empty()) throw Error("HSCAP has no plane metadata");
    const std::size_t stride = manifest.observedPlanes[0].stride ? manifest.observedPlanes[0].stride
                                                                 : (layout.csi2Packed ? packedRowBytes(w, layout.bitDepth)
                                                                                      : (layout.bitDepth <= 8 ? w : static_cast<std::size_t>(w) * 2));
    if (payload.size() < stride * h) throw Error("truncated raw payload");

    RawFrame16 out{{w, h}, layout.bitDepth, layout.bayer, std::vector<std::uint16_t>(static_cast<std::size_t>(w) * h)};
    const unsigned mask = layout.bitDepth >= 16 ? 0xffffU : ((1U << layout.bitDepth) - 1U);
    for (std::uint32_t y = 0; y < h; ++y) {
        const auto *row = payload.data() + static_cast<std::size_t>(y) * stride;
        auto *dst = out.pixels.data() + static_cast<std::size_t>(y) * w;
        if (layout.csi2Packed && layout.bitDepth == 10) unpack10(row, stride, dst, w);
        else if (layout.csi2Packed && layout.bitDepth == 12) unpack12(row, stride, dst, w);
        else if (!layout.csi2Packed && layout.bitDepth <= 8) {
            if (stride < w) throw Error("raw stride shorter than row");
            for (std::uint32_t x = 0; x < w; ++x) dst[x] = std::to_integer<unsigned char>(row[x]) & mask;
        } else if (!layout.csi2Packed && layout.bitDepth <= 16) {
            if (stride < static_cast<std::size_t>(w) * 2) throw Error("unpacked raw stride shorter than 16-bit row");
            for (std::uint32_t x = 0; x < w; ++x) dst[x] = readLe16(row + x * 2) & mask;
        } else {
            throw Unsupported("unsupported raw packing");
        }
    }
    return out;
}

RenderedImage renderPreview(const BundleManifest &manifest, std::span<const std::byte> payload)
{
    const RawLayout layout = describePixelFormat(manifest.configuration.stream.format.name);
    if (layout.kind == RawKind::Yuv420) return renderYuv420(manifest, payload);

    const RawFrame16 raw = decodeRaw(manifest, payload);
    RenderedImage out{raw.size, 3, std::vector<std::uint8_t>(static_cast<std::size_t>(raw.size.width) * raw.size.height * 3)};
    for (std::uint32_t y = 0; y < raw.size.height; ++y) {
        for (std::uint32_t x = 0; x < raw.size.width; ++x) {
            std::uint16_t r{}, g{}, b{};
            if (layout.kind == RawKind::Mono) {
                r = g = b = raw.pixels[static_cast<std::size_t>(y) * raw.size.width + x];
            } else {
                const Colour own = colourAt(x, y, raw.bayer);
                const auto center = raw.pixels[static_cast<std::size_t>(y) * raw.size.width + x];
                r = own == Colour::R ? center : interpolate(raw, x, y, Colour::R);
                g = own == Colour::G ? center : interpolate(raw, x, y, Colour::G);
                b = own == Colour::B ? center : interpolate(raw, x, y, Colour::B);
            }
            const auto i = (static_cast<std::size_t>(y) * raw.size.width + x) * 3;
            out.pixels[i + 0] = to8(r, raw.bitDepth);
            out.pixels[i + 1] = to8(g, raw.bitDepth);
            out.pixels[i + 2] = to8(b, raw.bitDepth);
        }
    }
    return out;
}

void writePng(const std::filesystem::path &path, const RenderedImage &image)
{
    if (!image.size.width || !image.size.height)
        throw Error("cannot write empty PNG");
    if (image.channels != 3)
        throw Unsupported("PNG writer currently requires RGB8 input");
    const std::size_t expected = static_cast<std::size_t>(image.size.width) * image.size.height * 3;
    if (image.pixels.size() != expected)
        throw Error("RGB pixel buffer size does not match image dimensions");

    std::vector<std::uint8_t> filtered;
    filtered.reserve((static_cast<std::size_t>(image.size.width) * 3 + 1) * image.size.height);
    const std::size_t rowBytes = static_cast<std::size_t>(image.size.width) * 3;
    for (std::uint32_t y = 0; y < image.size.height; ++y) {
        filtered.push_back(0);
        const auto begin = image.pixels.begin() + static_cast<std::ptrdiff_t>(y * rowBytes);
        filtered.insert(filtered.end(), begin, begin + static_cast<std::ptrdiff_t>(rowBytes));
    }

    std::vector<std::uint8_t> zlib;
    zlib.reserve(filtered.size() + filtered.size() / 65535 * 5 + 16);
    zlib.push_back(0x78);
    zlib.push_back(0x01);
    std::size_t offset{};
    while (offset < filtered.size()) {
        const std::size_t count = std::min<std::size_t>(65535, filtered.size() - offset);
        const bool final = offset + count == filtered.size();
        zlib.push_back(final ? 0x01 : 0x00);
        const auto len = static_cast<std::uint16_t>(count);
        const auto nlen = static_cast<std::uint16_t>(~len);
        zlib.push_back(static_cast<std::uint8_t>(len & 0xff));
        zlib.push_back(static_cast<std::uint8_t>((len >> 8) & 0xff));
        zlib.push_back(static_cast<std::uint8_t>(nlen & 0xff));
        zlib.push_back(static_cast<std::uint8_t>((nlen >> 8) & 0xff));
        zlib.insert(zlib.end(), filtered.begin() + static_cast<std::ptrdiff_t>(offset),
                    filtered.begin() + static_cast<std::ptrdiff_t>(offset + count));
        offset += count;
    }
    const auto adler = adler32(filtered);
    zlib.push_back(static_cast<std::uint8_t>((adler >> 24) & 0xff));
    zlib.push_back(static_cast<std::uint8_t>((adler >> 16) & 0xff));
    zlib.push_back(static_cast<std::uint8_t>((adler >> 8) & 0xff));
    zlib.push_back(static_cast<std::uint8_t>(adler & 0xff));

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw Error("failed to open PNG output: " + path.string());
    static constexpr std::array<std::uint8_t, 8> signature{137,80,78,71,13,10,26,10};
    out.write(reinterpret_cast<const char *>(signature.data()), signature.size());

    const std::array<std::uint8_t, 13> ihdr{
        static_cast<std::uint8_t>((image.size.width >> 24) & 0xff),
        static_cast<std::uint8_t>((image.size.width >> 16) & 0xff),
        static_cast<std::uint8_t>((image.size.width >> 8) & 0xff),
        static_cast<std::uint8_t>(image.size.width & 0xff),
        static_cast<std::uint8_t>((image.size.height >> 24) & 0xff),
        static_cast<std::uint8_t>((image.size.height >> 16) & 0xff),
        static_cast<std::uint8_t>((image.size.height >> 8) & 0xff),
        static_cast<std::uint8_t>(image.size.height & 0xff),
        8, 2, 0, 0, 0
    };
    writePngChunk(out, "IHDR", ihdr);
    writePngChunk(out, "IDAT", zlib);
    writePngChunk(out, "IEND", {});
    if (!out) throw Error("failed writing PNG output: " + path.string());
}

} // namespace hscam::raw
