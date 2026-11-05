#include "camera_session.h"

#include <utility>

#include <huxerui/app.h>
#include <huxerui/lifecycle.h>
#include <huxerui/state.h>
#include <huxerui/system.h>
#include <huxerui/task.h>

#include "camera_internal.h"

namespace huxerui::camera::detail {

struct PhotoRequest {
  CallbackCompletion<CameraResult<ImageAsset>> completion;
  bool completed = false;

  void Complete(CameraResult<ImageAsset> value) {
    if (!std::exchange(completed, true)) completion.Complete(std::move(value));
  }
};

CameraSessionData::CameraSessionData(State<CameraStatus> value, TaskScope scope, ApplicationHandle app)
    : status(value), tasks(scope), application(app) {}

void CameraSessionData::InterruptPhoto() {
  if (photo) {
    photo->Complete(
        CameraResult<ImageAsset>::Failure({CameraErrorCode::Interrupted, "Photo capture was interrupted."}));
  }
}

void CameraSessionData::BeginPhoto(PhotoOptions options, const std::shared_ptr<PhotoRequest>& request,
                                   bool preview_frame) {
  photo = request;
  auto value = status.Get();
  value.capturing_photo = true;
  status = value;
  options.mirror =
      options.mirror == MirrorMode::On || (options.mirror == MirrorMode::Auto && value.actual_facing == Facing::Front)
          ? MirrorMode::On
          : MirrorMode::Off;
  auto weak = weak_from_this();
  auto complete = [weak, request, scope = tasks](CameraResult<ImageAsset> result) {
    scope.Post([weak, request, result = std::move(result)]() mutable {
      auto owner = weak.lock();
      if (!owner || owner->photo != request)
        return;
      owner->photo.reset();
      auto value = owner->status.Get();
      value.capturing_photo = false;
      owner->status = std::move(value);
      request->Complete(std::move(result));
    });
  };
  try {
    if (preview_frame) {
      backend->CapturePreviewFrame(options, complete);
    } else {
      backend->CapturePhoto(options, complete);
    }
  } catch (const std::exception& error) {
    complete(CameraResult<ImageAsset>::Failure({CameraErrorCode::CaptureFailed, error.what()}));
  }
}

void CameraSessionData::Publish(SessionState state) {
  status = CameraStatus{.state = state,
                        .error = state == SessionState::Failed ? failure : std::nullopt,
                        .capturing_photo = photo && state != SessionState::Closed};
}

void CameraSessionData::Update(CameraOptions options, bool is_background) {
  if (closed) {
    return;
  }
  if (options != desired || (background && !is_background)) {
    failure.reset();
  }
  desired = options;
  background = is_background;
  Reconcile();
}

void CameraSessionData::SetScreenRegion(std::optional<Rect> region) {
  if (closed || screen_region == region) return;
  screen_region = region;
  if (backend && desired.source == CaptureSource::Screen)
    backend->SetScreenRegion(region);
}

void CameraSessionData::Reconcile() {
  if (closed || !backend || status->state == SessionState::Stopping) {
    return;
  }
  if (!desired.active || background || failure ||
      (status->state == SessionState::Starting &&
       (desired.source != opened_source || (desired.source == CaptureSource::Camera && desired.facing != opened_facing))) ||
      (engaged &&
       (desired.source != opened_source || (desired.source == CaptureSource::Camera && desired.facing != opened_facing)))) {
    const auto stopping_generation = ++generation;
    permission_check.Cancel();
    InterruptPhoto();
    if (engaged) {
      engaged = false;
      Publish(SessionState::Stopping);
      const auto weak = weak_from_this();
      backend->Stop([weak, scope = tasks, stopping_generation] {
        scope.Post([weak, stopping_generation] {
          if (auto self = weak.lock(); self && !self->closed && self->generation == stopping_generation) {
            self->Settle();
            self->Reconcile();
          }
        });
      });
    } else {
      Settle();
      if (desired.active && !background && !failure) {
        Reconcile();
      }
    }
    return;
  }
  if (engaged || status->state == SessionState::Starting) {
    return;
  }
  opened_facing = desired.facing;
  opened_source = desired.source;
  Publish(SessionState::Starting);
  const auto opening_generation = ++generation;
  const auto weak = weak_from_this();
  permission_check = tasks.Launch([weak, app = application, opening_generation]() -> Task<void> {
    try {
      PermissionStatus permission = PermissionStatus::Granted;
      if (auto pending = weak.lock(); pending && pending->opened_source == CaptureSource::Camera)
        permission = co_await app.CheckPermissionAsync(Permission::Camera);
      auto self = weak.lock();
      if (!self || self->closed || self->generation != opening_generation) {
        co_return;
      }
      if (permission != PermissionStatus::Granted) {
        CameraErrorCode code = CameraErrorCode::PermissionDenied;
        if (permission == PermissionStatus::NotDetermined) {
          code = CameraErrorCode::PermissionRequired;
        } else if (permission == PermissionStatus::Unavailable) {
          code = CameraErrorCode::Unavailable;
        }
        self->failure = CameraError{code, "Camera access is not authorized or available."};
        self->Settle();
        co_return;
      }
      self->engaged = true;
      auto changed = [weak, scope = self->tasks, opening_generation](CameraStatus value) {
        scope.Post([weak, opening_generation, value = std::move(value)]() mutable {
          auto owner = weak.lock();
          if (!owner || owner->closed || owner->generation != opening_generation) {
            return;
          }
          if (value.error) {
            owner->failure = std::move(value.error);
            owner->Reconcile();
          } else {
            if (value.state == SessionState::Suspended)
              owner->InterruptPhoto();
            value.capturing_photo = static_cast<bool>(owner->photo);
            owner->status = std::move(value);
          }
        });
      };
      if (self->opened_source == CaptureSource::Screen) {
        self->backend->SetScreenRegion(self->screen_region);
        self->backend->StartScreen(std::move(changed));
      } else {
        self->backend->Start(self->opened_facing, std::move(changed));
      }
    } catch (const std::exception& error) {
      if (auto self = weak.lock(); self && !self->closed && self->generation == opening_generation) {
        self->failure = CameraError{CameraErrorCode::CaptureFailed, error.what()};
        self->Reconcile();
      }
    }
  });
}

void CameraSessionData::Settle() {
  Publish(failure ? SessionState::Failed
                  : (desired.active && background ? SessionState::Suspended : SessionState::Stopped));
}

void CameraSessionData::Close() {
  if (closed) {
    return;
  }
  closed = true;
  ++generation;
  permission_check.Cancel();
  InterruptPhoto();
  Publish(SessionState::Closed);
  if (backend) {
    backend->Stop({});
    backend.reset();
  }
}

Task<CameraResult<ImageAsset>> Capture(std::shared_ptr<CameraSessionData> owner, PhotoOptions options,
                                     bool preview_frame) {
  return AwaitCallback<CameraResult<ImageAsset>>(
      [owner = std::move(owner), options, preview_frame](CallbackCompletion<CameraResult<ImageAsset>> completion)
          -> std::function<void()> {
        if (owner->closed || !owner->engaged || !owner->desired.active || owner->background ||
            owner->status->state != SessionState::Running) {
          completion.Complete(CameraResult<ImageAsset>::Failure(
              {CameraErrorCode::NotReady, "Start the camera before taking a photo."}));
        } else if (owner->photo) {
          completion.Complete(CameraResult<ImageAsset>::Failure(
              {CameraErrorCode::OperationInProgress, "A photo capture is already in progress."}));
        } else {
          auto request = std::make_shared<PhotoRequest>(PhotoRequest{completion});
          owner->BeginPhoto(options, request, preview_frame);
        }
        // Task cancellation suppresses delivery; native completion retains the busy slot until cleanup finishes.
        return {};
      });
}

Task<CameraResult<ImageAsset>> CapturePhoto(std::shared_ptr<CameraSessionData> owner, PhotoOptions options) {
  return Capture(std::move(owner), options, false);
}

Task<CameraResult<ImageAsset>> CapturePreviewFrame(std::shared_ptr<CameraSessionData> owner, PhotoOptions options) {
  return Capture(std::move(owner), options, true);
}

} // namespace huxerui::camera::detail

