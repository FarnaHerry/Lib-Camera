#include "camera_linux.h"

#include <gdk-pixbuf/gdk-pixbuf.h>

#include <linux/videodev2.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace huxerui;
using namespace huxerui::camera;

namespace {

void Check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void Near(int actual, int expected, int tolerance, const char* message) {
  Check(actual >= expected - tolerance && actual <= expected + tolerance, message);
}

camera::detail::LinuxStreamFormat Format(std::uint32_t fourcc, std::uint32_t width, std::uint32_t height) {
  return {fourcc, width, height};
}

void TestFormatSelection() {
  using namespace camera::detail;
  const auto preview = SelectLinuxPreviewFormat({Format(V4L2_PIX_FMT_YUYV, 640, 480),
                                                 Format(V4L2_PIX_FMT_YUYV, 1280, 720),
                                                 Format(V4L2_PIX_FMT_MJPEG, 1920, 1080)});
  Check(preview.fourcc == V4L2_PIX_FMT_YUYV && preview.width == 1280 && preview.height == 720,
        "Preview must pick the largest raw frame instead of MJPEG.");
  const auto priority = SelectLinuxPreviewFormat(
      {Format(V4L2_PIX_FMT_NV12, 640, 480), Format(V4L2_PIX_FMT_YUYV, 640, 480)});
  Check(priority.fourcc == V4L2_PIX_FMT_YUYV, "Equal sizes must prefer the packed 4:2:2 order.");
  Check(SelectLinuxPreviewFormat({Format(V4L2_PIX_FMT_YUYV, 8, 8), Format(V4L2_PIX_FMT_YUYV, 16384, 16384)}).fourcc == 0,
        "Preview must reject unusable frame sizes.");
  Check(SelectLinuxPreviewFormat({}).fourcc == 0, "Preview without candidates must be empty.");

  const auto photo = SelectLinuxPhotoFormat({Format(V4L2_PIX_FMT_YUYV, 1280, 720),
                                             Format(V4L2_PIX_FMT_MJPEG, 1280, 720),
                                             Format(V4L2_PIX_FMT_MJPEG, 1920, 1080),
                                             Format(V4L2_PIX_FMT_JPEG, 1920, 1080)});
  Check(photo.fourcc == V4L2_PIX_FMT_MJPEG && photo.width == 1920, "Still capture must prefer the largest MJPEG.");
  const auto photo_tie =
      SelectLinuxPhotoFormat({Format(V4L2_PIX_FMT_JPEG, 640, 480), Format(V4L2_PIX_FMT_MJPEG, 640, 480)});
  Check(photo_tie.fourcc == V4L2_PIX_FMT_MJPEG, "Equal still sizes must prefer MJPEG.");
  Check(SelectLinuxPhotoFormat({Format(V4L2_PIX_FMT_YUYV, 1920, 1080)}).fourcc == 0,
        "A camera without MJPEG must report no still output.");
  std::cout << "Linux format selection passed.\n";
}

struct Bgra {
  int blue = 0;
  int green = 0;
  int red = 0;
  int alpha = 0;
};

Bgra PixelAt(const std::vector<std::byte>& converted, std::uint32_t width, std::uint32_t x, std::uint32_t y) {
  const auto offset = (static_cast<std::size_t>(y) * width + x) * 4U;
  return {static_cast<int>(converted[offset]), static_cast<int>(converted[offset + 1]),
          static_cast<int>(converted[offset + 2]), static_cast<int>(converted[offset + 3])};
}

using Convert = camera::detail::LinuxFrameConverter;

/// Fills a 4x2 packed 4:2:2 frame: neutral luma with blue tint on the left pair and red tint on
/// the right pair, using the layout order the four-character code specifies.
std::vector<std::byte> PackedPattern(std::uint32_t fourcc) {
  constexpr std::uint32_t width = 4, height = 2;
  std::vector<std::byte> bytes(static_cast<std::size_t>(width) * height * 2U);
  for (std::uint32_t row = 0; row < height; ++row) {
    auto* line = bytes.data() + static_cast<std::size_t>(row) * width * 2U;
    for (std::uint32_t pair = 0; pair < 2U; ++pair) {
      const bool blue_side = pair == 0;
      const auto y0 = static_cast<std::byte>(blue_side ? 89 : 89);
      const auto y1 = std::byte{89};
      const auto u = static_cast<std::byte>(blue_side ? 240 : 128);
      const auto v = static_cast<std::byte>(blue_side ? 128 : 240);
      std::byte* block = line + pair * 4U;
      if (fourcc == V4L2_PIX_FMT_YUYV) {
        block[0] = y0, block[1] = u, block[2] = y1, block[3] = v;
      } else if (fourcc == V4L2_PIX_FMT_UYVY) {
        block[0] = u, block[1] = y0, block[2] = v, block[3] = y1;
      } else {
        block[0] = y0, block[1] = v, block[2] = y1, block[3] = u;
      }
    }
  }
  return bytes;
}

void CheckChromaTint(const std::vector<std::byte>& converted, const char* message) {
  const Bgra left = PixelAt(converted, 4, 0, 0);
  const Bgra right = PixelAt(converted, 4, 2, 0);
  Check(left.alpha == 255 && right.alpha == 255, "Converters must write opaque alpha.");
  Check(left.blue > 200 && left.red < 150, message);
  Check(right.red > 200 && right.blue < 150, message);
}

void TestConverters() {
  using namespace camera::detail;
  // One luma level per column exposes per-format Y placement through a neutral chroma plane.
  constexpr std::uint32_t width = 4, height = 2;
  const int expected[width] = {0, 254, 85, 170};
  struct PackedCase {
    std::uint32_t fourcc;
    std::vector<std::byte> row;
  };
  // YUYV [Y0,U,Y1,V], UYVY [U,Y0,V,Y1], YVYU [Y0,V,Y1,U]; the column pattern repeats per pair.
  const std::vector<PackedCase> packed_cases = {
      {V4L2_PIX_FMT_YUYV,
       {std::byte{16}, std::byte{128}, std::byte{235}, std::byte{128}, std::byte{89}, std::byte{128},
        std::byte{162}, std::byte{128}}},
      {V4L2_PIX_FMT_UYVY,
       {std::byte{128}, std::byte{16}, std::byte{128}, std::byte{235}, std::byte{128}, std::byte{89},
        std::byte{128}, std::byte{162}}},
      {V4L2_PIX_FMT_YVYU,
       {std::byte{16}, std::byte{128}, std::byte{235}, std::byte{128}, std::byte{89}, std::byte{128},
        std::byte{162}, std::byte{128}}},
  };
  for (const auto& packed : packed_cases) {
    std::vector<std::byte> source = packed.row;
    source.insert(source.end(), packed.row.begin(), packed.row.end());
    std::vector<std::byte> converted(static_cast<std::size_t>(width) * height * 4U);
    LinuxConverterFor(packed.fourcc)(source.data(), width * 2U, converted.data(), width, height);
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto pixel = PixelAt(converted, width, x, 0);
      Near(pixel.red, expected[x], 4, "Packed 4:2:2 luma placement or range conversion is wrong.");
      Check(pixel.alpha == 255, "Converters must write opaque alpha.");
    }
    const auto pattern = PackedPattern(packed.fourcc);
    std::vector<std::byte> tinted(static_cast<std::size_t>(width) * height * 4U);
    LinuxConverterFor(packed.fourcc)(pattern.data(), width * 2U, tinted.data(), width, height);
    CheckChromaTint(tinted, "Packed 4:2:2 chroma placement is wrong.");
  }
  {
    // 4:2:0: one luma plane followed by an interleaved chroma plane with half height.
    std::vector<std::byte> nv12(static_cast<std::size_t>(width) * height * 3U / 2U);
    std::fill(nv12.begin(), nv12.begin() + static_cast<std::ptrdiff_t>(width) * height, std::byte{89});
    nv12[width * height] = std::byte{240};      // blue tint, left pair
    nv12[width * height + 1U] = std::byte{128};
    nv12[width * height + 2U] = std::byte{128}; // red tint, right pair
    nv12[width * height + 3U] = std::byte{240};
    std::vector<std::byte> converted(static_cast<std::size_t>(width) * height * 4U);
    LinuxConverterFor(V4L2_PIX_FMT_NV12)(nv12.data(), width, converted.data(), width, height);
    CheckChromaTint(converted, "NV12 chroma placement is wrong.");
    std::swap(nv12[width * height], nv12[width * height + 1U]);
    std::swap(nv12[width * height + 2U], nv12[width * height + 3U]);
    std::fill(converted.begin(), converted.end(), std::byte{0});
    LinuxConverterFor(V4L2_PIX_FMT_NV21)(nv12.data(), width, converted.data(), width, height);
    CheckChromaTint(converted, "NV21 chroma placement is wrong.");
  }
  {
    // Packed RGB variants only reorder bytes.
    const std::byte rgb24_row[] = {std::byte{255}, std::byte{64}, std::byte{16},  std::byte{10},
                                   std::byte{20},  std::byte{30},  std::byte{255}, std::byte{255},
                                   std::byte{0},   std::byte{200}, std::byte{50},  std::byte{120}};
    std::vector<std::byte> source(rgb24_row, rgb24_row + std::size(rgb24_row));
    source.insert(source.end(), rgb24_row, rgb24_row + std::size(rgb24_row));
    std::vector<std::byte> converted(static_cast<std::size_t>(width) * height * 4U);
    LinuxConverterFor(V4L2_PIX_FMT_RGB24)(source.data(), width * 3U, converted.data(), width, height);
    const auto pixel = PixelAt(converted, width, 0, 0);
    Near(pixel.red, 255, 0, "RGB24 red placement is wrong.");
    Near(pixel.green, 64, 0, "RGB24 green placement is wrong.");
    Near(pixel.blue, 16, 0, "RGB24 blue placement is wrong.");
    Check(PixelAt(converted, width, 1, 0).alpha == 255, "RGB24 alpha must be opaque.");
    for (std::size_t base : {std::size_t{0}, std::size_t{12}}) {
      std::swap(source[base], source[base + 2U]);
      std::swap(source[base + 3U], source[base + 5U]);
      std::swap(source[base + 6U], source[base + 8U]);
      std::swap(source[base + 9U], source[base + 11U]);
    }
    std::vector<std::byte> bgr(static_cast<std::size_t>(width) * height * 4U);
    LinuxConverterFor(V4L2_PIX_FMT_BGR24)(source.data(), width * 3U, bgr.data(), width, height);
    Check(std::equal(converted.begin(), converted.end(), bgr.begin()),
          "BGR24 must produce the same Bgra8888 result as RGB24.");
  }
  std::cout << "Linux frame conversion passed.\n";
}

