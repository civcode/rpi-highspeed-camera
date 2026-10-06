#pragma once

#include <cstdint>
#include <ostream>

namespace hscam {

struct Size {
    std::uint32_t width{};
    std::uint32_t height{};

    [[nodiscard]] constexpr bool empty() const noexcept { return width == 0 || height == 0; }
    [[nodiscard]] constexpr std::uint64_t area() const noexcept {
        return static_cast<std::uint64_t>(width) * height;
    }
    friend constexpr bool operator==(const Size &, const Size &) = default;
};

struct Rect {
    std::int32_t x{};
    std::int32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};

    [[nodiscard]] constexpr bool empty() const noexcept { return width == 0 || height == 0; }
    [[nodiscard]] constexpr Size size() const noexcept { return {width, height}; }
    [[nodiscard]] constexpr std::int64_t right() const noexcept {
        return static_cast<std::int64_t>(x) + width;
    }
    [[nodiscard]] constexpr std::int64_t bottom() const noexcept {
        return static_cast<std::int64_t>(y) + height;
    }
    friend constexpr bool operator==(const Rect &, const Rect &) = default;
};

std::ostream &operator<<(std::ostream &os, const Size &size);
std::ostream &operator<<(std::ostream &os, const Rect &rect);

} // namespace hscam
