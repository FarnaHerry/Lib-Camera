#include "camera_screen_portal.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>

#ifdef HUXERUI_HAVE_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#endif

#include <huxerui/linux/window.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "camera_linux.h"

namespace huxerui::camera::detail {
namespace {

constexpr const char* kPortalService = "org.freedesktop.portal.Desktop";
constexpr const char* kPortalPath = "/org/freedesktop/portal/desktop";
constexpr const char* kScreenCastInterface = "org.freedesktop.portal.ScreenCast";
constexpr const char* kSessionInterface = "org.freedesktop.portal.Session";
constexpr const char* kRequestInterface = "org.freedesktop.portal.Request";
// The first capture in a session may open the compositor's source picker; the timeout covers the
// time the user may spend choosing a monitor.
constexpr guint kPortalTimeoutMs = 60000;
// Screen-cast streams deliver on damage, so waiting out a fully idle desktop must stay bounded.
constexpr std::chrono::milliseconds kFrameTimeout{10000};
constexpr std::chrono::milliseconds kMarkerSettle{250};
// The locator strip color, matching linux::MarkerColor, as separate channels.
constexpr unsigned char kMarkerBlue = 253;
constexpr unsigned char kMarkerGreen = 160;
constexpr unsigned char kMarkerRed = 1;

/// The grant token of the last accepted ScreenCast session in this process. Portal dialogs
/// reappear for every session; replaying the token lets compositors that support persistence
/// accept later captures without asking again. A refused token is dropped by the caller.
std::mutex g_restore_mutex;
std::string g_restore_token;

/// One portal Request outcome. The Response signal carries the portal response code plus its
/// results dictionary; a timeout simply never arrives.
struct PortalResponse {
  bool arrived = false;
  guint32 code = 2;
  GVariant* results = nullptr;

