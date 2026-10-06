# C++ library: first implementation concept

## Status

This document defines the first implementation of the production C++ library for `rpi-highspeed-camera`.

The first implementation is intentionally small. Its job is to make Raspberry Pi camera capture understandable, deterministic, and measurable while exposing the sensor-level controls needed for high-frame-rate work.

It is **not** the qualification harness described in [Official Raspberry Pi camera qualification protocol](official-camera-qualification-protocol.md), and it is not initially a computer-vision framework.

The first vertical slice should do this reliably:

```text
discover camera
    ->
inspect capabilities
    ->
request sensor mode / optional sensor crop / frame duration
    ->
report exact negotiated configuration
    ->
capture frames
    ->
deliver frames with timestamps and buffer metadata
    ->
stop and restore cleanly
```

Everything else should build on top of that.

---

# 1. Design goals

## 1.1 Primary goals

The first library version should:

- support every camera that libcamera exposes on Raspberry Pi through one common API;
- enumerate cameras and advertised sensor modes;
- expose sensor/crop capabilities without assuming a particular sensor model;
- allow an exact sensor crop when the V4L2 sensor subdevice supports it;
- allow fixed advertised modes when arbitrary crop is not supported;
- configure raw and processed streams through libcamera;
- request frame duration, exposure, and gain;
- expose requested and negotiated configuration separately;
- deliver frames without an unconditional image copy;
- expose sensor/request timestamps and sequence information required to detect frame loss;
- make buffer stride, plane layout, and actual byte sizes explicit;
- use deterministic ownership and RAII;
- leave processing, compression, storage policy, and Python bindings outside the first core.

## 1.2 Non-goals for v0.1

Do not put these into the first implementation:

- OpenCV dependency;
- debayering;
- video encoding;
- compression;
- Movidius/OpenVINO;
- object detection;
- a processing-plugin ABI;
- Python bindings;
- graphical preview;
- automatic selection of "best" crop from the published result database;
- automatic exhaustive crop testing;
- kernel patching;
- direct undocumented sensor-register programming.

Those features become much easier to evaluate after the capture path is stable.

---

# 2. Fundamental design rules

## 2.1 Requested and negotiated state are different types

libcamera configuration validation can adjust an application's requested stream configuration. V4L2 subdevice selection may also clamp, align, or otherwise alter a requested rectangle.

The API must therefore never return the caller's request as if it were hardware state.

Use separate types:

```cpp
hscam::CaptureRequest
hscam::CaptureConfiguration
```

where `CaptureRequest` is intent and `CaptureConfiguration` is the verified negotiated result.

For example:

```cpp
CaptureRequest request;
request.sensor.crop = Rect{0, 496, 1456, 96};
request.timing.frameDuration = 1866us;
request.stream.kind = StreamKind::Raw;

CaptureConfiguration actual = camera.configure(request);

std::cout << actual.sensor.crop << "\n";
std::cout << actual.timing.frameDuration << "\n";
std::cout << actual.stream.planes[0].stride << "\n";
```

No downstream code should need to guess whether the driver rounded the crop or changed the stream format.

## 2.2 Exact configuration is first-class

For experimentation and scientific capture, silent adjustment is dangerous.

The request carries a policy:

```cpp
enum class NegotiationPolicy {
    Exact,
    AllowAdjustments,
};
```

With `Exact`, configuration fails if a requested property that was marked exact changes during negotiation.

This should be the default used by the qualification harness.

Normal applications may choose `AllowAdjustments`.

## 2.3 Sensor crop and ScalerCrop are different concepts

Use different types and names:

```cpp
SensorCrop
ScalerCrop
```

Never expose one generic `crop` field.

A `SensorCrop` is only reported as active when the sensor/media subdevice confirms it.

## 2.4 The capture callback does almost nothing

The libcamera request-completion callback should:

1. read completion status and metadata;
2. construct/update a lightweight frame descriptor;
3. push a completed frame token into a bounded queue;
4. return.

It must not:

- debayer;
- encode;
- write to disk;
- perform CV;
- wait on user code.

This keeps the callback predictable at hundreds of frames per second.

## 2.5 No hidden copies

The first implementation maps libcamera frame-buffer planes and exposes views into those buffers.

A frame copy is an explicit operation performed by the caller or a later module.

---

# 3. Proposed public API

Use namespace:

```cpp
namespace hscam {}
```

The public API should be small enough to understand without knowing libcamera internals.

## 3.1 Library context