struct PixbufDeleter {
  void operator()(GdkPixbuf* pixbuf) const noexcept {
    if (pixbuf != nullptr) {
      g_object_unref(pixbuf);
    }
  }
};
using PixbufPtr = std::unique_ptr<GdkPixbuf, PixbufDeleter>;

void RequireGdk(bool succeeded, GError* error, const char* what) {
  if (!succeeded) {
    std::string message = std::string("Photo test failed while ") + what;
    if (error != nullptr && error->message != nullptr) {
      message += ": ";
      message += error->message;
    }
    if (error != nullptr) {
      g_error_free(error);
    }
    throw std::runtime_error(message);
  }
  if (error != nullptr) {
    g_error_free(error);
  }
}

PixbufPtr DecodeJpeg(std::span<const std::byte> jpeg) {
  GInputStream* raw = g_memory_input_stream_new_from_data(const_cast<std::byte*>(jpeg.data()),
                                                          static_cast<gssize>(jpeg.size()), nullptr);
  GError* error = nullptr;
  PixbufPtr decoded(gdk_pixbuf_new_from_stream(raw, nullptr, &error));
  g_object_unref(raw);
  RequireGdk(decoded != nullptr, error, "decoding a test photo");
  return decoded;
}

enum class CornerColor { Red, Green, Blue, White };