  PortalResponse() = default;
  PortalResponse(const PortalResponse&) = delete;
  PortalResponse& operator=(const PortalResponse&) = delete;
  PortalResponse(PortalResponse&& other) noexcept
      : arrived(other.arrived), code(other.code), results(std::exchange(other.results, nullptr)) {}
  PortalResponse& operator=(PortalResponse&& other) noexcept {
    if (this != &other) {
      if (results != nullptr) {
        g_variant_unref(results);
      }
      arrived = other.arrived;
      code = other.code;
      results = std::exchange(other.results, nullptr);
    }
    return *this;
  }
  ~PortalResponse() {
    if (results != nullptr) {
      g_variant_unref(results);
    }
  }
};

struct RequestWait {
  GMainContext* context = nullptr;
  GMainLoop* loop = nullptr;
  PortalResponse* response = nullptr;
  std::string path;
  guint subscription = 0;
};

void OnRequestResponse(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* object_path,
                       GVariant* parameters, gpointer user_data) {
  auto* wait = static_cast<RequestWait*>(user_data);
  if (wait->response->arrived || wait->path != object_path) {
    return;
  }
  wait->response->arrived = true;
  g_variant_get(parameters, "(u@a{sv})", &wait->response->code, &wait->response->results);
  g_main_loop_quit(wait->loop);
}

/// Calls one portal method that returns a request handle and waits for its Response signal on a
/// private thread-default loop. The Response of a well-formed portal cannot overtake the method
/// reply, so subscribing after the synchronous call returns is safe.
PortalResponse CallPortalRequest(GDBusConnection* connection, const char* interface, const char* method,
                                 GVariant* parameters) {
  PortalResponse response;
  GError* error = nullptr;
  GVariant* handle = g_dbus_connection_call_sync(connection, kPortalService, kPortalPath, interface, method,
                                                 parameters, G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE,
                                                 kPortalTimeoutMs, nullptr, &error);
  if (handle == nullptr) {
    g_error_free(error);
    return response;
  }
  const gchar* returned = nullptr;
  g_variant_get(handle, "(&o)", &returned);
  const std::string request_path = returned;
  g_variant_unref(handle);
  if (request_path.find("/request/") == std::string::npos) {
    return response;
  }

  RequestWait wait;
  wait.path = request_path;
  wait.response = &response;
  wait.context = g_main_context_new();
  g_main_context_push_thread_default(wait.context);
  wait.loop = g_main_loop_new(wait.context, FALSE);
  wait.subscription = g_dbus_connection_signal_subscribe(connection, kPortalService, kRequestInterface, "Response",
                                                         request_path.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                                         OnRequestResponse, &wait, nullptr);
  GSource* timeout = g_timeout_source_new(kPortalTimeoutMs);
  g_source_set_callback(
      timeout,
      [](gpointer user_data) -> gboolean {
        auto* wait = static_cast<RequestWait*>(user_data);
        g_main_loop_quit(wait->loop);
        return G_SOURCE_REMOVE;
      },
      &wait, nullptr);
  g_source_attach(timeout, wait.context);
  g_source_unref(timeout);

  g_main_loop_run(wait.loop);
  g_dbus_connection_signal_unsubscribe(connection, wait.subscription);
  g_main_loop_unref(wait.loop);
  g_main_context_pop_thread_default(wait.context);
  g_main_context_unref(wait.context);
  return response;
}

/// One monitor stream granted by the ScreenCast portal.
struct ScreenCastGrant {
  std::string session;
  guint32 node_id = 0;
  int remote_fd = -1;
};

/// Drops the portal session so the compositor stops tracking it before the shared bus outlives
/// the capture. Failures are not interesting; the session also ends with the process.
void ClosePortalSession(GDBusConnection* connection, const std::string& session) {
  if (session.empty()) {
    return;
  }
  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
  GError* error = nullptr;
  g_dbus_connection_call_sync(connection, kPortalService, session.c_str(), kSessionInterface, "Close", nullptr,
                              nullptr, G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &error);
  if (error != nullptr) {
    g_error_free(error);
  }
}

std::string CaptureToken() {
  static std::atomic<unsigned> sequence{0};
  return "huxeruicamera" + std::to_string(getpid()) + std::to_string(sequence.fetch_add(1));
}

#ifdef HUXERUI_HAVE_PIPEWIRE

/// Runs the ScreenCast portal handshake and opens the PipeWire remote for the first granted
/// monitor stream. Stores the returned restore token for later captures.
bool OpenScreenCastRemote(GDBusConnection* connection, ScreenCastGrant& grant, std::string& error) {
  std::string restore;
  {
    std::lock_guard lock(g_restore_mutex);
    restore = g_restore_token;
  }

  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(CaptureToken().c_str()));
  g_variant_builder_add(&options, "{sv}", "session_handle_token",
                        g_variant_new_string(("huxeruicamera" + std::to_string(getpid())).c_str()));
  PortalResponse created = CallPortalRequest(connection, kScreenCastInterface, "CreateSession",
                                             g_variant_new("(a{sv})", &options));
  const gchar* session_handle = nullptr;
  if (!created.arrived || created.code != 0 || created.results == nullptr ||
      !g_variant_lookup(created.results, "session_handle", "&o", &session_handle)) {
    error = "The screen capture session could not be created.";
    return false;
  }
  grant.session = session_handle;

  const auto select_sources = [&](const std::string& restore_token) {
    GVariantBuilder sources;
    g_variant_builder_init(&sources, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&sources, "{sv}", "handle_token", g_variant_new_string(CaptureToken().c_str()));
    g_variant_builder_add(&sources, "{sv}", "types", g_variant_new_uint32(1));        // monitors only
    g_variant_builder_add(&sources, "{sv}", "multiple", g_variant_new_boolean(FALSE));
    g_variant_builder_add(&sources, "{sv}", "cursor_mode", g_variant_new_uint32(1));  // hidden cursor
    g_variant_builder_add(&sources, "{sv}", "persist_mode", g_variant_new_uint32(2));
    if (!restore_token.empty()) {
      g_variant_builder_add(&sources, "{sv}", "restore_token", g_variant_new_string(restore_token.c_str()));
    }
    return CallPortalRequest(connection, kScreenCastInterface, "SelectSources",
                             g_variant_new("(oa{sv})", grant.session.c_str(), &sources));
  };
  PortalResponse selected = select_sources(restore);
  if ((!selected.arrived || selected.code != 0) && !restore.empty()) {
    // A stale token can be refused outright; one clean retry keeps later captures working.
    std::lock_guard lock(g_restore_mutex);
    g_restore_token.clear();
    selected = select_sources({});
  }
  if (!selected.arrived) {
    error = "The screen share request timed out.";
    return false;
  }
  if (selected.code != 0) {
    error = "The screen share request was declined.";
    return false;
  }

