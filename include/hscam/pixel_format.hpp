#pragma once

#include <string>

namespace hscam {

struct PixelFormat {
    std::string name;

    [[nodiscard]] bool empty() const noexcept { return name.empty(); }
    friend bool operator==(const PixelFormat &, const PixelFormat &) = default;
};

} // namespace hscam