namespace huxerui::camera {

CameraSession::CameraSession(std::shared_ptr<detail::CameraSessionData> data) : data_(std::move(data)) {}

CameraStatus CameraSession::Status() const {
  return data_->status.Get();
}

void CameraSession::Retry() const {
  if (!data_->closed && data_->status->state == SessionState::Failed) {
    data_->failure.reset();
    data_->Reconcile();
  }
}

Task<CameraResult<ImageAsset>> CameraSession::CapturePhotoAsync(PhotoOptions options) const {
  if (options.jpeg_quality < 1 || options.jpeg_quality > 100 ||
      (options.mirror != MirrorMode::Auto && options.mirror != MirrorMode::Off && options.mirror != MirrorMode::On)) {
    throw std::invalid_argument("Invalid photo quality or mirror mode.");
  }
  return detail::CapturePhoto(data_, options);
}

Task<CameraResult<ImageAsset>> CameraSession::CapturePreviewFrameAsync(PhotoOptions options) const {
  if (options.jpeg_quality < 1 || options.jpeg_quality > 100 ||
      (options.mirror != MirrorMode::Auto && options.mirror != MirrorMode::Off && options.mirror != MirrorMode::On)) {
    throw std::invalid_argument("Invalid photo quality or mirror mode.");
  }
  return detail::CapturePreviewFrame(data_, options);
}

CameraSession UseCamera(CameraOptions options, const std::source_location& location) {
  auto status = UseState(CameraStatus{}, location);
  auto tasks = UseTaskScope();
  auto application = UseApplication();
  auto retained = UseState(std::make_shared<detail::CameraSessionData>(status, tasks, application), location);
  auto data = retained.Get();
  const bool background = application.LifecycleState() == ApplicationLifecycleState::Background;
  Lifecycle([data] {
    data->backend = OpenPlatformModule<std::shared_ptr<detail::CameraBackend>>(detail::camera_module_name);
    return [data] { data->Close(); };
  }, data);
  Lifecycle([data, options, background] {
    data->Update(options, background);
    return [] {};
  }, data, options, background);
  return CameraSession(data);
}

} // namespace huxerui::camera
