#include "hscam/camera_info.hpp"
#include "hscam/capture_bundle.hpp"
#include "hscam/error.hpp"
#include "hscam/geometry.hpp"
#include "hscam/raw/render.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }

void geometryAndModeIds()
{
    using namespace hscam;
    static_assert(Size{2, 3}.area() == 6);
    static_assert(Rect{1, 2, 3, 4}.right() == 4);
    static_assert(Rect{1, 2, 3, 4}.bottom() == 6);
    SensorMode mode; mode.size={1456,96}; mode.format={"SBGGR10_CSI2P"}; mode.bitDepth=10;
    const auto a=stableModeId(mode); const auto b=stableModeId(mode);
    require(!a.empty()&&a==b,"mode ID must be stable"); mode.size.height=88; require(a!=stableModeId(mode),"different mode must have different ID");
}

void raw10Unpack()
{
    using namespace hscam; BundleManifest m; m.configuration.stream.size={4,1}; m.configuration.stream.format={"SRGGB10_CSI2P"}; m.observedPlanes={{5,5}};
    const std::array<std::byte,5> bytes{std::byte{0x00},std::byte{0x00},std::byte{0x00},std::byte{0xff},std::byte{0xf9}};
    const auto raw=raw::decodeRaw(m,bytes); require(raw.pixels.size()==4,"RAW10 pixel count"); require(raw.pixels[0]==1&&raw.pixels[1]==2&&raw.pixels[2]==3&&raw.pixels[3]==1023,"RAW10 unpack values");
}

void raw12Unpack()
{
    using namespace hscam; BundleManifest m; m.configuration.stream.size={2,1}; m.configuration.stream.format={"SBGGR12_CSI2P"}; m.observedPlanes={{3,3}};
    const std::array<std::byte,3> bytes{std::byte{0x12},std::byte{0xab},std::byte{0xc3}}; const auto raw=raw::decodeRaw(m,bytes); require(raw.pixels[0]==0x123&&raw.pixels[1]==0xabc,"RAW12 unpack values");
}

void rawPartialGroups()
{
    using namespace hscam;
    BundleManifest raw10; raw10.configuration.stream.size={1,1}; raw10.configuration.stream.format={"SRGGB10_CSI2P"}; raw10.observedPlanes={{2,2}};
    const std::array<std::byte,2> ten{std::byte{0xff},std::byte{0x03}}; const auto decoded10=raw::decodeRaw(raw10,ten); require(decoded10.pixels.size()==1&&decoded10.pixels[0]==1023,"partial RAW10 group");
    BundleManifest raw12; raw12.configuration.stream.size={1,1}; raw12.configuration.stream.format={"SRGGB12_CSI2P"}; raw12.observedPlanes={{2,2}};
    const std::array<std::byte,2> twelve{std::byte{0x12},std::byte{0x03}}; const auto decoded12=raw::decodeRaw(raw12,twelve); require(decoded12.pixels.size()==1&&decoded12.pixels[0]==0x123,"partial RAW12 group");
}

void yuv420InfersPaddedChromaStride()
{
    using namespace hscam;

    BundleManifest inferred;
    inferred.configuration.stream.size = {4, 4};
    inferred.configuration.stream.format = {"YUV420"};
    inferred.observedPlanes = {
        {8, 32},
        {0, 8},
        {0, 8},
    };

    BundleManifest explicitStride = inferred;
    explicitStride.observedPlanes[1].stride = 4;
    explicitStride.observedPlanes[2].stride = 4;

    std::array<std::byte, 48> payload{};
    // Y plane: neutral luma, with four padding bytes on every row.
    for (std::size_t y = 0; y < 4; ++y)
        for (std::size_t x = 0; x < 4; ++x)
            payload[y * 8 + x] = std::byte{128};

    // U plane starts at 32. Row 0 is neutral; row 1 is strongly blue.
    payload[32] = payload[33] = std::byte{128};
    payload[36] = payload[37] = std::byte{255};
    // V plane starts at 40 and stays neutral.
    payload[40] = payload[41] = std::byte{128};
    payload[44] = payload[45] = std::byte{128};

    const auto inferredImage = raw::renderPreview(inferred, payload);
    const auto explicitImage = raw::renderPreview(explicitStride, payload);

    require(inferredImage.pixels == explicitImage.pixels,
            "YUV420 unknown chroma stride must follow padded luma stride");
}

