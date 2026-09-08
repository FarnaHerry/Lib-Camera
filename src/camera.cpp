#include <huxerui/camera.h>

#include "camera_internal.h"

namespace huxerui::camera {

void Install(RootContext& root) {
  detail::InstallPlatformCamera(root);
}

} // namespace huxerui::camera
