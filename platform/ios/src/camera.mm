#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>

#include <exception>
#include <utility>

#include <huxerui/ios/external_texture.h>
#include <huxerui/ios/platform_registry.h>

#include "camera_internal.h"

using namespace huxerui;
using namespace huxerui::camera;

namespace {

PreviewRotation RotationForOrientation(UIInterfaceOrientation orientation) {
  switch (orientation) {
    case UIInterfaceOrientationLandscapeLeft: return PreviewRotation::R90;
    case UIInterfaceOrientationPortraitUpsideDown: return PreviewRotation::R180;
    case UIInterfaceOrientationLandscapeRight: return PreviewRotation::R270;
    default: return PreviewRotation::R0;
  }
}

// AVFoundation normalizes sensor mounting into a fixed portrait buffer, including on landscape-mounted iPads.
// The orientation API remains available on the iOS 15 deployment target.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
bool ConfigurePortraitOutput(AVCaptureConnection* connection) {
  if (!connection.isVideoOrientationSupported) {
    return false;
  }
  connection.videoOrientation = AVCaptureVideoOrientationPortrait;
  if (connection.isVideoMirroringSupported) {
    connection.automaticallyAdjustsVideoMirroring = NO;
    connection.videoMirrored = NO;
  }
  return true;
}
#pragma clang diagnostic pop

} // namespace

// A transparent child observes its owning window's transitions without replacing the application's delegates.
@interface HUXIOSCameraOrientation : UIViewController
@property(nonatomic, copy) void (^changed)(UIInterfaceOrientation);
- (void)updateOrientation;
@end

@implementation HUXIOSCameraOrientation {
  UIInterfaceOrientation orientation_;
}

- (void)loadView {
  self.view = [UIView new];
  self.view.userInteractionEnabled = NO;
  self.view.accessibilityElementsHidden = YES;
  self.view.backgroundColor = UIColor.clearColor;
}

- (void)updateOrientation {
  UIWindowScene* scene = self.view.window.windowScene;
  if (!self.view.window) {
    return;
  }
  UIInterfaceOrientation orientation = UIInterfaceOrientationUnknown;
  if (scene) {
    if (@available(iOS 16.0, *)) {
      orientation = scene.effectiveGeometry.interfaceOrientation;
    } else {
      orientation = scene.interfaceOrientation;
    }
  } else {
    // Hosts using the UIApplication lifecycle do not have a UIWindowScene.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    orientation = self.parentViewController.interfaceOrientation;
#pragma clang diagnostic pop
  }
  if (orientation != UIInterfaceOrientationUnknown && orientation != orientation_) {
    orientation_ = orientation;
    if (self.changed) {
      self.changed(orientation);
    }
  }
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  [self updateOrientation];
}

- (void)viewWillTransitionToSize:(CGSize)size
      withTransitionCoordinator:(id<UIViewControllerTransitionCoordinator>)coordinator {
  [super viewWillTransitionToSize:size withTransitionCoordinator:coordinator];
  __weak HUXIOSCameraOrientation* weak = self;
  [coordinator animateAlongsideTransition:nil completion:^(id<UIViewControllerTransitionCoordinatorContext>) {
    [weak updateOrientation];
  }];
}

@end

@interface HUXIOSCameraCapture : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate> {
  __weak UIViewController* owner_;
  HUXIOSCameraOrientation* orientationObserver_;
  dispatch_queue_t queue_;
  AVCaptureSession* session_;
  AVCaptureVideoDataOutput* output_;
  AVCaptureDevice* device_;
  NSMutableArray* observers_;
  std::shared_ptr<ios::PixelBufferTexture> texture_;
  std::optional<Facing> facing_;
  PreviewRotation rotation_;
  bool interrupted_;
  bool failed_;
  std::function<void(CameraStatus)> changed_;
}
- (instancetype)initWithOwner:(UIViewController*)owner;
- (void)start:(std::optional<Facing>)facing changed:(std::function<void(CameraStatus)>)changed;
- (void)stop:(std::function<void()>)completed;
@end

@implementation HUXIOSCameraCapture

