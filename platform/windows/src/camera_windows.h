#pragma once

#include <memory>
#include <span>

#include <windows.h>

#include "camera_internal.h"

namespace huxerui::camera::detail {

std::shared_ptr<CameraBackend> CreateWindowsCamera(HWND window = nullptr);
std::shared_ptr<CameraBackend> CreateWindowsScreenCamera(HWND window);
ImageAsset EncodeWindowsPhoto(std::span<const std::byte> bytes, PhotoOptions options, PreviewRotation rotation);
ImageAsset EncodeWindowsScreenPhoto(std::span<const std::byte> pixels, unsigned width, unsigned height,
                                    unsigned stride, PhotoOptions options);

} // namespace huxerui::camera::detail
