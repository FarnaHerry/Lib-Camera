#include "camera_linux.h"
#include "camera_screen_portal.h"
#include "camera_screen_x11.h"

#include <huxerui/linux/external_texture.h>
#include <huxerui/linux/window.h>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace huxerui::camera::detail {
namespace {

constexpr std::uint32_t kMaxFrameDimension = 8192;
constexpr std::size_t kMaxPhotoBytes = 64U * 1024U * 1024U;
constexpr std::uint32_t kPreviewBufferCount = 4U;
constexpr std::uint32_t kPhotoBufferCount = 3U;
constexpr std::uint32_t kPhotoSettleFrames = 3U;

int Xioctl(int fd, unsigned long request, void* argument) {
  int result = 0;
  do {
    result = ::ioctl(fd, request, argument);
  } while (result != 0 && errno == EINTR);
  return result;
}

CameraError CameraErrno(int error, CameraErrorCode fallback) {
  CameraErrorCode code = fallback;
  switch (error) {
  case EACCES:
    code = CameraErrorCode::PermissionDenied;
    break;
  case EBUSY:
    code = CameraErrorCode::DeviceBusy;
    break;
  case ENODEV:
    code = CameraErrorCode::DeviceDisconnected;
    break;
  case ENOENT:
    code = CameraErrorCode::DeviceNotFound;
    break;
  case EINVAL:
    code = CameraErrorCode::ConfigurationFailed;
    break;
  default:
    break;
  }
  return {code, std::string("Linux camera operation failed (") + std::strerror(error) + ")."};
}

CameraError CameraFailure(CameraErrorCode code, std::string message) {
  return {code, std::move(message)};
}

/// Throws the mapped error when a required V4L2 operation fails.
void RequireIoctl(int fd, unsigned long request, void* argument) {
  if (Xioctl(fd, request, argument) != 0) {
    throw CameraErrno(errno, CameraErrorCode::ConfigurationFailed);
  }
}

std::uint32_t EffectiveCaps(const v4l2_capability& capability) noexcept {
  return capability.capabilities & V4L2_CAP_DEVICE_CAPS ? capability.device_caps : capability.capabilities;
}

std::uint32_t MinRowBytes(std::uint32_t fourcc, std::uint32_t width) noexcept {
  switch (fourcc) {
  case V4L2_PIX_FMT_YUYV:
  case V4L2_PIX_FMT_UYVY:
  case V4L2_PIX_FMT_YVYU:
    return width * 2U;
  case V4L2_PIX_FMT_NV12:
  case V4L2_PIX_FMT_NV21:
    return width;
  case V4L2_PIX_FMT_RGB24:
  case V4L2_PIX_FMT_BGR24:
    return width * 3U;
  case V4L2_PIX_FMT_RGB32:
  case V4L2_PIX_FMT_BGR32:
    return width * 4U;
  default:
    return 0;
  }
}

bool IsPhotoFormat(std::uint32_t fourcc) noexcept {
  return fourcc == V4L2_PIX_FMT_MJPEG || fourcc == V4L2_PIX_FMT_JPEG;
}

std::uint32_t FormatPriority(std::uint32_t fourcc) noexcept {
  static constexpr std::uint32_t priority[] = {V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_UYVY, V4L2_PIX_FMT_YVYU,
                                               V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_NV21, V4L2_PIX_FMT_RGB24,
                                               V4L2_PIX_FMT_BGR24, V4L2_PIX_FMT_RGB32, V4L2_PIX_FMT_BGR32};
  const auto* found = std::find(std::begin(priority), std::end(priority), fourcc);
  return found == std::end(priority) ? static_cast<std::uint32_t>(std::size(priority))
                                     : static_cast<std::uint32_t>(found - std::begin(priority));
}

// ----- software frame conversion, writing packed Bgra8888 rows with straight alpha -----

std::byte ClampByte(int value) noexcept {
  return static_cast<std::byte>(value < 0 ? 0 : (value > 255 ? 255 : value));
}

void WriteBgra(std::byte* destination, int y, int u, int v) noexcept {
  // BT.601 limited range, fixed point scaled by 1 << 10.
  const int adjusted_y = (y - 16) * 1192;
  const int adjusted_u = u - 128;
  const int adjusted_v = v - 128;
  const int red = (adjusted_y + 1634 * adjusted_v) >> 10;
  const int green = (adjusted_y - 400 * adjusted_u - 833 * adjusted_v) >> 10;
  const int blue = (adjusted_y + 2066 * adjusted_u) >> 10;
  destination[0] = ClampByte(blue);
  destination[1] = ClampByte(green);
  destination[2] = ClampByte(red);
  destination[3] = std::byte{0xFF};
}

void ConvertYuyv(const std::byte* source, std::uint32_t stride, std::byte* destination, std::uint32_t width,
                 std::uint32_t height) {
  for (std::uint32_t row = 0; row < height; ++row) {
    const std::byte* line = source + static_cast<std::size_t>(row) * stride;
    std::byte* out = destination + static_cast<std::size_t>(row) * width * 4U;
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto pair = (x & ~1U) * 2U;
      const int y = static_cast<int>(line[pair + (x & 1U) * 2U]);
      const int u = static_cast<int>(line[pair + 1U]);
      const int v = static_cast<int>(line[pair + 3U]);
      WriteBgra(out + static_cast<std::size_t>(x) * 4U, y, u, v);
    }
  }
}

