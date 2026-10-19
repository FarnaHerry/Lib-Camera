#include "camera_windows.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

#include <chrono>
#include <algorithm>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace huxerui;
using namespace huxerui::camera;
using Microsoft::WRL::ComPtr;

namespace {

void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

void Require(HRESULT result) {
  Check(SUCCEEDED(result), "Windows image test operation failed.");
}

Bytes MarkerBitmap() {
  constexpr unsigned width = 80, height = 48;
  Bytes data(54 + width * height * 3);
  const auto write = [&](std::size_t offset, unsigned value, unsigned count) {
    for (unsigned index = 0; index < count; ++index)
      data[offset + index] = std::byte((value >> (index * 8)) & 255);
  };
  data[0] = std::byte{'B'};
  data[1] = std::byte{'M'};
  write(2, static_cast<unsigned>(data.size()), 4);
  write(10, 54, 4);
  write(14, 40, 4);
  write(18, width, 4);
  write(22, height, 4);
  write(26, 1, 2);
  write(28, 24, 2);
  for (unsigned y = 0; y < height; ++y) {
    for (unsigned x = 0; x < width; ++x) {
      const auto offset = 54 + ((height - 1 - y) * width + x) * 3;
      data[offset] = std::byte(y >= height / 2 ? 240 : 10);
      data[offset + 1] = std::byte(x >= width / 2 ? 240 : 10);
      data[offset + 2] = std::byte(y < height / 2 ? 240 : 10);
    }
  }
  return data;
}

void CheckPhoto(const ImageAsset& photo, bool swapped, int top_left, int top_right) {
  ComPtr<IWICImagingFactory> factory;
  Require(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
  const auto bytes = photo.EncodedBytes();
  Check(bytes[0] == std::byte{0xff} && bytes[1] == std::byte{0xd8}, "Output must be JPEG.");
  ComPtr<IWICStream> stream;
  Require(factory->CreateStream(&stream));
  Require(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<std::byte*>(bytes.data())),
                                       static_cast<DWORD>(bytes.size())));
  ComPtr<IWICBitmapDecoder> decoder;
  Require(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder));
  ComPtr<IWICBitmapFrameDecode> frame;
  Require(decoder->GetFrame(0, &frame));
  UINT width = 0, height = 0;
  Require(frame->GetSize(&width, &height));
  Check(width == (swapped ? 48U : 80U) && height == (swapped ? 80U : 48U),
        "Rotation must produce upright photo dimensions.");
  ComPtr<IWICFormatConverter> converter;
  Require(factory->CreateFormatConverter(&converter));
  Require(converter->Initialize(frame.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone,
                                nullptr, 0, WICBitmapPaletteTypeCustom));
  const int corners[] = {top_left, top_right, 3 - top_right, 3 - top_left};
  for (int index = 0; index < 4; ++index) {
    BYTE pixel[3]{};
    WICRect region{index % 2 ? static_cast<INT>(width) - 6 : 5,
                   index / 2 ? static_cast<INT>(height) - 6 : 5, 1, 1};
    Require(converter->CopyPixels(&region, 3, 3, pixel));
    const int corner = corners[index];
    Check((pixel[0] > 128) == (corner >= 2) && (pixel[1] > 128) == (corner % 2 == 1) &&
          (pixel[2] > 128) == (corner < 2), "Photo rotation or final mirroring moved the wrong corner.");
  }
}

Bytes WithExifOrientation(std::span<const std::byte> jpeg, unsigned orientation) {
  // A little-endian EXIF IFD containing only the orientation tag.
  const unsigned char app1[] = {
      0xff, 0xe1, 0x00, 0x22, 'E', 'x', 'i', 'f', 0, 0,
      'I', 'I', 42, 0, 8, 0, 0, 0, 1, 0,
      0x12, 0x01, 3, 0, 1, 0, 0, 0, static_cast<unsigned char>(orientation), 0, 0, 0,
      0, 0, 0, 0};
  Bytes result(jpeg.begin(), jpeg.begin() + 2);
  for (const auto value : app1)
    result.push_back(std::byte{value});
  result.insert(result.end(), jpeg.begin() + 2, jpeg.end());
  return result;
}