  GVariantBuilder start;
  g_variant_builder_init(&start, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&start, "{sv}", "handle_token", g_variant_new_string(CaptureToken().c_str()));
  PortalResponse started = CallPortalRequest(connection, kScreenCastInterface, "Start",
                                             g_variant_new("(osa{sv})", grant.session.c_str(), "", &start));
  if (!started.arrived) {
    error = "The screen share request timed out.";
    return false;
  }
  if (started.code != 0) {
    error = "The screen share request was declined.";
    return false;
  }
  if (started.results != nullptr) {
    const gchar* granted = nullptr;
    if (g_variant_lookup(started.results, "restore_token", "&s", &granted)) {
      std::lock_guard lock(g_restore_mutex);
      g_restore_token = granted;
    }
    GVariant* streams = nullptr;
    if (g_variant_lookup(started.results, "streams", "@a(ua{sv})", &streams)) {
      if (g_variant_n_children(streams) > 0) {
        GVariant* first = g_variant_get_child_value(streams, 0);
        g_variant_get_child(first, 0, "u", &grant.node_id);
        g_variant_unref(first);
      }
      g_variant_unref(streams);
    }
  }
  if (grant.node_id == 0) {
    error = "The compositor granted no monitor stream.";
    return false;
  }

  GVariantBuilder remote;
  g_variant_builder_init(&remote, G_VARIANT_TYPE_VARDICT);
  GUnixFDList* fd_list = nullptr;
  GError* fd_error = nullptr;
  GVariant* reply = g_dbus_connection_call_with_unix_fd_list_sync(
      connection, kPortalService, kPortalPath, kScreenCastInterface, "OpenPipeWireRemote",
      g_variant_new("(oa{sv})", grant.session.c_str(), &remote), G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE,
      kPortalTimeoutMs, nullptr, &fd_list, nullptr, &fd_error);
  if (reply == nullptr) {
    g_error_free(fd_error);
    error = "The compositor did not provide the screen stream.";
    return false;
  }
  gint index = -1;
  g_variant_get(reply, "(h)", &index);
  g_variant_unref(reply);
  grant.remote_fd = fd_list != nullptr ? g_unix_fd_list_get(fd_list, index, nullptr) : -1;
  if (fd_list != nullptr) {
    g_object_unref(fd_list);
  }
  if (grant.remote_fd < 0) {
    error = "The compositor did not provide the screen stream.";
    return false;
  }
  return true;
}

/// One frame snapshot the localization works on.
struct CapturedFrame {
  std::vector<std::byte> pixels;
  int width = 0;
  int height = 0;
  std::size_t stride = 0;
  Rect located;
};

/// Consumes the newest frame of one ScreenCast node over the portal remote. The PipeWire thread
/// loop owns the connection; capture threads only read the guarded latest frame.
class CastStream {
public:
  CastStream() = default;
  CastStream(const CastStream&) = delete;
  CastStream& operator=(const CastStream&) = delete;

  ~CastStream() {
    if (loop_ != nullptr) {
      pw_thread_loop_lock(loop_);
      if (stream_ != nullptr) {
        pw_stream_destroy(stream_);
      }
      if (core_ != nullptr) {
        pw_core_disconnect(core_);
      }
      if (context_ != nullptr) {
        pw_context_destroy(context_);
      }
      pw_thread_loop_unlock(loop_);
      pw_thread_loop_stop(loop_);
      pw_thread_loop_destroy(loop_);
    }
    pw_deinit();
  }

