#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace hscam {

enum class FrameStatus { Complete, Cancelled, Error };

struct FrameMetadata {
    std::uint64_t sequence{};
    std::uint64_t requestCookie{};
    std::optional<std::chrono::nanoseconds> sensorTimestamp;
    std::chrono::steady_clock::time_point completionTimestamp{};
    std::optional<std::chrono::microseconds> exposure;
    std::optional<std::chrono::microseconds> frameDuration;
    std::optional<double> analogueGain;
    FrameStatus status{FrameStatus::Complete};
};

struct PlaneView {
    std::span<const std::byte> data;
    std::uint32_t stride{};
    std::uint32_t length{};
    int dmaBufFd{-1};
};

struct CaptureStats {
    std::uint64_t requestsCompleted{};
    std::uint64_t requestsCancelled{};
    std::uint64_t sequenceGaps{};
    std::uint64_t consumerStarvations{};
    std::uint64_t completedQueueOverruns{};
    std::optional<double> measuredFps;
    std::optional<std::chrono::nanoseconds> minInterval;
    std::optional<std::chrono::nanoseconds> maxInterval;
};

class FrameLease {
public:
    class Impl;

    FrameLease();
    ~FrameLease();
    FrameLease(FrameLease &&) noexcept;
    FrameLease &operator=(FrameLease &&) noexcept;
    FrameLease(const FrameLease &) = delete;
    FrameLease &operator=(const FrameLease &) = delete;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] const FrameMetadata &metadata() const;
    [[nodiscard]] std::span<const PlaneView> planes() const;

private:
    explicit FrameLease(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    friend class CaptureSession;
};

} // namespace hscam
