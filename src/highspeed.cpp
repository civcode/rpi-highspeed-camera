#include "hscam/highspeed.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace hscam {
namespace {

std::string normalized(std::string_view text)
{
    std::string result;
    result.reserve(text.size());
    for (const unsigned char ch : text)
        result.push_back(static_cast<char>(std::tolower(ch)));
    return result;
}

std::string knownSensor(const CameraInfo &camera)
{
    const std::string label = normalized(
        camera.sensorModel ? *camera.sensorModel : camera.model);
    for (const auto name : {"ov5647", "imx219", "imx708",
                            "imx477", "imx296", "imx500"}) {
        if (label.find(name) != std::string::npos)
            return name;
    }
    // Don't claim that an unknown generic sensor supports a profile.
    return camera.sensorModel.value_or(camera.model);
}

} // namespace

const char *highspeedStrategyName(HighspeedStrategy strategy) noexcept
{
    switch (strategy) {
    case HighspeedStrategy::NativeSensorRoi: return "native_sensor_roi";
    case HighspeedStrategy::ExperimentalModesAdvertised: return "experimental_modes_advertised";
    case HighspeedStrategy::DriverExtensionRequired: return "driver_extension_required";
    case HighspeedStrategy::Uncharacterized: return "uncharacterized";
    }
    return "uncharacterized";
}

HighspeedAssessment assessHighspeed(const CameraInfo &camera)
{
    HighspeedAssessment result;
    result.sensor = knownSensor(camera);
    const auto sensor = normalized(result.sensor);
    result.officialSensor = sensor == "ov5647" || sensor == "imx219" ||
                            sensor == "imx708" || sensor == "imx477" ||
                            sensor == "imx296" || sensor == "imx500";

    result.nativeRoiAdvertised =
        camera.capabilities.sensorCropTryable &&
        camera.capabilities.sensorCropSettable;

    // Historical/reference sizes, not automatically selected kernel modes.
    // No FPS value is embedded: every claim requires an actual capture trace.
    if (sensor == "ov5647") {
        for (const auto height : {240U, 128U, 64U, 32U})
            result.referenceReadouts.push_back({{640U, height}, "raspiraw historical profile"});
    } else if (sensor == "imx296") {
        result.referenceReadouts.push_back({{1456U, 96U}, "GScrop historical profile"});
        result.referenceReadouts.push_back({{688U, 136U}, "GScrop historical profile"});
    }

    std::vector<Size> candidateSizes;
    for (const auto &candidate : result.referenceReadouts)
        candidateSizes.push_back(candidate.size);
    result.referenceModesAdvertised =
        !candidateSizes.empty() &&
        !matchExperimentalReadouts(camera, candidateSizes).empty();

    if (result.nativeRoiAdvertised) {
        result.strategy = HighspeedStrategy::NativeSensorRoi;
    } else if (result.referenceModesAdvertised) {
        // Sensor-sized mode exists; still not independent proof of a
        // custom driver, correct CSI readout or high FPS.
        result.strategy = HighspeedStrategy::ExperimentalModesAdvertised;
    } else if (result.officialSensor) {
        result.strategy = HighspeedStrategy::DriverExtensionRequired;
        result.requiresDriverExtension = true;
    }
    return result;
}

std::vector<SensorMode> matchExperimentalReadouts(
    const CameraInfo &camera, const std::vector<Size> &requested)
{
    std::vector<SensorMode> matches;
    for (const auto &mode : camera.sensorModes) {
        if (std::find(requested.begin(), requested.end(), mode.size) != requested.end())
            matches.push_back(mode);
    }
    return matches;
}

} // namespace hscam