CornerColor Classify(int red, int green, int blue) {
  if (red > 150 && green < 100 && blue < 100) {
    return CornerColor::Red;
  }
  if (green > 150 && red < 100 && blue < 100) {
    return CornerColor::Green;
  }
  if (blue > 150 && red < 100 && green < 100) {
    return CornerColor::Blue;
  }
  if (red > 150 && green > 150 && blue > 150) {
    return CornerColor::White;
  }
  throw std::runtime_error("A photo corner did not decode to a recognizable quadrant color.");
}

CornerColor CornerOf(const GdkPixbuf* pixbuf, int x, int y) {
  const int rowstride = gdk_pixbuf_get_rowstride(const_cast<GdkPixbuf*>(pixbuf));
  const auto* base = gdk_pixbuf_get_pixels(const_cast<GdkPixbuf*>(pixbuf));
  const auto* pixel = base + static_cast<std::size_t>(y) * rowstride + static_cast<std::size_t>(x) * 3U;
  return Classify(pixel[0], pixel[1], pixel[2]);
}

/// A 32x16 JPEG with red, green, blue, and white quadrants; the input is never rotated.
std::span<const std::byte> QuadJpeg() {
  static const std::vector<std::byte> jpeg = [] {
    PixbufPtr image(gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 32, 16));
    Check(image != nullptr, "Cannot allocate the test image.");
    const auto fill = [&](int x, int y, unsigned char red, unsigned char green, unsigned char blue) {
      auto* pixels = gdk_pixbuf_get_pixels(image.get());
      const int rowstride = gdk_pixbuf_get_rowstride(image.get());
      auto* pixel = pixels + static_cast<std::size_t>(y) * rowstride + static_cast<std::size_t>(x) * 3U;
      pixel[0] = red, pixel[1] = green, pixel[2] = blue;
    };
    for (int y = 0; y < 16; ++y) {
      for (int x = 0; x < 32; ++x) {
        if (x < 16 && y < 8) {
          fill(x, y, 220, 20, 20);
        } else if (x >= 16 && y < 8) {
          fill(x, y, 20, 220, 20);
        } else if (x < 16) {
          fill(x, y, 20, 20, 220);
        } else {
          fill(x, y, 230, 230, 230);
        }
      }
    }
    gchar* encoded = nullptr;
    gsize size = 0;
    char* names[] = {const_cast<char*>("quality"), nullptr};
    char* values[] = {const_cast<char*>("95"), nullptr};
    GError* error = nullptr;
    gdk_pixbuf_save_to_bufferv(image.get(), &encoded, &size, "jpeg", names, values, &error);
    RequireGdk(encoded != nullptr, error, "encoding the test photo");
    std::vector<std::byte> bytes(reinterpret_cast<std::byte*>(encoded), reinterpret_cast<std::byte*>(encoded) + size);
    g_free(encoded);
    return bytes;
  }();
  return jpeg;
}

