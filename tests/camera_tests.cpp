#include <huxerui/huxerui.h>
#include <huxerui/platform_adapter.h>

#include <array>
#include <cmath>
#include <deque>
#include <iostream>
#include <limits>
#include <stdexcept>

#include "camera_internal.h"

using namespace huxerui;
using namespace huxerui::camera;

namespace {

void Check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void Near(float actual, float expected) {
  Check(std::abs(actual - expected) < 0.001F, "Unexpected preview coordinate.");
}

void TestGeometry() {
  PreviewOutput output;
  const Size source{400.0F, 200.0F};
  const std::array<camera::PreviewRotation, 4> rotations{camera::PreviewRotation::R0, camera::PreviewRotation::R90, camera::PreviewRotation::R180, camera::PreviewRotation::R270};
  const std::array<Point, 4> origin{{{0.0F, 0.0F}, {200.0F, 0.0F}, {400.0F, 200.0F}, {0.0F, 400.0F}}};
  const std::array<Point, 4> top_right{{{400.0F, 0.0F}, {200.0F, 400.0F}, {0.0F, 200.0F}, {0.0F, 0.0F}}};
  for (std::size_t index = 0; index < rotations.size(); ++index) {
    output.rotation = rotations[index];
    const Size bounds = index % 2 == 0 ? source : Size{200.0F, 400.0F};
    const auto placed = camera::detail::PlacePreview(source, output, {.fit = ImageFit::Contain}, {}, bounds);
    Check(placed.has_value(), "Valid geometry was rejected.");
    const auto first = placed->transform.Apply({0.0F, 0.0F});
    const auto second = placed->transform.Apply({400.0F, 0.0F});
    Near(first.x, origin[index].x);
    Near(first.y, origin[index].y);
    Near(second.x, top_right[index].x);
    Near(second.y, top_right[index].y);
    const auto inverse = placed->transform.Inverse(second);
    Check(inverse.has_value(), "Preview transform must remain invertible.");
    Near(inverse->x, 400.0F);
    Near(inverse->y, 0.0F);
    for (bool already_mirrored : {false, true}) {
      output.mirrored = already_mirrored;
      const auto front = camera::detail::PlacePreview(source, output, {.fit = ImageFit::Contain}, Facing::Front, bounds);
      const auto point = front->transform.Apply({0.0F, 0.0F});
      Near(point.x, already_mirrored ? origin[index].x : bounds.width - origin[index].x);
      Near(point.y, origin[index].y);
    }
    output.mirrored = false;
  }
  output.rotation = camera::PreviewRotation::R0;
  const auto contain = camera::detail::PlacePreview(source, output, {.fit = ImageFit::Contain}, {}, {200.0F, 200.0F});
  Near(contain->transform.Apply({0.0F, 0.0F}).y, 50.0F);
  const auto cover = camera::detail::PlacePreview(source, output, {.fit = ImageFit::Cover}, {}, {200.0F, 200.0F});
  Near(cover->transform.Apply({0.0F, 0.0F}).x, -100.0F);
  const auto fill = camera::detail::PlacePreview(source, output, {.fit = ImageFit::Fill}, {}, {200.0F, 200.0F});
  Near(fill->transform.Apply({400.0F, 200.0F}).x, 200.0F);
  Near(fill->transform.Apply({400.0F, 200.0F}).y, 200.0F);
  const auto small = camera::detail::PlacePreview(source, output, {.fit = ImageFit::ScaleDown}, {}, {800.0F, 800.0F});
  Near(small->transform.m11, 1.0F);
  const auto none = camera::detail::PlacePreview(source, output, {.fit = ImageFit::None}, {}, {100.0F, 100.0F});
  Near(none->transform.m11, 1.0F);
  output.crop = {0.25F, 0.25F, 0.5F, 0.5F};
  const auto cropped = camera::detail::PlacePreview(source, output, {}, {}, {200.0F, 100.0F});
  Check(cropped->source == Rect{100.0F, 50.0F, 200.0F, 100.0F}, "Crop must use intrinsic source coordinates.");
  Check(!camera::detail::PlacePreview(source, output, {}, {}, {}), "Empty bounds must not draw.");
  output.crop.width = std::numeric_limits<float>::quiet_NaN();
  Check(!camera::detail::PlacePreview(source, output, {}, {}, source), "Non-finite geometry must not draw.");
}

class TestPlatform final : public PlatformAdapter {
public:
  TestPlatform() : PlatformAdapter([this](std::function<void()> work) { pending.push_back(std::move(work)); }) {}
  void RequestFrameAt(double) override {}
  double Now() const noexcept override { return 0.0; }
  FontMetrics Metrics(const Font&) override { return {}; }
  TextRunMetrics MeasureRun(std::string_view, const TextStyle&, const TextShapingOptions&) override { return {}; }
  TextLayoutMetrics MeasureText(const AttributedText&, const TextStyle&, float, const TextLayoutOptions&) override {
    return {};
  }
  void Drain(Runtime& runtime) {
    for (int turns = 0; turns < 30; ++turns) {
      while (!pending.empty()) {
        auto work = std::move(pending.front());
        pending.pop_front();
        work();
      }
      runtime.BuildFrame();
      if (pending.empty()) {
        return;
      }
    }
    throw std::runtime_error("UI dispatch did not settle.");
  }