  /// Takes ownership of @p remote_fd on every path.
  bool Connect(int remote_fd, guint32 node_id, std::string& error) {
    pw_init(nullptr, nullptr);
    loop_ = pw_thread_loop_new("huxerui-screen-cast", nullptr);
    if (loop_ == nullptr || pw_thread_loop_start(loop_) != 0) {
      ::close(remote_fd);
      error = "The screen stream loop could not start.";
      return false;
    }
    pw_thread_loop_lock(loop_);
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (context_ == nullptr) {
      pw_thread_loop_unlock(loop_);
      ::close(remote_fd);
      error = "The screen stream context could not be created.";
      return false;
    }
    // The context owns the remote descriptor from here on.
    core_ = pw_context_connect_fd(context_, remote_fd, nullptr, 0);
    if (core_ == nullptr) {
      pw_thread_loop_unlock(loop_);
      error = "The screen stream remote could not connect.";
      return false;
    }
    auto* properties = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                                         PW_KEY_MEDIA_ROLE, "Screen", nullptr);
    stream_ = pw_stream_new(core_, "huxerui-screen-photo", properties);
    if (stream_ == nullptr) {
      pw_thread_loop_unlock(loop_);
      error = "The screen stream could not be created.";
      return false;
    }
    pw_stream_add_listener(stream_, &listener_, &kStreamEvents, this);

    // Plain packed BGRx without DMA-BUF modifiers keeps every buffer mapped memory the capture
    // thread can read directly.
    std::uint8_t pod_storage[512];
    spa_pod_builder pod_builder = SPA_POD_BUILDER_INIT(pod_storage, sizeof(pod_storage));
    spa_video_info_raw requested{};
    requested.format = SPA_VIDEO_FORMAT_BGRx;
    const spa_pod* params[2];
    params[0] = spa_format_video_raw_build(&pod_builder, SPA_PARAM_EnumFormat, &requested);
    requested.format = SPA_VIDEO_FORMAT_BGRA;
    params[1] = spa_format_video_raw_build(&pod_builder, SPA_PARAM_EnumFormat, &requested);
    const int connected = pw_stream_connect(stream_, PW_DIRECTION_INPUT, node_id,
                                            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                                          PW_STREAM_FLAG_MAP_BUFFERS),
                                            params, 2);
    pw_thread_loop_unlock(loop_);
    if (connected != 0) {
      error = "The screen stream could not attach to the granted monitor.";
      return false;
    }
    return true;
  }

  /// Waits until the stream produced any frame, so a later marker paint is guaranteed to cause a
  /// fresh one even on a static desktop.
  bool AwaitFirstFrame(const std::atomic_bool& cancel, std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + kFrameTimeout;
    std::unique_lock lock(mutex_);
    const bool arrived = ready_.wait_until(lock, deadline, [&] { return frames_ > 0 || !failure_.empty(); });
    if (!failure_.empty()) {
      error = failure_;
      return false;
    }
    if (!arrived || cancel.load(std::memory_order_acquire)) {
      error = "The compositor stream did not deliver any frame.";
      return false;
    }
    return true;
  }

  /// Scans every frame that arrives after the call until the locator strip appears. Returns the
  /// matching frame snapshot with the strip position in stream pixels.
  bool AwaitMarker(const std::atomic_bool& cancel, CapturedFrame& captured, std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + kFrameTimeout;
    CapturedFrame frame;
    std::uint64_t scanned = 0;
    while (!cancel.load(std::memory_order_acquire)) {
      {
        std::lock_guard lock(mutex_);
        if (!failure_.empty()) {
          error = failure_;
          return false;
        }
        if (frames_ != scanned) {
          scanned = frames_;
          frame.pixels = latest_;
          frame.width = width_;
          frame.height = height_;
          frame.stride = stride_;
        }
      }
      if (!frame.pixels.empty() && LocateLinuxScreenMarker(frame.pixels, frame.width, frame.height, frame.stride,
                                                           frame.located)) {
        captured = std::move(frame);
        return true;
      }
      frame.pixels.clear();
      frame.pixels.shrink_to_fit();
      std::unique_lock lock(mutex_);
      if (!ready_.wait_until(lock, deadline, [&] { return frames_ != scanned || !failure_.empty(); })) {
        break;
      }
    }
    error = "The locator strip was not found in the captured stream; the viewfinder display may not be "
            "the one the portal captured.";
    return false;
  }