- (instancetype)initWithOwner:(UIViewController*)owner {
  self = [super init];
  if (self) {
    owner_ = owner;
    queue_ = dispatch_queue_create("org.huxerui.camera.ios.capture", DISPATCH_QUEUE_SERIAL);
    observers_ = [NSMutableArray new];
  }
  return self;
}

- (void)publish {
  if (!changed_ || failed_) {
    return;
  }
  CameraStatus status{.state = interrupted_ ? SessionState::Suspended : SessionState::Running,
                          .actual_facing = facing_};
  if (texture_) {
    status.preview = PreviewOutput{.texture = texture_, .rotation = rotation_};
  }
  changed_(std::move(status));
}

- (void)fail:(CameraErrorCode)code message:(NSString*)message {
  if (changed_ && !failed_) {
    failed_ = true;
    changed_({.state = SessionState::Failed,
              .error = CameraError{code, message.UTF8String ?: "Camera capture failed."}});
  }
}

- (void)observe:(NSNotificationName)name object:(id)object handler:(void (^)(NSNotification*))handler {
  __weak HUXIOSCameraCapture* weak = self;
  id token = [NSNotificationCenter.defaultCenter addObserverForName:name object:object queue:nil
                                                       usingBlock:^(NSNotification* notification) {
    HUXIOSCameraCapture* capture = weak;
    if (capture) {
      dispatch_async(capture->queue_, ^{
        if (capture->changed_ && (notification.object == capture->session_ ||
                                  notification.object == capture->device_)) {
          handler(notification);
        }
      });
    }
  }];
  [observers_ addObject:token];
}

- (void)start:(std::optional<Facing>)facing changed:(std::function<void(CameraStatus)>)changed {
  UIViewController* owner = owner_;
  if (!owner) {
    changed({.state = SessionState::Failed,
             .error = CameraError{CameraErrorCode::Unavailable, "The camera window is no longer available."}});
    return;
  }
  orientationObserver_ = [HUXIOSCameraOrientation new];
  [owner addChildViewController:orientationObserver_];
  orientationObserver_.view.frame = owner.view.bounds;
  orientationObserver_.view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  [owner.view addSubview:orientationObserver_.view];
  [orientationObserver_ didMoveToParentViewController:owner];
  __weak HUXIOSCameraCapture* weak = self;
  orientationObserver_.changed = ^(UIInterfaceOrientation orientation) {
    HUXIOSCameraCapture* capture = weak;
    if (capture) {
      dispatch_async(capture->queue_, ^{
        const auto rotation = RotationForOrientation(orientation);
        if (rotation != capture->rotation_) {
          capture->rotation_ = rotation;
          if (capture->texture_) {
            [capture publish];
          }
        }
      });
    }
  };
  [orientationObserver_ updateOrientation];

  dispatch_async(queue_, ^{
    changed_ = changed;
    failed_ = false;
    interrupted_ = false;
    facing_.reset();
    const AVCaptureDevicePosition position = !facing ? AVCaptureDevicePositionUnspecified :
        (*facing == Facing::Front ? AVCaptureDevicePositionFront : AVCaptureDevicePositionBack);
    if (!facing) {
      device_ = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
    } else {
      device_ = [AVCaptureDeviceDiscoverySession
          discoverySessionWithDeviceTypes:@[AVCaptureDeviceTypeBuiltInWideAngleCamera]
                                mediaType:AVMediaTypeVideo position:position].devices.firstObject;
    }
    if (!device_) {
      [self fail:CameraErrorCode::DeviceNotFound message:@"No camera matches the requested facing."];
      return;
    }
    if (device_.position == AVCaptureDevicePositionFront) {
      facing_ = Facing::Front;
    } else if (device_.position == AVCaptureDevicePositionBack) {
      facing_ = Facing::Back;
    }
    NSError* error = nil;
    AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device_ error:&error];
    if (!input) {
      const auto code = error.code == AVErrorDeviceInUseByAnotherApplication ? CameraErrorCode::DeviceBusy :
          (error.code == AVErrorApplicationIsNotAuthorizedToUseDevice ? CameraErrorCode::PermissionDenied :
                                                                     CameraErrorCode::ConfigurationFailed);
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
    if (![output_.availableVideoCVPixelFormatTypes containsObject:@(kCVPixelFormatType_32BGRA)]) {
      [session_ commitConfiguration];
      [self fail:CameraErrorCode::ConfigurationFailed message:@"The camera cannot provide BGRA preview frames."];
      return;
    }
    output_.videoSettings = @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)};
    if (!ConfigurePortraitOutput([output_ connectionWithMediaType:AVMediaTypeVideo])) {
      [session_ commitConfiguration];
      [self fail:CameraErrorCode::ConfigurationFailed message:@"The camera cannot provide portrait preview frames."];
      return;
    }
    [output_ setSampleBufferDelegate:self queue:queue_];
    [session_ commitConfiguration];

    [self observe:AVCaptureSessionRuntimeErrorNotification object:session_ handler:^(NSNotification* notification) {
      NSError* runtimeError = notification.userInfo[AVCaptureSessionErrorKey];
      [weak fail:CameraErrorCode::CaptureFailed message:runtimeError.localizedDescription];
    }];
    [self observe:AVCaptureDeviceWasDisconnectedNotification object:device_ handler:^(NSNotification*) {
      [weak fail:CameraErrorCode::DeviceDisconnected message:@"The camera was disconnected."];
    }];
    [self observe:AVCaptureSessionWasInterruptedNotification object:session_ handler:^(NSNotification*) {
      HUXIOSCameraCapture* capture = weak;
      if (capture) {
        capture->interrupted_ = true;
        [capture publish];
      }
    }];
    [self observe:AVCaptureSessionInterruptionEndedNotification object:session_ handler:^(NSNotification*) {
      HUXIOSCameraCapture* capture = weak;
      if (capture && !capture->failed_) {
        capture->interrupted_ = false;
        if (!capture->session_.isRunning) {
          [capture->session_ startRunning];
        }
        if (capture->session_.isRunning) {
          [capture publish];
        } else {
          [capture fail:CameraErrorCode::CaptureFailed message:@"The camera did not resume after interruption."];
        }
      }
    }];
    [session_ startRunning];
    if (!session_.isRunning) {
      [self fail:CameraErrorCode::CaptureFailed message:@"The camera did not start."];
      return;
    }
    interrupted_ = session_.isInterrupted;
    [self publish];
  });
}

