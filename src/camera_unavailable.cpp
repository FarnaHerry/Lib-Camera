#include "camera_internal.h"

// Linux ships the V4L2 backend under platform/linux; the placeholder only covers
// platforms without a real capture implementation.
#if !defined(__ANDROID__) && !defined(__APPLE__) && !defined(_WIN32) && !defined(__linux__)
namespace huxerui::camera::detail {
namespace {

class UnavailableCamera final : public CameraBackend {
public:
  void Start(std::optional<Facing>, std::function<void(CameraStatus)> changed) override {
    changed({.state = SessionState::Failed,
             .error = CameraError{CameraErrorCode::Unavailable, "Camera capture is not implemented on this platform."}});
  }

  void Stop(std::function<void()> completed) override {
    if (completed) {
      completed();
    }
  }
};

} // namespace

void InstallPlatformCamera(ApplicationContext& root) {
  root.RegisterPlatformModule<std::shared_ptr<CameraBackend>>(
      camera_module_name, [](UiWindow&) -> std::shared_ptr<CameraBackend> {
        return std::make_shared<UnavailableCamera>();
      }
  );
}

} // namespace huxerui::camera::detail
#endif
