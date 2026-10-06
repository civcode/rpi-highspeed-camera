#include "hscam/camera_info.hpp"
#include "hscam/geometry.hpp"

#include <cassert>
#include <iostream>

int main()
{
    using namespace hscam;
    static_assert(Size{2, 3}.area() == 6);
    static_assert(Rect{1, 2, 3, 4}.right() == 4);
    static_assert(Rect{1, 2, 3, 4}.bottom() == 6);

    SensorMode mode;
    mode.size = {1456, 96};
    mode.format = {"SBGGR10_CSI2P"};
    mode.bitDepth = 10;
    const auto a = stableModeId(mode);
    const auto b = stableModeId(mode);
    if (a.empty() || a != b)
        return 1;

    mode.size.height = 88;
    if (a == stableModeId(mode))
        return 1;

    std::cout << "hscam unit tests passed\n";
    return 0;
}