void ConvertUyvy(const std::byte* source, std::uint32_t stride, std::byte* destination, std::uint32_t width,
                 std::uint32_t height) {
  for (std::uint32_t row = 0; row < height; ++row) {
    const std::byte* line = source + static_cast<std::size_t>(row) * stride;
    std::byte* out = destination + static_cast<std::size_t>(row) * width * 4U;
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto pair = (x & ~1U) * 2U;
      const int y = static_cast<int>(line[pair + 1U + (x & 1U) * 2U]);
      const int u = static_cast<int>(line[pair]);
      const int v = static_cast<int>(line[pair + 2U]);
      WriteBgra(out + static_cast<std::size_t>(x) * 4U, y, u, v);
    }
  }
}

void ConvertYvyu(const std::byte* source, std::uint32_t stride, std::byte* destination, std::uint32_t width,
                 std::uint32_t height) {
  for (std::uint32_t row = 0; row < height; ++row) {
    const std::byte* line = source + static_cast<std::size_t>(row) * stride;
    std::byte* out = destination + static_cast<std::size_t>(row) * width * 4U;
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto pair = (x & ~1U) * 2U;
      const int y = static_cast<int>(line[pair + (x & 1U) * 2U]);
      const int u = static_cast<int>(line[pair + 3U]);
      const int v = static_cast<int>(line[pair + 1U]);
      WriteBgra(out + static_cast<std::size_t>(x) * 4U, y, u, v);
    }
  }
}

void ConvertNv(const std::byte* source, std::uint32_t stride, std::byte* destination, std::uint32_t width,
               std::uint32_t height, bool u_first) {
  const std::byte* chroma = source + static_cast<std::size_t>(stride) * height;
  for (std::uint32_t row = 0; row < height; ++row) {
    const std::byte* luma = source + static_cast<std::size_t>(row) * stride;
    const std::byte* line = chroma + static_cast<std::size_t>(row / 2U) * stride;
    std::byte* out = destination + static_cast<std::size_t>(row) * width * 4U;
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto pair = x & ~1U;
      const int y = static_cast<int>(luma[x]);
      const int first = static_cast<int>(line[pair]);
      const int second = static_cast<int>(line[pair + 1U]);
      WriteBgra(out + static_cast<std::size_t>(x) * 4U, y, u_first ? first : second, u_first ? second : first);
    }
  }
}

void ConvertNv12(const std::byte* source, std::uint32_t stride, std::byte* destination, std::uint32_t width,
                 std::uint32_t height) {
  ConvertNv(source, stride, destination, width, height, true);
}

void ConvertNv21(const std::byte* source, std::uint32_t stride, std::byte* destination, std::uint32_t width,
                 std::uint32_t height) {
  ConvertNv(source, stride, destination, width, height, false);
}

void ConvertRgb24(const std::byte* source, std::uint32_t stride, std::byte* destination, std::uint32_t width,
                  std::uint32_t height) {
  for (std::uint32_t row = 0; row < height; ++row) {
    const std::byte* line = source + static_cast<std::size_t>(row) * stride;
    std::byte* out = destination + static_cast<std::size_t>(row) * width * 4U;
    for (std::uint32_t x = 0; x < width; ++x) {
      out[x * 4U] = line[x * 3U + 2U];
      out[x * 4U + 1U] = line[x * 3U + 1U];
      out[x * 4U + 2U] = line[x * 3U];
      out[x * 4U + 3U] = std::byte{0xFF};
    }
  }
}

void ConvertBgr24(const std::byte* source, std::uint32_t stride, std::byte* destination, std::uint32_t width,
                  std::uint32_t height) {
  for (std::uint32_t row = 0; row < height; ++row) {
    const std::byte* line = source + static_cast<std::size_t>(row) * stride;
    std::byte* out = destination + static_cast<std::size_t>(row) * width * 4U;
    for (std::uint32_t x = 0; x < width; ++x) {
      out[x * 4U] = line[x * 3U];
      out[x * 4U + 1U] = line[x * 3U + 1U];
      out[x * 4U + 2U] = line[x * 3U + 2U];
      out[x * 4U + 3U] = std::byte{0xFF};
    }
  }
}

void ConvertRgb32(const std::byte* source, std::uint32_t stride, std::byte* destination, std::uint32_t width,
                  std::uint32_t height) {
  for (std::uint32_t row = 0; row < height; ++row) {
    const std::byte* line = source + static_cast<std::size_t>(row) * stride;
    std::byte* out = destination + static_cast<std::size_t>(row) * width * 4U;
    for (std::uint32_t x = 0; x < width; ++x) {
      out[x * 4U] = line[x * 4U + 2U];
      out[x * 4U + 1U] = line[x * 4U + 1U];
      out[x * 4U + 2U] = line[x * 4U];
      out[x * 4U + 3U] = std::byte{0xFF};
    }
  }
}

void ConvertBgr32(const std::byte* source, std::uint32_t stride, std::byte* destination, std::uint32_t width,
                  std::uint32_t height) {
  for (std::uint32_t row = 0; row < height; ++row) {
    const std::byte* line = source + static_cast<std::size_t>(row) * stride;
    std::byte* out = destination + static_cast<std::size_t>(row) * width * 4U;
    std::memcpy(out, line, static_cast<std::size_t>(width) * 4U);
    for (std::uint32_t x = 0; x < width; ++x) {
      out[x * 4U + 3U] = std::byte{0xFF};
    }
  }
}

/// Serializes status delivery so a producer-thread failure cannot interleave with queue-thread reports.
class StatusSink {
public:
  void Set(std::function<void(CameraStatus)> changed) {
    std::lock_guard lock(mutex_);
    changed_ = std::move(changed);
    reported_ = false;
  }

