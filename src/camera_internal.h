#pragma once

#include <functional>

#include <huxerui/camera.h>

namespace huxerui::camera::detail {

inline constexpr char camera_module_name[] = "camera/Session";

/// Operations enter on the UI thread; callbacks may originate on a producer queue.
class CameraBackend {
public:
  virtual ~CameraBackend() = default;
  virtual void Start(std::optional<Facing> facing, std::function<void(CameraStatus)> changed) = 0;
  /// Completion follows producer shutdown and texture Finish. An empty callback is allowed.
  virtual void Stop(std::function<void()> completed) = 0;
};

void InstallPlatformCamera(RootContext& root);

struct PreviewPlacement {
  Rect source;
  Rect destination;
  Transform2D transform;
};

[[nodiscard]] std::optional<PreviewPlacement> PlacePreview(
    Size intrinsic, const PreviewOutput& output, PreviewOptions options, std::optional<Facing> facing, Size bounds
);

} // namespace huxerui::camera::detail
