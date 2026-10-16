#include "hscam/highspeed.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
void require(bool v, const char *message)
{
    if (!v) throw std::runtime_error(message);
}
}

int main()
{
    try {
        using namespace hscam;

        const std::array<std::string_view, 6> official{
            "ov5647", "imx219", "imx708", "imx477", "imx296", "imx500"};
        for (const auto sensor : official) {
            CameraInfo camera;
            camera.model = std::string(sensor);
            camera.sensorModel = std::string(sensor);
            auto assessment = assessHighspeed(camera);
            require(assessment.officialSensor, "official sensor must be identified");
            require(assessment.strategy == HighspeedStrategy::DriverExtensionRequired,
                    "no crop API must require an experimental driver");
            require(assessment.requiresDriverExtension, "driver requirement not reflected");
            require(!assessment.experimentallyValidated, "metadata is not FPS evidence");
        }

        CameraInfo camera;
        camera.sensorModel = "imx296";
        camera.capabilities.sensorCropSettable = true;
        camera.capabilities.sensorCropTryable = true;
        auto assessment = assessHighspeed(camera);
        require(assessment.strategy == HighspeedStrategy::NativeSensorRoi,
                "IMX296 with mutable ROI uses stock driver");
        require(!assessment.requiresDriverExtension, "no replacement needed for native ROI");
        require(!assessment.experimentallyValidated, "native ROI still needs timed proof");

        camera.capabilities.sensorCropTryable = false;
        assessment = assessHighspeed(camera);
        require(assessment.strategy == HighspeedStrategy::DriverExtensionRequired,
                "settable without TRY is not automatically safe for our crop tester");

        camera = CameraInfo{};
        camera.model = "unrecognized-camera";
        assessment = assessHighspeed(camera);
        require(!assessment.officialSensor, "unknown sensor cannot be declared official");
        require(assessment.strategy == HighspeedStrategy::Uncharacterized,
                "unknown sensor has no hardcoded driver plan");
        require(assessment.referenceReadouts.empty(),
                "unknown sensors cannot inherit arbitrary reference crops");

        camera = CameraInfo{};
        camera.sensorModel = "OV5647";
        SensorMode normal;
        normal.id = "normal";
        normal.size = {640, 480};
        SensorMode experimental;
        experimental.id = "experimental";
        experimental.size = {640, 128};
        SensorMode other;
        other.id = "other";
        other.size = {2592, 1944};
        camera.sensorModes = {normal, experimental, other};
        assessment = assessHighspeed(camera);
        require(assessment.referenceReadouts.size() == 4,
                "OV5647 reference list should include four reduced heights");
        require(assessment.strategy == HighspeedStrategy::ExperimentalModesAdvertised,
                "experimental-size modes are candidates, not driver-needed stock inventory");
        require(assessment.referenceModesAdvertised,
                "reference mode presence should be detected");
        require(!assessment.experimentallyValidated,
                "newly advertised readout must not be mistaken for proof");
        const auto modes = matchExperimentalReadouts(
            camera, {{640, 128}, {640, 64}});
        require(modes.size() == 1 && modes.front().id == "experimental",
                "only exact experimental readout geometry should match");
        require(matchExperimentalReadouts(camera, {{640, 64}}).empty(),
                "missing mode must not fall back to nearest stock mode");
        std::cout << "hscam high-speed strategy tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "highspeed tests: " << e.what() << '\n';
        return 1;
    }
}