  void Clear() {
    std::lock_guard lock(mutex_);
    changed_ = {};
  }

  void Publish(CameraStatus status) {
    std::function<void(CameraStatus)> callback;
    {
      std::lock_guard lock(mutex_);
      callback = changed_;
    }
    if (callback) {
      callback(std::move(status));
    }
  }

  /// Reports a failure once; later failures are dropped because the session already saw one cause.
  void Fail(CameraError error) {
    std::function<void(CameraStatus)> callback;
    {
      std::lock_guard lock(mutex_);
      if (changed_ && !reported_) {
        reported_ = true;
        callback = changed_;
      }
    }
    if (callback) {
      callback({.state = SessionState::Failed, .error = std::move(error)});
    }
  }

private:
  std::mutex mutex_;
  std::function<void(CameraStatus)> changed_;
  bool reported_ = false;
};

struct MappedBuffer {
  void* address = nullptr;
  std::size_t size = 0;
};

/// Scoped device handle used while probing candidate nodes.
class DeviceFile {
public:
  explicit DeviceFile(const std::filesystem::path& path) : fd_(::open(path.c_str(), O_RDWR | O_CLOEXEC)) {}
  ~DeviceFile() {
    Close();
  }
  DeviceFile(const DeviceFile&) = delete;
  DeviceFile& operator=(const DeviceFile&) = delete;

  bool Opened() const noexcept {
    return fd_ >= 0;
  }
  [[nodiscard]] int Get() const noexcept {
    return fd_;
  }

private:
  void Close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  int fd_ = -1;
};

/// V4L2 capture state machine. Lifecycle operations serialize on one queue drained by a short-lived
/// worker; only the frame pump touches the device concurrently, and Stop joins it before any queue
/// operation tears resources down, so publication and texture Finish never overlap.
class LinuxCapture final : public std::enable_shared_from_this<LinuxCapture> {
public:
  void Post(std::function<void()> operation) {
    {
      std::lock_guard lock(queue_mutex_);
      queue_.push_back(std::move(operation));
      if (scheduled_) {
        return;
      }
      scheduled_ = true;
    }
    auto owner = std::make_unique<std::shared_ptr<LinuxCapture>>(shared_from_this());
    std::thread(
        [](std::shared_ptr<LinuxCapture>* retained) {
          std::unique_ptr<std::shared_ptr<LinuxCapture>> owner(retained);
          (*owner)->Drain();
        },
        owner.release())
        .detach();
  }

  void Start(std::optional<Facing> facing, std::function<void(CameraStatus)> changed) {
    sink_.Set(std::move(changed));
    try {
      StopStream();
      ShutdownStream();
      CloseDevice();
      auto device = SelectDevice(facing);
      if (!device) {
        throw CameraFailure(CameraErrorCode::DeviceNotFound, "No camera matches the requested facing.");
      }
      fd_ = ::open(device->path.c_str(), O_RDWR | O_CLOEXEC);
      if (fd_ < 0) {
        throw CameraErrno(errno,
                          errno == EACCES ? CameraErrorCode::PermissionDenied : CameraErrorCode::DeviceNotFound);
      }
      facing_ = device->facing;
      rotation_ = static_cast<PreviewRotation>(device->rotation / 90U);
      const auto candidates = EnumerateStreamFormats(fd_);
      preview_format_ = SelectLinuxPreviewFormat(candidates);
      photo_format_ = SelectLinuxPhotoFormat(candidates);
      if (preview_format_.fourcc == 0) {
        throw CameraFailure(CameraErrorCode::Unavailable, "This camera offers no supported preview format.");
      }
      BeginPreview();
      running_ = true;
      sink_.Publish({.state = SessionState::Running,
                     .actual_facing = facing_,
                     .preview = PreviewOutput{.texture = texture_, .rotation = rotation_}});
    } catch (const CameraError& error) {
      RecoverFromFailedStart();
      sink_.Fail(error);
    } catch (const std::exception& error) {
      RecoverFromFailedStart();
      sink_.Fail(CameraFailure(CameraErrorCode::ConfigurationFailed, error.what()));
    }
  }

  void Stop(std::function<void()> completed) {
    running_ = false;
    sink_.Clear();
    StopStream();
    ShutdownStream();
    CloseDevice();
    pixels_.clear();
    pixels_.shrink_to_fit();
    preview_format_ = {};
    photo_format_ = {};
    facing_.reset();
    rotation_ = PreviewRotation::R0;
    if (completed) {
      completed();
    }
  }

  void Photo(PhotoOptions options, std::function<void(CameraResult<ImageAsset>)> completed) {
    if (!running_) {
      completed(CameraResult<ImageAsset>::Failure(
          {CameraErrorCode::NotReady, "Start the camera before taking a photo."}));
      return;
    }
    if (photo_format_.fourcc == 0) {
      completed(CameraResult<ImageAsset>::Failure(
          {CameraErrorCode::Unavailable, "This camera does not expose MJPEG still-photo output."}));
      return;
    }
    bool resumed = false;
    CameraResult<ImageAsset> result =
        CameraResult<ImageAsset>::Failure({CameraErrorCode::CaptureFailed, "Photo capture failed."});
    try {
      PausePreview();
      auto jpeg = CaptureStillFrames();
      ResumePreview();
      resumed = true;
      result = CameraResult<ImageAsset>::Success(EncodeLinuxPhoto(jpeg, options, rotation_));
    } catch (const CameraError& error) {
      result = CameraResult<ImageAsset>::Failure(error);
    } catch (const std::exception& error) {
      result = CameraResult<ImageAsset>::Failure(CameraFailure(CameraErrorCode::CaptureFailed, error.what()));
    }
    if (!resumed) {
      // A photo failure stays local while preview survives; a lost device fails the session instead.
      try {
        ResumePreview();
      } catch (const CameraError& error) {
        sink_.Fail(error);
      } catch (const std::exception& error) {
        sink_.Fail(CameraFailure(CameraErrorCode::ConfigurationFailed, error.what()));
      }
    }
    completed(std::move(result));
  }

