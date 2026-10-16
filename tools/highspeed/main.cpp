#include "hscam/context.hpp"
#include "hscam/highspeed.hpp"
#include "internal/json.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using hscam::internal::json::Value;

Value toJson(const hscam::CameraInfo &camera)
{
    const auto assessment = hscam::assessHighspeed(camera);
    Value::Array targets;
    for (const auto &readout : assessment.referenceReadouts) {
        targets.emplace_back(Value::Object{
            {"width", readout.size.width},
            {"height", readout.size.height},
            {"origin", readout.origin},
            {"measured_fps", nullptr},
            {"validated", false}
        });
    }
    return Value::Object{
        {"camera_id", camera.id},
        {"sensor", assessment.sensor},
        {"official_sensor", assessment.officialSensor},
        {"strategy", hscam::highspeedStrategyName(assessment.strategy)},
        {"native_roi_advertised", assessment.nativeRoiAdvertised},
        {"reference_modes_advertised", assessment.referenceModesAdvertised},
        {"requires_driver_extension", assessment.requiresDriverExtension},
        {"experimentally_validated", false},
        {"reference_readouts", std::move(targets)}
    };
}
}

int main(int argc, char **argv)
{
    bool json = false;
    std::string filter;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--json") json = true;
        else if (arg == "--camera" && i + 1 < argc) filter = argv[++i];
        else {
            std::cerr << "usage: hscam-highspeed [--json] [--camera CAMERA_ID]\n";
            return 2;
        }
    }

    try {
        hscam::Context context;
        const bool available = context.cameraBackendAvailable();
        const auto cameras = available ? context.cameras()
                                       : std::vector<hscam::CameraInfo>{};
        Value::Array assessments;
        for (const auto &camera : cameras) {
            if (filter.empty() || filter == camera.id)
                assessments.push_back(toJson(camera));
        }
        if (!filter.empty() && assessments.empty()) {
            std::cerr << "camera not detected: " << filter << '\n';
            return 2;
        }

        if (json) {
            std::cout << hscam::internal::json::stringify(Value::Object{
                {"schema_version", 1},
                {"kind", "highspeed_discovery_dry_run"},
                {"camera_backend_available", available},
                {"camera_assessments", std::move(assessments)}
            }, 2) << '\n';
        } else {
            std::cout << "High-FPS experimental strategy discovery (read-only)\n";
            if (!available)
                std::cout << "libcamera unavailable; no cameras examined\n";
            for (const auto &camera : cameras) {
                if (!filter.empty() && filter != camera.id) continue;
                const auto a = hscam::assessHighspeed(camera);
                std::cout << camera.id << " (" << a.sensor << ")\n";
                std::cout << "  strategy: " << hscam::highspeedStrategyName(a.strategy) << '\n';
                std::cout << "  mutable sensor ROI exposed: "
                          << (a.nativeRoiAdvertised ? "yes" : "no") << '\n';
                std::cout << "  experimentally validated: no (requires hardware proof)\n";
                for (const auto &target : a.referenceReadouts)
                    std::cout << "  candidate " << target.size.width << 'x'
                              << target.size.height << " — " << target.origin
                              << " (not validated)\n";
            }
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "hscam-highspeed: " << e.what() << '\n';
        return 1;
    }
}
