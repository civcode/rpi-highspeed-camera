#include "hscam/capture_bundle.hpp"
#include "hscam/error.hpp"
#include "hscam/raw/render.hpp"

#ifdef HSCAM_HAS_TIFF
#include <tiffio.h>
#endif

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
void usage()
{
    std::cerr << "usage:\n"
                 "  hscam-export inspect CAPTURE.hscap\n"
                 "  hscam-export recover CAPTURE.hscap\n"
                 "  hscam-export image CAPTURE.hscap --frame N --output frame.png\n"
                 "  hscam-export dng CAPTURE.hscap --frame N --output frame.dng\n"
                 "  hscam-export video CAPTURE.hscap --output preview.mp4 [--playback-fps N] [--timing fixed|sensor]\n";
}

std::optional<double> sensorAverageFps(const hscam::BundleReader &reader)
{
    const auto frames = reader.manifest().frameCount;
    if (frames < 2)
        return std::nullopt;

    std::optional<std::uint64_t> firstFrame;
    std::optional<std::uint64_t> lastFrame;
    std::optional<std::int64_t> firstTimestamp;
    std::optional<std::int64_t> lastTimestamp;

    for (std::uint64_t frame = 0; frame < frames; ++frame) {
        const auto &metadata = reader.frameMetadata(frame);
        if (!metadata.sensorTimestamp)
            continue;
        if (!firstFrame) {
            firstFrame = frame;
            firstTimestamp = metadata.sensorTimestamp->count();
        }
        lastFrame = frame;
        lastTimestamp = metadata.sensorTimestamp->count();
    }

    if (!firstFrame || !lastFrame || !firstTimestamp || !lastTimestamp ||
        *lastFrame <= *firstFrame || *lastTimestamp <= *firstTimestamp)
        return std::nullopt;

    return static_cast<double>(*lastFrame - *firstFrame) * 1e9 /
           static_cast<double>(*lastTimestamp - *firstTimestamp);
}

#ifdef HSCAM_HAS_TIFF
std::uint16_t cfaCode(hscam::raw::BayerOrder order, int i)
{
    static constexpr std::uint8_t patterns[][4] = {{1,1,1,1},{0,1,1,2},{1,0,2,1},{1,2,0,1},{2,1,1,0}};
    return patterns[static_cast<int>(order)][i];
}

void writeDng(const std::filesystem::path &path, const hscam::BundleManifest &manifest,
              std::span<const std::byte> payload,
              const hscam::BundleFrameMetadata &metadata)
{
    auto raw = hscam::raw::decodeRaw(manifest, payload);
    if (raw.bayer == hscam::raw::BayerOrder::None)
        throw hscam::Unsupported("DNG exporter currently requires Bayer raw input");

    TIFF *tif = TIFFOpen(path.c_str(), "w");
    if (!tif) throw hscam::Error("failed to create DNG");
    auto close = std::unique_ptr<TIFF, decltype(&TIFFClose)>(tif, &TIFFClose);

    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, raw.size.width);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, raw.size.height);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 16);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_CFA);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
    TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(tif, TIFFTAG_SOFTWARE, "rpi-highspeed-camera");
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif, raw.size.width * 2));
    const std::uint8_t dngVersion[4] = {1, 4, 0, 0};
    TIFFSetField(tif, TIFFTAG_DNGVERSION, dngVersion);
    TIFFSetField(tif, TIFFTAG_UNIQUECAMERAMODEL, manifest.cameraModel.c_str());
    std::uint16_t repeat[2] = {2, 2};
    std::uint8_t pattern[4] = {static_cast<std::uint8_t>(cfaCode(raw.bayer,0)), static_cast<std::uint8_t>(cfaCode(raw.bayer,1)),
                               static_cast<std::uint8_t>(cfaCode(raw.bayer,2)), static_cast<std::uint8_t>(cfaCode(raw.bayer,3))};
    TIFFSetField(tif, TIFFTAG_CFAREPEATPATTERNDIM, repeat);
    TIFFSetField(tif, TIFFTAG_CFAPATTERN, 4, pattern);
    const std::uint32_t white = raw.bitDepth >= 16 ? 65535U : ((1U << raw.bitDepth) - 1U);
    TIFFSetField(tif, TIFFTAG_WHITELEVEL, 1, &white);

    std::ostringstream description;
    description << "hscam frame=" << metadata.frame;
    if (metadata.sensorTimestamp)
        description << " sensor_timestamp_ns=" << metadata.sensorTimestamp->count();
    if (metadata.exposure)
        description << " exposure_us=" << metadata.exposure->count();
    if (metadata.analogueGain)
        description << " analogue_gain=" << std::setprecision(9) << *metadata.analogueGain;
    if (manifest.configuration.sensor.crop)
        description << " sensor_crop="
                    << manifest.configuration.sensor.crop->x << ","
                    << manifest.configuration.sensor.crop->y << ","
                    << manifest.configuration.sensor.crop->width << ","
                    << manifest.configuration.sensor.crop->height;
    const auto descriptionText = description.str();
    TIFFSetField(tif, TIFFTAG_IMAGEDESCRIPTION, descriptionText.c_str());

    for (std::uint32_t y = 0; y < raw.size.height; ++y) {
        auto *row = raw.pixels.data() + static_cast<std::size_t>(y) * raw.size.width;
        if (TIFFWriteScanline(tif, row, y, 0) < 0) throw hscam::Error("failed writing DNG scanline");
    }
}

#else
void writeDng(const std::filesystem::path &, const hscam::BundleManifest &,
              std::span<const std::byte>, const hscam::BundleFrameMetadata &)
{
    throw hscam::Unsupported("DNG export was not built because libtiff was not found");
}
#endif