```cpp
class Context {
public:
    Context();
    ~Context();

    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;

    std::vector<CameraInfo> cameras() const;
    Camera open(std::string_view cameraId);
};
```

`Context` owns the single libcamera `CameraManager` instance.

libcamera documents `CameraManager` as the application entry point and states that only one manager instance should exist at a time. The wrapper should enforce that lifetime naturally.

## 3.2 CameraInfo

```cpp
struct CameraInfo {
    std::string id;
    std::string model;
    std::optional<std::string> sensorModel;

    Size pixelArray;
    std::vector<Rect> activeAreas;

    std::vector<SensorMode> sensorModes;
    CameraCapabilities capabilities;
};
```

This is a snapshot suitable for display, selection, and logging.

It should contain no owning libcamera pointers.

## 3.3 SensorMode

```cpp
struct SensorMode {
    std::string id;

    Size size;
    PixelFormat format;
    unsigned bitDepth;

    std::optional<DurationRange> frameDuration;
    std::optional<std::int64_t> pixelRate;
    std::vector<std::int64_t> linkFrequencies;

    std::optional<Rect> sensorCrop;
};
```

The `id` is a deterministic identifier generated from the properties that distinguish the mode. It should not be a vector index because enumeration order can change.

## 3.4 CameraCapabilities

```cpp
struct CameraCapabilities {
    bool rawCapture;
    bool processedCapture;

    bool sensorCropQueryable;
    bool sensorCropTryable;
    bool sensorCropSettable;

    bool frameDurationControl;
    bool exposureControl;
    bool analogueGainControl;

    std::optional<Rect> sensorNativeSize;
    std::optional<Rect> sensorCropBounds;
    std::optional<Rect> sensorDefaultCrop;
};
```

These describe observed runtime support.

Do not infer `sensorCropSettable=true` merely because the sensor model is IMX296.

## 3.5 CaptureRequest

A request should describe intent without leaking libcamera classes into the public interface.

```cpp
struct SensorRequest {
    std::optional<std::string> modeId;
    std::optional<Rect> crop;
};

enum class StreamKind {
    Raw,
    Processed,
};

struct StreamRequest {
    StreamKind kind = StreamKind::Raw;
    std::optional<Size> size;
    std::optional<PixelFormat> format;
    unsigned bufferCount = 0; // 0 = library default
};

struct TimingRequest {
    std::optional<std::chrono::microseconds> frameDuration;
    std::optional<std::chrono::microseconds> exposure;
    std::optional<double> analogueGain;
};

struct CaptureRequest {
    SensorRequest sensor;
    StreamRequest stream;
    TimingRequest timing;

    NegotiationPolicy negotiation = NegotiationPolicy::AllowAdjustments;
};
```

Do not add dozens of controls to this type initially.

Additional libcamera controls can later be exposed through a typed advanced-control mechanism if there is a demonstrated need.

## 3.6 CaptureConfiguration

```cpp
struct PlaneConfiguration {
    std::uint32_t stride;
    std::uint32_t size;
};

struct StreamConfiguration {
    StreamKind kind;
    Size size;
    PixelFormat format;
    std::vector<PlaneConfiguration> planes;
    std::size_t frameBytes;
};

struct SensorConfiguration {
    std::optional<std::string> modeId;
    std::optional<Rect> crop;
};

struct TimingConfiguration {
    std::optional<std::chrono::microseconds> requestedFrameDuration;
    std::optional<std::chrono::microseconds> negotiatedFrameDuration;
    std::optional<std::chrono::microseconds> exposure;
    std::optional<double> analogueGain;
};

struct CaptureConfiguration {
    SensorConfiguration sensor;
    StreamConfiguration stream;
    TimingConfiguration timing;

    std::vector<Adjustment> adjustments;
};
```

An `Adjustment` records:

```cpp
struct Adjustment {
    std::string field;
    std::string requested;
    std::string negotiated;
    AdjustmentSource source;
};
```

This gives the caller an audit trail even when adjustments were allowed.

---

# 4. Camera object and state machine

The public `Camera` object owns access to one physical libcamera camera.

Suggested state machine:

```text
                  configure()
    Opened --------------------------> Configured
      ^                                  |
      |                                  | start()
      |                                  v
   close()                            Streaming
      ^                                  |
      |                                  | stop()
      +----------------------------------+
```

The implementation should reject invalid transitions rather than trying to be clever.

Example:

