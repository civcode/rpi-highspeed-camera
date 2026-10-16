#pragma once

#include "hscam/capture_configuration.hpp"
#include "hscam/capture_request.hpp"
#include "hscam/error.hpp"
#include "hscam/pixel_format.hpp"

#include <optional>
#include <string>

namespace hscam::qualification {

// This selection is part of the frozen case plan. It must be used for both
// timing workers and later HSCAP/PNG evidence of the same configuration.
struct RawCaptureSelection {
    RawRepresentation representation{RawRepresentation::Auto};
    std::optional<PixelFormat> format;
    std::optional<SampleAlignment> sampleAlignment;
};

inline const char *representationName(RawRepresentation value)
{
    switch (value) {
    case RawRepresentation::Auto: return "auto";
    case RawRepresentation::Uncompressed: return "uncompressed";
    case RawRepresentation::Compressed: return "compressed";
    }
    return "unknown";
}

inline const char *alignmentName(SampleAlignment value)
{
    switch (value) {
    case SampleAlignment::Native: return "native";
    case SampleAlignment::Left: return "left";
    case SampleAlignment::Right: return "right";
    }
    return "unknown";
}

inline void applyRawCaptureSelection(CaptureRequest &request,
                                    const RawCaptureSelection &selection)
{
    if (request.stream.kind != StreamKind::Raw)
        return;
    request.stream.representation = selection.representation;
    request.stream.format = selection.format;
    request.stream.sampleAlignment = selection.sampleAlignment;
}

// libcamera may adjust a requested Bayer pixel format, but a qualification
// case must never silently change its requested encoding class. Renderer
// support is a separate constraint: COMP1 is capturable but not decodable.
inline void verifyRawCaptureSelection(const CaptureRequest &request,
                                      const CaptureConfiguration &config,
                                      bool requirePreviewable)
{
    if (request.stream.kind != StreamKind::Raw)
        return;

    const RawEncoding actual = classifyRawEncoding(config.stream.format.name);
    const RawEncoding requestedFormat = request.stream.format
        ? classifyRawEncoding(request.stream.format->name)
        : RawEncoding::Unknown;

    if (request.stream.representation == RawRepresentation::Uncompressed &&
        actual != RawEncoding::UnpackedBayer)
        throw NegotiationFailed("qualification requested unpacked Bayer but libcamera negotiated " +
                                config.stream.format.name);

    if (request.stream.representation == RawRepresentation::Compressed &&
        (actual == RawEncoding::UnpackedBayer || actual == RawEncoding::Unknown))
        throw NegotiationFailed("qualification requested packed/compressed Bayer but libcamera negotiated " +
                                config.stream.format.name);

    if (requestedFormat == RawEncoding::UnpackedBayer &&
        actual != RawEncoding::UnpackedBayer)
        throw NegotiationFailed("qualification requested an unpacked Bayer pixel format but libcamera negotiated " +
                                config.stream.format.name);

    if (requirePreviewable &&
        actual != RawEncoding::UnpackedBayer &&
        !(actual == RawEncoding::Csi2PackedBayer &&
          (bayerBitDepth(config.stream.format.name) == 10 ||
           bayerBitDepth(config.stream.format.name) == 12)))
        throw Unsupported("qualification cannot export RAW pixel format " +
                          config.stream.format.name +
                          "; set raw_capture.representation=uncompressed and, if "
                          "the native sensor bit depth is not advertised, an explicit "
                          "raw_capture.format such as SBGGR10");
}

} // namespace hscam::qualification
