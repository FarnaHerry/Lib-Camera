#include <huxerui/camera.h>

#include <cstdint>
#include <utility>

#include <huxerui/app.h>
#include <huxerui/lifecycle.h>
#include <huxerui/state.h>
#include <huxerui/system.h>
#include <huxerui/task.h>

#include "camera_internal.h"

namespace huxerui::camera::detail {

struct CameraSessionData : std::enable_shared_from_this<CameraSessionData> {
  State<CameraStatus> status;
  TaskScope tasks;
  ApplicationHandle application;
  std::shared_ptr<CameraBackend> backend;
  CameraOptions desired;
  std::optional<Facing> opened_facing;
  std::optional<CameraError> failure;
  TaskHandle permission_check;
  std::uint64_t generation = 0;
  bool background = false;
  bool engaged = false;
  bool closed = false;

  CameraSessionData(State<CameraStatus> value, TaskScope scope, ApplicationHandle app)
      : status(value), tasks(scope), application(app) {}

  void Publish(SessionState state) {
    status = CameraStatus{.state = state, .error = state == SessionState::Failed ? failure : std::nullopt};
  }

  void Update(CameraOptions options, bool is_background) {
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

  void Reconcile() {
    if (closed || !backend || status->state == SessionState::Stopping) {
      return;
    }
    if (!desired.active || background || failure ||
        (status->state == SessionState::Starting && desired.facing != opened_facing) ||
        (engaged && desired.facing != opened_facing)) {
      const auto stopping_generation = ++generation;
      permission_check.Cancel();
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
    Publish(SessionState::Starting);
    const auto opening_generation = ++generation;
    const auto weak = weak_from_this();
    permission_check = tasks.Launch([weak, app = application, opening_generation]() -> Task<void> {
      try {
        const auto permission = co_await app.CheckPermissionAsync(Permission::Camera);
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
        self->backend->Start(self->opened_facing, [weak, scope = self->tasks, opening_generation](CameraStatus value) {
          scope.Post([weak, opening_generation, value = std::move(value)]() mutable {
            auto owner = weak.lock();
            if (!owner || owner->closed || owner->generation != opening_generation) {
              return;
            }
            if (value.error) {
              owner->failure = std::move(value.error);
              owner->Reconcile();
            } else {
              owner->status = std::move(value);
            }
          });
        });
      } catch (const std::exception& error) {
        if (auto self = weak.lock(); self && !self->closed && self->generation == opening_generation) {
          self->failure = CameraError{CameraErrorCode::CaptureFailed, error.what()};
          self->Reconcile();
        }
      }
    });
  }

  void Settle() {
    Publish(failure ? SessionState::Failed :
                     (desired.active && background ? SessionState::Suspended : SessionState::Stopped));
  }

  void Close() {
    if (closed) {
      return;
    }
    closed = true;
    ++generation;
    permission_check.Cancel();
    Publish(SessionState::Closed);
    if (backend) {
      backend->Stop({});
      backend.reset();
    }
  }
};

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
