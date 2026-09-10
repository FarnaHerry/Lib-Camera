#include "camera_windows.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <limits>
#include <stdexcept>

namespace huxerui::camera::detail {
namespace {

using Microsoft::WRL::ComPtr;

void Require(HRESULT result) {
  if (FAILED(result))
    throw std::runtime_error("Windows photo processing failed (HRESULT " + std::to_string(result) + ").");
}

ComPtr<IWICBitmapSource> Transform(IWICImagingFactory* factory, ComPtr<IWICBitmapSource> source,
                                  WICBitmapTransformOptions options) {
  if (options == WICBitmapTransformRotate0)
    return source;
  ComPtr<IWICBitmapFlipRotator> transform;
  Require(factory->CreateBitmapFlipRotator(&transform));
  Require(transform->Initialize(source.Get(), options));
  Require(transform.As(&source));
  return source;
}

} // namespace

ImageAsset EncodeWindowsPhoto(std::span<const std::byte> bytes, PhotoOptions options, PreviewRotation rotation) {
  if (bytes.empty() || bytes.size() > std::numeric_limits<DWORD>::max())
    throw std::runtime_error("The camera returned invalid photo data.");
  ComPtr<IWICImagingFactory> factory;
  Require(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
  ComPtr<IWICStream> input;
  Require(factory->CreateStream(&input));
  Require(input->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<std::byte*>(bytes.data())),
                                      static_cast<DWORD>(bytes.size())));
  ComPtr<IWICBitmapDecoder> decoder;
  Require(factory->CreateDecoderFromStream(input.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder));
  ComPtr<IWICBitmapFrameDecode> frame;
  Require(decoder->GetFrame(0, &frame));

  auto orientation = static_cast<WICBitmapTransformOptions>(static_cast<int>(rotation));
  ComPtr<IWICMetadataQueryReader> metadata;
  if (SUCCEEDED(frame->GetMetadataQueryReader(&metadata))) {
    PROPVARIANT value{};
    if (SUCCEEDED(metadata->GetMetadataByName(L"/app1/ifd/{ushort=274}", &value)) && value.vt == VT_UI2) {
      // EXIF orientation describes the encoded pixels and takes precedence over stream metadata.
      switch (value.uiVal) {
      case 1: orientation = WICBitmapTransformRotate0; break;
      case 2: orientation = WICBitmapTransformFlipHorizontal; break;
      case 3: orientation = WICBitmapTransformRotate180; break;
      case 4: orientation = WICBitmapTransformFlipVertical; break;
      case 5:
        orientation = static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
        break;
      case 6: orientation = WICBitmapTransformRotate90; break;
      case 7:
        orientation = static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
        break;
      case 8: orientation = WICBitmapTransformRotate270; break;
      }
    }
    PropVariantClear(&value);
  }
  ComPtr<IWICBitmapSource> source;
  Require(frame.As(&source));
  source = Transform(factory.Get(), source, orientation);
  if (options.mirror == MirrorMode::On)
    source = Transform(factory.Get(), source, WICBitmapTransformFlipHorizontal);

  ComPtr<IStream> output;
  Require(CreateStreamOnHGlobal(nullptr, TRUE, &output));
  ComPtr<IWICBitmapEncoder> encoder;
  Require(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder));
  Require(encoder->Initialize(output.Get(), WICBitmapEncoderNoCache));
  ComPtr<IWICBitmapFrameEncode> encoded;
  ComPtr<IPropertyBag2> properties;
  Require(encoder->CreateNewFrame(&encoded, &properties));
  PROPBAG2 property{};
  property.pstrName = const_cast<wchar_t*>(L"ImageQuality");
  VARIANT quality{};
  quality.vt = VT_R4;
  quality.fltVal = static_cast<float>(options.jpeg_quality) / 100.0F;
  Require(properties->Write(1, &property, &quality));
  Require(encoded->Initialize(properties.Get()));
  UINT width = 0, height = 0;
  Require(source->GetSize(&width, &height));
  Require(encoded->SetSize(width, height));
  WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
  Require(encoded->SetPixelFormat(&format));
  ComPtr<IWICFormatConverter> converter;
  Require(factory->CreateFormatConverter(&converter));
  Require(converter->Initialize(source.Get(), format, WICBitmapDitherTypeNone, nullptr, 0.0,
                                 WICBitmapPaletteTypeCustom));
  Require(encoded->WriteSource(converter.Get(), nullptr));
  Require(encoded->Commit());
  Require(encoder->Commit());
  STATSTG stat{};
  Require(output->Stat(&stat, STATFLAG_NONAME));
  if (stat.cbSize.QuadPart > std::numeric_limits<ULONG>::max())
    throw std::runtime_error("The encoded photo is too large.");
  Bytes jpeg(static_cast<std::size_t>(stat.cbSize.QuadPart));
  Require(output->Seek({}, STREAM_SEEK_SET, nullptr));
  ULONG read = 0;
  Require(output->Read(jpeg.data(), static_cast<ULONG>(jpeg.size()), &read));
  if (read != jpeg.size())
    throw std::runtime_error("Cannot read the encoded photo.");
  return ImageAsset::FromEncoded(std::move(jpeg));
}

} // namespace huxerui::camera::detail