private:
  static void OnStateChanged(void* data, pw_stream_state, pw_stream_state state, const char* message) {
    auto* self = static_cast<CastStream*>(data);
    if (state != PW_STREAM_STATE_ERROR) {
      return;
    }
    std::lock_guard lock(self->mutex_);
    self->failure_ = message != nullptr ? std::string("The screen stream failed: ") + message
                                        : std::string("The screen stream failed.");
    self->ready_.notify_all();
  }

  static void OnParamChanged(void* data, std::uint32_t id, const struct spa_pod* param) {
    auto* self = static_cast<CastStream*>(data);
    if (id != SPA_PARAM_Format || param == nullptr) {
      return;
    }
    spa_video_info_raw format{};
    std::lock_guard lock(self->mutex_);
    if (spa_format_video_raw_parse(param, &format) < 0) {
      self->failure_ = "The screen stream format could not be read.";
    } else if (format.format != SPA_VIDEO_FORMAT_BGRx && format.format != SPA_VIDEO_FORMAT_BGRA) {
      self->failure_ = "The compositor delivered an unsupported pixel format.";
    } else {
      self->width_ = static_cast<int>(format.size.width);
      self->height_ = static_cast<int>(format.size.height);
    }
    self->ready_.notify_all();
  }

  static void OnProcess(void* data) {
    auto* self = static_cast<CastStream*>(data);
    pw_buffer* buffered = pw_stream_dequeue_buffer(self->stream_);
    if (buffered == nullptr) {
      return;
    }
    const spa_buffer* raw = buffered->buffer;
    int width = 0;
    int height = 0;
    {
      std::lock_guard lock(self->mutex_);
      width = self->width_;
      height = self->height_;
    }
    if (raw->n_datas > 0 && raw->datas[0].data != nullptr && width > 0 && height > 0) {
      const auto* bytes = static_cast<const std::byte*>(raw->datas[0].data) + raw->datas[0].chunk->offset;
      const std::size_t stride = static_cast<std::size_t>(raw->datas[0].chunk->stride);
      const std::size_t needed = stride * static_cast<std::size_t>(height);
      if (stride > 0 && raw->datas[0].chunk->size >= needed) {
        std::lock_guard lock(self->mutex_);
        self->latest_.resize(needed);
        std::memcpy(self->latest_.data(), bytes, needed);
        self->stride_ = stride;
        ++self->frames_;
        self->ready_.notify_all();
      }
    }
    pw_stream_queue_buffer(self->stream_, buffered);
  }

  static const pw_stream_events kStreamEvents;

  pw_thread_loop* loop_ = nullptr;
  pw_context* context_ = nullptr;
  pw_core* core_ = nullptr;
  pw_stream* stream_ = nullptr;
  spa_hook listener_{};
  std::mutex mutex_;
  std::condition_variable ready_;
  std::vector<std::byte> latest_;
  int width_ = 0;
  int height_ = 0;
  std::size_t stride_ = 0;
  std::uint64_t frames_ = 0;
  std::string failure_;
};

const pw_stream_events CastStream::kStreamEvents = {.version = PW_VERSION_STREAM_EVENTS,
                                                    .state_changed = OnStateChanged,
                                                    .param_changed = OnParamChanged,
                                                    .process = OnProcess};

#endif // HUXERUI_HAVE_PIPEWIRE

} // namespace