  /// Encodes one JPEG from the newest live preview frame. Runs on the serial queue while the
  /// stream keeps running: the pump hands over its next converted frame through a one-shot box.
  void PreviewFrame(PhotoOptions options, std::function<void(CameraResult<ImageAsset>)> completed) {
    if (!running_ || !converter_) {
      completed(CameraResult<ImageAsset>::Failure(
          {CameraErrorCode::NotReady, "Start the camera before taking a photo."}));
      return;
    }
    {
      std::lock_guard lock(frame_mutex_);
      delivered_frame_.clear();
    }
    frame_requested_ = true;
    std::vector<std::byte> frame;
    {
      std::unique_lock lock(frame_mutex_);
      const bool delivered =
          frame_ready_.wait_for(lock, std::chrono::seconds(2), [&] { return !delivered_frame_.empty(); });
      if (delivered) {
        frame = std::move(delivered_frame_);
        delivered_frame_.clear();
      }
    }
    if (frame.empty()) {
      completed(CameraResult<ImageAsset>::Failure(
          {CameraErrorCode::CaptureFailed, "The camera preview did not deliver a frame."}));
      return;
    }
    try {
      completed(CameraResult<ImageAsset>::Success(
          EncodeLinuxPhotoFrame(frame, static_cast<int>(width_), static_cast<int>(height_),
                                static_cast<std::size_t>(width_) * 4U, options, rotation_)));
    } catch (const std::exception& error) {
      completed(CameraResult<ImageAsset>::Failure(CameraFailure(CameraErrorCode::CaptureFailed, error.what())));
    }
  }

private:
  void Drain() noexcept {
    while (true) {
      std::function<void()> operation;
      {
        std::lock_guard lock(queue_mutex_);
        if (queue_.empty()) {
          scheduled_ = false;
          return;
        }
        operation = std::move(queue_.front());
        queue_.pop_front();
      }
      try {
        operation();
      } catch (...) {
      }
    }
  }

  struct LinuxDeviceNode {
    std::filesystem::path path;
    std::uint64_t index = 0;
    std::optional<Facing> facing;
    std::uint32_t rotation = 0;
  };

  static std::vector<LinuxDeviceNode> EnumerateDeviceNodes() {
    std::vector<LinuxDeviceNode> nodes;
    std::error_code ignored;
    for (const auto& entry : std::filesystem::directory_iterator("/dev", ignored)) {
      const auto name = entry.path().filename().string();
      if (name.size() <= 5U || name.compare(0, 5, "video") != 0) {
        continue;
      }
      std::uint64_t index = 0;
      bool numeric = true;
      for (const char digit : name.substr(5)) {
        if (digit < '0' || digit > '9') {
          numeric = false;
          break;
        }
        index = index * 10U + static_cast<std::uint64_t>(digit - '0');
      }
      if (numeric) {
        nodes.push_back({entry.path(), index, {}, 0});
      }
    }
    std::sort(nodes.begin(), nodes.end(), [](const LinuxDeviceNode& first, const LinuxDeviceNode& second) {
      return first.index < second.index;
    });
    return nodes;
  }

  static std::optional<std::int32_t> QueryControl(int fd, std::uint32_t id) {
    v4l2_queryctrl query{};
    query.id = id;
    if (Xioctl(fd, VIDIOC_QUERYCTRL, &query) != 0 || (query.flags & V4L2_CTRL_FLAG_DISABLED) != 0 ||
        query.type == V4L2_CTRL_TYPE_CTRL_CLASS) {
      return {};
    }
    v4l2_control control{};
    control.id = id;
    if (Xioctl(fd, VIDIOC_G_CTRL, &control) != 0) {
      return {};
    }
    return control.value;
  }

  static bool IsCaptureNode(int fd) {
    v4l2_capability capability{};
    if (Xioctl(fd, VIDIOC_QUERYCAP, &capability) != 0) {
      return false;
    }
    const std::uint32_t caps = EffectiveCaps(capability);
    return (caps & V4L2_CAP_VIDEO_CAPTURE) != 0 && (caps & V4L2_CAP_STREAMING) != 0;
  }

  static std::optional<Facing> QueryFacing(int fd) {
    const auto orientation = QueryControl(fd, V4L2_CID_CAMERA_ORIENTATION);
    if (!orientation) {
      return {};
    }
    if (*orientation == V4L2_CAMERA_ORIENTATION_FRONT) {
      return Facing::Front;
    }
    if (*orientation == V4L2_CAMERA_ORIENTATION_BACK) {
      return Facing::Back;
    }
    return {};
  }

  static std::uint32_t QueryRotation(int fd) {
    const auto rotation = QueryControl(fd, V4L2_CID_CAMERA_SENSOR_ROTATION);
    if (rotation && *rotation >= 0 && *rotation <= 270 && *rotation % 90 == 0) {
      return static_cast<std::uint32_t>(*rotation);
    }
    return 0;
  }