std::vector<std::byte> WithExifOrientation(std::span<const std::byte> jpeg, unsigned orientation) {
  // A little-endian EXIF IFD containing only the orientation tag.
  const unsigned char app1[] = {
      0xff, 0xe1, 0x00, 0x22, 'E', 'x', 'i', 'f', 0, 0,
      'I', 'I', 42, 0, 8, 0, 0, 0, 1, 0,
      0x12, 0x01, 3, 0, 1, 0, 0, 0, static_cast<unsigned char>(orientation), 0, 0, 0,
      0, 0, 0, 0};
  std::vector<std::byte> result(jpeg.begin(), jpeg.begin() + 2);
  for (const auto value : app1) {
    result.push_back(std::byte{value});
  }
  result.insert(result.end(), jpeg.begin() + 2, jpeg.end());
  return result;
}

void CheckPhoto(std::span<const std::byte> encoded, std::size_t width, std::size_t height,
                const std::array<CornerColor, 4>& corners) {
  const auto decoded = DecodeJpeg(encoded);
  Check(static_cast<std::size_t>(gdk_pixbuf_get_width(decoded.get())) == width &&
            static_cast<std::size_t>(gdk_pixbuf_get_height(decoded.get())) == height,
        "Photo rotation must produce upright dimensions.");
  const std::array<std::pair<int, int>, 4> centers = {{
      {static_cast<int>(width / 4U), static_cast<int>(height / 4U)},
      {static_cast<int>(3U * width / 4U), static_cast<int>(height / 4U)},
      {static_cast<int>(width / 4U), static_cast<int>(3U * height / 4U)},
      {static_cast<int>(3U * width / 4U), static_cast<int>(3U * height / 4U)},
  }};
  for (std::size_t index = 0; index < 4; ++index) {
    const auto color = CornerOf(decoded.get(), centers[index].first, centers[index].second);
    if (color != corners[index]) {
      std::cerr << "corner " << index << " at " << centers[index].first << ',' << centers[index].second << " got "
                << static_cast<int>(color) << " want " << static_cast<int>(corners[index]) << '\n';
    }
    Check(color == corners[index], "Photo rotation or final mirroring moved the wrong corner.");
  }
}