void TestPhotos() {
  const auto original = MarkerBitmap();
  const int top_left[] = {0, 2, 3, 1};
  const int top_right[] = {1, 0, 2, 3};
  for (int rotation = 0; rotation < 4; ++rotation) {
    for (bool mirror : {false, true}) {
      const auto photo = camera::detail::EncodeWindowsPhoto(original, {.jpeg_quality = 98,
          .mirror = mirror ? MirrorMode::On : MirrorMode::Off}, static_cast<PreviewRotation>(rotation));
      CheckPhoto(photo, rotation % 2 != 0, mirror ? top_right[rotation] : top_left[rotation],
                  mirror ? top_left[rotation] : top_right[rotation]);
    }
  }
  const auto jpeg = camera::detail::EncodeWindowsPhoto(original, {.jpeg_quality = 98}, PreviewRotation::R0);
  const int exif_left[] = {0, 1, 3, 2, 0, 2, 3, 1};
  const int exif_right[] = {1, 0, 2, 3, 2, 0, 1, 3};
  for (unsigned orientation = 1; orientation <= 8; ++orientation) {
    const auto input = WithExifOrientation(jpeg.EncodedBytes(), orientation);
    for (int rotation = 0; rotation < 4; ++rotation) {
      for (bool mirror : {false, true}) {
        const auto photo = camera::detail::EncodeWindowsPhoto(input, {.jpeg_quality = 98,
            .mirror = mirror ? MirrorMode::On : MirrorMode::Off}, static_cast<PreviewRotation>(rotation));
        const auto index = orientation - 1;
        try {
          CheckPhoto(photo, orientation >= 5, mirror ? exif_right[index] : exif_left[index],
                      mirror ? exif_left[index] : exif_right[index]);
        } catch (const std::exception& error) {
          throw std::runtime_error("EXIF " + std::to_string(orientation) + ", stream rotation " +
              std::to_string(rotation * 90) + ", mirror " + std::to_string(mirror) + ": " + error.what());
        }
      }
    }
  }
  bool rejected = false;
  try {
    (void)camera::detail::EncodeWindowsPhoto({}, {}, PreviewRotation::R0);
  } catch (const std::exception&) {
    rejected = true;
  }
  Check(rejected, "Invalid photo bytes must be rejected.");
  std::cout << "Windows JPEG encoding, stream rotations, eight EXIF orientations and final mirroring passed.\n";
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
  const auto permission = winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapability::Create(L"webcam");
  std::cout << "Windows webcam CheckAccess status: " << static_cast<int>(permission.CheckAccess()) << '\n';
  auto camera = camera::detail::CreateWindowsCamera();
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
  std::optional<Facing> actual_facing;
  for (int run = 0; run < 2; ++run) {
    auto probe = begin(actual_facing);
    probe->Await([&] { return probe->status.state == SessionState::Running || probe->status.error.has_value(); },
                 "Native camera did not start.");
    std::shared_ptr<ExternalTexture> texture;
    {
      std::lock_guard lock(probe->mutex);
      if (probe->status.error)
        throw std::runtime_error(probe->status.error->message);
      Check(probe->status.preview && probe->status.preview->texture, "Running camera must expose its preview.");
      texture = probe->status.preview->texture;
      actual_facing = probe->status.actual_facing;
      std::cout << "Native preview: " << texture->IntrinsicSize().width << 'x' << texture->IntrinsicSize().height
                << ", facing=" << (probe->status.actual_facing ? (*probe->status.actual_facing == Facing::Front ? "front" : "back") : "unknown") << '\n';
    }
    const auto revision = texture->Revision();
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (texture->Revision() <= revision + 2 && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(20ms);
    Check(texture->Revision() > revision + 2, "Native preview did not publish frames.");
    camera->CapturePhoto({.mirror = run == 0 ? MirrorMode::Off : MirrorMode::On}, [probe](CameraResult<ImageAsset> result) {
      std::lock_guard lock(probe->mutex);
      probe->photo = std::move(result);
      probe->ready.notify_all();
    });
    if (run == 1)
      request_stop(probe);
    probe->Await([&] { return probe->photo.has_value(); }, "Native photo did not complete.");
    {
      std::lock_guard lock(probe->mutex);
      if (probe->photo->Succeeded())
        std::cout << "Native photo: " << probe->photo->Value().PixelWidth() << 'x' << probe->photo->Value().PixelHeight() << '\n';
      else if (probe->photo->Error().code == CameraErrorCode::Unavailable)
        std::cout << "Native photo unavailable on this device: " << probe->photo->Error().message << '\n';
      else
        throw std::runtime_error(probe->photo->Error().message);
    }
    if (run == 0)
      stop(probe);
    else
      probe->Await([&] { return probe->stopped; }, "Stop must finish after native photo processing.");
    const auto final_revision = texture->Revision();
    std::this_thread::sleep_for(100ms);
    Check(texture->Revision() == final_revision, "Preview published after stop completion.");
    if (probe->photo->Succeeded())
      Check(!probe->photo->Value().EncodedBytes().empty(), "A photo must remain usable after stop.");
  }
  auto starting = begin({});
  stop(starting);
  for (const auto facing : {Facing::Front, Facing::Back}) {
    auto probe = begin(facing);
    probe->Await([&] { return probe->status.state == SessionState::Running || probe->status.error.has_value(); },
                 "Facing selection did not settle.");
    {
      std::lock_guard lock(probe->mutex);
      if (probe->status.error)
        Check(probe->status.error->code == CameraErrorCode::DeviceNotFound, "Missing facing must report DeviceNotFound.");
      else
        Check(probe->status.actual_facing == facing, "Explicit facing must not select a different camera facing.");
    }
    stop(probe);
  }
  auto closing = begin({});
  stop(closing);
  camera.reset();
  std::cout << "Native Windows preview, photos, pending-operation stops, facing selection and restart passed.\n";
}

LRESULT CALLBACK ScreenWindowProcedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
  if (message == WM_PAINT) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT bounds{};
    GetClientRect(window, &bounds);
    if (GetWindowLongPtrW(window, GWLP_USERDATA) == 1) {
      RECT left{0, 0, (bounds.right - bounds.left) / 2, bounds.bottom};
      RECT right{left.right, 0, bounds.right, bounds.bottom};
      HBRUSH red = CreateSolidBrush(RGB(235, 20, 20));
      HBRUSH green = CreateSolidBrush(RGB(20, 235, 20));
      FillRect(dc, &left, red);
      FillRect(dc, &right, green);
      DeleteObject(red);
      DeleteObject(green);
    } else {
      HBRUSH blue = CreateSolidBrush(RGB(20, 20, 235));
      FillRect(dc, &bounds, blue);
      DeleteObject(blue);
    }
    EndPaint(window, &paint);
    return 0;
  }
  return DefWindowProcW(window, message, w_param, l_param);
}

