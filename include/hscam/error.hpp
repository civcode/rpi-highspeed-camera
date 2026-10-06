#pragma once

#include <stdexcept>

namespace hscam {

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class CameraNotFound : public Error { using Error::Error; };
class CameraBusy : public Error { using Error::Error; };
class Unsupported : public Error { using Error::Error; };
class NegotiationFailed : public Error { using Error::Error; };
class ExactConfigurationFailed : public NegotiationFailed { using NegotiationFailed::NegotiationFailed; };
class CaptureFailed : public Error { using Error::Error; };
class Timeout : public Error { using Error::Error; };
class DeviceDisconnected : public Error { using Error::Error; };

} // namespace hscam
