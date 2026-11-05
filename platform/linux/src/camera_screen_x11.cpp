#include "camera_screen_x11.h"

#include <gdk/gdk.h>
#if defined(GDK_WINDOWING_X11) && defined(HUXERUI_CAMERA_HAVE_X11)
#include <gdk/x11/gdkx.h>
#endif

#include <huxerui/linux/window.h>

#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include "camera_linux.h"

// X11 headers define identifiers that clash with application code; they are included last and
// kept out of this file's public surface. None is undefined after them.
#if defined(GDK_WINDOWING_X11) && defined(HUXERUI_CAMERA_HAVE_X11)
#include <X11/Xlib.h>
#include <X11/extensions/shape.h>
#ifdef None
#undef None
#endif
// Xlib defines Success as a result code, which collides with Result<T>::Success below.
#ifdef Success
#undef Success
#endif
#endif

namespace huxerui::camera::detail {
namespace {

constexpr double kMaxFrameDimension = 8192.0;

/// X errors are connection-wide and GDK's handler terminates the host, so reads that a server
/// may reject (XWayland does not back the root window) run under a quiet handler and degrade
/// into a normal failure instead.
#if defined(GDK_WINDOWING_X11) && defined(HUXERUI_CAMERA_HAVE_X11)

int QuietXErrorHandler(Display*, XErrorEvent*) noexcept {
  return 0;
}

class QuietXErrors final {
public:
  explicit QuietXErrors(Display* display) : display_(display), previous_(XSetErrorHandler(QuietXErrorHandler)) {}
  ~QuietXErrors() {
    XSync(display_, False);
    XSetErrorHandler(previous_);
  }
  QuietXErrors(const QuietXErrors&) = delete;
  QuietXErrors& operator=(const QuietXErrors&) = delete;

private:
  Display* display_;
  XErrorHandler previous_;
};

/// The viewfinder rectangle translated to root-window physical pixels.
/// Returns an empty value when the rectangle is empty, non-finite, or leaves the root window.
std::optional<Rect> ResolvePhysicalArea(Display* display, Window window, double scale,
                                        const std::optional<Rect>& region) {
  if (!region.has_value() || region->IsEmpty() || !std::isfinite(region->x) || !std::isfinite(region->y) ||
      !std::isfinite(region->width) || !std::isfinite(region->height)) {
    return std::nullopt;
  }
  int origin_x = 0;
  int origin_y = 0;
  Window ignored = 0;
  if (XTranslateCoordinates(display, window, DefaultRootWindow(display), 0, 0, &origin_x, &origin_y, &ignored) ==
      False) {
    return std::nullopt;
  }
  Window root = DefaultRootWindow(display);
  Window root_parent = 0;
  unsigned root_width = 0;
  unsigned root_height = 0;
  unsigned border = 0;
  unsigned depth = 0;
  if (!XGetGeometry(display, root, &root_parent, &origin_x, &origin_y, &root_width, &root_height, &border,
                    &depth)) {
    return std::nullopt;
  }
  Rect physical;
  physical.x = origin_x + region->x * scale;
  physical.y = origin_y + region->y * scale;
  physical.width = region->width * scale;
  physical.height = region->height * scale;
  if (physical.width <= 0.0F || physical.height <= 0.0F || physical.width > kMaxFrameDimension ||
      physical.height > kMaxFrameDimension) {
    return std::nullopt;
  }
  // The rectangle must sit wholly inside the root window; mixed-DPI extents are not supported.
  if (physical.x < 0.0F || physical.y < 0.0F ||
      physical.x + physical.width > static_cast<double>(root_width) ||
      physical.y + physical.height > static_cast<double>(root_height)) {
    return std::nullopt;
  }
  return physical;
}

Region CombineWithoutHole(Display* display, Window window, const XRectangle& hole) {
  Region remaining = XCreateRegion();
  int count = 0;
  int ordering = 0;
  XRectangle* rectangles = XShapeGetRectangles(display, window, ShapeBounding, &count, &ordering);
  if (rectangles == nullptr || count <= 0) {
    Window parent = 0;
    Window ignored_child = 0;
    int origin_x = 0;
    int origin_y = 0;
    unsigned width = 0;
    unsigned height = 0;
    unsigned border = 0;
    unsigned depth = 0;
    if (XGetGeometry(display, window, &parent, &origin_x, &origin_y, &width, &height, &border, &depth)) {
      XRectangle whole{0, 0, static_cast<unsigned short>(width), static_cast<unsigned short>(height)};
      XUnionRectWithRegion(&whole, remaining, remaining);
    }
  } else {
    for (int index = 0; index < count; ++index) {
      XUnionRectWithRegion(&rectangles[index], remaining, remaining);
    }
    XFree(rectangles);
  }
  Region hole_region = XCreateRegion();
  XUnionRectWithRegion(const_cast<XRectangle*>(&hole), hole_region, hole_region);
  XSubtractRegion(remaining, hole_region, remaining);
  XDestroyRegion(hole_region);
  return remaining;
}

#endif // GDK_WINDOWING_X11

} // namespace

bool LinuxX11ScreenAvailable(UiWindow& window) {
#if defined(GDK_WINDOWING_X11) && defined(HUXERUI_CAMERA_HAVE_X11)
  const auto geometry = linux::NativeGeometry(window);
  return geometry.surface != nullptr && GDK_IS_X11_SURFACE(geometry.surface);
#else
  (void)window;
  return false;
#endif
}

struct LinuxX11Screen::State {
#if defined(GDK_WINDOWING_X11) && defined(HUXERUI_CAMERA_HAVE_X11)
  Display* display = nullptr;
  Window window = 0;
  double content_x = 0.0;
  double content_y = 0.0;
  double scale = 1.0;
  bool available = false;