void CheckScreenPhoto(const ImageAsset& photo, bool green, bool verify_color) {
  Check(photo.PixelWidth() == 80 && photo.PixelHeight() == 60, "Screen photo dimensions are not the anchor size.");
  const auto bytes = photo.EncodedBytes();
  ComPtr<IWICImagingFactory> factory;
  Require(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
  ComPtr<IWICStream> stream;
  Require(factory->CreateStream(&stream));
  Require(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<std::byte*>(bytes.data())),
                                       static_cast<DWORD>(bytes.size())));
  ComPtr<IWICBitmapDecoder> decoder;
  Require(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder));
  ComPtr<IWICBitmapFrameDecode> frame;
  Require(decoder->GetFrame(0, &frame));
  ComPtr<IWICFormatConverter> converter;
  Require(factory->CreateFormatConverter(&converter));
  Require(converter->Initialize(frame.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone,
                                nullptr, 0, WICBitmapPaletteTypeCustom));
  BYTE pixel[3]{};
  WICRect center{40, 30, 1, 1};
  Require(converter->CopyPixels(&center, 3, 3, pixel));
  std::cout << "Screen sample BGR: " << static_cast<int>(pixel[0]) << ','
            << static_cast<int>(pixel[1]) << ',' << static_cast<int>(pixel[2]) << '\n';
  if (verify_color)
    Check(pixel[0] < 80 && (green ? pixel[1] > 170 && pixel[2] < 80
                                  : pixel[2] > 170 && pixel[1] < 80),
          "Screen capture did not reveal the content beneath the excluded window.");
}