```cpp
Camera camera = context.open(info.id);

CaptureConfiguration config = camera.configure(request);

CaptureSession session = camera.start();

while (...) {
    auto result = session.nextFrame(100ms);
    ...
}

session.stop();
```

A `Camera` must not be copyable.

Moving it is acceptable if the internal implementation guarantees that callback targets remain stable.

---

# 5. CaptureSession

`CaptureSession` represents one active stream.

```cpp
class CaptureSession {
public:
    FrameLease nextFrame(std::chrono::milliseconds timeout);

    CaptureStats stats() const;

    void stop();
};
```

The first API should favor a blocking/polling consumer over invoking arbitrary user callbacks from libcamera's completion thread.

That makes ownership and back-pressure much easier to reason about.

A future asynchronous API can be layered on top.

---

# 6. Frame ownership

This is the most important lifetime rule in the capture API.

## 6.1 FrameLease

```cpp
class FrameLease {
public:
    const FrameMetadata &metadata() const;
    std::span<const PlaneView> planes() const;

    FrameLease(FrameLease &&);
    FrameLease &operator=(FrameLease &&);

    FrameLease(const FrameLease &) = delete;
    ~FrameLease();
};
```

A `FrameLease` represents temporary ownership of one completed request/buffer set.

When the lease is destroyed, its request is returned to the capture session and requeued.

This gives zero-copy access while making the lifetime explicit.

## 6.2 Consequence of holding frames

If the application holds too many `FrameLease` objects, the camera eventually runs out of queued buffers.

The library should expose this visibly:

- number of buffers in flight;
- number held by consumer;
- starvation events;
- queue overruns if a separate completed-frame queue fills.

Never allocate more frames silently to hide a slow consumer.

## 6.3 PlaneView

```cpp
struct PlaneView {
    std::span<const std::byte> data;
    std::uint32_t stride;
    std::uint32_t length;
    int dmaBufFd; // borrowed, optional/advanced use
};
```

The DMA-BUF fd remains owned by the underlying buffer.

The caller must not close it.

If exposing the fd publicly is premature, it can initially live in an `AdvancedPlaneInfo` structure.

---

# 7. Frame metadata

Every frame should expose the information needed for performance validation.

```cpp
struct FrameMetadata {
    std::uint64_t sequence;
    std::uint64_t requestCookie;

    std::optional<std::chrono::nanoseconds> sensorTimestamp;
    std::chrono::steady_clock::time_point completionTimestamp;

    std::optional<std::chrono::microseconds> exposure;
    std::optional<std::chrono::microseconds> frameDuration;
    std::optional<double> analogueGain;

    FrameStatus status;
};
```

The distinction between sensor timestamp and userspace completion time is important.

FPS/drop analysis should prefer the sensor timestamp when the pipeline exposes one.

Do not manufacture a synthetic sensor timestamp from callback arrival time.

---

# 8. Capture statistics

The core library should maintain lightweight statistics because the high-FPS use case requires them.

```cpp
struct CaptureStats {
    std::uint64_t requestsCompleted;
    std::uint64_t requestsCancelled;
    std::uint64_t sequenceGaps;

    std::uint64_t consumerStarvations;
    std::uint64_t completedQueueOverruns;

    std::optional<double> measuredFps;
    std::optional<std::chrono::nanoseconds> minInterval;
    std::optional<std::chrono::nanoseconds> maxInterval;
};
```

Detailed percentile/histogram computation belongs in the qualification harness.

The library should preserve enough raw metadata for the harness to calculate it exactly.

---

# 9. Internal architecture

Avoid introducing a generic backend/plugin framework before there is a second real backend.

The first implementation can use concrete internal classes:

```text
Context::Impl
    libcamera::CameraManager

Camera::Impl
    shared_ptr<libcamera::Camera>
    CameraDescriptor
    MediaGraph
    SensorSubdevice
    current CaptureConfiguration

CaptureSession::Impl
    FrameBufferAllocator
    mapped buffers
    libcamera::Request objects
    request-completion connection
    bounded completed queue
    stats
```

Suggested source tree:

```text
include/hscam/
    context.hpp
    camera.hpp
    camera_info.hpp
    capture_request.hpp
    capture_configuration.hpp
    frame.hpp
    geometry.hpp
    pixel_format.hpp
    error.hpp

src/
    context.cpp
    camera.cpp
    capture_session.cpp
    frame.cpp

    libcamera/
        camera_inventory.cpp
        camera_configuration.cpp
        buffer_pool.cpp
        metadata.cpp

    media/
        media_graph.cpp
        sensor_subdevice.cpp
        sensor_selection.cpp

    internal/
        bounded_queue.hpp
        mmap_plane.cpp
        state_machine.hpp

tests/
    unit/
        geometry_test.cpp
        negotiation_test.cpp
        state_machine_test.cpp
        timestamp_stats_test.cpp
        mode_id_test.cpp

tools/
    inspect/
        main.cpp

    qualification/
        ...
```