using camera::detail::EncodeLinuxPhoto;
using camera::detail::EncodeLinuxPhotoFrame;

void TestPhotoEncoding() {
  const auto jpeg = QuadJpeg();
  const CornerColor neutral[4] = {CornerColor::Red, CornerColor::Green, CornerColor::Blue, CornerColor::White};
  for (int rotation = 0; rotation < 4; ++rotation) {
    const std::size_t width = rotation % 2 == 0 ? 32 : 16;
    const std::size_t height = rotation % 2 == 0 ? 16 : 32;
    // Source quadrants map clockwise for each remaining clockwise rotation; final mirroring is
    // applied after orientation, matching the CameraPreview transform order.
    const CornerColor rotated[4] = {neutral[rotation == 0 ? 0 : rotation == 1 ? 2 : rotation == 2 ? 3 : 1],
                                    neutral[rotation == 0 ? 1 : rotation == 1 ? 0 : rotation == 2 ? 2 : 3],
                                    neutral[rotation == 0 ? 2 : rotation == 1 ? 3 : rotation == 2 ? 1 : 0],
                                    neutral[rotation == 0 ? 3 : rotation == 1 ? 1 : rotation == 2 ? 0 : 2]};
    for (bool mirror : {false, true}) {
      const std::array<CornerColor, 4> corners = {
          mirror ? rotated[1] : rotated[0], mirror ? rotated[0] : rotated[1],
          mirror ? rotated[3] : rotated[2], mirror ? rotated[2] : rotated[3]};
      const auto photo =
          EncodeLinuxPhoto(jpeg, {.jpeg_quality = 95, .mirror = mirror ? MirrorMode::On : MirrorMode::Off},
                           static_cast<PreviewRotation>(rotation));
      CheckPhoto(photo.EncodedBytes(), width, height, corners);
    }
  }
  // Embedded EXIF orientation takes precedence over the reported sensor rotation.
  const auto with_exif = WithExifOrientation(jpeg, 6);
  const auto exif_zero = EncodeLinuxPhoto(with_exif, {.mirror = MirrorMode::Off}, PreviewRotation::R0);
  const auto exif_rotated = EncodeLinuxPhoto(with_exif, {.mirror = MirrorMode::Off}, PreviewRotation::R90);
  Check(exif_zero.PixelWidth() == 16 && exif_zero.PixelHeight() == 32,
        "Embedded EXIF orientation must be applied.");
  Check(exif_rotated.PixelWidth() == 16 && exif_rotated.PixelHeight() == 32,
        "EXIF orientation must take precedence over stream rotation.");
  const auto left = DecodeJpeg(exif_zero.EncodedBytes());
  const auto right = DecodeJpeg(exif_rotated.EncodedBytes());
  Check(CornerOf(left.get(), 4, 4) == CornerOf(right.get(), 4, 4) &&
            CornerOf(left.get(), 12, 28) == CornerOf(right.get(), 12, 28),
        "EXIF precedence must not rotate a photo twice.");
  bool rejected = false;
  try {
    (void)EncodeLinuxPhoto(std::span<const std::byte>{}, {}, PreviewRotation::R0);
  } catch (const std::exception&) {
    rejected = true;
  }
  Check(rejected, "Empty photo bytes must be rejected.");
  std::cout << "Linux JPEG encoding, sensor rotations, EXIF precedence and final mirroring passed.\n";
}

