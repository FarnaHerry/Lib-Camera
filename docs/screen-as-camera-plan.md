# 桌面内容作为相机输入：实施计划

## 目标与行为定义

在桌面端增加“屏幕取景”模式。取景框像一块透视镜：它显示**当前屏幕上同一位置、位于本应用窗口后方**的画面；移动或缩放窗口、调整取景框布局后，取样区域同步变化。按下拍照，得到按下时同一区域的 JPEG。现有实体摄像头模式保持原有语义。

例子：应用窗口为 200×200 逻辑单位，取景框为其中 100×100。屏幕取景只读取取景框在桌面上的投影区域；按钮、边框和本应用窗口不进入预览或照片。JPEG 尺寸按该区域的物理像素计算，可能受显示器缩放影响，并不固定等于 100×100。

实现目标为 Windows 10 2004 及以上、macOS 12.3 及以上的桌面窗口。Windows 已在本机编译，macOS 代码已接入但尚未在 macOS 上编译或运行，标记为**未验证**。Linux、iOS、Android、Web 目前使用统一的 `Unavailable` 回退；公共 API 已为它们留出接入位置。

## 与现有 `main` 接口保持一致

已对照仓库 `main` 分支：公开模式是 `UseCamera(CameraOptions)` 创建会话，`CameraPreview(session, PreviewOptions)` 返回可继续 `.With(...)` 修饰的普通 `View`，`session.CapturePhotoAsync(PhotoOptions)` 返回照片。`farna/main` 对这些接口的主要变动是适配新的 `ApplicationContext`/窗口所有权，并未改变上述使用方式。新增能力沿用这一风格，不引入另一套启动、拍照和保存流程。

在 `CameraOptions` 尾部追加默认值为实体摄像头的 `source`，以保留旧调用行为；桌面模式用 `ScreenPreview(session, PreviewOptions)` 建立屏幕区域锚点，并像 `CameraPreview` 一样返回可设置 `Frame`、背景、圆角和裁剪的 `View`。其他预览仍可用 `CameraPreview(session)` 显示同一输入，但不会决定采样位置。

```cpp
auto session = camera::UseCamera({.active = active, .source = camera::CaptureSource::Screen});
return camera::ScreenPreview(session).With(Frame{.width = 100.0F, .height = 100.0F});
// 拍照仍调用 co_await session.CapturePhotoAsync();
```

当前取景框**可以自定义大小和样式**。`CameraPreview` 自身是绘制到父布局所给范围的 `Canvas`，不指定固定尺寸或边框；调用者可用 `Frame{.width, .height}` 设置大小、用 `Grow()` 跟随容器，用 `Background`、`Border`、`CornerRadius`、`ClipChildren` 等修饰外层。现有示例的主取景框使用 `Grow()` 和圆角边框，另一个对比取景框显式设为 144×96。实体摄像头照片分辨率不随取景框大小变化；新增桌面模式的照片则按主取景框对应的屏幕物理像素区域生成。

## 先验证的关键假设

做一个小型 Windows 原型：对本进程顶层窗口设置 `WDA_EXCLUDEFROMCAPTURE`，用 Windows Graphics Capture 捕获显示器，取本窗口覆盖位置的像素。验证捕获结果确实是下方窗口/桌面，而不是本应用、黑块或旧帧；同时验证移动、遮挡、不同 DPI 和第二显示器。这里必须实测，不能只根据 API 名称断定效果。如果排除窗口在目标配置上无法稳定露出下方画面，先停在原型阶段，重新评估专用叠加窗口或其他合成方案，不发布带递归画面的实现。

## API 与架构