The public headers should not include Linux media-controller headers or V4L2 ioctl structures.

---

# 10. Media graph and sensor control

This is the part that replaces the obscure `media-ctl` scripts.

## 10.1 MediaGraph

`MediaGraph` should enumerate media devices and entities and associate the selected libcamera camera with its sensor entity/subdevice.

It must not assume:

- `/dev/media0`;
- one CSI port;
- a fixed I2C bus/address;
- an entity name beyond what is needed to identify the camera's actual sensor.

The discovered mapping becomes part of `CameraInfo`.

## 10.2 SensorSubdevice

`SensorSubdevice` wraps the V4L2 subdevice fd and owns operations such as:

```cpp
SelectionCapabilities selections() const;

CropNegotiation tryCrop(Rect crop);
CropNegotiation setCrop(Rect crop);

SubdevFormat format() const;
FormatNegotiation tryFormat(SubdevFormat);
FormatNegotiation setFormat(SubdevFormat);
```

Internally this uses the standard V4L2 subdevice format/selection ioctls where the driver exposes them.

Do not expose raw ioctls through the public library API.

## 10.3 Safe order of operations

The tricky integration problem is that libcamera also configures the sensor.

The first implementation should make the sequence explicit and verify each stage:

```text
1. acquire camera
2. snapshot initial sensor/media state
3. generate libcamera configuration
4. validate requested stream
5. apply required sensor-subdevice crop/format
6. configure libcamera
7. re-read sensor-subdevice state
8. fail if exact crop was lost/changed
9. allocate buffers
10. start capture
11. verify first-frame metadata/configuration assumptions
```

If experiments show that the sensor crop must be applied after a particular libcamera step, encode that order in one place and document it.

The qualification tests should prevent accidental regressions.

---

# 11. Libcamera configuration layer

Use libcamera's public application API for normal capture.

The wrapper should cover:

- `CameraManager`;
- `Camera::acquire()`;
- `Camera::generateConfiguration()`;
- `CameraConfiguration::validate()`;
- `Camera::configure()`;
- `FrameBufferAllocator`;
- `Camera::createRequest()`;
- `Request::addBuffer()`;
- `Camera::start()`;
- `Camera::queueRequest()`;
- request completion;
- `Request::reuse()`;
- `Camera::stop()`;
- `Camera::release()`.

libcamera's configuration validation has "try" semantics and may adjust a configuration. That is why the wrapper's exact-vs-adjusted negotiation policy is necessary.

---

# 12. Buffer pool and request lifecycle

A fixed buffer/request pool should be created at session start.

Conceptually:

```text
buffer 0 <-> request 0
buffer 1 <-> request 1
buffer 2 <-> request 2
...
```

At startup all requests are queued.

When a request completes:

```text
libcamera completion
      |
      v
CompletedFrame{request*, metadata}
      |
      v
bounded queue
      |
      v
FrameLease returned to caller
      |
      v
FrameLease destroyed
      |
      v
request->reuse(ReuseBuffers)
      |
      v
queueRequest(request)
```

This avoids per-frame request allocation.

The pool size should come from the negotiated stream's buffer requirements plus a conservative library default, while allowing the caller to request a larger count.

The actual count must be reported in `CaptureConfiguration`.

---

# 13. Queue behavior

Use a fixed-capacity queue allocated before streaming.

For v0.1, correctness is more important than claiming a lock-free design prematurely.

A bounded mutex/condition-variable queue is acceptable if profiling proves that it sustains the target rate. At 500-600 fps, one completion every roughly 1.7-2.0 ms is not inherently too fast for a well-designed bounded queue.

If measurement later shows contention, replace the internal queue without changing the public API.

When the queue is full, do **not** overwrite an unconsumed frame silently.

Mark a queue-overrun condition and apply one documented policy.

For the initial library, the preferred policy is:

- count the overrun;
- immediately recycle/requeue the newly completed request;
- return a visible overrun through statistics/status.

This keeps capture alive while making data loss observable.

The qualification harness treats any overrun as a failed test.

---

# 14. Error model

