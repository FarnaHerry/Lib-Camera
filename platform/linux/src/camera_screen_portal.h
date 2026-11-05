#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

#include "camera_internal.h"

namespace huxerui {
class UiWindow;
}

namespace huxerui::camera::detail {

/// Captures one photo of the desktop pixels behind the viewfinder through the ScreenCast portal
/// and PipeWire. A locator strip painted by the window makes one frame of the live monitor stream
/// self-localizing, so the crop needs no window-position information. The portal handshake, the
/// stream consumption, and the encoding run on a detached worker; the caller invokes this on the
/// GTK application thread. The strip is painted mid-capture from the worker, once the stream can
/// actually deliver the frame that contains it, and cleared again the same way; @p marker_alive
/// is the session's strip lifecycle flag and cancels both paints once it goes false.
void CapturePortalScreenPhoto(UiWindow& window, const Rect& viewfinder, const Rect& marker, double scale_hint,
                              std::shared_ptr<std::atomic_bool> marker_alive, PhotoOptions options,
                              std::function<void(CameraResult<ImageAsset>)> completed);

} // namespace huxerui::camera::detail
