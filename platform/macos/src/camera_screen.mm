#import <AppKit/AppKit.h>
#import <CoreImage/CoreImage.h>
#import <ImageIO/ImageIO.h>
#import <QuartzCore/QuartzCore.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <objc/runtime.h>

#include <cmath>
#include <optional>
#include <utility>
#include <vector>
#include <unistd.h>

#include "camera_macos.h"

using namespace huxerui;
using namespace huxerui::camera;

namespace {

char aperture_owner_key;

struct ScreenArea {
  CGDirectDisplayID display = 0;
  CGRect source = CGRectZero;
  size_t width = 0;
  size_t height = 0;
};

std::optional<ScreenArea> ScreenRegion(NSWindow* window, std::optional<Rect> region) {
  if (!window || !region || region->IsEmpty() || !std::isfinite(region->x) || !std::isfinite(region->y) ||
      !std::isfinite(region->width) || !std::isfinite(region->height) || !window.screen || !window.contentView ||
      window.miniaturized) return std::nullopt;
  NSView* content = window.contentView;
  const NSRect bounds = content.bounds;
  const NSRect local = NSMakeRect(region->x,
      content.isFlipped ? region->y : NSHeight(bounds) - region->y - region->height,
      region->width, region->height);
  const NSRect global = [window convertRectToScreen:[content convertRect:local toView:nil]];
  const NSRect screen = window.screen.frame;
  if (!NSContainsRect(screen, global)) return std::nullopt;
  const CGFloat scale = window.backingScaleFactor;
  const size_t width = static_cast<size_t>(std::lround(NSWidth(global) * scale));
  const size_t height = static_cast<size_t>(std::lround(NSHeight(global) * scale));
  if (!width || !height) return std::nullopt;
  NSNumber* display = window.screen.deviceDescription[@"NSScreenNumber"];
  return ScreenArea{static_cast<CGDirectDisplayID>(display.unsignedIntValue),
      CGRectMake(NSMinX(global) - NSMinX(screen), NSMaxY(screen) - NSMaxY(global),
                 NSWidth(global), NSHeight(global)), width, height};
}

CameraResult<ImageAsset> EncodeScreenPhoto(CVPixelBufferRef buffer, PhotoOptions options) {
  @autoreleasepool {
    CIImage* image = [CIImage imageWithCVPixelBuffer:buffer];
    if (options.mirror == MirrorMode::On)
      image = [image imageByApplyingTransform:CGAffineTransformMakeScale(-1, 1)];
    image = [image imageByApplyingTransform:
        CGAffineTransformMakeTranslation(-image.extent.origin.x, -image.extent.origin.y)];
    CIContext* context = [CIContext contextWithOptions:@{kCIContextCacheIntermediates : @NO}];
    CGImageRef pixels = [context createCGImage:image fromRect:image.extent];
    if (!pixels)
      return CameraResult<ImageAsset>::Failure({CameraErrorCode::CaptureFailed, "Cannot read the screen photo."});
    NSMutableData* jpeg = [NSMutableData new];
    CGImageDestinationRef destination =
        CGImageDestinationCreateWithData((__bridge CFMutableDataRef)jpeg, CFSTR("public.jpeg"), 1, nullptr);
    bool encoded = false;
    if (destination) {
      NSDictionary* properties = @{
          (id)kCGImageDestinationLossyCompressionQuality : @(options.jpeg_quality / 100.0),
          (id)kCGImagePropertyOrientation : @1};
      CGImageDestinationAddImage(destination, pixels, (__bridge CFDictionaryRef)properties);
      encoded = CGImageDestinationFinalize(destination);
      CFRelease(destination);
    }
    CGImageRelease(pixels);
    if (!encoded)
      return CameraResult<ImageAsset>::Failure({CameraErrorCode::CaptureFailed, "Cannot encode the screen JPEG."});
    const auto* begin = static_cast<const std::byte*>(jpeg.bytes);
    return CameraResult<ImageAsset>::Success(ImageAsset::FromEncoded(Bytes(begin, begin + jpeg.length)));
  }
}

} // namespace

