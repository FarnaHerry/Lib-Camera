#include "camera_windows.h"

#include <commctrl.h>
#include <d3d11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Foundation.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace huxerui::camera::detail {
namespace {

using Microsoft::WRL::ComPtr;
namespace capture = winrt::Windows::Graphics::Capture;
namespace directx = winrt::Windows::Graphics::DirectX;
namespace direct3d = winrt::Windows::Graphics::DirectX::Direct3D11;

struct ScreenArea {
  HMONITOR monitor = nullptr;
  RECT monitor_bounds{};
  RECT pixels{};
};

std::optional<ScreenArea> ResolveArea(HWND window, std::optional<Rect> region) {
  if (!IsWindow(window) || !region || region->IsEmpty() || IsIconic(window) ||
      !std::isfinite(region->x) || !std::isfinite(region->y) ||
      !std::isfinite(region->width) || !std::isfinite(region->height)) return std::nullopt;
  POINT origin{};
  if (!ClientToScreen(window, &origin)) return std::nullopt;
  const double scale = static_cast<double>(GetDpiForWindow(window)) / 96.0;
  RECT pixels{
      origin.x + static_cast<LONG>(std::lround(region->x * scale)),
      origin.y + static_cast<LONG>(std::lround(region->y * scale)),
      origin.x + static_cast<LONG>(std::lround((region->x + region->width) * scale)),
      origin.y + static_cast<LONG>(std::lround((region->y + region->height) * scale))};
  if (pixels.left >= pixels.right || pixels.top >= pixels.bottom) return std::nullopt;
  HMONITOR monitor = MonitorFromRect(&pixels, MONITOR_DEFAULTTONULL);
  MONITORINFO info{sizeof(info)};
  if (!monitor || !GetMonitorInfoW(monitor, &info)) return std::nullopt;
  RECT intersection{};
  if (!IntersectRect(&intersection, &pixels, &info.rcMonitor) || !EqualRect(&intersection, &pixels))
    return std::nullopt;
  return ScreenArea{monitor, info.rcMonitor, pixels};
}

struct CaptureResources {
  capture::Direct3D11CaptureFramePool pool{nullptr};
  capture::GraphicsCaptureSession session{nullptr};
  winrt::event_token token{};
  bool subscribed = false;
  ~CaptureResources() {
    try {
      if (session) session.Close();
      if (pool) {
        if (subscribed) pool.FrameArrived(token);
        pool.Close();
      }
    } catch (...) {}
  }
};

class WindowsScreenCamera final : public CameraBackend, public std::enable_shared_from_this<WindowsScreenCamera> {
public:
  explicit WindowsScreenCamera(HWND window) : window_(window) {}
  ~WindowsScreenCamera() override {
    stopping_ = true;
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    RestoreWindow();
    RestoreAffinity();
    if (original_region_) DeleteObject(original_region_);
  }

  void Start(std::optional<Facing>, std::function<void(CameraStatus)> changed) override {
    changed({.state = SessionState::Failed,
             .error = CameraError{CameraErrorCode::Unavailable, "This backend captures the screen only."}});
  }

