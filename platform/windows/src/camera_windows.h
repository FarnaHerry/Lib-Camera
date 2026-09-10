#pragma once

#include <memory>
#include <span>

#include "camera_internal.h"

namespace huxerui::camera::detail {

std::shared_ptr<CameraBackend> CreateWindowsCamera();
ImageAsset EncodeWindowsPhoto(std::span<const std::byte> bytes, PhotoOptions options, PreviewRotation rotation);

} // namespace huxerui::camera::detail