// The aperture and on-demand stream paths have not been compiled or run on macOS hardware.
@interface HUXMacScreenCapture : NSObject <SCStreamOutput, SCStreamDelegate> {
  __weak NSWindow* window_;
  __weak NSView* masked_view_;
  CALayer* original_mask_;
  CAShapeLayer* aperture_mask_;
  NSColor* original_background_;
  BOOL original_opaque_;
  BOOL original_wants_layer_;
  bool selecting_;
  std::optional<bool> ready_;
  std::optional<Rect> region_;
  NSMutableArray* observers_;
  std::function<void(CameraStatus)> changed_;

  dispatch_queue_t queue_;
  SCStream* stream_;
  NSUInteger generation_;
  bool active_;
  bool stopping_stream_;
  PhotoOptions photo_options_;
  std::optional<ScreenArea> photo_area_;
  std::function<void(CameraResult<ImageAsset>)> photo_;
  std::vector<std::function<void()>> stop_callbacks_;
}
- (instancetype)initWithWindow:(NSWindow*)window;
- (void)start:(std::function<void(CameraStatus)>)changed;
- (void)setRegion:(std::optional<Rect>)region;
- (void)stop:(std::function<void()>)completed;
- (void)photo:(PhotoOptions)options completed:(std::function<void(CameraResult<ImageAsset>)>)completed;
@end

@implementation HUXMacScreenCapture

- (instancetype)initWithWindow:(NSWindow*)window {
  self = [super init];
  if (self) {
    window_ = window;
    queue_ = dispatch_queue_create("org.huxerui.camera.macos.screen", DISPATCH_QUEUE_SERIAL);
    observers_ = [NSMutableArray new];
  }
  return self;
}

- (void)updateAperture {
  if (!selecting_) return;
  NSView* content = masked_view_;
  const bool valid = ScreenRegion(window_, region_).has_value();
  if (content && aperture_mask_) {
    const CGRect bounds = content.layer.bounds;
    CGMutablePathRef path = CGPathCreateMutable();
    CGPathAddRect(path, nullptr, bounds);
    if (valid) {
      // Convert the top-left UI rectangle to the backing layer's local geometry.
      const CGFloat y = content.layer.geometryFlipped ? region_->y : bounds.size.height - region_->y - region_->height;
      CGPathAddRect(path, nullptr, CGRectMake(bounds.origin.x + region_->x, bounds.origin.y + y,
                                            region_->width, region_->height));
    }
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    aperture_mask_.frame = bounds;
    aperture_mask_.path = path;
    [CATransaction commit];
    CGPathRelease(path);
  }
  if (ready_ != valid) {
    ready_ = valid;
    changed_({.state = valid ? SessionState::Running : SessionState::Starting});
  }
}

- (void)setRegion:(std::optional<Rect>)region {
  region_ = region;
  [self updateAperture];
}

- (void)start:(std::function<void(CameraStatus)>)changed {
  NSWindow* window = window_;
  if (!window.contentView) {
    changed({.state = SessionState::Failed,
             .error = CameraError{CameraErrorCode::Unavailable, "Screen capture requires a desktop window."}});
    return;
  }
  if (@available(macOS 12.3, *)) {} else {
    changed({.state = SessionState::Failed,
             .error = CameraError{CameraErrorCode::Unavailable, "Screen capture requires macOS 12.3 or later."}});
    return;
  }
  if (objc_getAssociatedObject(window, &aperture_owner_key)) {
    changed({.state = SessionState::Failed,
             .error = CameraError{CameraErrorCode::DeviceBusy, "This window already has a screen viewfinder."}});
    return;
  }
  objc_setAssociatedObject(window, &aperture_owner_key, self, OBJC_ASSOCIATION_ASSIGN);
  NSView* content = window.contentView;
  masked_view_ = content;
  original_opaque_ = window.opaque;
  original_background_ = window.backgroundColor;
  original_wants_layer_ = content.wantsLayer;
  content.wantsLayer = YES;
  original_mask_ = content.layer.mask;
  aperture_mask_ = [CAShapeLayer layer];
  aperture_mask_.fillRule = kCAFillRuleEvenOdd;
  aperture_mask_.fillColor = NSColor.blackColor.CGColor;
  content.layer.mask = aperture_mask_;
  window.opaque = NO;
  window.backgroundColor = NSColor.clearColor;
  selecting_ = true;
  ready_.reset();
  changed_ = std::move(changed);
  __weak HUXMacScreenCapture* weak = self;
  for (NSNotificationName name in @[NSWindowDidMoveNotification, NSWindowDidResizeNotification,
                                    NSWindowDidChangeBackingPropertiesNotification, NSWindowDidChangeScreenNotification,
                                    NSWindowDidMiniaturizeNotification, NSWindowDidDeminiaturizeNotification]) {
    id observer = [NSNotificationCenter.defaultCenter addObserverForName:name object:window
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*) { [weak updateAperture]; }];
    [observers_ addObject:observer];
  }
  dispatch_async(queue_, ^{ active_ = true; });
  [self updateAperture];
}

