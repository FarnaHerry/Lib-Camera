#include <huxerui/camera.h>

#include "camera_internal.h"

namespace huxerui::camera {

void Install(ApplicationContext& root) {
  detail::InstallPlatformCamera(root);
}

} // namespace huxerui::camera