void bundleRejectsTrailingPayload()
{
    using namespace hscam;
    const auto path = std::filesystem::temp_directory_path() /
                      "hscam-validation-unit.hscap";
    std::error_code ec;
    std::filesystem::remove_all(path, ec);

    CaptureConfiguration cfg;
    cfg.stream.kind = StreamKind::Raw;
    cfg.stream.size = {2, 1};
    cfg.stream.format = {"SRGGB8"};
    cfg.stream.frameBytes = 2;

    std::array<std::byte, 2> pixels{std::byte{1}, std::byte{2}};
    PlaneView plane{pixels, 2, 2, -1};
    FrameMetadata metadata;
    metadata.sequence = 1;

    {
        BundleWriter writer(path, "test-camera", "synthetic", cfg);
        writer.append(metadata, std::span<const PlaneView>(&plane, 1));
        writer.finalize();
    }

    {
        std::ofstream out(path / "frames.bin", std::ios::binary | std::ios::app);
        out.put('x');
    }

    bool rejected = false;
    try {
        BundleReader reader(path);
        (void)reader;
    } catch (const Error &) {
        rejected = true;
    }
    require(rejected, "reader must reject unindexed trailing payload");

    const auto recovered = recoverBundle(path);
    require(recovered.discardedPayloadBytes == 1,
            "recovery must discard trailing payload");
    BundleReader reader(path);
    require(reader.manifest().frameCount == 1,
            "recovered validation fixture frame count");

    std::filesystem::remove_all(path, ec);
}

void bundleRecovery()
{
    using namespace hscam; const auto path=std::filesystem::temp_directory_path()/"hscam-recovery-unit.hscap"; std::error_code ec; std::filesystem::remove_all(path,ec);
    CaptureConfiguration cfg; cfg.stream.kind=StreamKind::Raw; cfg.stream.size={4,2}; cfg.stream.format={"SRGGB8"}; cfg.stream.frameBytes=8;
    std::array<std::byte,8> pixels{std::byte{0},std::byte{1},std::byte{2},std::byte{3},std::byte{4},std::byte{5},std::byte{6},std::byte{7}};
    PlaneView plane{pixels,4,8,-1}; FrameMetadata metadata; metadata.sequence=1;
    { BundleWriter writer(path,"test-camera","synthetic",cfg); writer.append(metadata,std::span<const PlaneView>(&plane,1)); metadata.sequence=2; writer.append(metadata,std::span<const PlaneView>(&plane,1)); }
    {
        std::ifstream in(path/"manifest.json"); std::ostringstream text; text<<in.rdbuf(); auto manifest=text.str(); const auto needle=std::string{"\"frame_count\": 2"}; const auto pos=manifest.find(needle); require(pos!=std::string::npos,"recovery fixture manifest frame count"); manifest.replace(pos,needle.size(),"\"frame_count\": 0"); std::ofstream out(path/"manifest.json",std::ios::trunc); out<<manifest;
    }
    { std::ofstream out(path/"frames.idx",std::ios::binary|std::ios::app); const std::array<char,3> partial{'x','y','z'}; out.write(partial.data(),partial.size()); }
    { std::ofstream out(path/"frames.bin",std::ios::binary|std::ios::app); const std::array<char,2> junk{'q','r'}; out.write(junk.data(),junk.size()); }
    {
        std::ifstream in(path/"metadata.jsonl",std::ios::binary); std::ostringstream text; text<<in.rdbuf(); const auto firstLineEnd=text.str().find('\n'); require(firstLineEnd!=std::string::npos,"recovery fixture metadata"); std::filesystem::resize_file(path/"metadata.jsonl",firstLineEnd+1,ec); require(!ec,"truncate recovery metadata"); std::ofstream out(path/"metadata.jsonl",std::ios::binary|std::ios::app); out<<"{\"frame\":1";
    }
    const auto recovered=recoverBundle(path); require(!recovered.wasComplete,"recovery fixture must be incomplete"); require(recovered.recoveredFrames==2,"recover both complete indexed frames"); require(recovered.discardedIndexBytes==3,"discard partial index record"); require(recovered.discardedPayloadBytes==2,"discard unreferenced payload bytes"); require(recovered.discardedMetadataBytes>0,"discard partial metadata line"); require(recovered.synthesizedMetadataFrames==1,"synthesize missing metadata");
    BundleReader reader(path); require(!reader.manifest().complete,"recovered bundle remains marked incomplete"); require(reader.manifest().frameCount==2,"recovered manifest frame count"); require(reader.readFrame(1).size()==pixels.size(),"recovered second payload");
    std::ifstream metadataFile(path/"metadata.jsonl"); std::ostringstream metadataText; metadataText<<metadataFile.rdbuf(); require(metadataText.str().find("recovered_without_metadata")!=std::string::npos,"recovered metadata marker"); std::filesystem::remove_all(path,ec);
}