  static std::optional<LinuxDeviceNode> SelectDevice(std::optional<Facing> requested) {
    bool access_denied = false;
    std::optional<LinuxDeviceNode> fallback;
    for (auto& candidate : EnumerateDeviceNodes()) {
      DeviceFile file(candidate.path);
      if (!file.Opened()) {
        access_denied |= errno == EACCES;
        continue;
      }
      const int fd = file.Get();
      if (!IsCaptureNode(fd)) {
        continue;
      }
      const auto facing = QueryFacing(fd);
      const auto rotation = QueryRotation(fd);
      const auto formats = EnumerateStreamFormats(fd);
      const bool usable = SelectLinuxPreviewFormat(formats).fourcc != 0 || SelectLinuxPhotoFormat(formats).fourcc != 0;
      if (!usable) {
        continue;
      }
      if (requested) {
        // An explicit facing has no fallback; unknown-facing devices are not substitutes.
        if (facing != requested) {
          continue;
        }
        candidate.facing = facing;
        candidate.rotation = rotation;
        return candidate;
      }
      if (!fallback) {
        candidate.facing = facing;
        candidate.rotation = rotation;
        fallback = std::move(candidate);
      }
    }
    if (!fallback && access_denied) {
      throw CameraFailure(CameraErrorCode::PermissionDenied, "Camera device access was denied.");
    }
    return fallback;
  }