- (void)captureOutput:(AVCaptureOutput*)output didOutputSampleBuffer:(CMSampleBufferRef)sample
       fromConnection:(AVCaptureConnection*)connection {
  if (output != output_ || !changed_ || failed_ || interrupted_) {
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
      texture_ = std::make_shared<ios::PixelBufferTexture>(size);
    }
    texture_->Publish(buffer);
    if (replaced) {
      [self publish];
    }
  } catch (const std::exception& error) {
    [self fail:CameraErrorCode::CaptureFailed message:[NSString stringWithUTF8String:error.what()]];
  }
}

- (void)stop:(std::function<void()>)completed {
  orientationObserver_.changed = nil;
  [orientationObserver_ willMoveToParentViewController:nil];
  [orientationObserver_.view removeFromSuperview];
  [orientationObserver_ removeFromParentViewController];
  orientationObserver_ = nil;
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

class IOSCamera final : public CameraBackend {
public:
  explicit IOSCamera(UIViewController* owner) : capture_([[HUXIOSCameraCapture alloc] initWithOwner:owner]) {}

  void Start(std::optional<Facing> facing, std::function<void(CameraStatus)> changed) override {
    [capture_ start:facing changed:std::move(changed)];
  }

  void Stop(std::function<void()> completed) override {
    [capture_ stop:std::move(completed)];
  }

private:
  HUXIOSCameraCapture* capture_;
};

} // namespace

void InstallPlatformCamera(RootContext& root) {
  root.RegisterPlatformModule<std::shared_ptr<CameraBackend>>(
      camera_module_name, ios::PlatformModuleFactory<std::shared_ptr<CameraBackend>>{
          .create = [](PlatformAdapter&, UIViewController* owner) -> std::shared_ptr<CameraBackend> {
            return std::make_shared<IOSCamera>(owner);
          },
      }
  );
}

} // namespace huxerui::camera::detail