bool LocateLinuxScreenMarker(std::span<const std::byte> pixels, int width, int height, std::size_t bytes_per_row,
                             Rect& found) {
  if (width <= 0 || height <= 0 || bytes_per_row < static_cast<std::size_t>(width) * 4U ||
      pixels.size() < bytes_per_row * static_cast<std::size_t>(height - 1) + static_cast<std::size_t>(width) * 4U) {
    return false;
  }
  int min_x = width;
  int min_y = height;
  int max_x = -1;
  int max_y = -1;
  std::size_t matches = 0;
  for (int y = 0; y < height; ++y) {
    const std::byte* line = pixels.data() + static_cast<std::size_t>(y) * bytes_per_row;
    for (int x = 0; x < width; ++x) {
      if (line[x * 4U] == std::byte{kMarkerBlue} && line[x * 4U + 1U] == std::byte{kMarkerGreen} &&
          line[x * 4U + 2U] == std::byte{kMarkerRed}) {
        ++matches;
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
        min_y = std::min(min_y, y);
        max_y = std::max(max_y, y);
      }
    }
  }
  if (max_x < 0) {
    return false;
  }
  found = Rect{static_cast<float>(min_x), static_cast<float>(min_y),
               static_cast<float>(max_x - min_x + 1), static_cast<float>(max_y - min_y + 1)};
  const double area = static_cast<double>(found.width) * found.height;
  // A coincidental desktop pixel or an antialiasing fringe must not satisfy the search alone.
  return matches >= area * 0.55;
}

