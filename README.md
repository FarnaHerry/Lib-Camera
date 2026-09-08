# HuxerUI Camera

Camera capture and texture-based preview for HuxerUI applications.

The initial implementation provides a shared `CameraSession`, declarative capture control through `UseCamera`, and a `CameraPreview` component. Preview frames use the SDK's `ExternalTexture` publication mechanism instead of triggering UI recomposition on every frame.

## Implementation Status

| Area | Status |
| --- | --- |
| Public API and session ownership | Implemented |
| Preview rotation, mirroring, cropping, and ImageFit | Implemented; geometry tests pass |
| macOS AVFoundation capture | Implemented; example builds and has been launched |
| Android CameraX capture | Device preview, switching, geometry, and background recovery confirmed on TAS_AN00 with the updated SDK |
| iOS AVFoundation capture | Implemented independently; device and Simulator builds pass; physical-device operation confirmed by the user |
| Windows, Linux, Web capture | Pending |

Platforms without a capture backend report `CameraErrorCode::Unavailable`. Device acceptance is platform-specific: Android preview behavior has been confirmed on TAS_AN00, and the user confirmed successful iOS physical-device testing. Sustained performance and the remaining stress scenarios have not been established. Automated tests and Simulator launch do not establish physical capture correctness.

## Add the Library

Use the installed HuxerUI SDK and its generated application structure:

```cmake
huxerui_use_library(my_app
        TARGET HuxerUI::Camera
        PATH "/path/to/Lib-Camera"
)
```

Register `huxerui::camera::Install` in the application's `root_hooks`. Registration does not open a camera or request authorization.

```cpp
const huxerui::Application application{
    App,
    {
        .root_hooks = {
            huxerui::camera::Install,
        },
    }
};
```

## Display a Preview

The application obtains camera authorization through HuxerUI's `ApplicationHandle`. The reusable component below assumes authorization has already been granted:

```cpp
#include <huxerui/camera.h>
#include <huxerui/huxerui.h>

using namespace huxerui;

[[huxerui::composable]]
View CameraContent(bool active) {
  auto session = camera::UseCamera({.active = active});

  return camera::CameraPreview(session, {
      .fit = ImageFit::Cover,
      .mirror = camera::MirrorMode::Auto,
  }).With(Frame{.width = 320.0F, .height = 240.0F});
}
```

An empty `CameraOptions::facing` selects the platform's default video device. Set it to `camera::Facing::Front` or `camera::Facing::Back` to require that facing. Devices with unknown facing report an empty `actual_facing`.

`active` is the sole start/stop control. The session also suspends in the background. `session.Status()` provides observed state, an optional diagnostic error, and an optional preview output. `session.Retry()` retries a failed session without changing the configuration or opening permission UI.

Multiple `CameraPreview` components can share one session. Unmounting a preview does not stop capture. Unmounting the component that owns `UseCamera` closes capture even when another object retains a copy of the session handle.

`MirrorMode::Auto` mirrors confirmed front cameras. Off and On specify the final mirrored appearance. Custom drawing can use `Status().preview`, whose texture, normalized crop, remaining clockwise rotation, and existing mirror state must be interpreted together. The rotation enum is `PreviewRotation`.

## macOS Example

The application must include `NSCameraUsageDescription` in its Info.plist. The example already includes this description and does not request microphone access.

```sh
cd examples/preview
huxerui build macos --profile debug
huxerui run macos --profile debug
```

The example uses a dark camera workspace with a large viewfinder and a single **Start camera / Stop camera** action. Starting requests camera access when needed; a failed session offers **Try again**. The adjacent flip control switches between front and rear cameras.

**Preview settings** appears in a right sidebar on wide windows and a bottom sheet on smaller screens. Choose the camera source, Fill / Fit, and Auto / Off / On mirroring; optionally enable a composition grid or a comparison preview. The comparison shows the full, unmirrored frame from the same session. Settings persist while starting, stopping, or resizing the workspace. This example provides live preview only; it does not take photos or record video.

The macOS backend requests a 1280×720 preset when supported and publishes BGRA pixel buffers. This is an initial SDR implementation, not a zero-copy or HDR guarantee.

## Android Example

The library includes the CameraX Camera2 and lifecycle dependencies, the camera permission declaration, and optional camera hardware features. The application still owns runtime authorization through HuxerUI. No Activity edits or extra factory registration are needed.