Do not expose negative libcamera/Linux error integers as the primary public interface.

Suggested exception hierarchy:

```cpp
class Error : public std::runtime_error {};

class CameraNotFound : public Error {};
class CameraBusy : public Error {};
class Unsupported : public Error {};
class NegotiationFailed : public Error {};
class ExactConfigurationFailed : public NegotiationFailed {};
class CaptureFailed : public Error {};
class Timeout : public Error {};
class DeviceDisconnected : public Error {};
```

Every error that wraps a lower-level failure should preserve:

- operation name;
- lower-level errno/libcamera error code;
- camera ID;
- current state where relevant.

For the high-frequency frame path, expected per-frame status should be represented as values rather than exceptions.

---

# 15. Logging

The library should have minimal structured logging categories:

```text
inventory
media
sensor
configure
capture
buffer
timing
```

Default library behavior should be quiet.

The qualification harness can enable verbose logs.

Never print directly from the request-completion callback except for a last-resort fatal diagnostic.

---

# 16. Threading model

The first implementation should use the threading already required by libcamera plus one consumer-facing queue.

Avoid creating a thread per processing stage.

Conceptually:

```text
libcamera event/callback context
           |
           | completion
           v
     bounded queue
           |
           v
application thread calling nextFrame()
```

The user decides whether to create additional worker threads.

This keeps the core library agnostic to processing architecture.

If libcamera callback-thread behavior differs across versions/pipeline handlers, the library should not expose those details as an API guarantee.

---

# 17. What the first executable should be

Before a full application, build one diagnostic executable:

```text
hscam-inspect
```

It uses only the public library API and prints:

- cameras;
- sensor identity;
- pixel array/active areas;
- sensor modes;
- crop capabilities and bounds;
- raw/processed formats;
- timing controls.

Optional:

```bash
hscam-inspect --json
```

The JSON output becomes useful to the qualification harness, but `hscam-inspect` itself should remain small.

The second executable should be:

```text
hscam-capture
```

for a controlled raw capture smoke test.

Example eventual interface:

```bash
hscam-capture \
  --camera <id> \
  --mode <mode-id> \
  --sensor-crop 0,496,1456,96 \
  --frame-duration-us 1866 \
  --frames 5000 \
  --discard-payload
```

`--discard-payload` is important: it measures capture/timing without making storage a confounding variable.

These executables are development/diagnostic programs, not the one-time exhaustive qualification interface.

---

# 18. First vertical slice

The first implementation milestone should be intentionally end-to-end.

## Milestone A — inventory

Implement:

- build system;
- `Context`;
- `CameraInfo`;
- camera enumeration;
- mode/property enumeration;
- `hscam-inspect`.

Success:

```text
hscam-inspect --json
```

correctly identifies every attached official Raspberry Pi camera that libcamera exposes.

## Milestone B — normal libcamera raw capture

Implement:

- `Camera::configure()`;
- raw stream configuration;
- `CaptureSession`;
- `FrameBufferAllocator`;
- persistent requests;
- buffer mmap;
- `FrameLease`;
- timestamps/stats;
- `hscam-capture --discard-payload`.

Success:

capture an advertised mode continuously with no library queue overruns and report measured FPS.

## Milestone C — media graph and sensor selection

Implement:

- camera-to-media-device association;
- sensor subdevice discovery;
- selection capability queries;
- TRY crop;
- ACTIVE crop;
- exact negotiation verification.

Success:

on IMX296, configure one known GScrop-equivalent sensor crop without invoking `media-ctl`.

## Milestone D — high-FPS regression

Configure and measure a published IMX296 reference case, initially:

```text
1456 x 96 at approximately 536 fps
```

The purpose is not to special-case IMX296. It is simply the strongest known regression test for the new generic sensor-selection path.

Success criteria:

- crop set through library code;
- libcamera starts successfully;
- actual crop re-read and verified;
- requested/negotiated frame duration recorded;
- sensor timestamps collected;
- no application queue overruns;
- frame-loss statistics produced.

Only after this should the one-time cross-camera qualification harness be built on the library.

---

# 19. Tests

## 19.1 Host/unit tests

Most logic should be testable without a Raspberry Pi camera:

- rectangle math;
- mode ID generation;
- exact/adjusted negotiation comparison;
- state-machine transitions;
- queue behavior;
- request-to-frame ownership bookkeeping;
- timestamp gap detection;
- frame-rate calculations;
- JSON serialization if included;
- V4L2 result translation using captured fixtures.