  std::deque<std::function<void()>> pending;
};

class TestBackend final : public camera::detail::CameraBackend {
public:
  int starts = 0;
  int stops = 0;
  void Start(std::optional<Facing>, std::function<void(CameraStatus)>) override { ++starts; }
  void Stop(std::function<void()> completed) override {
    ++stops;
    if (completed) {
      completed();
    }
  }
};

State<bool> visible;
State<CameraOptions> requested;
std::optional<CameraSession> retained_session;
std::vector<std::shared_ptr<TestBackend>> backends;

[[huxerui::composable]]
View TestOwner() {
  requested = UseState(CameraOptions{.active = false});
  auto session = UseCamera(requested.Get());
  retained_session = session;
  return Column {
    CameraPreview(session).With(Frame{.width = 80.0F, .height = 60.0F}),
    CameraPreview(session).With(Frame{.width = 80.0F, .height = 60.0F}),
  };
}

View TestRoot() {
  visible = UseState(true);
  return visible.Get() ? TestOwner() : Canvas([](PaintContext&, Size) {});
}

void TestSessionLifetime() {
  Application application(TestRoot, {.show_debug_overlay = false, .root_hooks = {
    [](RootContext& root) {
      root.RegisterPlatformModule<std::shared_ptr<camera::detail::CameraBackend>>(
          camera::detail::camera_module_name, [](PlatformAdapter&) -> std::shared_ptr<camera::detail::CameraBackend> {
            auto backend = std::make_shared<TestBackend>();
            backends.push_back(backend);
            return backend;
          }
      );
    },
  }});
  TestPlatform platform;
  {
    Runtime runtime(application, platform);
    runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
    runtime.BuildFrame();
    platform.Drain(runtime);
    Check(backends.size() == 1, "Two previews must share one backend.");
    Check(retained_session->Status().state == SessionState::Stopped, "Inactive session must start stopped.");
    requested = CameraOptions{.active = true};
    runtime.BuildFrame();
    platform.Drain(runtime);
    Check(retained_session->Status().state == SessionState::Failed, "Unavailable permission must fail.");
    Check(retained_session->Status().error->code == CameraErrorCode::Unavailable, "Permission capability error lost.");
    Check(backends.front()->starts == 0, "Unauthorized capture must not start.");
    retained_session->Retry();
    platform.Drain(runtime);
    Check(backends.front()->starts == 0, "Retry must not bypass authorization.");
    requested = CameraOptions{.active = false};
    runtime.BuildFrame();
    platform.Drain(runtime);
    Check(retained_session->Status().state == SessionState::Stopped, "Stop must clear a previous error.");
    visible = false;
    runtime.BuildFrame();
    platform.Drain(runtime);
    Check(retained_session->Status().state == SessionState::Closed, "Retained handles must close on owner unmount.");
    Check(backends.front()->stops == 1, "Owner unmount must release the backend exactly once.");
    retained_session->Retry();
    Check(retained_session->Status().state == SessionState::Closed, "Retry must not reopen a closed owner.");
  }
  backends.clear();
  retained_session.reset();
}

} // namespace

int main() {
  try {
    TestGeometry();
    TestSessionLifetime();
    std::cout << "Camera geometry and session lifetime tests passed.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