namespace {

/// Paints or clears the locator strip on the application thread. The alive flag drops the request
/// once the camera session stopped caring about this window.
void PostMarkerRequest(UiWindow* window, const std::shared_ptr<std::atomic_bool>& alive, std::optional<Rect> region) {
  struct MarkerRequest {
    UiWindow* window;
    std::shared_ptr<std::atomic_bool> alive;
    std::optional<Rect> region;
  };
  auto* request = new MarkerRequest{window, alive, std::move(region)};
  g_main_context_invoke(
      nullptr,
      [](gpointer data) -> int {
        std::unique_ptr<MarkerRequest> request(static_cast<MarkerRequest*>(data));
        if (request->alive->load(std::memory_order_acquire)) {
          linux::SetWindowMarker(*request->window, request->region);
        }
        return G_SOURCE_REMOVE;
      },
      request);
}

#ifdef HUXERUI_HAVE_PIPEWIRE

/// Crops the viewfinder rectangle out of the captured frame using the strip position and its
/// measured device-pixel scale, then encodes the crop as the photo.
CameraResult<ImageAsset> CropAndEncode(const CapturedFrame& frame, const Rect& viewfinder, const Rect& marker,
                                       double scale_hint, const PhotoOptions& options) {
  // The strip doubles as the scale ruler: its on-stream extent versus its DIP extent.
  const double measured_scale =
      marker.width > 0.0F ? frame.located.width / marker.width : (scale_hint > 0.0 ? scale_hint : 1.0);
  const Rect crop{frame.located.x + (viewfinder.x - marker.x) * static_cast<float>(measured_scale),
                  frame.located.y + (viewfinder.y - marker.y) * static_cast<float>(measured_scale),
                  viewfinder.width * static_cast<float>(measured_scale),
                  viewfinder.height * static_cast<float>(measured_scale)};
  const int crop_x = static_cast<int>(crop.x);
  const int crop_y = static_cast<int>(crop.y);
  const int crop_width = static_cast<int>(crop.width);
  const int crop_height = static_cast<int>(crop.height);
  if (crop_x < 0 || crop_y < 0 || crop_width <= 0 || crop_height <= 0 || crop_x + crop_width > frame.width ||
      crop_y + crop_height > frame.height) {
    return CameraResult<ImageAsset>::Failure(
        {CameraErrorCode::CaptureFailed, "The viewfinder rectangle left the captured display."});
  }
  const std::size_t row_bytes = static_cast<std::size_t>(crop_width) * 4U;
  std::vector<std::byte> packed(row_bytes * static_cast<std::size_t>(crop_height));
  for (int row = 0; row < crop_height; ++row) {
    const auto* source = frame.pixels.data() + static_cast<std::size_t>(crop_y + row) * frame.stride +
                         static_cast<std::size_t>(crop_x) * 4U;
    std::memcpy(packed.data() + static_cast<std::size_t>(row) * row_bytes, source, row_bytes);
  }
  try {
    return CameraResult<ImageAsset>::Success(
        EncodeLinuxPhotoFrame(packed, crop_width, crop_height, row_bytes, options, PreviewRotation::R0));
  } catch (const std::exception& encode_error) {
    return CameraResult<ImageAsset>::Failure(CameraError{CameraErrorCode::CaptureFailed, encode_error.what()});
  }
}

CameraResult<ImageAsset> RunScreenCastCapture(UiWindow* window, const std::shared_ptr<std::atomic_bool>& alive,
                                              const Rect& viewfinder, const Rect& marker, double scale_hint,
                                              const PhotoOptions& options) {
  GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
  if (connection == nullptr) {
    return CameraResult<ImageAsset>::Failure({CameraErrorCode::Unavailable, "The session portal is not reachable."});
  }
  CameraResult<ImageAsset> result = CameraResult<ImageAsset>::Failure(
      {CameraErrorCode::CaptureFailed, "The screen capture did not produce a photo."});
  ScreenCastGrant grant;
  std::string error;
  if (!OpenScreenCastRemote(connection, grant, error)) {
    result = CameraResult<ImageAsset>::Failure({CameraErrorCode::Unavailable, error});
  } else {
    CastStream stream;
    if (!stream.Connect(grant.remote_fd, grant.node_id, error)) {
      result = CameraResult<ImageAsset>::Failure({CameraErrorCode::Unavailable, error});
    } else if (!stream.AwaitFirstFrame(*alive, error)) {
      result = CameraResult<ImageAsset>::Failure({CameraErrorCode::CaptureFailed, error});
    } else {
      // The strip is painted only once the stream consumes frames, so its damage is guaranteed to
      // produce the frame the localization scans for, even on a static desktop.
      PostMarkerRequest(window, alive, marker);
      std::this_thread::sleep_for(kMarkerSettle);
      CapturedFrame frame;
      if (!stream.AwaitMarker(*alive, frame, error)) {
        result = CameraResult<ImageAsset>::Failure({CameraErrorCode::CaptureFailed, error});
      } else {
        result = CropAndEncode(frame, viewfinder, marker, scale_hint, options);
      }
    }
  }
  ClosePortalSession(connection, grant.session);
  g_object_unref(connection);
  return result;
}

#else // !HUXERUI_HAVE_PIPEWIRE

CameraResult<ImageAsset> RunScreenCastCapture(UiWindow*, const std::shared_ptr<std::atomic_bool>&, const Rect&,
                                              const Rect&, double, const PhotoOptions&) {
  return CameraResult<ImageAsset>::Failure(
      {CameraErrorCode::Unavailable, "The screen photo path needs PipeWire, which this build lacks."});
}

#endif // HUXERUI_HAVE_PIPEWIRE

} // namespace

void CapturePortalScreenPhoto(UiWindow& window, const Rect& viewfinder, const Rect& marker, double scale_hint,
                              std::shared_ptr<std::atomic_bool> marker_alive, PhotoOptions options,
                              std::function<void(CameraResult<ImageAsset>)> completed) {
  // Capability probe only: an empty strip equals the current state, so nothing is painted here.
  if (!linux::SetWindowMarker(window, {})) {
    completed(CameraResult<ImageAsset>::Failure(
        {CameraErrorCode::Unavailable, "This window cannot host the screen-read locator."}));
    return;
  }
  const double scale = scale_hint > 0.0 ? scale_hint : 1.0;
  std::thread(
      [window_pointer = &window, marker_alive, viewfinder, marker, scale, options,
       completed = std::move(completed)]() mutable {
        auto result = RunScreenCastCapture(window_pointer, marker_alive, viewfinder, marker, scale, options);
        PostMarkerRequest(window_pointer, marker_alive, {});
        completed(std::move(result));
      })
      .detach();
}

} // namespace huxerui::camera::detail
