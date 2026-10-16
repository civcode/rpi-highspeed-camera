#pragma once

#include "hscam/camera_info.hpp"

#include <string>
#include <vector>

namespace hscam {

// Discovery is *not* proof of a working high-FPS sensor configuration.
// Experimental register profiles and measured FPS are validated separately.
enum class HighspeedStrategy {
    NativeSensorRoi,
    ExperimentalModesAdvertised,
    DriverExtensionRequired,
    Uncharacterized
};

struct ExperimentalReadout {
    Size size;
    std::string origin; // e.g. "raspiraw historical profile"; not evidence
};

struct HighspeedAssessment {
    std::string sensor;
    bool officialSensor{};
    HighspeedStrategy strategy{HighspeedStrategy::Uncharacterized};
    bool nativeRoiAdvertised{};
    bool referenceModesAdvertised{};
    bool requiresDriverExtension{};
    bool experimentallyValidated{}; // always false until actual evidence is checked
    std::vector<ExperimentalReadout> referenceReadouts;
};

// Sensor-specific references are candidates only. Never program registers
// or claim hardware FPS from the sensor name.
HighspeedAssessment assessHighspeed(const CameraInfo &camera);

const char *highspeedStrategyName(HighspeedStrategy strategy) noexcept;

// Exact match against libcamera-advertised sensor readouts. This deliberately
// excludes merely scaled output and avoids nearest-mode negotiation.
std::vector<SensorMode> matchExperimentalReadouts(
    const CameraInfo &camera, const std::vector<Size> &requested);

} // namespace hscam