void bundleRoundTrip()
{
    using namespace hscam; const auto path=std::filesystem::temp_directory_path()/"hscam-unit.hscap"; std::error_code ec; std::filesystem::remove_all(path,ec);
    CaptureConfiguration cfg; cfg.stream.kind=StreamKind::Raw; cfg.stream.size={4,2}; cfg.stream.format={"SRGGB8"}; cfg.stream.frameBytes=8;
    std::array<std::byte,8> pixels{std::byte{0},std::byte{64},std::byte{128},std::byte{255},std::byte{255},std::byte{128},std::byte{64},std::byte{0}}; PlaneView plane{pixels,4,8,-1}; FrameMetadata metadata; metadata.sequence=7; metadata.sensorTimestamp=std::chrono::nanoseconds(9007199254740993LL);
    { BundleWriter writer(path,"test-camera","synthetic",cfg); writer.append(metadata,std::span<const PlaneView>(&plane,1)); writer.finalize(); }
    BundleReader reader(path); require(reader.manifest().complete,"finalized bundle must be complete"); require(reader.manifest().frameCount==1,"bundle frame count"); require(reader.manifest().observedPlanes.at(0).stride==4,"bundle stride"); const auto &storedMetadata=reader.frameMetadata(0); require(storedMetadata.sequence && *storedMetadata.sequence==7,"bundle metadata sequence"); require(storedMetadata.sensorTimestamp && storedMetadata.sensorTimestamp->count()==9007199254740993LL,"bundle metadata timestamp"); require(storedMetadata.status=="complete","bundle metadata status"); const auto stored=reader.readFrame(0); require(stored.size()==pixels.size(),"bundle payload size"); require(std::equal(stored.begin(),stored.end(),pixels.begin()),"bundle payload equality"); const auto preview=raw::renderPreview(reader.manifest(),stored); require(preview.pixels.size()==4*2*3,"preview RGB size");
    { std::ifstream metadataFile(path/"metadata.jsonl"); std::ostringstream metadataText; metadataText<<metadataFile.rdbuf(); require(metadataText.str().find("9007199254740993")!=std::string::npos,"64-bit timestamp must be serialized exactly"); }
    const auto png=path/"preview.png"; raw::writePng(png,preview); { std::ifstream image(png,std::ios::binary); std::array<unsigned char,8> signature{}; image.read(reinterpret_cast<char*>(signature.data()),signature.size()); const std::array<unsigned char,8> expected{137,80,78,71,13,10,26,10}; require(signature==expected,"PNG signature"); }
    std::filesystem::remove_all(path,ec);
}
}

int main(){try{geometryAndModeIds();raw10Unpack();raw12Unpack();rawPartialGroups();yuv420InfersPaddedChromaStride();bundleRejectsTrailingPayload();bundleRecovery();bundleRoundTrip();std::cout<<"hscam unit tests passed\n";return 0;}catch(const std::exception&e){std::cerr<<"unit test failed: "<<e.what()<<'\n';return 1;}}