  void StartScreen(std::function<void(CameraStatus)> changed) override {
    if (active_ || GetPropW(window_, L"HuxerUI.Camera.ScreenAperture")) {
      changed({.state = SessionState::Failed,
               .error = CameraError{CameraErrorCode::DeviceBusy, "This window already has a screen viewfinder."}});
      return;
    }
    if (!IsWindow(window_) || GetAncestor(window_, GA_ROOT) != window_) {
      changed({.state = SessionState::Failed,
               .error = CameraError{CameraErrorCode::Unavailable, "Screen capture requires an owned top-level window."}});
      return;
    }
    if (!GetWindowDisplayAffinity(window_, &previous_affinity_) ||
        !SetWindowDisplayAffinity(window_, WDA_EXCLUDEFROMCAPTURE)) {
      changed({.state = SessionState::Failed,
               .error = CameraError{CameraErrorCode::Unavailable, "Windows cannot exclude this window from capture."}});
      return;
    }
    affinity_applied_ = true;
    if (original_region_) DeleteObject(std::exchange(original_region_, nullptr));
    original_region_ = CreateRectRgn(0, 0, 0, 0);
    if (!original_region_) {
      RestoreAffinity();
      changed({.state = SessionState::Failed,
               .error = CameraError{CameraErrorCode::ConfigurationFailed, "Cannot save the window region."}});
      return;
    }
    if (GetWindowRgn(window_, original_region_) == ERROR)
      DeleteObject(std::exchange(original_region_, nullptr));
    if (!SetWindowSubclass(window_, WindowProcedure, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this))) {
      RestoreAffinity();
      changed({.state = SessionState::Failed,
               .error = CameraError{CameraErrorCode::ConfigurationFailed, "Cannot attach the screen viewfinder."}});
      return;
    }
    if (!SetPropW(window_, L"HuxerUI.Camera.ScreenAperture", this)) {
      RemoveWindowSubclass(window_, WindowProcedure, reinterpret_cast<UINT_PTR>(this));
      RestoreAffinity();
      changed({.state = SessionState::Failed,
               .error = CameraError{CameraErrorCode::ConfigurationFailed, "Cannot own the screen viewfinder."}});
      return;
    }
    changed_ = std::move(changed);
    active_ = true;
    stopping_ = false;
    running_.reset();
    UpdateWindowRegion();
  }

  void SetScreenRegion(std::optional<Rect> region) override {
    if (region_ == region) return;
    region_ = region;
    if (active_) UpdateWindowRegion();
  }

  void Stop(std::function<void()> completed) override {
    active_ = false;
    stopping_ = true;
    RestoreWindow();
    RestoreAffinity();
    wake_.notify_all();
    {
      std::lock_guard lock(mutex_);
      if (completed) stop_callbacks_.push_back(std::move(completed));
      if (joining_) return;
      joining_ = true;
    }
    if (!worker_.joinable()) {
      FinishStop();
      return;
    }
    std::thread([self = shared_from_this()] {
      self->worker_.join();
      self->FinishStop();
    }).detach();
  }

