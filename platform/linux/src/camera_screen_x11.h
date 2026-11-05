#pragma once

#include <functional>
#include <memory>
#include <optional>

#include "camera_internal.h"

namespace huxerui::camera::detail {

/// Reports whether the window is backed by an X11 surface, where the viewfinder can punch
/// through with input passthrough and screen reading can align to the desktop rectangle.
bool LinuxX11ScreenAvailable(UiWindow& window);

/// X11 screen viewfinder state for one window: owns the shape punch-through, keeps the
/// desktop rectangle aligned in physical pixels, and reads single frames for photos.
/// All operations run on the GTK application thread; X11 calls are synchronous.
class LinuxX11Screen final {
public:
  explicit LinuxX11Screen(UiWindow& window);
  ~LinuxX11Screen();

  LinuxX11Screen(const LinuxX11Screen&) = delete;
  LinuxX11Screen& operator=(const LinuxX11Screen&) = delete;

  /// Saves the window shape and enables the viewfinder. Reports Running once the rectangle
  /// is valid, Starting while it is not. Returns false when the X11 backend is unusable.
  bool Activate(std::function<void(CameraStatus)> changed);

  /// Updates the viewfinder rectangle in window-local DIPs.
  void SetRegion(std::optional<Rect> region);

  /// Restores the original window shape and stops reporting.
  void Deactivate();

  /// Reads one aligned desktop frame and encodes it as the requested JPEG.
  void CapturePhoto(PhotoOptions options, std::function<void(CameraResult<ImageAsset>)> completed);

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace huxerui::camera::detail