1. 给会话配置增加输入源选择（实体摄像头 / 桌面取景）。`Facing` 仅在实体摄像头模式有效；切换输入源按现有 `Stopping → Starting` 生命周期重开后端。桌面模式不检查 `Permission::Camera`。
2. 明确一个**主取景框**作为屏幕区域锚点。由取景框组件的布局几何和所在窗口位置计算屏幕矩形；会话中的其他预览可以显示同一纹理，但不重新定义输入区域。避免现有“一个会话有多个不同大小预览”的语义冲突。
3. 把窗口逻辑坐标转换成虚拟桌面物理像素坐标，处理每显示器 DPI、负坐标、窗口移动、取景框尺寸变化、屏幕边界和跨屏。几何变化传给捕获后端时只更新裁切区，必要时重建对应显示器捕获。
4. Windows 后端用 Windows Graphics Capture 获取显示器帧，只保留最新原生帧，在 GPU 上直接裁切到三个可复用的共享纹理中，通过 `windows::D3D11Texture::PublishShared` / `PreviewOutput` 发布。Direct2D 直接读取共享资源，缓存导入结果；预览不做整屏中间复制、不等待 GPU 完成查询，也不逐帧读回 CPU 或写入 HuxerUI `State`。缓冲忙时丢帧，移动窗口时每 16ms 补查区域。首期取景框须完整位于一块显示器内。
5. 对本应用顶层窗口设置捕获排除，并在会话结束时恢复原状态。需要在 HuxerUI 的 Windows 窗口层提供稳定、受生命周期约束的原生窗口访问/排除能力；目前相机平台工厂只得到 `UiWindow&`，公开接口没有 `HWND`。不要用窗口标题或 `FindWindow` 猜句柄。
6. 拍照从捕获后端最近的**完整有效帧**复制同一裁切区域，冻结后再进行 JPEG 编码，保证预览与照片坐标一致，避免异步移动窗口导致错位。沿用 `CapturePhotoAsync` 的单请求、停止中断和错误返回规则；桌面模式默认不镜像，画质选项继续有效。
7. 示例增加“摄像头 / 屏幕取景”切换、屏幕取景提示、拍照和保存路径。屏幕模式不申请摄像头权限；系统捕获权限/提示按平台要求处理。
8. 将屏幕捕获后端设为独立的平台接入点：Windows 用 Windows Graphics Capture，macOS 用 ScreenCaptureKit，并排除本应用进程；iOS、Android、Linux、Web 使用默认 `Unavailable` 行为，以后添加实现时不必改动公共会话、取景及拍照接口。macOS 实现须在有设备后验证。

## 验收

- 200×200 窗口内的 100×100 取景框，只显示并拍到该框位置后面的桌面内容；应用自身与控件不入镜，也没有画中画递归。
- 拖动窗口、调整窗口或取景框大小后，预览与照片持续对齐；不同 DPI、负桌面坐标和双屏场景得到正确区域或明确错误。
- 未收到第一帧、窗口最小化、显示器断开、捕获被拒、受保护内容不可见时，状态和错误可解释，不输出旧照片。
- 反复切换两种输入源、停止、卸载页面后释放捕获与排除设置；现有实体摄像头测试继续通过，并加入几何换算及屏幕截图集成测试。

## 实施顺序

1. Windows 原型验证排除窗口后能否得到真实下层像素，并记录系统版本和局限。
2. 补齐 HuxerUI 窗口句柄/几何契约，设计主取景框绑定与输入源 API。
3. 实现 Windows 屏幕帧、裁切、纹理发布和静态 JPEG。
4. 接入示例页面，完成几何、生命周期和人工验收，更新 README/API 文档。

## 实施与验证状态

- Windows 库、现有 JPEG 单元测试和示例已在 Windows 编译；现有 JPEG 单元测试通过。
- Windows 实时预览路径已改为原生帧、共享 D3D11 缓冲池与 Direct2D 资源复用，更新后的示例已编译通过；本轮未运行测试，实际帧率、延迟与长时间表现尚未量化。下述自动化捕获结果来自优化前的实现。
- `camera_windows_tests --screen` 已跑通捕获、JPEG 尺寸和窗口移动流程。本次自动化桌面环境中，桌面 DC 未显示测试窗口，测试明确跳过下层像素颜色断言；须在正常交互式 Windows 桌面重测颜色与排除效果。
- macOS 后端已实现并接线，**未在 macOS SDK 下编译，未在 macOS 设备上验证**。多显示器、屏幕录制授权、移动窗口与排除本应用的效果需在 macOS 上检查。
- Linux、iOS、Android、Web 尚未实现屏幕源；默认后端返回 `Unavailable`。

参考：[Windows Graphics Capture](https://learn.microsoft.com/en-us/windows/uwp/audio-video-camera/screen-capture)、[CreateForMonitor](https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createformonitor)、[WDA_EXCLUDEFROMCAPTURE](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowdisplayaffinity)、[ScreenCaptureKit](https://developer.apple.com/documentation/screencapturekit)。
