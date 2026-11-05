# HuxerUI Camera

This downstream fork maintains application fixes on `farna/main`, including the library-graph-only configuration guard and current application/window ownership contracts. The official baseline is tracked through `upstream/main` from `HuxerUI/Lib-Camera`. Consumers should pin a validated commit and use matching C++ and Android Java revisions.

The library uses HuxerUI’s application/window ownership API, `AwaitCallback`, the iOS window module factory, and the Android Java window module factory. Windows and Linux screen capture also require the matching fork’s native window/surface extension APIs; update and build the framework together with this library.

Use [`FarnaHerry/HuxerUI` at `a755f9ff17ac83cea6c48795ada4b2a560083fc1`](https://github.com/FarnaHerry/HuxerUI/commit/a755f9ff17ac83cea6c48795ada4b2a560083fc1) as the framework baseline, or a later revision retaining these extension APIs.

Camera preview and JPEG photo capture for HuxerUI applications on Android, iOS, macOS, Windows, and Linux.

Use `UseCamera` to control a camera session, `CameraPreview` to display it, and `CapturePhotoAsync` to take photos. Multiple previews can share one session with independent fitting and mirroring. Preview frames update without recomposing the UI on every frame.

## Screen as a Camera Source

Screen selection uses a native transparent aperture, so the desktop beneath the window remains directly visible while choosing a position. Select `CaptureSource::Screen` and mount one `ScreenPreview` per session, with at most one active screen session per window. No screen stream, preview texture, or periodic frame processing runs while selecting. Only `CapturePhotoAsync` starts capture and reads a fresh image; capture resources close before that request finishes.

```cpp
auto session = camera::UseCamera({.active = active, .source = camera::CaptureSource::Screen});
return Column {
  camera::ScreenPreview(session).With(Frame{.width = 100.0F, .height = 100.0F}),
}.With(Padding(2.0F), Border(Color::White(), 2.0F));
```

The aperture follows the anchor's position and size within the application window. A 100×100 logical-unit aperture selects that area of the desktop; photo dimensions use physical display pixels and can differ with display scaling. The rectangle is resolved when a photo request is accepted, and the resulting JPEG comes from a newly delivered screen frame. Capture startup adds latency, so it does not promise the exact pixels at the instant of the button press. Moving the window during capture does not move that request's frozen rectangle. An anchor spanning displays or leaving the display is unavailable until wholly inside one display.

The aperture is rectangular. Put borders and controls outside it; application drawing inside its bounds, including grids or overlay controls, is hidden by the native mask. On Windows its interior passes mouse input to the desktop behind it. Unmount `ScreenPreview` while showing overlapping dialogs or sheets, then remount it after dismissal; the example does this for photo review. Native window geometry and display scaling determine placement, and ordinary `CornerRadius`, clips, fitting, and mirroring do not reshape the aperture. `PreviewOptions` are ignored for `ScreenPreview`; photo mirroring remains supported through `PhotoOptions`.

`CameraStatus::preview` stays empty in screen mode; `Running` means selection is ready for a capture request. Additional `CameraPreview(session)` views have no screen texture to display. There is currently no separate raw-frame read API; callers can read the encoded bytes of the `ImageAsset` returned by `CapturePhotoAsync`.

Screen mode does not request camera permission. Windows uses `SetWindowRgn` for the aperture and Windows Graphics Capture for each photo, excluding the application's top-level window; Windows 10 version 2004 or later is required. macOS uses a transparent AppKit window with a Core Animation mask, and starts ScreenCaptureKit only for each photo, excluding the application. It requires macOS 12.3 or later and Screen Recording access when taking a photo. The macOS aperture and capture implementation have **not been built or verified with a macOS SDK or hardware**, including mask coordinates and native mouse behavior. iOS, Android, and Web report `CameraErrorCode::Unavailable` for this source. Linux uses X11 window shapes for selection and direct photos on native X11, or a transparent GTK canvas with on-demand ScreenCast/PipeWire capture on Wayland.

Web capture is not supported; capture requests report `CameraErrorCode::Unavailable`.

## Installation

In an application created with the installed HuxerUI SDK, add the library to your application target:

```cmake
huxerui_use_library(my_app
        TARGET HuxerUI::Camera
        PATH "/path/to/Lib-Camera"
)
```

Include the public headers and register the camera module in your application's `application_hooks`:

```cpp
#include <huxerui/camera.h>
#include <huxerui/huxerui.h>

using namespace huxerui;

const Application application{
    App,
    {
        .application_hooks = {
            camera::Install,
        },
    }
};
```

Here, `App` is your application's root view function. Registration does not open the camera or request permission.

## Permissions and Preview

The application owns camera authorization. Use `ApplicationHandle::CheckPermissionAsync(Permission::Camera)` to check access and `RequestPermissionAsync(Permission::Camera)` from a user action to request it. Set capture intent to active only after authorization is granted.

- **Android:** the library declares the camera permission and optional camera hardware features. Your application must still request runtime permission.
- **iOS and macOS:** add `NSCameraUsageDescription` to the application's Info.plist with a description of how the camera is used.
- **Windows:** allow camera access for desktop applications in Windows privacy settings. Packaged applications also need the `webcam` capability in their package manifest.
- **Linux:** desktop sessions expose camera devices without an authorization prompt, so `CheckPermissionAsync(Permission::Camera)` reports `Granted` directly; requesting has no effect and permission settings are unavailable.

This component assumes the caller has obtained permission before setting `active` to true:

```cpp
[[huxerui::composable]]
View CameraContent(bool active) {
  auto session = camera::UseCamera({.active = active});

  return camera::CameraPreview(session, {
      .fit = ImageFit::Cover,
      .mirror = camera::MirrorMode::Auto,
  }).With(Frame{.width = 320.0F, .height = 240.0F});
}
```

Configure capture through `CameraOptions`:

| Option | Default | Behavior |
| --- | --- | --- |
| `active` | `true` | Starts or stops capture asynchronously. Capture suspends in the background. |
| `facing` | Empty | Selects the default camera. `Facing::Front` or `Facing::Back` requires that facing, without falling back to the opposite one. |

Configure each preview through `PreviewOptions`:

| Option | Default | Behavior |
| --- | --- | --- |
| `fit` | `ImageFit::Cover` | Fills and crops to the view's bounds. Use `Contain` to show the complete frame. |
| `mirror` | `MirrorMode::Auto` | Mirrors confirmed front cameras. `Off` and `On` select the final unmirrored or mirrored appearance. |

`session.Status()` observes lifecycle state, failure details, and photo activity. When a session fails, inspect `status.error->code` for recovery decisions and `status.error->message` for diagnostics. After resolving the cause, call `session.Retry()`; it does not request permission or change `active`.

The component calling `UseCamera` owns the session lifetime. Removing that component closes capture, even if another object retains a session handle. Removing an individual `CameraPreview` does not stop its shared session.

## Capture a Photo

Launch `CapturePhotoAsync` from a UI event or task. It returns `CameraResult<ImageAsset>`, an alias for `Result<ImageAsset, CameraError>`.

The following component stores the captured image in state supplied by its caller:

```cpp
[[huxerui::composable]]
View PhotoCapture(camera::CameraSession session, State<ImageAsset> photo) {
  auto tasks = UseTaskScope();
  auto message = UseState(std::string{});
  const auto status = session.Status();

  return Column {
    camera::CameraPreview(session).With(Frame{.height = 240.0F}),
    Button("Take photo").OnClick([tasks, session, photo, message] {
      tasks.Launch([session, photo, message]() -> Task<void> {
        auto result = co_await session.CapturePhotoAsync({
            .jpeg_quality = 90,
            .mirror = camera::MirrorMode::Off,
        });
        if (result.Succeeded()) {
          photo = result.Value();
          message = std::string{"Photo captured."};
        } else {
          message = result.Error().message;
        }
      });
    }).With(Enabled(status.state == camera::SessionState::Running && !status.capturing_photo)),
    Text(message.Get()),
  };
}
```

The result is an upright JPEG whose resolution is selected by the native camera output. Preview size, fitting, and cropping do not affect the photo. Photo mirroring is independent of preview mirroring and defaults to `Off`. JPEG quality defaults to 90 and must be between 1 and 100; invalid options throw `std::invalid_argument` before returning the task.

`CapturePreviewFrameAsync` accepts the same options and returns a JPEG of the frame the live preview is currently showing. It never changes the capture mode: where the backend can take the newest preview frame directly it does, and it otherwise falls back to the native still-photo output, so resolution follows the preview stream. Use it for an immediate what-you-see-is-what-you-get snapshot; use `CapturePhotoAsync` when the native photo resolution matters. Both share the one-request-at-a-time rule and the same interruption behavior.

Keep the original `ImageAsset` for saving. For photo review on Android, decode a separate thumbnail before displaying it: constraining an `Image` view's layout does not reduce the original bitmap's decoded dimensions. The [example](examples/preview/src/app.cpp) displays a thumbnail with a maximum edge of 2048 pixels while preserving the original JPEG for export.

Only one photo request can be in progress per session. Additional requests return `OperationInProgress`; a session that is not ready returns `NotReady`. `capturing_photo` stays true until native processing finishes, including after the caller cancels waiting. Stopping, switching cameras, backgrounding, or session failure interrupts a pending photo with `Interrupted`.

A photo failure does not stop an otherwise healthy preview. A captured `ImageAsset` remains usable after the session closes. Devices that cannot provide still-photo output report `Unavailable` for photo requests. Video recording, RAW, HDR, maximum sensor resolution, and metadata preservation are outside the current API's guarantees.

## Save a Photo

Capture does not write a file or add an item to the system photo library. Save the JPEG bytes with the SDK's `File` API:

```cpp
Task<bool> WritePhoto(ImageAsset photo, File destination) {
  const auto encoded = photo.EncodedBytes();
  co_return co_await destination.WriteBytesAsync(Bytes(encoded.begin(), encoded.end()));
}
```

The destination must be writable and its parent directory must exist. Writing creates or truncates the file. Obtain application directories with `UseApplication().Directories()` during composition.

To let the user choose a destination, write a temporary JPEG and export it with `FilePicker`:

```cpp
Task<bool> ExportPhoto(ImageAsset photo, File prepared_file, std::shared_ptr<FilePicker> picker) {
  if (!picker->CanSaveFiles()) co_return false;
  if (!co_await WritePhoto(photo, prepared_file)) co_return false;

  SaveFileOptions options{
      .suggested_name = "photo.jpg",
      .filter = {.name = "JPEG image", .extensions = {"jpg"}, .content_types = {"image/jpeg"}},
  };
  co_return co_await picker->SaveFileAsync(prepared_file, options);
}
```

Obtain the picker with `UseService<FilePicker>()` during composition and pass a unique temporary file as `prepared_file`. The task returns false when saving is unavailable, dismissed, or unsuccessful. File operations use SDK file results rather than `CameraError`.

Your application owns temporary-file cleanup. Keep the file available until writing and native export finish; canceling the caller does not make it safe to delete immediately. The example keeps saving in the camera page's task scope so dismissing photo review does not cancel the export. Saving directly to the system photo library is not provided by this module.

## Run the Example

The [preview example](examples/preview) includes permission handling, camera switching, fitting and mirroring controls, a composition grid, simultaneous previews, photo review, and file export.

From `examples/preview`, build and run for your target platform:

```sh
huxerui build android --profile debug
huxerui run android --profile debug
```

Replace `android` with `ios`, `macos`, or `windows` as needed. For Android, use `--java-home /path/to/jdk17` if your default Java installation is unsuitable. Running on an iOS device requires your own signing configuration. The example already includes the Apple camera usage descriptions.

Select **Start camera** to authorize and start the preview, then **Take photo** to capture. The camera icon snapshots the frame the preview is currently showing. The photo icon opens the latest photo and its **Save photo** action. Use the adjacent controls to stop capture, switch cameras, or open preview settings. A failed session offers **Try again**.

On Windows, the backend uses Media Foundation capture and D3D11 preview textures. Camera facing comes from device enclosure metadata; external cameras may have unknown facing and should use the default selection. Still photos use the native photo output and WIC to normalize orientation and JPEG quality.

On Linux, the camera backend uses V4L2 streaming capture and publishes converted frames through a software `PixelTexture`. Preview negotiates the largest raw format the device offers among packed 4:2:2, 4:2:0, and packed RGB layouts; MJPEG serves still photos at the largest MJPEG frame, decoded and re-encoded through gdk-pixbuf (an automatic dependency on Linux). `CapturePreviewFrameAsync` takes the newest converted preview frame without pausing the stream. Facing and mounting rotation come from the optional `V4L2_CID_CAMERA_ORIENTATION` and `V4L2_CID_CAMERA_SENSOR_ROTATION` controls, so most USB webcams report unknown facing. A camera that exposes no MJPEG still output reports `Unavailable` for `CapturePhotoAsync` while `CapturePreviewFrameAsync` and preview stay healthy. Single-planar capture only: multi-planar devices need a driver that also exposes a single-planar node.

The screen source on Linux has two runtime branches. **X11 sessions** get the full path: the viewfinder punches through the window shape (including mouse input, like Windows), and `CapturePhotoAsync` reads the aligned desktop rectangle in one `XGetImage` call with no capture loop. **This X11 path is compiled and smoke-tested under XWayland but has not been verified against a native X11 session.** **Wayland sessions** keep the compositor-transparency viewfinder, which needs no coordinates and no data. `CapturePhotoAsync` opens an `xdg-desktop-portal` ScreenCast session for one monitor and consumes its frames through PipeWire: a locator strip painted on the window makes one stream frame self-localizing, so the desktop pixels behind the viewfinder are cropped without any window-position information. The strip exists only during the capture itself; the first capture in a process may open the compositor's share dialog, and compositors that support persistence accept later captures silently. The photo path needs the optional PipeWire dependency and reports `Unavailable` without it; see the design document.

## Tests

Configure with `-DHUXERUI_CAMERA_BUILD_TESTS=ON` and run `ctest --test-dir <build-directory> --output-on-failure`. On Windows, this includes JPEG corner checks for all stream rotations, all eight EXIF orientations, and final mirroring without opening a camera. On Linux, this includes format selection, corner-marker conversion checks for every supported V4L2 layout, the same photo rotation, EXIF-precedence, and mirroring checks, and screen marker localization, all without opening a camera.

Run `<build-directory>/camera_windows_tests.exe --device` explicitly to check live preview, photos, facing selection, stopping during pending operations, and restart. A camera and camera access are required. The Windows backend was tested with an ACER HD User Facing camera at 1280×720, and the example was manually verified. Device removal, permission revocation during capture, and sustained performance remain unverified.

Run `<build-directory>/camera_linux_tests --device` explicitly for the same live checks through V4L2. The Linux backend was tested with a 1080p USB webcam, and a full GTK application probe confirmed the session end to end, including permission handling. Device removal and sustained performance remain unverified.

See the [public API reference](include/huxerui/camera.h) for complete options and contracts, or the [design document](docs/camera-design.md) for implementation details.
