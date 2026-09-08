#import <AVFoundation/AVFoundation.h>

#include <huxerui/macos/external_texture.h>

#include "camera_internal.h"

using namespace huxerui;
using namespace huxerui::camera;

@interface HUXCameraCapture : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate> {
  dispatch_queue_t queue_;
  AVCaptureSession* session_;
  AVCaptureVideoDataOutput* output_;
  AVCaptureDevice* device_;
  NSMutableArray* observers_;
  std::shared_ptr<macos::PixelBufferTexture> texture_;
  std::optional<Facing> facing_;
  std::function<void(CameraStatus)> changed_;
}
- (void)start:(std::optional<Facing>)facing changed:(std::function<void(CameraStatus)>)changed;
- (void)stop:(std::function<void()>)completed;
@end

@implementation HUXCameraCapture

- (instancetype)init {
  self = [super init];
  if (self) {
    queue_ = dispatch_queue_create("org.huxerui.camera.capture", DISPATCH_QUEUE_SERIAL);
    observers_ = [NSMutableArray new];
  }
  return self;
}

- (void)fail:(CameraErrorCode)code message:(NSString*)message {
  if (changed_) {
    changed_({.state = SessionState::Failed,
              .error = CameraError{code, message.UTF8String ?: "Camera capture failed."}});
  }
}

- (void)start:(std::optional<Facing>)facing changed:(std::function<void(CameraStatus)>)changed {
  dispatch_async(queue_, ^{
    changed_ = changed;
    if (!facing) {
      device_ = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
    } else {
      const AVCaptureDevicePosition position = *facing == Facing::Front ? AVCaptureDevicePositionFront :
                                                                         AVCaptureDevicePositionBack;
      AVCaptureDeviceDiscoverySession* discovery = [AVCaptureDeviceDiscoverySession
          discoverySessionWithDeviceTypes:@[AVCaptureDeviceTypeBuiltInWideAngleCamera]
                                mediaType:AVMediaTypeVideo position:position];
      for (AVCaptureDevice* candidate in discovery.devices) {
        if (candidate.position == position) {
          device_ = candidate;
          break;
        }
      }
    }
    if (!device_) {
      [self fail:CameraErrorCode::DeviceNotFound message:@"No camera matches the requested facing."];
      return;
    }
    facing_.reset();
    if (device_.position == AVCaptureDevicePositionFront) {
      facing_ = Facing::Front;
    } else if (device_.position == AVCaptureDevicePositionBack) {
      facing_ = Facing::Back;
    }
    NSError* error = nil;
    AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device_ error:&error];
    if (!input) {
      const auto code = error.code == AVErrorDeviceInUseByAnotherApplication ? CameraErrorCode::DeviceBusy :
                                                                             CameraErrorCode::ConfigurationFailed;
      [self fail:code message:error.localizedDescription];
      return;
    }
    session_ = [AVCaptureSession new];
    [session_ beginConfiguration];
    if ([session_ canSetSessionPreset:AVCaptureSessionPreset1280x720]) {
      session_.sessionPreset = AVCaptureSessionPreset1280x720;
    }
    output_ = [AVCaptureVideoDataOutput new];
    output_.alwaysDiscardsLateVideoFrames = YES;
    output_.videoSettings = @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)};
    if (![session_ canAddInput:input]) {
      [session_ commitConfiguration];
      [self fail:CameraErrorCode::ConfigurationFailed message:@"Cannot attach the camera input."];
      return;
    }
    [session_ addInput:input];
    if (![session_ canAddOutput:output_]) {
      [session_ commitConfiguration];
      [self fail:CameraErrorCode::ConfigurationFailed message:@"Cannot attach the preview output."];
      return;
    }
    [session_ addOutput:output_];
    AVCaptureConnection* connection = [output_ connectionWithMediaType:AVMediaTypeVideo];
    if (connection.isVideoMirroringSupported) {
      connection.automaticallyAdjustsVideoMirroring = NO;
      connection.videoMirrored = NO;
    }
    [output_ setSampleBufferDelegate:self queue:queue_];
    [session_ commitConfiguration];
    __weak HUXCameraCapture* weak = self;
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    [observers_ addObject:[center addObserverForName:AVCaptureSessionRuntimeErrorNotification object:session_
                                             queue:nil usingBlock:^(NSNotification* notification) {
      HUXCameraCapture* capture = weak;
      if (!capture) {
        return;
      }
      NSError* runtimeError = notification.userInfo[AVCaptureSessionErrorKey];
      dispatch_async(capture->queue_, ^{
        if (notification.object == capture->session_) {
          [capture fail:CameraErrorCode::CaptureFailed message:runtimeError.localizedDescription];
        }
      });
    }]];
    [observers_ addObject:[center addObserverForName:AVCaptureDeviceWasDisconnectedNotification object:device_
                                             queue:nil usingBlock:^(NSNotification* notification) {
      HUXCameraCapture* capture = weak;
      if (capture) {
        dispatch_async(capture->queue_, ^{
          if (notification.object == capture->device_) {
            [capture fail:CameraErrorCode::DeviceDisconnected message:@"The camera was disconnected."];
          }
        });
      }
    }]];
    [session_ startRunning];
    if (!session_.isRunning) {
      [self fail:CameraErrorCode::CaptureFailed message:@"The camera did not start."];
      return;
    }
    changed_({.state = SessionState::Running, .actual_facing = facing_});
  });
}

