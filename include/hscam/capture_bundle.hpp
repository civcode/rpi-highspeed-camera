#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "hscam/capture_configuration.hpp"
#include "hscam/frame.hpp"

namespace hscam {

struct BundleManifest {
    unsigned schemaVersion{1};
    bool complete{};
    std::string cameraId;
    std::string cameraModel;
    CaptureConfiguration configuration;
    std::vector<PlaneConfiguration> observedPlanes;
    std::uint64_t frameCount{};
};

class BundleWriter {
public:
    BundleWriter(const std::filesystem::path &path, std::string cameraId, std::string cameraModel,
                 CaptureConfiguration configuration);
    ~BundleWriter();
    BundleWriter(BundleWriter &&) noexcept;
    BundleWriter &operator=(BundleWriter &&) noexcept;
    BundleWriter(const BundleWriter &) = delete;
    BundleWriter &operator=(const BundleWriter &) = delete;

    void append(const FrameMetadata &metadata, std::span<const PlaneView> planes);
    void append(const FrameLease &frame) { append(frame.metadata(), frame.planes()); }
    void finalize();

    [[nodiscard]] const BundleManifest &manifest() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class BundleReader {
public:
    explicit BundleReader(const std::filesystem::path &path);

    [[nodiscard]] const BundleManifest &manifest() const noexcept { return manifest_; }
    [[nodiscard]] std::vector<std::byte> readFrame(std::uint64_t frameNumber) const;

private:
    struct IndexRecord {
        std::uint64_t frame{};
        std::uint64_t offset{};
        std::uint64_t length{};
        std::uint64_t metadataLine{};
    };

    std::filesystem::path path_;
    BundleManifest manifest_;
    std::vector<IndexRecord> index_;
};

} // namespace hscam