- (void)completeStops {
  if (photo_ || stream_ || stopping_stream_) return;
  auto callbacks = std::exchange(stop_callbacks_, {});
  for (const auto& callback : callbacks) callback();
}

- (void)finishPhoto:(CameraResult<ImageAsset>)result {
  if (!photo_ || stopping_stream_) return;
  ++generation_;
  SCStream* previous = stream_;
  stream_ = nil;
  stopping_stream_ = previous != nil;
  auto deliver = ^{
    stopping_stream_ = false;
    auto completed = std::exchange(photo_, {});
    photo_area_.reset();
    if (completed) {
      if (active_) completed(result);
      else completed(CameraResult<ImageAsset>::Failure({CameraErrorCode::Interrupted, "Screen capture was interrupted."}));
    }
    [self completeStops];
  };
  if (previous) {
    [previous stopCaptureWithCompletionHandler:^(NSError*) { dispatch_async(queue_, deliver); }];
  } else {
    deliver();
  }
}

- (void)stop:(std::function<void()>)completed {
  selecting_ = false;
  for (id observer in observers_) [NSNotificationCenter.defaultCenter removeObserver:observer];
  [observers_ removeAllObjects];
  NSWindow* window = window_;
  if (objc_getAssociatedObject(window, &aperture_owner_key) == self) {
    objc_setAssociatedObject(window, &aperture_owner_key, nil, OBJC_ASSOCIATION_ASSIGN);
    NSView* content = masked_view_;
    if (content.layer.mask == aperture_mask_) content.layer.mask = original_mask_;
    content.wantsLayer = original_wants_layer_;
    window.opaque = original_opaque_;
    window.backgroundColor = original_background_;
    [content setNeedsDisplay:YES];
  }
  aperture_mask_ = nil;
  original_mask_ = nil;
  original_background_ = nil;
  masked_view_ = nil;
  ready_.reset();
  dispatch_async(queue_, ^{
    active_ = false;
    ++generation_;
    if (completed) stop_callbacks_.push_back(completed);
    [self finishPhoto:CameraResult<ImageAsset>::Failure({CameraErrorCode::Interrupted, "Screen capture was interrupted."})];
    [self completeStops];
  });
}