  bool active = false;
  std::optional<Rect> region;
  std::function<void(CameraStatus)> changed;
  XRectangle* saved_rectangles = nullptr;
  int saved_count = 0;
#endif

  bool Available() const noexcept {
#if defined(GDK_WINDOWING_X11) && defined(HUXERUI_CAMERA_HAVE_X11)
    return available;
#else
    return false;
#endif
  }
};

LinuxX11Screen::LinuxX11Screen(UiWindow& window) : state_(std::make_unique<State>()) {
#if defined(GDK_WINDOWING_X11) && defined(HUXERUI_CAMERA_HAVE_X11)
  const auto geometry = linux::NativeGeometry(window);
  if (geometry.surface == nullptr || !GDK_IS_X11_SURFACE(geometry.surface)) {
    return;
  }
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  state_->display = gdk_x11_display_get_xdisplay(gdk_surface_get_display(geometry.surface));
  state_->window = gdk_x11_surface_get_xid(geometry.surface);
  G_GNUC_END_IGNORE_DEPRECATIONS
  state_->content_x = geometry.content_x;
  state_->content_y = geometry.content_y;
  state_->scale = geometry.scale > 0.0 ? geometry.scale : 1.0;
  state_->available = state_->display != nullptr && state_->window != 0;
#else
  (void)window;
#endif
}

LinuxX11Screen::~LinuxX11Screen() {
#if defined(GDK_WINDOWING_X11) && defined(HUXERUI_CAMERA_HAVE_X11)
  if (state_->active) {
    Deactivate();
  }
#endif
}

#if defined(GDK_WINDOWING_X11) && defined(HUXERUI_CAMERA_HAVE_X11)

void LinuxX11Screen::Deactivate() {
  auto& state = *state_;
  state.active = false;
  state.changed = {};
  if (state.saved_rectangles != nullptr) {
    XShapeCombineRectangles(state.display, state.window, ShapeBounding, 0, 0, state.saved_rectangles,
                            state.saved_count, ShapeSet, YXBanded);
    XFree(state.saved_rectangles);
    state.saved_rectangles = nullptr;
  }
  state.saved_count = 0;
}

bool LinuxX11Screen::Activate(std::function<void(CameraStatus)> changed) {
  auto& state = *state_;
  if (!state.Available()) {
    return false;
  }
  if (state.active) {
    std::move(changed)({.state = SessionState::Failed,
                        .error = CameraError{CameraErrorCode::DeviceBusy,
                                             "This window already has a screen viewfinder."}});
    return true;
  }
  // Save the current bounding shape so stop can restore an unshaped or previously shaped window.
  int count = 0;
  int ordering = 0;
  XRectangle* rectangles = XShapeGetRectangles(state.display, state.window, ShapeBounding, &count, &ordering);
  if (rectangles == nullptr || count <= 0) {
    std::move(changed)({.state = SessionState::Failed,
                        .error = CameraError{CameraErrorCode::ConfigurationFailed,
                                             "Cannot save the window shape."}});
    return true;
  }
  state.saved_rectangles = rectangles;
  state.saved_count = count;
  state.active = true;
  state.changed = std::move(changed);
  // Force an update even when the seeded rectangle is unchanged by the equality guard.
  std::optional<Rect> seeded = std::move(state.region);
  state.region.reset();
  SetRegion(std::move(seeded));
  return true;
}

void LinuxX11Screen::SetRegion(std::optional<Rect> region) {
  auto& state = *state_;
  if (state.region == region) {
    return;
  }
  state.region = std::move(region);
  if (!state.active) {
    return;
  }
  // The picture rectangle is validated against the root for photo parity; the punched hole is
  // the same rectangle in window-relative physical pixels, offset by the drawing content origin.
  const auto physical = ResolvePhysicalArea(state.display, state.window, state.scale, state.region);
  if (!physical.has_value()) {
    // An invalid rectangle restores the unshaped window and keeps selection in Starting.
    XShapeCombineRectangles(state.display, state.window, ShapeBounding, 0, 0, state.saved_rectangles,
                            state.saved_count, ShapeSet, YXBanded);
    if (state.changed) {
      state.changed({.state = SessionState::Starting});
    }
    return;
  }
  const XRectangle hole{static_cast<short>((state.content_x + state.region->x) * state.scale),
                        static_cast<short>((state.content_y + state.region->y) * state.scale),
                        static_cast<unsigned short>(state.region->width * state.scale),
                        static_cast<unsigned short>(state.region->height * state.scale)};
  Region remaining = CombineWithoutHole(state.display, state.window, hole);
  XShapeCombineRegion(state.display, state.window, ShapeBounding, 0, 0, remaining, ShapeSet);
  XDestroyRegion(remaining);
  if (state.changed) {
    state.changed({.state = SessionState::Running});
  }
}

void LinuxX11Screen::CapturePhoto(PhotoOptions options,
                                  std::function<void(CameraResult<ImageAsset>)> completed) {
  auto& state = *state_;
  const auto physical = ResolvePhysicalArea(state.display, state.window, state.scale, state.region);
  if (!state.active || !physical.has_value()) {
    completed(CameraResult<ImageAsset>::Failure(
        {CameraErrorCode::NotReady, "Place the screen viewfinder wholly inside one visible display."}));
    return;
  }
  const auto width = static_cast<unsigned>(physical->width);
  const auto height = static_cast<unsigned>(physical->height);
  QuietXErrors quiet(state.display);
  XImage* image = XGetImage(state.display, DefaultRootWindow(state.display),
                            static_cast<int>(physical->x), static_cast<int>(physical->y), width, height,
                            AllPlanes, ZPixmap);
  if (image == nullptr) {
    completed(CameraResult<ImageAsset>::Failure(
        {CameraErrorCode::Unavailable,
         "This X server does not allow reading the desktop; native X11 sessions are required."}));
    return;
  }
  if (image->bits_per_pixel != 32 || image->bytes_per_line <= 0) {
    XDestroyImage(image);
    completed(CameraResult<ImageAsset>::Failure(
        {CameraErrorCode::CaptureFailed, "The desktop frame has an unsupported pixel layout."}));
    return;
  }
  try {
    const auto* pixels = reinterpret_cast<const std::byte*>(image->data);
    completed(CameraResult<ImageAsset>::Success(
        EncodeLinuxPhotoFrame({pixels, pixels + static_cast<std::size_t>(image->bytes_per_line) * (height - 1) +
                                          static_cast<std::size_t>(width) * 4U},
                              static_cast<int>(width), static_cast<int>(height),
                              static_cast<std::size_t>(image->bytes_per_line), options, PreviewRotation::R0)));
  } catch (const std::exception& error) {
    completed(CameraResult<ImageAsset>::Failure(CameraError{CameraErrorCode::CaptureFailed, error.what()}));
  }
  XDestroyImage(image);
}

#else // !GDK_WINDOWING_X11

bool LinuxX11Screen::Activate(std::function<void(CameraStatus)>) { return false; }
void LinuxX11Screen::SetRegion(std::optional<Rect>) {}
void LinuxX11Screen::Deactivate() {}
void LinuxX11Screen::CapturePhoto(PhotoOptions,
                                  std::function<void(CameraResult<ImageAsset>)> completed) {
  completed(CameraResult<ImageAsset>::Failure(
      {CameraErrorCode::Unavailable, "Screen capture is unavailable on this platform."}));
}

#endif

} // namespace huxerui::camera::detail