  static std::vector<LinuxStreamFormat> EnumerateStreamFormats(int fd) {
    std::vector<LinuxStreamFormat> formats;
    v4l2_fmtdesc description{};
    description.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    for (description.index = 0; Xioctl(fd, VIDIOC_ENUM_FMT, &description) == 0; ++description.index) {
      v4l2_frmsizeenum sizes{};
      sizes.pixel_format = description.pixelformat;
      for (sizes.index = 0; Xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &sizes) == 0; ++sizes.index) {
        if (sizes.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
          formats.push_back({description.pixelformat, sizes.discrete.width, sizes.discrete.height});
        } else if (sizes.type == V4L2_FRMSIZE_TYPE_STEPWISE || sizes.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
          formats.push_back({description.pixelformat, sizes.stepwise.min_width, sizes.stepwise.min_height});
          formats.push_back({description.pixelformat, sizes.stepwise.max_width, sizes.stepwise.max_height});
          break;
        }
      }
    }
    return formats;
  }

  static v4l2_format MakeFormat(const LinuxStreamFormat& format) {
    v4l2_format requested{};
    requested.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    requested.fmt.pix.width = format.width;
    requested.fmt.pix.height = format.height;
    requested.fmt.pix.pixelformat = format.fourcc;
    requested.fmt.pix.field = V4L2_FIELD_ANY;
    return requested;
  }

  /// Negotiates one stream format. The driver may adjust every field; the returned values are
  /// authoritative. Preview requires a software converter, still capture does not.
  void ConfigureStream(const LinuxStreamFormat& format, bool preview) {
    v4l2_format negotiated = MakeFormat(format);
    RequireIoctl(fd_, VIDIOC_S_FMT, &negotiated);
    if (negotiated.fmt.pix.field != V4L2_FIELD_ANY && negotiated.fmt.pix.field != V4L2_FIELD_NONE) {
      throw CameraFailure(CameraErrorCode::ConfigurationFailed,
                          "This camera exposes an interlaced stream, which preview cannot display.");
    }
    width_ = negotiated.fmt.pix.width;
    height_ = negotiated.fmt.pix.height;
    stride_ = negotiated.fmt.pix.bytesperline;
    current_format_ = {negotiated.fmt.pix.pixelformat, width_, height_};
    converter_ = LinuxConverterFor(negotiated.fmt.pix.pixelformat);
    if (preview && converter_ == nullptr) {
      throw CameraFailure(CameraErrorCode::ConfigurationFailed, "The camera substituted an unsupported format.");
    }
    const std::uint32_t minimum = MinRowBytes(negotiated.fmt.pix.pixelformat, width_);
    if (width_ == 0 || height_ == 0 || stride_ < minimum ||
        (!preview && !IsPhotoFormat(negotiated.fmt.pix.pixelformat))) {
      throw CameraFailure(CameraErrorCode::ConfigurationFailed, "The camera reported an invalid frame geometry.");
    }
    if (preview) {
      pixels_.resize(static_cast<std::size_t>(width_) * height_ * 4U);
    }
  }

  void AllocateBuffers(std::uint32_t count) {
    v4l2_requestbuffers request{};
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    request.count = count;
    RequireIoctl(fd_, VIDIOC_REQBUFS, &request);
    if (request.count < 2U) {
      throw CameraFailure(CameraErrorCode::ConfigurationFailed, "This camera cannot allocate streaming buffers.");
    }
    buffers_.resize(request.count);
    for (std::uint32_t index = 0; index < request.count; ++index) {
      v4l2_buffer buffer{};
      buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buffer.memory = V4L2_MEMORY_MMAP;
      buffer.index = index;
      RequireIoctl(fd_, VIDIOC_QUERYBUF, &buffer);
      auto* address = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buffer.m.offset);
      if (address == MAP_FAILED) {
        throw CameraErrno(errno, CameraErrorCode::ConfigurationFailed);
      }
      buffers_[index] = {address, buffer.length};
      RequireIoctl(fd_, VIDIOC_QBUF, &buffer);
    }
  }

  void FreeBuffers() {
    for (const auto& buffer : buffers_) {
      if (buffer.address != nullptr) {
        munmap(buffer.address, buffer.size);
      }
    }
    buffers_.clear();
    v4l2_requestbuffers request{};
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    Xioctl(fd_, VIDIOC_REQBUFS, &request);
  }

  void StreamOn() {
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    RequireIoctl(fd_, VIDIOC_STREAMON, &type);
  }

  void StreamOff() {
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    RequireIoctl(fd_, VIDIOC_STREAMOFF, &type);
  }

  void BeginPreview() {
    ConfigureStream(preview_format_, true);
    texture_ = std::make_shared<linux::PixelTexture>(Size{static_cast<float>(width_), static_cast<float>(height_)});
    AllocateBuffers(kPreviewBufferCount);
    StreamOn();
    stop_stream_ = false;
    stream_thread_ = std::thread([capture = shared_from_this()] { capture->Pump(); });
  }

  void Pump() noexcept {
    try {
      while (!stop_stream_) {
        pollfd descriptor{fd_, POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, 200);
        if (stop_stream_) {
          break;
        }
        if (ready < 0) {
          if (errno == EINTR) {
            continue;
          }
          sink_.Fail(CameraErrno(errno, CameraErrorCode::CaptureFailed));
          break;
        }
        if (ready == 0) {
          continue;
        }
        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (Xioctl(fd_, VIDIOC_DQBUF, &buffer) != 0) {
          if (errno == EAGAIN) {
            continue;
          }
          sink_.Fail(CameraErrno(errno, errno == EIO || errno == ENODEV ? CameraErrorCode::DeviceDisconnected
                                                                        : CameraErrorCode::CaptureFailed));
          break;
        }
        PublishFrame(buffer);
        Xioctl(fd_, VIDIOC_QBUF, &buffer);
      }
    } catch (...) {
    }
  }

  void PublishFrame(const v4l2_buffer& buffer) {
    const auto& mapped = buffers_[buffer.index];
    const auto* source = static_cast<const std::byte*>(mapped.address);
    const std::size_t required =
        static_cast<std::size_t>(height_ - 1U) * stride_ + MinRowBytes(current_format_.fourcc, width_);
    if (buffer.bytesused < required) {
      return;
    }
    converter_(source, stride_, pixels_.data(), width_, height_);
    texture_->Publish({.pixel_width = static_cast<int>(width_),
                       .pixel_height = static_cast<int>(height_),
                       .bytes_per_row = static_cast<std::size_t>(width_) * 4U,
                       .format = linux::PixelFormat::Bgra8888,
                       .pixels = pixels_});
    DeliverFrameIfRequested();
  }

  /// Hands one converted frame to a waiting snapshot request without disturbing the stream.
  void DeliverFrameIfRequested() {
    if (!frame_requested_.exchange(false, std::memory_order_acq_rel)) {
      return;
    }
    {
      std::lock_guard lock(frame_mutex_);
      delivered_frame_ = pixels_;
    }
    frame_ready_.notify_all();
  }

  void StopStream() {
    stop_stream_ = true;
    if (stream_thread_.joinable()) {
      stream_thread_.join();
    }
  }

  /// Best-effort release used by Stop and failed starts. Runs only after the pump has exited.
  void ShutdownStream() {
    if (fd_ < 0) {
      return;
    }
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    Xioctl(fd_, VIDIOC_STREAMOFF, &type);
    FreeBuffers();
    if (texture_) {
      texture_->Finish();
      texture_.reset();
    }
    converter_ = nullptr;
    width_ = 0;
    height_ = 0;
    stride_ = 0;
    current_format_ = {};
  }

  void CloseDevice() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  void RecoverFromFailedStart() {
    StopStream();
    ShutdownStream();
    CloseDevice();
    running_ = false;
  }

  void PausePreview() {
    // The published PreviewOutput keeps its texture across a photo; only the stream pauses.
    StopStream();
    StreamOff();
    FreeBuffers();
  }

  void ResumePreview() {
    ConfigureStream(preview_format_, true);
    if (!texture_) {
      texture_ = std::make_shared<linux::PixelTexture>(Size{static_cast<float>(width_), static_cast<float>(height_)});
    }
    AllocateBuffers(kPreviewBufferCount);
    StreamOn();
    stop_stream_ = false;
    stream_thread_ = std::thread([capture = shared_from_this()] { capture->Pump(); });
  }

  /// Runs the still stream; the caller configures and releases the photo format around it.
  std::vector<std::byte> CaptureStillFrames() {
    ConfigureStream(photo_format_, false);
    AllocateBuffers(kPhotoBufferCount);
    StreamOn();
    std::vector<std::byte> jpeg;
    for (std::uint32_t frame = 0; frame < kPhotoSettleFrames; ++frame) {
      v4l2_buffer buffer{};
      buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buffer.memory = V4L2_MEMORY_MMAP;
      const int ready = WaitReadable();
      if (ready <= 0) {
        throw CameraFailure(CameraErrorCode::CaptureFailed,
                            ready == 0 ? "The camera stopped producing photo frames."
                                       : "Waiting for a photo frame failed.");
      }
      RequireIoctl(fd_, VIDIOC_DQBUF, &buffer);
      if (frame + 1U == kPhotoSettleFrames) {
        // Later frames give automatic exposure time to settle after the format switch.
        const auto* source = static_cast<const std::byte*>(buffers_[buffer.index].address);
        if (buffer.bytesused < 2U || buffer.bytesused > kMaxPhotoBytes) {
          throw CameraFailure(CameraErrorCode::CaptureFailed, "The camera produced an invalid photo frame.");
        }
        jpeg.assign(source, source + buffer.bytesused);
      }
      RequireIoctl(fd_, VIDIOC_QBUF, &buffer);
    }
    StreamOff();
    FreeBuffers();
    return jpeg;
  }

  int WaitReadable() {
    int ready = 0;
    do {
      pollfd descriptor{fd_, POLLIN, 0};
      ready = ::poll(&descriptor, 1, 5000);
    } while (ready < 0 && errno == EINTR);
    return ready;
  }

  std::mutex queue_mutex_;
  std::deque<std::function<void()>> queue_;
  bool scheduled_ = false;
  StatusSink sink_;
  int fd_ = -1;
  bool running_ = false;
  std::atomic<bool> stop_stream_{false};
  std::thread stream_thread_;
  std::optional<Facing> facing_;
  PreviewRotation rotation_ = PreviewRotation::R0;
  LinuxStreamFormat preview_format_;
  LinuxStreamFormat photo_format_;
  LinuxStreamFormat current_format_;
  LinuxFrameConverter converter_ = nullptr;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  std::uint32_t stride_ = 0;
  std::vector<std::byte> pixels_;
  std::vector<MappedBuffer> buffers_;
  std::shared_ptr<linux::PixelTexture> texture_;
  std::atomic<bool> frame_requested_{false};
  std::mutex frame_mutex_;
  std::condition_variable frame_ready_;
  std::vector<std::byte> delivered_frame_;
};

