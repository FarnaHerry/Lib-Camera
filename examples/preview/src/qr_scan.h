#pragma once

#include <memory>
#include <string>

#include <huxerui/huxerui.h>

namespace app {

/// Result of decoding one encoded camera frame.
struct ScanOutcome {
  std::string text;
  std::string format;
  std::string error;
};

/// Opaque platform decoder module. Only Android needs a platform image decoder;
/// desktop and Apple platforms decode in-process.
class QrDecoder;
using QrDecoderHandle = std::shared_ptr<QrDecoder>;

/// Reports whether this platform can turn a camera JPEG into QR payload bytes.
[[nodiscard]] bool QrScanningAvailable() noexcept;

/// Opens the platform decoder module. Returns an empty handle where decoding is local.
[[nodiscard]] QrDecoderHandle OpenQrDecoder();

/// Registers the platform decoder module when the platform needs one.
void InstallQrScanning(huxerui::ApplicationContext& context);

/// Decodes the first QR code found in an encoded PNG/JPEG camera frame.
///
/// An empty text with an empty error means no symbol was present in this frame.
/// Decoding runs off the UI thread except on Android, where the platform module
/// delivers raw pixels and the codec runs on the UI thread.
[[nodiscard]] huxerui::Task<ScanOutcome> ScanQrFromJpeg(huxerui::Bytes jpeg, QrDecoderHandle decoder);

} // namespace app