/// The 32x16 quadrant pattern as raw Bgra8888 pixels with a padded stride, as a preview frame delivers it.
std::vector<std::byte> QuadFrame(int& width, int& height, std::size_t& bytes_per_row) {
  width = 32;
  height = 16;
  bytes_per_row = 32U * 4U + 16U;
  std::vector<std::byte> pixels(static_cast<std::size_t>(bytes_per_row) * (height - 1) + 32U * 4U);
  const auto fill = [&](int x, int y, std::byte red, std::byte green, std::byte blue) {
    const auto offset = static_cast<std::size_t>(y) * bytes_per_row + static_cast<std::size_t>(x) * 4U;
    pixels[offset] = blue, pixels[offset + 1U] = green, pixels[offset + 2U] = red, pixels[offset + 3U] = std::byte{0xFF};
  };
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      if (x < 16 && y < 8) {
        fill(x, y, std::byte{220}, std::byte{20}, std::byte{20});
      } else if (x >= 16 && y < 8) {
        fill(x, y, std::byte{20}, std::byte{220}, std::byte{20});
      } else if (x < 16) {
        fill(x, y, std::byte{20}, std::byte{20}, std::byte{220});
      } else {
        fill(x, y, std::byte{230}, std::byte{230}, std::byte{230});
      }
    }
  }
  return pixels;
}

void TestPreviewFrameEncoding() {
  int width = 0;
  int height = 0;
  std::size_t bytes_per_row = 0;
  const auto pixels = QuadFrame(width, height, bytes_per_row);
  const CornerColor neutral[4] = {CornerColor::Red, CornerColor::Green, CornerColor::Blue, CornerColor::White};
  for (int rotation = 0; rotation < 4; ++rotation) {
    const std::size_t out_width = rotation % 2 == 0 ? 32 : 16;
    const std::size_t out_height = rotation % 2 == 0 ? 16 : 32;
    const CornerColor rotated[4] = {neutral[rotation == 0 ? 0 : rotation == 1 ? 2 : rotation == 2 ? 3 : 1],
                                    neutral[rotation == 0 ? 1 : rotation == 1 ? 0 : rotation == 2 ? 2 : 3],
                                    neutral[rotation == 0 ? 2 : rotation == 1 ? 3 : rotation == 2 ? 1 : 0],
                                    neutral[rotation == 0 ? 3 : rotation == 1 ? 1 : rotation == 2 ? 0 : 2]};
    for (bool mirror : {false, true}) {
      const std::array<CornerColor, 4> corners = {
          mirror ? rotated[1] : rotated[0], mirror ? rotated[0] : rotated[1],
          mirror ? rotated[3] : rotated[2], mirror ? rotated[2] : rotated[3]};
      const auto photo = EncodeLinuxPhotoFrame(pixels, width, height, bytes_per_row,
                                               {.jpeg_quality = 95, .mirror = mirror ? MirrorMode::On : MirrorMode::Off},
                                               static_cast<PreviewRotation>(rotation));
      CheckPhoto(photo.EncodedBytes(), out_width, out_height, corners);
    }
  }
  bool rejected = false;
  try {
    (void)EncodeLinuxPhotoFrame(pixels, width, height, 4U, {}, PreviewRotation::R0);
  } catch (const std::exception&) {
    rejected = true;
  }
  Check(rejected, "A stride below the frame width must be rejected.");
  std::cout << "Linux preview-frame snapshot encoding passed.\n";
}