/// Owns the transparent viewfinder of one window. All operations run on the GTK application
/// thread, where the session calls them; there is no capture loop or frame source behind it.
/// X11 surfaces get the full path: a punched window shape with input passthrough and aligned
/// single-frame photo reads. Other surfaces get the compositor-transparency aperture only.
class LinuxScreenCamera final : public CameraBackend {
public:
  explicit LinuxScreenCamera(UiWindow& window) : window_(window), marker_alive_(std::make_shared<std::atomic_bool>(true)) {
    if (LinuxX11ScreenAvailable(window)) {
      x11_ = std::make_unique<LinuxX11Screen>(window);
    }
  }

  ~LinuxScreenCamera() override {
    RetireMarker();
    if (active_ && x11_) {
      x11_->Deactivate();
    } else if (active_) {
      linux::SetWindowAperture(window_, {});
    }
  }

  void Start(std::optional<Facing>, std::function<void(CameraStatus)> changed) override {
    changed({.state = SessionState::Failed,
             .error = CameraError{CameraErrorCode::Unavailable, "This backend captures the screen only."}});
  }

  void StartScreen(std::function<void(CameraStatus)> changed) override {
    marker_alive_ = std::make_shared<std::atomic_bool>(true);
    active_ = true;
    if (x11_) {
      // The anchor may have laid out before activation; seed the saved rectangle first.
      x11_->SetRegion(region_);
      x11_->Activate(std::move(changed));
      return;
    }
    changed_ = std::move(changed);
    UpdateAperture();
  }

  void SetScreenRegion(std::optional<Rect> region) override {
    if (region_ == region) {
      return;
    }
    region_ = std::move(region);
    if (!active_) {
      return;
    }
    if (x11_) {
      x11_->SetRegion(region_);
    } else {
      UpdateAperture();
    }
  }

  void Stop(std::function<void()> completed) override {
    active_ = false;
    changed_ = {};
    RetireMarker();
    if (x11_) {
      x11_->Deactivate();
    } else {
      linux::SetWindowAperture(window_, {});
    }
    if (completed) {
      completed();
    }
  }

  void CapturePhoto(PhotoOptions options,
                    std::function<void(CameraResult<ImageAsset>)> completed) override {
    if (x11_) {
      x11_->CapturePhoto(options, std::move(completed));
      return;
    }
    // Wayland: one ScreenCast portal stream supplies the desktop pixels behind the transparent
    // viewfinder; a locator strip painted mid-capture makes one stream frame self-localizing.
    // Marker lifecycles stay on this thread.
    if (!active_ || !region_.has_value() || region_->IsEmpty()) {
      completed(CameraResult<ImageAsset>::Failure(
          {CameraErrorCode::NotReady, "Place the screen viewfinder wholly inside one visible display."}));
      return;
    }
    Rect marker = *region_;
    marker.x = marker.x;
    marker.y = marker.y >= 32.0F ? marker.y - 28.0F : marker.y + marker.height + 12.0F;
    marker.width = 64.0F;
    marker.height = 12.0F;
    const double scale = linux::NativeGeometry(window_).scale;
    CapturePortalScreenPhoto(window_, *region_, marker, scale, marker_alive_, options, std::move(completed));
  }

private:
  /// Clears the locator strip and invalidates its cleanup callback.
  void RetireMarker() {
    if (marker_alive_) {
      marker_alive_->store(false, std::memory_order_release);
      linux::SetWindowMarker(window_, {});
    }
  }

  /// A valid region reports Running without any preview; until then selection keeps Starting.
  void UpdateAperture() {
    const bool ready = region_.has_value() && !region_->IsEmpty();
    linux::SetWindowAperture(window_, ready ? region_ : std::nullopt);
    if (changed_) {
      changed_({.state = ready ? SessionState::Running : SessionState::Starting});
    }
  }

  UiWindow& window_;
  std::unique_ptr<LinuxX11Screen> x11_;
  std::optional<Rect> region_;
  std::function<void(CameraStatus)> changed_;
  std::shared_ptr<std::atomic_bool> marker_alive_;
  bool active_ = false;
};