  void CapturePhoto(PhotoOptions options, std::function<void(CameraResult<ImageAsset>)> completed) override {
    const auto area = ResolveArea(window_, region_);
    if (!active_ || !area) {
      completed(CameraResult<ImageAsset>::Failure(
          {CameraErrorCode::NotReady, "Place the screen viewfinder wholly inside one visible display."}));
      return;
    }
    bool busy;
    {
      std::lock_guard lock(mutex_);
      busy = worker_busy_ || joining_;
      if (!busy) worker_busy_ = true;
    }
    if (busy) {
      completed(CameraResult<ImageAsset>::Failure(
          {CameraErrorCode::OperationInProgress, "A screen capture is already in progress."}));
      return;
    }
    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([self = shared_from_this(), area = *area, options, completed = std::move(completed)] {
      auto result = self->CaptureOnce(area, options);
      {
        std::lock_guard lock(self->mutex_);
        self->worker_busy_ = false;
      }
      completed(std::move(result));
    });
  }

private:
  static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param,
                                          UINT_PTR identifier, DWORD_PTR reference) {
    auto* self = reinterpret_cast<WindowsScreenCamera*>(reference);
    if (message == WM_NCDESTROY) {
      RemoveWindowSubclass(window, WindowProcedure, identifier);
      self->active_ = false;
      self->stopping_ = true;
      self->window_ = nullptr;
      self->wake_.notify_all();
      return DefSubclassProc(window, message, w_param, l_param);
    }
    const LRESULT result = DefSubclassProc(window, message, w_param, l_param);
    if (self->active_ && (message == WM_SIZE || message == WM_MOVE || message == WM_DPICHANGED))
      self->UpdateWindowRegion();
    return result;
  }

  void UpdateWindowRegion() {
    if (updating_region_) return;
    updating_region_ = true;
    RECT window_bounds{};
    GetWindowRect(window_, &window_bounds);
    HRGN visible = CreateRectRgn(0, 0, window_bounds.right - window_bounds.left,
                                 window_bounds.bottom - window_bounds.top);
    bool valid = visible != nullptr;
    if (visible && original_region_) valid = CombineRgn(visible, original_region_, nullptr, RGN_COPY) != ERROR;
    const auto area = ResolveArea(window_, region_);
    if (visible && valid && area) {
      HRGN hole = CreateRectRgn(area->pixels.left - window_bounds.left, area->pixels.top - window_bounds.top,
                               area->pixels.right - window_bounds.left, area->pixels.bottom - window_bounds.top);
      valid = hole && CombineRgn(visible, visible, hole, RGN_DIFF) != ERROR;
      if (hole) DeleteObject(hole);
    }
    if (!valid || !SetWindowRgn(window_, visible, TRUE)) {
      if (visible) DeleteObject(visible);
      updating_region_ = false;
      changed_({.state = SessionState::Failed,
                 .error = CameraError{CameraErrorCode::ConfigurationFailed, "Cannot make the screen viewfinder transparent."}});
      return;
    }
    // SetWindowRgn owns visible after success. The desktop compositor supplies the pixels in the hole.
    updating_region_ = false;
    const bool ready = area.has_value();
    if (running_ != ready) {
      running_ = ready;
      changed_({.state = ready ? SessionState::Running : SessionState::Starting});
    }
  }

  void RestoreWindow() {
    if (!IsWindow(window_)) return;
    if (GetPropW(window_, L"HuxerUI.Camera.ScreenAperture") != this) return;
    RemovePropW(window_, L"HuxerUI.Camera.ScreenAperture");
    RemoveWindowSubclass(window_, WindowProcedure, reinterpret_cast<UINT_PTR>(this));
    if (!running_.has_value()) return;
    HRGN restored = nullptr;
    if (original_region_) {
      restored = CreateRectRgn(0, 0, 0, 0);
      if (!restored || CombineRgn(restored, original_region_, nullptr, RGN_COPY) == ERROR) {
        if (restored) DeleteObject(restored);
        return;
      }
    }
    if (!SetWindowRgn(window_, restored, TRUE) && restored) DeleteObject(restored);
    running_.reset();
  }

  void RestoreAffinity() {
    if (std::exchange(affinity_applied_, false) && IsWindow(window_))
      SetWindowDisplayAffinity(window_, previous_affinity_);
  }

  void FinishStop() {
    std::vector<std::function<void()>> callbacks;
    {
      std::lock_guard lock(mutex_);
      joining_ = false;
      callbacks = std::exchange(stop_callbacks_, {});
    }
    for (const auto& callback : callbacks) callback();
  }

  CameraResult<ImageAsset> CaptureOnce(const ScreenArea& area, PhotoOptions options) {
    bool initialized = false;
    auto result = CameraResult<ImageAsset>::Failure({CameraErrorCode::CaptureFailed, "Screen capture failed."});
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
      initialized = true;
      result = ReadScreen(area, options);
    } catch (const winrt::hresult_error& error) {
      result = CameraResult<ImageAsset>::Failure({CameraErrorCode::CaptureFailed, winrt::to_string(error.message())});
    } catch (const std::exception& error) {
      result = CameraResult<ImageAsset>::Failure({CameraErrorCode::CaptureFailed, error.what()});
    }
    if (initialized) winrt::uninit_apartment();
    if (stopping_) return CameraResult<ImageAsset>::Failure({CameraErrorCode::Interrupted, "Screen capture was interrupted."});
    return result;
  }

  CameraResult<ImageAsset> ReadScreen(const ScreenArea& area, PhotoOptions options) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
    ComPtr<IDXGIDevice> dxgi;
    winrt::check_hresult(device.As(&dxgi));
    direct3d::IDirect3DDevice winrt_device{nullptr};
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(
        dxgi.Get(), reinterpret_cast<IInspectable**>(winrt::put_abi(winrt_device))));
    auto factory = winrt::get_activation_factory<capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    capture::GraphicsCaptureItem item{nullptr};
    winrt::check_hresult(factory->CreateForMonitor(area.monitor,
        winrt::guid_of<capture::GraphicsCaptureItem>(), winrt::put_abi(item)));
    CaptureResources resources;
    resources.pool = capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
        winrt_device, directx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, item.Size());
    {
      std::lock_guard lock(mutex_);
      frame_ready_ = false;
    }
    resources.token = resources.pool.FrameArrived([weak = weak_from_this()](auto&&, auto&&) {
      if (auto self = weak.lock()) {
        {
          std::lock_guard lock(self->mutex_);
          self->frame_ready_ = true;
        }
        self->wake_.notify_all();
      }
    });
    resources.subscribed = true;
    resources.session = resources.pool.CreateCaptureSession(item);
    resources.session.IsCursorCaptureEnabled(false);
    resources.session.StartCapture();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    capture::Direct3D11CaptureFrame frame{nullptr};
    while (!frame && !stopping_) {
      {
        std::unique_lock lock(mutex_);
        if (!wake_.wait_until(lock, deadline, [this] { return stopping_ || frame_ready_; }))
          throw std::runtime_error("Windows did not deliver a screen image before the capture timeout.");
        frame_ready_ = false;
      }
      while (auto next = resources.pool.TryGetNextFrame()) {
        if (frame) frame.Close();
        frame = std::move(next);
      }
    }
    if (stopping_) return CameraResult<ImageAsset>::Failure({CameraErrorCode::Interrupted, "Screen capture was interrupted."});
    const auto surface = frame.Surface();
    auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    ComPtr<ID3D11Texture2D> captured;
    winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&captured)));
    const D3D11_BOX source{
        static_cast<UINT>(area.pixels.left - area.monitor_bounds.left),
        static_cast<UINT>(area.pixels.top - area.monitor_bounds.top), 0,
        static_cast<UINT>(area.pixels.right - area.monitor_bounds.left),
        static_cast<UINT>(area.pixels.bottom - area.monitor_bounds.top), 1};
    D3D11_TEXTURE2D_DESC description{};
    captured->GetDesc(&description);
    if (source.right > description.Width || source.bottom > description.Height)
      throw std::runtime_error("The captured display no longer covers the requested rectangle.");
    description.Width = source.right - source.left;
    description.Height = source.bottom - source.top;
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    description.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    winrt::check_hresult(device->CreateTexture2D(&description, nullptr, &staging));
    context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, captured.Get(), 0, &source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    winrt::check_hresult(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    ImageAsset photo;
    try {
      const auto* pixels = static_cast<const std::byte*>(mapped.pData);
      photo = EncodeWindowsScreenPhoto(
          std::span<const std::byte>(pixels, static_cast<std::size_t>(mapped.RowPitch) * description.Height),
          description.Width, description.Height, mapped.RowPitch, options);
    } catch (...) {
      context->Unmap(staging.Get(), 0);
      throw;
    }
    context->Unmap(staging.Get(), 0);
    frame.Close();
    return CameraResult<ImageAsset>::Success(std::move(photo));
  }

  HWND window_ = nullptr;
  HRGN original_region_ = nullptr;
  std::optional<Rect> region_;
  std::optional<bool> running_;
  std::function<void(CameraStatus)> changed_;
  bool active_ = false;
  bool updating_region_ = false;
  bool affinity_applied_ = false;
  DWORD previous_affinity_ = WDA_NONE;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::thread worker_;
  std::atomic<bool> stopping_ = false;
  bool worker_busy_ = false;
  bool joining_ = false;
  bool frame_ready_ = false;
  std::vector<std::function<void()>> stop_callbacks_;
};

} // namespace

std::shared_ptr<CameraBackend> CreateWindowsScreenCamera(HWND window) {
  return std::make_shared<WindowsScreenCamera>(window);
}

} // namespace huxerui::camera::detail