void TestScreenMarker() {
  using namespace camera::detail;
  constexpr int width = 320;
  constexpr int height = 200;
  const std::size_t stride = static_cast<std::size_t>(width) * 4U + 24U;
  std::vector<std::byte> pixels(stride * height);
  const auto fill = [&](int x, int y, std::byte blue, std::byte green, std::byte red) {
    const auto offset = static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4U;
    pixels[offset] = blue, pixels[offset + 1U] = green, pixels[offset + 2U] = red;
  };
  // A varied desktop background whose dithered colors never coincide with the locator color.
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      fill(x, y, std::byte((x * 7) & 255), std::byte((y * 5) & 255), std::byte(((x + y) * 3) & 255));
    }
  }
  Rect located;
  Check(!LocateLinuxScreenMarker(pixels, width, height, stride, located),
        "A frame without the strip must not localize.");
  for (int y = 40; y < 52; ++y) {
    for (int x = 100; x < 164; ++x) {
      fill(x, y, std::byte{253}, std::byte{160}, std::byte{1});
    }
  }
  Check(LocateLinuxScreenMarker(pixels, width, height, stride, located),
        "The strip must localize inside the stream frame.");
  Check(located.x == 100.0F && located.y == 40.0F && located.width == 64.0F && located.height == 12.0F,
        "The localized strip rectangle must match the painted rectangle.");
  // A compositor fringe breaks exact-color pixels at the edges while the interior survives.
  for (int x = 100; x < 164; ++x) {
    fill(x, 40, std::byte{1}, std::byte{2}, std::byte{3});
    fill(x, 51, std::byte{1}, std::byte{2}, std::byte{3});
  }
  Check(LocateLinuxScreenMarker(pixels, width, height, stride, located),
        "The strip must survive an antialiased edge.");
  Check(located.y == 41.0F && located.height == 10.0F, "The bounding box must follow the surviving interior.");
  // When too little of the box carries the color the pattern is coincidental, not the strip.
  for (int y = 41; y < 51; ++y) {
    for (int x = 112; x < 152; ++x) {
      fill(x, y, std::byte{1}, std::byte{2}, std::byte{3});
    }
  }
  Check(!LocateLinuxScreenMarker(pixels, width, height, stride, located),
        "A mostly coincidental match must not localize.");
  std::cout << "Linux screen marker localization passed.\n";
}

struct DeviceProbe {
  std::mutex mutex;
  std::condition_variable ready;
  CameraStatus status;
  std::optional<CameraResult<ImageAsset>> photo;
  bool stopped = false;

  template <class Predicate> void Await(Predicate predicate, const char* message) {
    std::unique_lock lock(mutex);
    Check(ready.wait_for(lock, std::chrono::seconds(30), predicate), message);
  }
};

