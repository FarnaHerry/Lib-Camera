package org.huxerui.lib.camera;

import android.content.Context;
import android.graphics.Rect;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Matrix;
import android.hardware.display.DisplayManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Surface;
import android.view.WindowManager;

import androidx.annotation.NonNull;
import androidx.camera.core.Camera;
import androidx.camera.core.CameraInfo;
import androidx.camera.core.CameraSelector;
import androidx.camera.core.CameraState;
import androidx.camera.core.Preview;
import androidx.camera.core.ImageCapture;
import androidx.camera.core.ImageCaptureException;
import androidx.camera.core.ImageProxy;
import androidx.camera.core.SurfaceRequest;
import androidx.camera.lifecycle.ProcessCameraProvider;
import androidx.lifecycle.Lifecycle;
import androidx.lifecycle.LifecycleOwner;
import androidx.lifecycle.LifecycleRegistry;
import androidx.lifecycle.Observer;

import com.google.common.util.concurrent.ListenableFuture;

import org.huxerui.HuxerUIPlatformChannel;
import org.huxerui.HuxerUIPlatformModule;
import org.huxerui.PlatformPayload;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class CameraModule implements HuxerUIPlatformModule.Factory {
    @Override
    public HuxerUIPlatformModule create(Context context, PlatformPayload options,
                                       HuxerUIPlatformChannel.Events events) {
        return new Session(context, events);
    }

    private static native PlatformPayload createOutput(int width, int height, boolean swap);
    private static native Surface outputSurface(PlatformPayload texture);
    private static native void finishOutput(PlatformPayload texture);

    private static final class Session implements HuxerUIPlatformModule, LifecycleOwner,
            DisplayManager.DisplayListener {
        private final Context context;
        private final HuxerUIPlatformChannel.Events events;
        private final Handler main = new Handler(Looper.getMainLooper());
        private final Executor executor = command -> main.post(command);
        private final LifecycleRegistry lifecycle = new LifecycleRegistry(this);
        private final DisplayManager displays;
        private final Set<Output> outputs = new HashSet<>();
        private final ArrayList<HuxerUIPlatformChannel.Result> stops = new ArrayList<>();
        private ProcessCameraProvider provider;
        private Preview preview;
        private ImageCapture photoOutput;
        private final ExecutorService photoExecutor = Executors.newSingleThreadExecutor();
        private boolean photoPending;
        private Camera camera;
        private Observer<CameraState> cameraObserver;
        private Output current;
        private long generation;
        private long run;
        private boolean active;
        private boolean opened;
        private boolean disposed;
        private String facing;

        Session(Context context, HuxerUIPlatformChannel.Events events) {
            this.context = context;
            this.events = events;
            displays = (DisplayManager) context.getSystemService(Context.DISPLAY_SERVICE);
            lifecycle.handleLifecycleEvent(Lifecycle.Event.ON_CREATE);
        }

        @NonNull
        @Override
        public Lifecycle getLifecycle() { return lifecycle; }

        @Override
        public HuxerUIPlatformChannel.Cancellation invoke(String method, PlatformPayload arguments,
                                                          HuxerUIPlatformChannel.Result result) {
            if (disposed) {
                result.fail("camera/closed", "The camera session is closed.", PlatformPayload.nullValue());
            } else if (method.equals("start")) {
                if (active || photoPending || !outputs.isEmpty()) {
                    result.fail("camera/busy", "The previous camera output is still stopping.", PlatformPayload.nullValue());
                    return null;
                }
                run = arguments.requireField("run").requireInt64();
                PlatformPayload requested = arguments.requireField("facing");
                start(requested.isNull() ? null : requested.requireString());
                result.complete(PlatformPayload.nullValue());
            } else if (method.equals("capturePhoto")) {
                capturePhoto(arguments, result);
            } else if (method.equals("stop")) {
                stops.add(result);
                stop();
            } else {
                result.fail("camera/unknown-method", "Unknown camera method.", PlatformPayload.nullValue());
            }
            return null;
        }

        private void start(String requested) {
            active = true;
            opened = false;
            facing = null;
            long token = ++generation;
            try {
                ListenableFuture<ProcessCameraProvider> future = ProcessCameraProvider.getInstance(context);
                future.addListener(() -> {
                    if (!active || token != generation) return;
                    try {
                        provider = future.get();
                        bind(requested, token);
                    } catch (SecurityException error) {
                        fail("permission-denied", error.toString());
                    } catch (Exception error) {
                        fail("configuration-failed", error.toString());
                    }
                }, executor);
            } catch (Exception error) {
                fail("unavailable", error.toString());
            }
        }

        private void bind(String requested, long token) throws Exception {
            CameraInfo selected = null;
            for (CameraInfo candidate : provider.getAvailableCameraInfos()) {
                int lens = candidate.getLensFacing();
                if (requested == null) {
                    if (selected == null || lens == CameraSelector.LENS_FACING_BACK) selected = candidate;
                    if (lens == CameraSelector.LENS_FACING_BACK) break;
                } else if ((requested.equals("front") && lens == CameraSelector.LENS_FACING_FRONT)
                        || (requested.equals("back") && lens == CameraSelector.LENS_FACING_BACK)) {
                    selected = candidate;
                    break;
                }
            }
            if (selected == null) {
                fail("device-not-found", "No camera matches the requested facing.");
                return;
            }
            int lens = selected.getLensFacing();
            facing = lens == CameraSelector.LENS_FACING_FRONT ? "front"
                    : lens == CameraSelector.LENS_FACING_BACK ? "back" : null;
            preview = new Preview.Builder().setTargetRotation(displayRotation()).build();
            preview.setSurfaceProvider(executor, request -> {
                if (!active || token != generation) {
                    request.willNotProvideSurface();
                    return;
                }
                Output output = new Output(request, token);
                outputs.add(output);
                request.addRequestCancellationListener(executor, () -> {
                    if (output.texture == null) output.release();
                });
                request.setTransformationInfoListener(executor, output::configure);
            });
            photoOutput = new ImageCapture.Builder().setJpegQuality(100)
                    .setTargetRotation(displayRotation()).build();
            try {
                camera = provider.bindToLifecycle(this, selected.getCameraSelector(), preview, photoOutput);
            } catch (IllegalArgumentException unsupported) {
                provider.unbind(preview, photoOutput);
                photoOutput = null;
                camera = provider.bindToLifecycle(this, selected.getCameraSelector(), preview);
            }
            cameraObserver = state -> {
                if (!active || token != generation) return;
                CameraState.StateError error = state.getError();
                if (error != null) {
                    String code;
                    switch (error.getCode()) {
                        case CameraState.ERROR_CAMERA_IN_USE:
                        case CameraState.ERROR_MAX_CAMERAS_IN_USE: code = "device-busy"; break;
                        case CameraState.ERROR_STREAM_CONFIG: code = "configuration-failed"; break;
                        case CameraState.ERROR_CAMERA_DISABLED: code = "permission-denied"; break;
                        default: code = "capture-failed"; break;
                    }
                    fail(code, "CameraX error " + error.getCode()
                            + (error.getCause() == null ? "" : ": " + error.getCause()));
                } else if (state.getType() == CameraState.Type.OPEN && !opened) {
                    opened = true;
                    emit(null);
                }
            };
            camera.getCameraInfo().getCameraState().observeForever(cameraObserver);
            displays.registerDisplayListener(this, main);
            lifecycle.handleLifecycleEvent(Lifecycle.Event.ON_RESUME);
        }

        private void capturePhoto(PlatformPayload arguments, HuxerUIPlatformChannel.Result result) {
            if (!active || !opened || arguments.requireField("run").requireInt64() != run) {
                result.fail("not-ready", "Start the camera before taking a photo.", PlatformPayload.nullValue());
                return;
            }
            if (photoPending) {
                result.fail("operation-in-progress", "A photo is still being processed.", PlatformPayload.nullValue());
                return;
            }
            if (photoOutput == null) {
                result.fail("unavailable", "This camera cannot combine preview and still photos.", PlatformPayload.nullValue());
                return;
            }
            int quality = (int) arguments.requireField("quality").requireInt64();
            boolean mirror = arguments.requireField("mirror").requireBoolean();
            long token = generation;
            photoPending = true;
            try {
                photoOutput.setTargetRotation(displayRotation());
                photoOutput.takePicture(photoExecutor, new ImageCapture.OnImageCapturedCallback() {
                    @Override public void onCaptureSuccess(@NonNull ImageProxy image) {
                        byte[] encoded = null;
                        String failure = null;
                        Bitmap source = null;
                        Bitmap upright = null;
                        try {
                            ByteBuffer buffer = image.getPlanes()[0].getBuffer();
                            byte[] jpeg = new byte[buffer.remaining()];
                            buffer.get(jpeg);
                            source = BitmapFactory.decodeByteArray(jpeg, 0, jpeg.length);
                            if (source == null) throw new IllegalStateException("Cannot decode the captured JPEG.");
                            Matrix transform = new Matrix();
                            transform.postRotate(image.getImageInfo().getRotationDegrees());
                            if (mirror) transform.postScale(-1, 1);
                            upright = Bitmap.createBitmap(source, 0, 0, source.getWidth(), source.getHeight(), transform, true);
                            ByteArrayOutputStream stream = new ByteArrayOutputStream();
                            if (!upright.compress(Bitmap.CompressFormat.JPEG, quality, stream)) {
                                throw new IllegalStateException("Cannot encode the captured JPEG.");
                            }
                            encoded = stream.toByteArray();
                        } catch (Exception | OutOfMemoryError error) {
                            failure = error.toString();
                        } finally {
                            image.close();
                            if (upright != null && upright != source) upright.recycle();
                            if (source != null) source.recycle();
                        }
                        finishPhoto(token, result, encoded, failure);
                    }

                    @Override public void onError(@NonNull ImageCaptureException error) {
                        finishPhoto(token, result, null, error.toString());
                    }
                });
            } catch (RuntimeException error) {
                finishPhoto(token, result, null, error.toString());
            }
        }

        private void finishPhoto(long token, HuxerUIPlatformChannel.Result result, byte[] jpeg, String failure) {
            main.post(() -> {
                photoPending = false;
                if (!active || token != generation) {
                    result.fail("interrupted", "Photo capture was interrupted.", PlatformPayload.nullValue());
                } else if (failure != null) {
                    result.fail("capture-failed", failure, PlatformPayload.nullValue());
                } else {
                    result.complete(PlatformPayload.bytes(jpeg));
                }
                completeStops();
            });
        }

        private void stop() {
            active = false;
            opened = false;
            ++generation;
            current = null;
            displays.unregisterDisplayListener(this);
            if (camera != null && cameraObserver != null) {
                camera.getCameraInfo().getCameraState().removeObserver(cameraObserver);
            }
            cameraObserver = null;
            camera = null;
            lifecycle.handleLifecycleEvent(Lifecycle.Event.ON_STOP);
            if (preview != null) {
                preview.setSurfaceProvider(null);
                provider.unbind(preview);
                preview = null;
            }
            if (photoOutput != null) {
                provider.unbind(photoOutput);
                photoOutput = null;
            }
            for (Output output : new ArrayList<>(outputs)) {
                if (output.texture == null) {
                    output.request.willNotProvideSurface();
                    output.release();
                }
            }
            completeStops();
        }

        private void completeStops() {
            if (active || photoPending || !outputs.isEmpty()) return;
            for (HuxerUIPlatformChannel.Result result : stops) result.complete(PlatformPayload.nullValue());
            stops.clear();
            if (disposed) {
                lifecycle.handleLifecycleEvent(Lifecycle.Event.ON_DESTROY);
                photoExecutor.shutdown();
            }
        }

        @Override
        public void dispose() {
            if (disposed) return;
            disposed = true;
            stop();
            events.close();
        }

        private void fail(String code, String message) {
            if (!active || disposed) return;
            Log.e("HuxerUICamera", code + ": " + message);
            Map<String, PlatformPayload> error = new HashMap<>();
            error.put("code", PlatformPayload.string(code));
            error.put("message", PlatformPayload.string(message));
            emit(PlatformPayload.object(error));
            stop();
        }

        private void emit(PlatformPayload error) {
            if (!active || disposed) return;
            Map<String, PlatformPayload> snapshot = new HashMap<>();
            snapshot.put("run", PlatformPayload.int64(run));
            snapshot.put("facing", facing == null ? PlatformPayload.nullValue() : PlatformPayload.string(facing));
            snapshot.put("error", error == null ? PlatformPayload.nullValue() : error);
            snapshot.put("texture", current == null ? PlatformPayload.nullValue() : current.texture);
            snapshot.put("rotation", PlatformPayload.int64(current == null ? 0 : current.rotation));
            snapshot.put("mirrored", PlatformPayload.booleanValue(current != null && current.mirrored));
            events.emit("snapshot", PlatformPayload.object(snapshot));
        }

        private int displayRotation() {
            WindowManager windows = (WindowManager) context.getSystemService(Context.WINDOW_SERVICE);
            return windows.getDefaultDisplay().getRotation();
        }

        @Override public void onDisplayAdded(int displayId) {}
        @Override public void onDisplayRemoved(int displayId) {}
        @Override public void onDisplayChanged(int displayId) {
            if (!active || preview == null || preview.getTargetRotation() == displayRotation()) return;
            preview.setTargetRotation(displayRotation());
        }

        private final class Output {
            final SurfaceRequest request;
            final long token;
            PlatformPayload texture;
            int rotation;
            boolean mirrored;
            boolean swapped;
            boolean invalidated;
            boolean released;

            Output(SurfaceRequest request, long token) {
                this.request = request;
                this.token = token;
            }

            void configure(SurfaceRequest.TransformationInfo info) {
                if (released || invalidated || !active || token != generation) return;
                int width = request.getResolution().getWidth();
                int height = request.getResolution().getHeight();
                if (!info.getCropRect().equals(new Rect(0, 0, width, height))) {
                    request.willNotProvideSurface();
                    fail("configuration-failed", "CameraX returned a cropped Surface without a ViewPort.");
                    return;
                }
                // This integration expects the SDK's SurfaceTexture consumer to produce natural
                // orientation; the inverse display rotation remains. See the device validation notes.
                int nextRotation = info.hasCameraTransform()
                        ? (360 - preview.getTargetRotation() * 90) % 360 : info.getRotationDegrees();
                boolean nextSwap = info.hasCameraTransform()
                        && camera.getCameraInfo().getSensorRotationDegrees(Surface.ROTATION_0) % 180 != 0;
                boolean nextMirror = info.hasCameraTransform() && "front".equals(facing);
                if (texture != null) {
                    if (rotation != nextRotation || swapped != nextSwap || mirrored != nextMirror) {
                        invalidated = true;
                        request.invalidate();
                    }
                    return;
                }
                rotation = nextRotation;
                swapped = nextSwap;
                mirrored = nextMirror;
                try {
                    texture = createOutput(width, height, swapped);
                    Surface surface = outputSurface(texture);
                    request.provideSurface(surface, executor, result -> {
                        release();
                        if (active && token == generation && !invalidated
                                && result.getResultCode() != SurfaceRequest.Result.RESULT_SURFACE_USED_SUCCESSFULLY
                                && result.getResultCode() != SurfaceRequest.Result.RESULT_REQUEST_CANCELLED) {
                            fail("configuration-failed", "CameraX rejected the preview Surface: " + result.getResultCode());
                        }
                    });
                } catch (RuntimeException error) {
                    request.willNotProvideSurface();
                    release();
                    fail("configuration-failed", error.toString());
                    return;
                }
                Log.d("HuxerUICamera", "Preview Surface " + width + "x" + height
                        + ", rotation=" + rotation + ", swapped=" + swapped + ", mirrored=" + mirrored);
                current = this;
                if (opened) emit(null);
            }

            void release() {
                if (released) return;
                released = true;
                request.clearTransformationInfoListener();
                // A provided Surface reaches this path only after CameraX releases it.
                if (texture != null) {
                    finishOutput(texture);
                    texture.requireExternalTexture().close();
                    texture = null;
                }
                if (current == this) {
                    current = null;
                    if (opened && active && token == generation) emit(null);
                }
                outputs.remove(this);
                completeStops();
            }
        }
    }
}
