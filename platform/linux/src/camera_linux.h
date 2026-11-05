#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <huxerui/app.h>

#include "camera_internal.h"

typedef struct _GdkPixbuf GdkPixbuf;

namespace huxerui::camera::detail {

/// Camera-only backend used by focused tests; screen selection reports Unavailable.
std::shared_ptr<CameraBackend> CreateLinuxCamera();
/// Full backend for one attached window; screen selection hosts the transparent viewfinder.
std::shared_ptr<CameraBackend> CreateLinuxCamera(UiWindow& window);
std::shared_ptr<CameraBackend> CreateLinuxScreenCamera(UiWindow& window);

/// One streamable V4L2 pixel format and frame size offered by a device.
struct LinuxStreamFormat {
  std::uint32_t fourcc = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

/// Chooses the preview format from the formats a device actually offers.
/// Raw formats convert in software; MJPEG only serves still photos because per-frame
/// JPEG decoding is too costly for preview. Preference is the largest raw frame, with
/// packed 4:2:2 breaking ties because every UVC camera provides it.
[[nodiscard]] LinuxStreamFormat SelectLinuxPreviewFormat(const std::vector<LinuxStreamFormat>& candidates);

/// Chooses the largest MJPEG frame for still capture, or a default-constructed value when absent.
[[nodiscard]] LinuxStreamFormat SelectLinuxPhotoFormat(const std::vector<LinuxStreamFormat>& candidates);

/// Decodes one camera JPEG, normalizes orientation, mirrors it when requested,
/// and re-encodes it at the requested quality. Embedded EXIF orientation takes
/// precedence over the reported sensor rotation.
/// @throws std::runtime_error When decoding or encoding fails.
[[nodiscard]] ImageAsset EncodeLinuxPhoto(std::span<const std::byte> jpeg, PhotoOptions options,
                                          PreviewRotation rotation);

/// Normalizes one borrowed Bgra8888 preview frame (straight alpha) the same way and encodes it
/// at the requested quality. @param bytes_per_row Distance between frame rows, at least width * 4.
/// @throws std::runtime_error When the geometry or encoding fails.
[[nodiscard]] ImageAsset EncodeLinuxPhotoFrame(std::span<const std::byte> pixels, int width, int height,
                                               std::size_t bytes_per_row, PhotoOptions options,
                                               PreviewRotation rotation);

/// Mirrors and encodes one already-cropped RGB pixbuf (for example a compositor screenshot region)
/// at the requested quality. @throws std::runtime_error When the geometry or encoding fails.
[[nodiscard]] ImageAsset EncodeLinuxPhotoPixbuf(GdkPixbuf* pixbuf, PhotoOptions options);

/// Converts one raw frame into tightly packed Bgra8888 rows with straight alpha 0xFF.
/// @param destination Storage of at least width * height * 4 bytes.
using LinuxFrameConverter = void (*)(const std::byte* source, std::uint32_t source_stride, std::byte* destination,
                                     std::uint32_t width, std::uint32_t height);

/// Scans packed BGRx pixels for the exact linux::MarkerColor strip and reports its pixel bounding
/// box when at least 55 percent of the box matches, which rejects coincidental desktop pixels and
/// lone antialiasing fringes. Used to localize the viewfinder inside compositor screen streams.
/// @param bytes_per_row Distance between frame rows, at least width * 4.
[[nodiscard]] bool LocateLinuxScreenMarker(std::span<const std::byte> pixels, int width, int height,
                                           std::size_t bytes_per_row, Rect& found);

/// Reports the converter for a V4L2 four-character code, or null when preview cannot consume it.
[[nodiscard]] LinuxFrameConverter LinuxConverterFor(std::uint32_t fourcc);

} // namespace huxerui::camera::detail
