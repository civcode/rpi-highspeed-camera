#include "hscam/context.hpp"
#include "hscam/version.hpp"

#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string jsonEscape(std::string_view input)
{
    std::ostringstream out;
    for (const unsigned char ch : input) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<unsigned>(ch) << std::dec;
            } else {
                out << static_cast<char>(ch);
            }
        }
    }
    return out.str();
}

void printString(std::ostream &out, std::string_view value)
{
    out << '"' << jsonEscape(value) << '"';
}

void printOptionalString(std::ostream &out, const std::optional<std::string> &value)
{
    if (value) printString(out, *value);
    else out << "null";
}

void printRect(std::ostream &out, const hscam::Rect &rect)
{
    out << "[" << rect.x << "," << rect.y << "," << rect.width << "," << rect.height << "]";
}

void printOptionalRect(std::ostream &out, const std::optional<hscam::Rect> &rect)
{
    if (rect) printRect(out, *rect);
    else out << "null";
}

void printJson(const std::vector<hscam::CameraInfo> &cameras)
{
    std::cout << "{\"schema_version\":1,\"hscam_version\":";
    printString(std::cout, HSCAM_VERSION_STRING);
    std::cout << ",\"cameras\":[";

    for (std::size_t i = 0; i < cameras.size(); ++i) {
        const auto &camera = cameras[i];
        if (i) std::cout << ',';

        std::cout << "{\"id\":";
        printString(std::cout, camera.id);
        std::cout << ",\"model\":";
        printString(std::cout, camera.model);
        std::cout << ",\"sensor_model\":";
        printOptionalString(std::cout, camera.sensorModel);
        std::cout << ",\"sensor_subdevice\":";
        printOptionalString(std::cout, camera.sensorSubdevice);
        std::cout << ",\"pixel_array\":[" << camera.pixelArray.width << ","
                  << camera.pixelArray.height << "]";

        std::cout << ",\"active_areas\":[";
        for (std::size_t j = 0; j < camera.activeAreas.size(); ++j) {
            if (j) std::cout << ',';
            printRect(std::cout, camera.activeAreas[j]);
        }
        std::cout << "]";

        const auto &caps = camera.capabilities;
        std::cout << ",\"capabilities\":{"
                  << "\"raw_capture\":" << (caps.rawCapture ? "true" : "false")
                  << ",\"processed_capture\":" << (caps.processedCapture ? "true" : "false")
                  << ",\"sensor_crop_queryable\":" << (caps.sensorCropQueryable ? "true" : "false")
                  << ",\"sensor_crop_tryable\":" << (caps.sensorCropTryable ? "true" : "false")
                  << ",\"sensor_crop_settable\":" << (caps.sensorCropSettable ? "true" : "false")
                  << ",\"sensor_crop_exact\":" << (caps.sensorCropExact ? "true" : "false")
                  << ",\"frame_duration_control\":" << (caps.frameDurationControl ? "true" : "false")
                  << ",\"exposure_control\":" << (caps.exposureControl ? "true" : "false")
                  << ",\"analogue_gain_control\":" << (caps.analogueGainControl ? "true" : "false")
                  << ",\"sensor_native_size\":";
        printOptionalRect(std::cout, caps.sensorNativeSize);
        std::cout << ",\"sensor_crop_bounds\":";
        printOptionalRect(std::cout, caps.sensorCropBounds);
        std::cout << ",\"sensor_default_crop\":";
        printOptionalRect(std::cout, caps.sensorDefaultCrop);
        std::cout << "}";

        std::cout << ",\"modes\":[";
        for (std::size_t j = 0; j < camera.sensorModes.size(); ++j) {
            const auto &mode = camera.sensorModes[j];
            if (j) std::cout << ',';
            std::cout << "{\"id\":";
            printString(std::cout, mode.id);
            std::cout << ",\"width\":" << mode.size.width
                      << ",\"height\":" << mode.size.height
                      << ",\"format\":";
            printString(std::cout, mode.format.name);
            std::cout << ",\"bit_depth\":" << mode.bitDepth
                      << ",\"sensor_crop\":";
            printOptionalRect(std::cout, mode.sensorCrop);
            std::cout << "}";
        }
        std::cout << "]}";
    }

    std::cout << "]}\n";
}

void printHuman(const std::vector<hscam::CameraInfo> &cameras)
{
    std::cout << "hscam " << HSCAM_VERSION_STRING << "\n";
    std::cout << "cameras: " << cameras.size() << "\n";

    for (const auto &camera : cameras) {
        std::cout << "\n" << camera.model << "\n";
        std::cout << "  id: " << camera.id << "\n";
        if (camera.sensorModel)
            std::cout << "  sensor: " << *camera.sensorModel << "\n";
        if (camera.sensorSubdevice)
            std::cout << "  sensor subdevice: " << *camera.sensorSubdevice << "\n";
        if (!camera.pixelArray.empty())
            std::cout << "  pixel array: " << camera.pixelArray << "\n";

        const auto &caps = camera.capabilities;
        std::cout << "  raw capture: " << (caps.rawCapture ? "yes" : "no") << "\n";
        std::cout << "  processed capture: " << (caps.processedCapture ? "yes" : "no") << "\n";
        std::cout << "  sensor crop: "
                  << (caps.sensorCropSettable ? "settable" :
                      caps.sensorCropQueryable ? "query-only" : "not exposed")
                  << "\n";
        if (caps.sensorCropBounds)
            std::cout << "  sensor crop bounds: " << *caps.sensorCropBounds << "\n";
        if (caps.sensorDefaultCrop)
            std::cout << "  sensor default crop: " << *caps.sensorDefaultCrop << "\n";

        std::cout << "  modes: " << camera.sensorModes.size() << "\n";
        for (const auto &mode : camera.sensorModes) {
            std::cout << "    " << mode.id << "  " << mode.size << "  "
                      << mode.format.name;
            if (mode.bitDepth)
                std::cout << "  " << mode.bitDepth << "-bit";
            std::cout << "\n";
        }
    }
}

void usage()
{
    std::cerr << "usage: hscam-inspect [--json]\n";
}

} // namespace

int main(int argc, char **argv)
{
    bool json = false;
    if (argc == 2 && std::string_view(argv[1]) == "--json")
        json = true;
    else if (argc != 1) {
        usage();
        return 2;
    }

    try {
        hscam::Context context;
        if (!context.cameraBackendAvailable()) {
            std::cerr << "hscam-inspect: built without libcamera support\n";
            return 2;
        }

        const auto cameras = context.cameras();
        if (json) printJson(cameras);
        else printHuman(cameras);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "hscam-inspect: " << e.what() << '\n';
        return 1;
    }
}