- (void)captureOutput:(AVCaptureOutput*)output didOutputSampleBuffer:(CMSampleBufferRef)sample
       fromConnection:(AVCaptureConnection*)connection {
  if (output != output_ || !changed_) {
    return;
  }
  CVPixelBufferRef buffer = CMSampleBufferGetImageBuffer(sample);
  if (!buffer) {
    return;
  }
  try {
    const huxerui::Size size{static_cast<float>(CVPixelBufferGetWidth(buffer)),
                    static_cast<float>(CVPixelBufferGetHeight(buffer))};
    const bool replaced = !texture_ || texture_->IntrinsicSize() != size;
    if (replaced) {
      if (texture_) {
        texture_->Finish();
      }
      texture_ = std::make_shared<macos::PixelBufferTexture>(size);
    }
    texture_->Publish(buffer);
    if (replaced) {
      changed_({.state = SessionState::Running, .actual_facing = facing_,
                .preview = PreviewOutput{.texture = texture_, .mirrored = static_cast<bool>(connection.videoMirrored)}});
    }
  } catch (const std::exception& error) {
    [self fail:CameraErrorCode::CaptureFailed message:[NSString stringWithUTF8String:error.what()]];
  }
}

- (void)stop:(std::function<void()>)completed {
  dispatch_async(queue_, ^{
    changed_ = {};
    for (id observer in observers_) {
      [NSNotificationCenter.defaultCenter removeObserver:observer];
    }
    [observers_ removeAllObjects];
    [output_ setSampleBufferDelegate:nil queue:nullptr];
    [session_ stopRunning];
    output_ = nil;
    session_ = nil;
    device_ = nil;
    if (texture_) {
      texture_->Finish();
      texture_.reset();
    }
    if (completed) {
      completed();
    }
  });
}

@end

namespace huxerui::camera::detail {
namespace {

class AppleCamera final : public CameraBackend {
public:
  AppleCamera() : capture_([HUXCameraCapture new]) {}
  void Start(std::optional<Facing> facing, std::function<void(CameraStatus)> changed) override {
    [capture_ start:facing changed:std::move(changed)];
  }
  void Stop(std::function<void()> completed) override {
    [capture_ stop:std::move(completed)];
  }

private:
  HUXCameraCapture* capture_;
};

} // namespace

void InstallPlatformCamera(RootContext& root) {
  root.RegisterPlatformModule<std::shared_ptr<CameraBackend>>(
      camera_module_name, [](PlatformAdapter&) -> std::shared_ptr<CameraBackend> {
        return std::make_shared<AppleCamera>();
      }
  );
}

} // namespace huxerui::camera::detail
