#include "camera_internal.h"
#include "camera_session.h"

// Linux ships the V4L2 backend under platform/linux and shares the texture and
// recording stubs below; other unsupported platforms also use the placeholder.
namespace huxerui::camera::detail {

struct TextureResizer::State {};

TextureResizer::TextureResizer() = default;
TextureResizer::~TextureResizer() = default;
void TextureResizer::Reset() {}

std::optional<Rect> TextureResizer::Resize(PreviewOutput&, std::uint32_t, std::uint32_t) {
  throw CameraError{CameraErrorCode::Unavailable, "Sizing camera textures is not implemented on this platform."};
}

namespace {

#if !defined(__linux__)
class UnavailableCamera final : public CameraBackend {
public:
  bool SupportsFrameProcessing() const noexcept override { return false; }
  void Start(std::optional<Facing>, std::optional<std::string>, CapturePreferences, StatusCallback changed) override {
    changed({.state = SessionState::Failed,
             .error = CameraError{CameraErrorCode::Unavailable, "Camera capture is not implemented on this platform."}},
            full_texture_crop);
  }

  void Stop(std::function<void()> completed) override {
    if (completed) {
      completed();
    }
  }
};
#endif

Task<CameraResult<void>> UnavailableRecording() {
  co_return CameraResult<void>::Failure(
      {CameraErrorCode::Unavailable, "Camera recording is not implemented on this platform."});
}

} // namespace

#if !defined(__linux__)
void InstallPlatformCamera(ApplicationContext& context) {
  context.RegisterPlatformModule<std::shared_ptr<CameraBackend>>(
      camera_module_name, [](UiWindow&) -> std::shared_ptr<CameraBackend> {
        return std::make_shared<UnavailableCamera>();
      }
  );
}
#endif

void FramePipeline::OfferRecordingFrame(const PreviewOutput&, std::chrono::nanoseconds,
                                        std::optional<std::chrono::nanoseconds>) {}

std::chrono::microseconds CameraSessionData::RecordedDuration() const noexcept { return {}; }
void CameraSessionData::SetRecordingPaused(bool) {}
void CameraSessionData::StopRecording() {}

Task<CameraResult<void>> RecordVideo(std::shared_ptr<CameraSessionData>, File,
                                    std::optional<audio::AudioCaptureSession>) {
  return UnavailableRecording();
}

Task<CameraResult<void>> RecordVideo(std::shared_ptr<CameraSessionData>, RandomAccessStream,
                                    std::optional<audio::AudioCaptureSession>) {
  return UnavailableRecording();
}

Task<CameraResult<void>> RecordVideo(std::shared_ptr<CameraSessionData>, OutputStream,
                                    std::optional<audio::AudioCaptureSession>) {
  return UnavailableRecording();
}

} // namespace huxerui::camera::detail