void TestDevice() {
  using namespace std::chrono_literals;
  using namespace camera::detail;
  auto camera = CreateLinuxCamera();
  const auto begin = [&](std::optional<Facing> facing) {
    auto probe = std::make_shared<DeviceProbe>();
    camera->Start(facing, [probe](CameraStatus value) {
      std::lock_guard lock(probe->mutex);
      probe->status = std::move(value);
      probe->ready.notify_all();
    });
    return probe;
  };
  const auto request_stop = [&](const std::shared_ptr<DeviceProbe>& probe) {
    camera->Stop([probe] {
      std::lock_guard lock(probe->mutex);
      probe->stopped = true;
      probe->ready.notify_all();
    });
  };
  const auto stop = [&](const std::shared_ptr<DeviceProbe>& probe) {
    request_stop(probe);
    probe->Await([&] { return probe->stopped; }, "Native camera did not stop.");
  };
  for (int run = 0; run < 2; ++run) {
    auto probe = begin({});
    probe->Await([&] { return probe->status.state == SessionState::Running || probe->status.error.has_value(); },
                 "Native camera did not start.");
    std::shared_ptr<ExternalTexture> texture;
    {
      std::lock_guard lock(probe->mutex);
      if (probe->status.error) {
        throw std::runtime_error(probe->status.error->message);
      }
      Check(probe->status.preview && probe->status.preview->texture, "Running camera must expose its preview.");
      texture = probe->status.preview->texture;
      std::cout << "Native preview: " << texture->IntrinsicSize().width << 'x' << texture->IntrinsicSize().height
                << ", facing="
                << (probe->status.actual_facing ? (*probe->status.actual_facing == Facing::Front ? "front" : "back")
                                                : "unknown")
                << '\n';
    }
    const auto revision = texture->Revision();
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (texture->Revision() <= revision + 2 && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(20ms);
    }
    Check(texture->Revision() > revision + 2, "Native preview did not publish frames.");
    camera->CapturePhoto({.mirror = run == 0 ? MirrorMode::Off : MirrorMode::On},
                         [probe](CameraResult<ImageAsset> result) {
                           std::lock_guard lock(probe->mutex);
                           probe->photo = std::move(result);
                           probe->ready.notify_all();
                         });
    if (run == 1) {
      request_stop(probe);
    }
    probe->Await([&] { return probe->photo.has_value(); }, "Native photo did not complete.");
    {
      std::lock_guard lock(probe->mutex);
      if (probe->photo->Succeeded()) {
        std::cout << "Native photo: " << probe->photo->Value().PixelWidth() << 'x'
                  << probe->photo->Value().PixelHeight() << '\n';
      } else if (probe->photo->Error().code == CameraErrorCode::Unavailable) {
        std::cout << "Native photo unavailable on this device: " << probe->photo->Error().message << '\n';
      } else {
        throw std::runtime_error(probe->photo->Error().message);
      }
    }
    // The live-frame snapshot must not disturb the running stream; a run that stops mid-photo
    // has already released the stream by this point, so only the healthy run snapshots.
    if (run == 0) {
      probe->photo.reset();
      camera->CapturePreviewFrame({.mirror = MirrorMode::Off},
                                  [probe](CameraResult<ImageAsset> result) {
                                    std::lock_guard lock(probe->mutex);
                                    probe->photo = std::move(result);
                                    probe->ready.notify_all();
                                  });
      probe->Await([&] { return probe->photo.has_value(); }, "Native preview-frame snapshot did not complete.");
      {
        std::lock_guard lock(probe->mutex);
        if (!probe->photo->Succeeded()) {
          throw std::runtime_error(probe->photo->Error().message);
        }
        std::cout << "Native snapshot: " << probe->photo->Value().PixelWidth() << 'x'
                  << probe->photo->Value().PixelHeight() << '\n';
      }
    }
    if (run == 0) {
      stop(probe);
    } else {
      probe->Await([&] { return probe->stopped; }, "Stop must finish after native photo processing.");
    }
    const auto final_revision = texture->Revision();
    std::this_thread::sleep_for(100ms);
    Check(texture->Revision() == final_revision, "Preview published after stop completion.");
    if (probe->photo->Succeeded()) {
      Check(!probe->photo->Value().EncodedBytes().empty(), "A photo must remain usable after stop.");
    }
  }
  for (const auto facing : {Facing::Front, Facing::Back}) {
    auto probe = begin(facing);
    probe->Await([&] { return probe->status.state == SessionState::Running || probe->status.error.has_value(); },
                 "Facing selection did not settle.");
    {
      std::lock_guard lock(probe->mutex);
      if (probe->status.error) {
        Check(probe->status.error->code == CameraErrorCode::DeviceNotFound,
              "Missing facing must report DeviceNotFound.");
      } else {
        Check(probe->status.actual_facing == facing,
              "Explicit facing must not select a different camera facing.");
      }
    }
    stop(probe);
  }
  camera.reset();
  std::cout << "Native Linux preview, photos, pending-operation stops, facing selection and restart passed.\n";
}

} // namespace

int main(int argc, char** argv) {
  int result = 0;
  try {
    TestFormatSelection();
    TestConverters();
    TestPhotoEncoding();
    TestPreviewFrameEncoding();
    TestScreenMarker();
    if (argc > 1 && std::string_view(argv[1]) == "--device") {
      TestDevice();
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    result = 1;
  }
  return result;
}