- (void)photo:(PhotoOptions)options completed:(std::function<void(CameraResult<ImageAsset>)>)completed {
  const auto area = selecting_ ? ScreenRegion(window_, region_) : std::nullopt;
  if (!area) {
    completed(CameraResult<ImageAsset>::Failure(
        {CameraErrorCode::NotReady, "Place the screen viewfinder wholly inside one visible display."}));
    return;
  }
  dispatch_async(queue_, ^{
    if (!active_) {
      completed(CameraResult<ImageAsset>::Failure({CameraErrorCode::NotReady, "Screen selection is stopped."}));
      return;
    }
    if (photo_) {
      completed(CameraResult<ImageAsset>::Failure({CameraErrorCode::OperationInProgress, "A screen capture is in progress."}));
      return;
    }
    photo_ = completed;
    photo_options_ = options;
    photo_area_ = area;
    const NSUInteger token = ++generation_;
    if (@available(macOS 12.3, *)) {
      [SCShareableContent getShareableContentExcludingDesktopWindows:NO onScreenWindowsOnly:YES
          completionHandler:^(SCShareableContent* content, NSError* error) {
        dispatch_async(queue_, ^{
          if (token != generation_ || !active_ || !photo_) return;
          if (error || !content) {
            [self finishPhoto:CameraResult<ImageAsset>::Failure({CameraErrorCode::PermissionDenied,
                error.localizedDescription.UTF8String ?: "Screen Recording access is required."})];
            return;
          }
          SCDisplay* selected = nil;
          for (SCDisplay* display in content.displays)
            if (display.displayID == area->display) { selected = display; break; }
          SCRunningApplication* own_app = nil;
          for (SCRunningApplication* app in content.applications)
            if (app.processID == getpid()) { own_app = app; break; }
          if (!selected || !own_app) {
            [self finishPhoto:CameraResult<ImageAsset>::Failure(
                {CameraErrorCode::Unavailable, "Cannot resolve the display or exclude this application."})];
            return;
          }
          SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:selected
              excludingApplications:@[own_app] exceptingWindows:@[]];
          SCStreamConfiguration* configuration = [SCStreamConfiguration new];
          configuration.sourceRect = area->source;
          configuration.width = area->width;
          configuration.height = area->height;
          configuration.scalesToFit = NO;
          configuration.pixelFormat = kCVPixelFormatType_32BGRA;
          configuration.showsCursor = NO;
          configuration.queueDepth = 2;
          stream_ = [[SCStream alloc] initWithFilter:filter configuration:configuration delegate:self];
          NSError* output_error = nil;
          if (![stream_ addStreamOutput:self type:SCStreamOutputTypeScreen sampleHandlerQueue:queue_ error:&output_error]) {
            stream_ = nil;
            [self finishPhoto:CameraResult<ImageAsset>::Failure({CameraErrorCode::ConfigurationFailed,
                output_error.localizedDescription.UTF8String ?: "Cannot configure screen capture."})];
            return;
          }
          [stream_ startCaptureWithCompletionHandler:^(NSError* start_error) {
            dispatch_async(queue_, ^{
              if (token != generation_ || !photo_) return;
              if (start_error) [self finishPhoto:CameraResult<ImageAsset>::Failure({CameraErrorCode::CaptureFailed,
                  start_error.localizedDescription.UTF8String ?: "Cannot start screen capture."})];
            });
          }];
        });
      }];
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), queue_, ^{
        if (token == generation_ && photo_)
          [self finishPhoto:CameraResult<ImageAsset>::Failure({CameraErrorCode::CaptureFailed, "Screen capture timed out."})];
      });
    }
  });
}

- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type {
  if (stream != stream_ || !active_ || !photo_ || !photo_area_ ||
      type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sample)) return;
  NSArray* attachments = (__bridge NSArray*)CMSampleBufferGetSampleAttachmentsArray(sample, false);
  NSNumber* frame_status = attachments.firstObject[SCStreamFrameInfoStatus];
  if (!frame_status || frame_status.integerValue != SCFrameStatusComplete) return;
  CVPixelBufferRef buffer = CMSampleBufferGetImageBuffer(sample);
  if (!buffer || CVPixelBufferGetWidth(buffer) != photo_area_->width ||
      CVPixelBufferGetHeight(buffer) != photo_area_->height) return;
  try {
    [self finishPhoto:EncodeScreenPhoto(buffer, photo_options_)];
  } catch (const std::exception& error) {
    [self finishPhoto:CameraResult<ImageAsset>::Failure({CameraErrorCode::CaptureFailed, error.what()})];
  }
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
  dispatch_async(queue_, ^{
    if (stream != stream_) return;
    [self finishPhoto:CameraResult<ImageAsset>::Failure({CameraErrorCode::CaptureFailed,
        error.localizedDescription.UTF8String ?: "Screen capture stopped."})];
  });
}

@end

namespace huxerui::camera::detail {
namespace {

class MacScreenCamera final : public CameraBackend {
public:
  explicit MacScreenCamera(NSWindow* window) : capture_([[HUXMacScreenCapture alloc] initWithWindow:window]) {}
  void Start(std::optional<Facing>, std::function<void(CameraStatus)> changed) override {
    changed({.state = SessionState::Failed,
             .error = CameraError{CameraErrorCode::Unavailable, "This backend captures the screen only."}});
  }
  void StartScreen(std::function<void(CameraStatus)> changed) override { [capture_ start:std::move(changed)]; }
  void SetScreenRegion(std::optional<Rect> region) override { [capture_ setRegion:region]; }
  void Stop(std::function<void()> completed) override { [capture_ stop:std::move(completed)]; }
  void CapturePhoto(PhotoOptions options, std::function<void(CameraResult<ImageAsset>)> completed) override {
    [capture_ photo:options completed:std::move(completed)];
  }

private:
  HUXMacScreenCapture* capture_;
};

} // namespace

std::shared_ptr<CameraBackend> CreateMacScreenCamera(NSWindow* window) {
  return std::make_shared<MacScreenCamera>(window);
}

} // namespace huxerui::camera::detail