class LinuxCamera final : public CameraBackend {
public:
  explicit LinuxCamera(UiWindow* window)
      : capture_(std::make_shared<LinuxCapture>()),
        screen_(window != nullptr ? CreateLinuxScreenCamera(*window) : nullptr) {}
  ~LinuxCamera() override {
    if (needs_stop_) {
      if (screen_active_) {
        screen_->Stop({});
      } else {
        capture_->Post([capture = capture_] { capture->Stop({}); });
      }
    }
  }

  void Start(std::optional<Facing> facing, std::function<void(CameraStatus)> changed) override {
    screen_active_ = false;
    capture_->Post([capture = capture_, facing, changed = std::move(changed)] { capture->Start(facing, changed); });
    needs_stop_ = true;
  }

  void StartScreen(std::function<void(CameraStatus)> changed) override {
    screen_active_ = true;
    if (screen_) {
      screen_->StartScreen(std::move(changed));
    } else {
      std::move(changed)({.state = SessionState::Failed,
                          .error = CameraError{CameraErrorCode::Unavailable,
                                               "Screen capture requires an attached window."}});
    }
    needs_stop_ = true;
  }

  void SetScreenRegion(std::optional<Rect> region) override {
    if (screen_) {
      screen_->SetScreenRegion(std::move(region));
    }
  }

  void Stop(std::function<void()> completed) override {
    if (screen_active_) {
      screen_->Stop(std::move(completed));
    } else {
      capture_->Post([capture = capture_, completed = std::move(completed)] { capture->Stop(completed); });
    }
    needs_stop_ = false;
  }

  void CapturePhoto(PhotoOptions options,
                    std::function<void(CameraResult<ImageAsset>)> completed) override {
    if (screen_active_) {
      screen_->CapturePhoto(options, std::move(completed));
    } else {
      capture_->Post([capture = capture_, options, completed = std::move(completed)] {
        capture->Photo(options, completed);
      });
    }
  }

  void CapturePreviewFrame(PhotoOptions options,
                           std::function<void(CameraResult<ImageAsset>)> completed) override {
    if (screen_active_) {
      screen_->CapturePhoto(options, std::move(completed));
    } else {
      capture_->Post([capture = capture_, options, completed = std::move(completed)] {
        capture->PreviewFrame(options, completed);
      });
    }
  }

private:
  std::shared_ptr<LinuxCapture> capture_;
  std::shared_ptr<CameraBackend> screen_;
  bool needs_stop_ = false;
  bool screen_active_ = false;
};

LinuxStreamFormat SelectLargest(const std::vector<LinuxStreamFormat>& candidates,
                                const std::function<bool(std::uint32_t)>& accepted, bool prefer_format_priority) {
  LinuxStreamFormat best;
  for (const auto& candidate : candidates) {
    if (!accepted(candidate.fourcc) || candidate.width < 16U || candidate.height < 16U ||
        candidate.width > kMaxFrameDimension || candidate.height > kMaxFrameDimension) {
      continue;
    }
    const auto pixels = static_cast<std::uint64_t>(candidate.width) * candidate.height;
    const auto best_pixels = static_cast<std::uint64_t>(best.width) * best.height;
    const bool better =
        best.fourcc == 0 || pixels > best_pixels ||
        (pixels == best_pixels &&
         (prefer_format_priority ? FormatPriority(candidate.fourcc) < FormatPriority(best.fourcc)
                                 : candidate.fourcc == V4L2_PIX_FMT_MJPEG && best.fourcc != V4L2_PIX_FMT_MJPEG));
    if (better) {
      best = candidate;
    }
  }
  return best;
}

} // namespace

LinuxStreamFormat SelectLinuxPreviewFormat(const std::vector<LinuxStreamFormat>& candidates) {
  return SelectLargest(candidates,
                       [](std::uint32_t fourcc) { return LinuxConverterFor(fourcc) != nullptr; }, true);
}

LinuxStreamFormat SelectLinuxPhotoFormat(const std::vector<LinuxStreamFormat>& candidates) {
  return SelectLargest(candidates, IsPhotoFormat, false);
}

LinuxFrameConverter LinuxConverterFor(std::uint32_t fourcc) {
  switch (fourcc) {
  case V4L2_PIX_FMT_YUYV:
    return ConvertYuyv;
  case V4L2_PIX_FMT_UYVY:
    return ConvertUyvy;
  case V4L2_PIX_FMT_YVYU:
    return ConvertYvyu;
  case V4L2_PIX_FMT_NV12:
    return ConvertNv12;
  case V4L2_PIX_FMT_NV21:
    return ConvertNv21;
  case V4L2_PIX_FMT_RGB24:
    return ConvertRgb24;
  case V4L2_PIX_FMT_BGR24:
    return ConvertBgr24;
  case V4L2_PIX_FMT_RGB32:
    return ConvertRgb32;
  case V4L2_PIX_FMT_BGR32:
    return ConvertBgr32;
  default:
    return nullptr;
  }
}

std::shared_ptr<CameraBackend> CreateLinuxCamera() {
  return std::make_shared<LinuxCamera>(nullptr);
}

std::shared_ptr<CameraBackend> CreateLinuxCamera(UiWindow& window) {
  return std::make_shared<LinuxCamera>(&window);
}

std::shared_ptr<CameraBackend> CreateLinuxScreenCamera(UiWindow& window) {
  return std::make_shared<LinuxScreenCamera>(window);
}

void InstallPlatformCamera(ApplicationContext& root) {
  root.RegisterPlatformModule<std::shared_ptr<CameraBackend>>(
      camera_module_name, [](UiWindow& window) { return CreateLinuxCamera(window); });
}

} // namespace huxerui::camera::detail
