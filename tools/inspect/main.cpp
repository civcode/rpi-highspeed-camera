#include "hscam/context.hpp"
#include "hscam/version.hpp"

#include <iostream>
#include <string_view>

namespace {
void printJson(const std::vector<hscam::CameraInfo> &cameras)
{
    std::cout << "{\"schema_version\":1,\"hscam_version\":\"" << HSCAM_VERSION_STRING << "\",\"cameras\":[";
    for (std::size_t i = 0; i < cameras.size(); ++i) {
        const auto &camera = cameras[i];
        if (i) std::cout << ',';
        std::cout << "{\"id\":\"" << camera.id << "\",\"model\":\"" << camera.model << "\",\"modes\":[";
        for (std::size_t j = 0; j < camera.sensorModes.size(); ++j) {
            const auto &mode = camera.sensorModes[j];
            if (j) std::cout << ',';
            std::cout << "{\"id\":\"" << mode.id << "\",\"width\":" << mode.size.width
                      << ",\"height\":" << mode.size.height << ",\"format\":\"" << mode.format.name << "\"}";
        }
        std::cout << "]}";
    }
    std::cout << "]}\n";
}
}

int main(int argc, char **argv)
{
    const bool json = argc > 1 && std::string_view(argv[1]) == "--json";

    try {
        hscam::Context context;
        if (!context.cameraBackendAvailable()) {
            std::cerr << "hscam-inspect: built without libcamera support\n";
            return 2;
        }

        const auto cameras = context.cameras();
        if (json) {
            printJson(cameras);
            return 0;
        }

        std::cout << "hscam " << HSCAM_VERSION_STRING << "\n";
        std::cout << "cameras: " << cameras.size() << "\n";
        for (const auto &camera : cameras) {
            std::cout << "\n" << camera.model << "\n";
            std::cout << "  id: " << camera.id << "\n";
            if (camera.sensorModel)
                std::cout << "  sensor: " << *camera.sensorModel << "\n";
            if (!camera.pixelArray.empty())
                std::cout << "  pixel array: " << camera.pixelArray << "\n";
            std::cout << "  modes: " << camera.sensorModes.size() << "\n";
            for (const auto &mode : camera.sensorModes)
                std::cout << "    " << mode.id << "  " << mode.size << "  " << mode.format.name << "\n";
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "hscam-inspect: " << e.what() << '\n';
        return 1;
    }
}