These run locally during development.

## 19.2 Hardware tests

Hardware tests are explicit and never automatically triggered.

They should be grouped separately:

```text
tests/hardware/
```

Initial cases:

- enumerate camera;
- capture advertised mode;
- start/stop repeatedly;
- consumer intentionally holds buffers;
- invalid crop rejected/adjusted visibly;
- IMX296 known high-FPS crop;
- device recovery after failed configuration.

The qualification campaign is stronger than these smoke/regression tests and remains in `tools/qualification`.

---

# 20. Build system

Use CMake for the first implementation.

Suggested targets:

```text
hscam                shared/static library target
hscam-inspect        diagnostic executable
hscam-capture        capture smoke-test executable
hscam-unit-tests     host tests
```

External dependency for the core should initially be only:

- libcamera;
- standard Linux media/V4L2 headers;
- pthread/runtime facilities already provided by the platform.

If a test framework is added, keep it a development dependency.

Do not make OpenCV, FFmpeg, Boost, or OpenVINO core dependencies.

---

# 21. ABI/API policy

The initial implementation should explicitly be pre-1.0.

Use semantic versions such as:

```text
0.1.x
```

Do not promise ABI stability yet.

Still keep libcamera/Linux types out of public headers where practical. That reduces accidental coupling and gives the project room to evolve internally.

Public types should favor:

- fixed-width integers;
- `std::chrono`;
- `std::span`;
- ordinary value types;
- `std::optional`;
- move-only resource handles.

---

# 22. Relationship to the published qualification data

The production library and published camera database should be related but independent.

The library answers:

> What does this camera expose right now, and can I configure/capture it?

The qualification data answers:

> On the canonical tested Raspberry Pi/software environment, what configurations were measured and how did they perform?

The first library version should **not** silently override runtime capability discovery with the checked-in database.

Later, an optional helper can use the published dataset to answer questions such as:

```cpp
auto recommendation =
    database.fastestStableConfiguration(camera.fingerprint(),
                                        MinimumSize{640, 100});
```

but that is not required for v0.1.

---

# 23. Coding principles for this project

1. No hard-coded `/dev/mediaN` or `/dev/v4l-subdevN`.
2. No shelling out to `media-ctl`, `rpicam-vid`, or FFmpeg from the core library.
3. No sensor-name branches unless a real driver quirk requires one.
4. No silent crop/format/frame-rate adjustment.
5. No payload processing inside the completion callback.
6. No unconditional frame copies.
7. No FPS claims derived only from requested controls.
8. No buffer-size calculations that ignore negotiated stride/plane sizes.
9. No hidden frame drops.
10. Every resource-owning type is RAII and move-safe.
11. Keep the public API smaller than the internal implementation.
12. Measure before replacing simple synchronization with complex lock-free code.

---

# 24. Immediate implementation order

The first code work should proceed in this order:

1. CMake project skeleton and public geometry/error types.
2. `Context` and libcamera manager lifetime.
3. `CameraInfo` and advertised-mode inventory.
4. `hscam-inspect`.
5. `Camera` state machine and normal libcamera configuration.
6. framebuffer allocation/mapping.
7. persistent request pool.
8. completion queue and `FrameLease`.
9. sensor timestamp extraction and basic stats.
10. `hscam-capture --discard-payload`.
11. media graph association.
12. generic V4L2 sensor selection queries.
13. TRY/ACTIVE sensor crop.
14. exact post-libcamera crop verification.
15. IMX296 1456x96 high-FPS regression.
16. only then begin the official-camera qualification harness.

That sequence gives us useful, testable software at every checkpoint and avoids designing the exhaustive qualification machinery on top of an unstable capture core.

---

# References

libcamera public application API:

- https://docs.libcamera.org/master/public-api/
- https://docs.libcamera.org/master/public-api/classlibcamera_1_1CameraManager.html
- https://docs.libcamera.org/master/public-api/classlibcamera_1_1Camera.html
- https://docs.libcamera.org/master/public-api/classlibcamera_1_1CameraConfiguration.html

Linux media/V4L2 subdevice selection:

- https://docs.kernel.org/userspace-api/media/v4l/vidioc-subdev-g-selection.html
- https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/v4l2-selection-targets.html

Project design documents:

- [High-FPS capture method](high-fps-capture-method.md)
- [Camera support and automated crop testing](camera-support-and-crop-testing.md)
- [Official Raspberry Pi camera qualification protocol](official-camera-qualification-protocol.md)