class FfmpegPipe {
public:
    FfmpegPipe(const std::filesystem::path &output, std::uint32_t width, std::uint32_t height, double fps)
    {
        int fds[2];
        if (::pipe(fds) < 0) throw hscam::Error("pipe() failed");
        pid_ = ::fork();
        if (pid_ < 0) { ::close(fds[0]); ::close(fds[1]); throw hscam::Error("fork() failed"); }
        if (pid_ == 0) {
            ::dup2(fds[0], STDIN_FILENO);
            ::close(fds[0]); ::close(fds[1]);
            const std::string size = std::to_string(width) + "x" + std::to_string(height);
            std::ostringstream rateStream;
            rateStream << std::fixed << std::setprecision(9) << fps;
            const std::string rate = rateStream.str();
            ::execlp("ffmpeg", "ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "rawvideo",
                     "-pix_fmt", "rgb24", "-s", size.c_str(), "-r", rate.c_str(), "-i", "pipe:0",
                     "-an", "-pix_fmt", "yuv420p", "-y", output.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }
        ::close(fds[0]);
        fd_ = fds[1];
    }

    ~FfmpegPipe() { try { finish(); } catch (...) {} }

    void write(std::span<const std::uint8_t> bytes)
    {
        std::size_t off{};
        while (off < bytes.size()) {
            const ssize_t n = ::write(fd_, bytes.data() + off, bytes.size() - off);
            if (n < 0) { if (errno == EINTR) continue; throw hscam::Error("write to ffmpeg failed"); }
            off += static_cast<std::size_t>(n);
        }
    }

    void finish()
    {
        if (fd_ < 0) return;
        ::close(fd_); fd_ = -1;
        int status{};
        if (::waitpid(pid_, &status, 0) < 0) throw hscam::Error("waitpid(ffmpeg) failed");
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) throw hscam::Error("ffmpeg export failed");
    }

private:
    int fd_{-1};
    pid_t pid_{-1};
};
}

int main(int argc, char **argv)
{
    if (argc < 3) { usage(); return 2; }
    try {
        const std::string command = argv[1];
        const std::filesystem::path capture = argv[2];
        std::filesystem::path output;
        std::uint64_t frame = 0;
        unsigned playbackFps = 30;
        std::string timing = "fixed";
        for (int i = 3; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--frame" && i + 1 < argc) frame = std::stoull(argv[++i]);
            else if (arg == "--output" && i + 1 < argc) output = argv[++i];
            else if (arg == "--playback-fps" && i + 1 < argc) playbackFps = static_cast<unsigned>(std::stoul(argv[++i]));
            else if (arg == "--timing" && i + 1 < argc) timing = argv[++i];
            else { usage(); return 2; }
        }

        if (command == "recover") {
            if (argc != 3) {
                usage();
                return 2;
            }
            const auto recovery = hscam::recoverBundle(capture);
            std::cout << "recovered frames: " << recovery.recoveredFrames << "\n"
                      << "was complete: " << (recovery.wasComplete ? "yes" : "no") << "\n"
                      << "discarded index bytes: " << recovery.discardedIndexBytes << "\n"
                      << "discarded payload bytes: " << recovery.discardedPayloadBytes << "\n"
                      << "discarded metadata bytes: " << recovery.discardedMetadataBytes << "\n"
                      << "synthesized metadata frames: "
                      << recovery.synthesizedMetadataFrames << "\n";
            return 0;
        }

        hscam::BundleReader reader(capture);
        const auto &m = reader.manifest();
        if (command == "inspect") {
            std::cout << "camera: " << m.cameraModel << " (" << m.cameraId << ")\n"
                      << "stream: " << m.configuration.stream.size << " " << m.configuration.stream.format.name << "\n"
                      << "frames: " << m.frameCount << "\n"
                      << "complete: " << (m.complete ? "yes" : "no") << "\n";
            if (const auto fps = sensorAverageFps(reader))
                std::cout << "sensor timestamp FPS: " << std::fixed
                          << std::setprecision(6) << *fps << "\n";
            return 0;
        }
        if (output.empty()) throw std::runtime_error("--output is required");
        if (command == "image") {
            hscam::raw::writePng(output, hscam::raw::renderPreview(m, reader.readFrame(frame)));
        } else if (command == "dng") {
            const auto payload = reader.readFrame(frame);
            writeDng(output, m, payload, reader.frameMetadata(frame));
        } else if (command == "video") {
            double videoFps{};
            if (timing == "fixed") {
                if (!playbackFps) throw std::runtime_error("playback FPS must be positive");
                videoFps = static_cast<double>(playbackFps);
            } else if (timing == "sensor") {
                const auto fps = sensorAverageFps(reader);
                if (!fps)
                    throw std::runtime_error("capture does not contain enough sensor timestamps for real-time playback");
                videoFps = *fps;
            } else {
                throw std::runtime_error("--timing must be fixed or sensor");
            }

            FfmpegPipe ffmpeg(output, m.configuration.stream.size.width,
                              m.configuration.stream.size.height, videoFps);
            for (std::uint64_t i = 0; i < m.frameCount; ++i) {
                const auto image = hscam::raw::renderPreview(m, reader.readFrame(i));
                ffmpeg.write(image.pixels);
            }
            ffmpeg.finish();
            std::cout << "video FPS: " << std::fixed << std::setprecision(6)
                      << videoFps << "\n";
        } else {
            usage(); return 2;
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "hscam-export: " << e.what() << '\n';
        return 1;
    }
}