void TestScreen() {
  using namespace std::chrono_literals;
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = ScreenWindowProcedure;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.lpszClassName = L"HuxerUICameraScreenTest";
  Check(RegisterClassW(&window_class) != 0, "Cannot register screen test window.");
  const int x = std::max(10, (GetSystemMetrics(SM_CXSCREEN) - 500) / 2);
  const int y = std::max(10, (GetSystemMetrics(SM_CYSCREEN) - 300) / 2);
  HWND background = CreateWindowExW(WS_EX_TOPMOST, window_class.lpszClassName, L"screen background", WS_POPUP,
                                    x, y, 450, 250, nullptr, nullptr, window_class.hInstance, nullptr);
  HWND foreground = CreateWindowExW(WS_EX_TOPMOST, window_class.lpszClassName, L"screen camera", WS_POPUP,
                                    x + 20, y + 20, 120, 100, nullptr, nullptr, window_class.hInstance, nullptr);
  Check(background && foreground, "Cannot create screen test windows.");
  SetWindowLongPtrW(background, GWLP_USERDATA, 1);
  ShowWindow(background, SW_SHOW);
  UpdateWindow(background);
  ShowWindow(foreground, SW_SHOW);
  UpdateWindow(foreground);
  const auto compose_until = std::chrono::steady_clock::now() + 500ms;
  while (std::chrono::steady_clock::now() < compose_until) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    std::this_thread::sleep_for(10ms);
  }
  HDC desktop = GetDC(nullptr);
  COLORREF exposed = GetPixel(desktop, x + 180, y + 100);
  ReleaseDC(nullptr, desktop);
  const bool verify_color = GetRValue(exposed) > 170 && GetGValue(exposed) < 80 && GetBValue(exposed) < 80;
  if (!verify_color)
    std::cout << "Desktop compositor did not expose the test window; pixel content check will be skipped.\n";
  auto camera = camera::detail::CreateWindowsScreenCamera(foreground);
  auto probe = std::make_shared<DeviceProbe>();
  camera->SetScreenRegion(Rect{10.0F, 10.0F, 80.0F, 60.0F});
  camera->StartScreen([probe](CameraStatus value) {
    std::lock_guard lock(probe->mutex);
    probe->status = std::move(value);
    probe->ready.notify_all();
  });
  probe->Await([&] { return probe->status.state == SessionState::Running || probe->status.error.has_value(); },
               "Screen capture did not start.");
  {
    std::lock_guard lock(probe->mutex);
    if (probe->status.error) throw std::runtime_error(probe->status.error->message);
    Check(!probe->status.preview, "Transparent screen selection must not publish preview textures.");
  }
  HRGN aperture = CreateRectRgn(0, 0, 0, 0);
  Check(aperture && GetWindowRgn(foreground, aperture) == COMPLEXREGION,
        "Screen selection must make a native window aperture.");
  Check(!PtInRegion(aperture, 50, 40) && PtInRegion(aperture, 5, 5),
        "The viewfinder interior must be transparent while its surrounding controls remain visible.");
  DeleteObject(aperture);
  const auto take = [&](bool green) {
    probe->photo.reset();
    camera->CapturePhoto({}, [probe](CameraResult<ImageAsset> result) {
      std::lock_guard lock(probe->mutex);
      probe->photo = std::move(result);
      probe->ready.notify_all();
    });
    probe->Await([&] { return probe->photo.has_value(); }, "Screen photo did not complete.");
    if (!probe->photo->Succeeded()) throw std::runtime_error(probe->photo->Error().message);
    CheckScreenPhoto(probe->photo->Value(), green, verify_color);
  };
  take(false);
  MoveWindow(foreground, x + 300, y + 20, 120, 100, TRUE);
  std::this_thread::sleep_for(300ms);
  take(true);
  camera->SetScreenRegion(std::nullopt);
  aperture = CreateRectRgn(0, 0, 0, 0);
  Check(GetWindowRgn(foreground, aperture) != ERROR && PtInRegion(aperture, 50, 40),
        "Removing the screen anchor must close its aperture for overlapping application UI.");
  DeleteObject(aperture);
  camera->Stop([probe] {
    std::lock_guard lock(probe->mutex);
    probe->stopped = true;
    probe->ready.notify_all();
  });
  probe->Await([&] { return probe->stopped; }, "Screen capture did not stop.");
  DWORD restored_affinity = WDA_EXCLUDEFROMCAPTURE;
  Check(GetWindowDisplayAffinity(foreground, &restored_affinity) && restored_affinity == WDA_NONE,
        "Stopping screen capture must restore window display affinity.");
  aperture = CreateRectRgn(0, 0, 0, 0);
  Check(GetWindowRgn(foreground, aperture) == ERROR, "Stopping must restore the original unshaped window.");
  DeleteObject(aperture);
  DestroyWindow(foreground);
  DestroyWindow(background);
  UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
  std::cout << "Native aperture, on-demand photos, window movement, and window restoration passed."
            << (verify_color ? " Pixel content passed.\n" : " Pixel content remains unverified.\n");
}
} // namespace

int main(int argc, char** argv) {
  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(initialized))
    return 1;
  int result = 0;
  try {
    TestPhotos();
    if (argc > 1 && std::string_view(argv[1]) == "--device")
      TestDevice();
    if (argc > 1 && std::string_view(argv[1]) == "--screen")
      TestScreen();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    result = 1;
  }
  CoUninitialize();
  return result;
}
