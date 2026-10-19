#pragma once

#import <AppKit/AppKit.h>

#include <memory>

#include "camera_internal.h"

namespace huxerui::camera::detail {

std::shared_ptr<CameraBackend> CreateMacScreenCamera(NSWindow* window);

} // namespace huxerui::camera::detail