```sh
cd examples/preview
huxerui build android --profile debug
huxerui run android --profile debug
```

Use `--java-home /path/to/jdk17` when the default Java installation does not satisfy the generated Android build. The example has the same authorization, facing, retry, and shared-preview controls on both platforms.

The Android backend uses CameraX 1.5.3 `Preview` with a module-owned lifecycle and the SDK's `SurfaceStreamTexture`. An unspecified facing prefers a back camera, then the first available camera. It unbinds only its own use case and waits for CameraX's Surface result callbacks before finishing textures or completing stop. Output geometry changes request a replacement Surface; native texture capabilities cross the SDK channel without a texture-ID registry.

The initial integration targets full-frame SDR preview without a CameraX ViewPort or effects. It interprets direct Surface output in natural orientation, applies the remaining display rotation, and accounts for native front-camera mirroring. The user confirmed all four display orientations, front/back switching, simultaneous previews, mirroring, Cover/Contain, and background recovery on TAS_AN00. Unexpected source cropping reports `ConfigurationFailed` rather than displaying incorrect geometry.

The TAS_AN00 check exposed duplicate PlatformChannel event registration on restart. The backend now subscribes once at creation; subsequent starts update only the capture callback and run generation. A temporary device regression sequence passed four front/back capture runs, stop/restart, live facing changes, and advancing texture revisions on 1440×1080 Surfaces. Subsequent interactive checks confirmed visible preview geometry and recovery. Session failures and Surface configuration are logged under `HuxerUICamera` to support diagnosis.

The black-preview issue was traced to HuxerUI Android texture-layer initialization. After the framework fix and an installed SDK update, the camera example was rebuilt against the installed SDK and the user confirmed normal output. See [the Surface investigation](docs/android-surface-investigation.md) for the reproduction and validation scope.

## iOS Example

The iOS backend is independent of the macOS implementation. It uses AVFoundation video data output and `ios::PixelBufferTexture`, with capture configuration, frame publication, and shutdown serialized on one queue. It requests BGRA SDR frames and a 1280×720 preset when supported. Late frames are discarded. Permissions and background intent use the existing shared session API.

The output connection requests unmirrored portrait buffers; CameraPreview applies the remaining window orientation and requested mirror mode. A transparent, noninteractive child view controller observes the owning window's orientation without rendering camera content. Session interruptions report Suspended and resume when AVFoundation permits; runtime failures report an error for explicit Retry. The user confirmed successful physical-device testing. Orientation-locked windows, landscape-mounted iPad cameras, and system interruption reasons were not separately recorded in that confirmation and remain focused acceptance cases.

The example includes `NSCameraUsageDescription`. Native code compiles through CMake; the existing Swift Package declares the Apple framework dependencies and carries no second camera implementation or bridge.

```sh
cd examples/preview
huxerui build ios --profile debug
huxerui run ios --profile debug
```

The HuxerUI SDK must export its iOS UserNotifications dependency. The local SDK export template and installed package now include this fix, so normal builds require no additional linker flags.

Simulator arm64/x86_64 and unsigned device arm64 builds passed, including an incremental build in the reported Xcode device build directory and a fresh Simulator build directory. The example also launched on an iPhone 17 Pro simulator running iOS 26.5. After the SDK link fix, the user tested on a physical device and reported normal operation; the device model and OS version were not recorded. The deployment target remains iOS 15.

A temporary Simulator regression called the real iOS backend with default, front, back, and default selections. All four attempts reported DeviceNotFound and completed stop before the next attempt. The normal manual-start example was restored afterward. This checks failure cleanup and repeated startup without physical camera hardware.

## Tests

On macOS with the installed SDK:

```sh
cmake -S . -B .huxerui/tests -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DHUXERUI_CAMERA_BUILD_TESTS=ON
cmake --build .huxerui/tests --parallel
ctest --test-dir .huxerui/tests --output-on-failure
```

Tests cover all four rotations, mirroring, crop coordinates, fitting, invalid geometry, shared session ownership, unavailable authorization, retry behavior, and owner teardown. They use a test platform without camera permission support and do not open a physical camera. Successful-session asynchronous stop, rapid option changes, and stale backend callbacks are not covered by this suite; previous device checks are complementary evidence, not automated regression coverage.

See [the design document](docs/camera-design.md) for lifecycle contracts, platform integration plans, and device validation requirements.
