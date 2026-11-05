#include "camera_linux.h"

#include <gdk-pixbuf/gdk-pixbuf.h>

#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace huxerui::camera::detail {
namespace {

struct PixbufDeleter {
  void operator()(GdkPixbuf* pixbuf) const noexcept {
    if (pixbuf != nullptr) {
      g_object_unref(pixbuf);
    }
  }
};
using PixbufPtr = std::unique_ptr<GdkPixbuf, PixbufDeleter>;

struct StreamDeleter {
  void operator()(GInputStream* stream) const noexcept {
    if (stream != nullptr) {
      g_object_unref(stream);
    }
  }
};
using StreamPtr = std::unique_ptr<GInputStream, StreamDeleter>;

[[noreturn]] void ThrowGdk(GError* error, const char* what) {
  std::string message = std::string("Linux photo processing failed while ") + what;
  if (error != nullptr && error->message != nullptr) {
    message += ": ";
    message += error->message;
  }
  message += ".";
  if (error != nullptr) {
    g_error_free(error);
  }
  throw std::runtime_error(message);
}

void RequireGdk(bool succeeded, GError* error, const char* what) {
  if (!succeeded) {
    ThrowGdk(error, what);
  }
  if (error != nullptr) {
    g_error_free(error);
  }
}

/// Normalizes one decoded image and encodes the requested JPEG.
/// Embedded EXIF orientation takes precedence over the reported sensor rotation; final mirroring
/// is applied after orientation, matching the CameraPreview transform order.
ImageAsset NormalizeAndEncode(PixbufPtr source, PhotoOptions options, PreviewRotation rotation) {
  const char* embedded = gdk_pixbuf_get_option(source.get(), "orientation");
  const bool oriented = embedded != nullptr && std::strcmp(embedded, "1") != 0;
  PixbufPtr upright(std::move(source));
  if (oriented) {
    upright.reset(gdk_pixbuf_apply_embedded_orientation(upright.release()));
  } else {
    const GdkPixbufRotation rotations[] = {GDK_PIXBUF_ROTATE_NONE, GDK_PIXBUF_ROTATE_CLOCKWISE,
                                           GDK_PIXBUF_ROTATE_UPSIDEDOWN, GDK_PIXBUF_ROTATE_COUNTERCLOCKWISE};
    const GdkPixbufRotation required = rotations[static_cast<std::size_t>(rotation)];
    if (required != GDK_PIXBUF_ROTATE_NONE) {
      upright.reset(gdk_pixbuf_rotate_simple(upright.release(), required));
    }
  }
  if (options.mirror == MirrorMode::On) {
    upright.reset(gdk_pixbuf_flip(upright.release(), TRUE));
  }
  const auto width = gdk_pixbuf_get_width(upright.get());
  const auto height = gdk_pixbuf_get_height(upright.get());
  if (width <= 0 || height <= 0) {
    throw std::runtime_error("The camera returned an empty photo.");
  }
  gchar* encoded = nullptr;
  gsize encoded_size = 0;
  // gdk-pixbuf declares these string arrays non-const even though it does not modify them.
  char* option_names[] = {const_cast<char*>("quality"), nullptr};
  const std::string quality = std::to_string(options.jpeg_quality);
  char* option_values[] = {const_cast<char*>(quality.c_str()), nullptr};
  GError* error = nullptr;
  gdk_pixbuf_save_to_bufferv(upright.get(), &encoded, &encoded_size, "jpeg", option_names, option_values, &error);
  RequireGdk(encoded != nullptr, error, "encoding the camera photo");
  const auto* first = reinterpret_cast<const std::byte*>(encoded);
  Bytes bytes(first, first + encoded_size);
  g_free(encoded);
  return ImageAsset::FromEncoded(std::move(bytes));
}

} // namespace

ImageAsset EncodeLinuxPhoto(std::span<const std::byte> jpeg, PhotoOptions options, PreviewRotation rotation) {
  if (jpeg.empty() || jpeg.size() > static_cast<std::size_t>(std::numeric_limits<gssize>::max())) {
    throw std::runtime_error("The camera returned invalid photo data.");
  }
  StreamPtr stream(g_memory_input_stream_new_from_data(const_cast<std::byte*>(jpeg.data()),
                                                       static_cast<gssize>(jpeg.size()), nullptr));
  GError* error = nullptr;
  PixbufPtr decoded(gdk_pixbuf_new_from_stream(stream.get(), nullptr, &error));
  RequireGdk(decoded != nullptr, error, "decoding the camera photo");
  return NormalizeAndEncode(std::move(decoded), options, rotation);
}

ImageAsset EncodeLinuxPhotoFrame(std::span<const std::byte> pixels, int width, int height, std::size_t bytes_per_row,
                                 PhotoOptions options, PreviewRotation rotation) {
  constexpr std::size_t kChannels = 4U;
  if (width <= 0 || height <= 0 || bytes_per_row < static_cast<std::size_t>(width) * kChannels ||
      pixels.size() < static_cast<std::size_t>(height - 1) * bytes_per_row + static_cast<std::size_t>(width) * kChannels) {
    throw std::runtime_error("The camera returned invalid preview frame data.");
  }
  PixbufPtr frame(gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, width, height));
  if (frame == nullptr) {
    throw std::runtime_error("Linux photo processing failed while allocating the photo buffer.");
  }
  const auto rowstride = static_cast<std::size_t>(gdk_pixbuf_get_rowstride(frame.get()));
  auto* destination = gdk_pixbuf_get_pixels(frame.get());
  for (int row = 0; row < height; ++row) {
    const auto* source = pixels.data() + static_cast<std::size_t>(row) * bytes_per_row;
    auto* target = destination + static_cast<std::size_t>(row) * rowstride;
    for (int column = 0; column < width; ++column) {
      target[column * 3U] = static_cast<unsigned char>(source[column * kChannels + 2U]);
      target[column * 3U + 1U] = static_cast<unsigned char>(source[column * kChannels + 1U]);
      target[column * 3U + 2U] = static_cast<unsigned char>(source[column * kChannels]);
    }
  }
  return NormalizeAndEncode(std::move(frame), options, rotation);
}

ImageAsset EncodeLinuxPhotoPixbuf(GdkPixbuf* pixbuf, PhotoOptions options) {
  if (pixbuf == nullptr) {
    throw std::runtime_error("The compositor screenshot is missing.");
  }
  PixbufPtr retained;
  g_object_ref(pixbuf);
  retained.reset(pixbuf);
  return NormalizeAndEncode(std::move(retained), options, PreviewRotation::R0);
}
} // namespace huxerui::camera::detail
