package org.huxerui.lib.camera.preview;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;

import org.huxerui.HuxerUIPlatformChannel;
import org.huxerui.HuxerUIPlatformModule;
import org.huxerui.PlatformPayload;

import java.io.ByteArrayOutputStream;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.atomic.AtomicBoolean;

public final class PhotoThumbnailModule implements HuxerUIPlatformModule.Factory {
    @Override
    public HuxerUIPlatformModule create(Context context, PlatformPayload options,
                                       HuxerUIPlatformChannel.Events events) {
        return new Decoder();
    }

    private static final class Decoder implements HuxerUIPlatformModule {
        private final ExecutorService executor = Executors.newSingleThreadExecutor();
        private volatile boolean disposed;

        @Override
        public HuxerUIPlatformChannel.Cancellation invoke(String method, PlatformPayload arguments,
                                                          HuxerUIPlatformChannel.Result result) {
            if (disposed || !method.equals("decode")) {
                result.fail("thumbnail/unavailable", "Photo preview is unavailable.", PlatformPayload.nullValue());
                return null;
            }
            byte[] jpeg = arguments.requireBytes();
            AtomicBoolean canceled = new AtomicBoolean();
            Future<?> work = executor.submit(() -> {
                Bitmap thumbnail = null;
                try {
                    if (canceled.get() || disposed) return;
                    BitmapFactory.Options options = new BitmapFactory.Options();
                    options.inJustDecodeBounds = true;
                    BitmapFactory.decodeByteArray(jpeg, 0, jpeg.length, options);
                    if (options.outWidth <= 0 || options.outHeight <= 0) {
                        throw new IllegalArgumentException("Cannot read photo dimensions.");
                    }
                    options.inSampleSize = 1;
                    // Sample during decoding so the full-resolution bitmap is never allocated for review.
                    while ((Math.max(options.outWidth, options.outHeight) + (long) options.inSampleSize - 1)
                            / options.inSampleSize > 2048) {
                        options.inSampleSize *= 2;
                    }
                    options.inJustDecodeBounds = false;
                    options.inScaled = false;
                    thumbnail = BitmapFactory.decodeByteArray(jpeg, 0, jpeg.length, options);
                    if (thumbnail == null) throw new IllegalArgumentException("Cannot decode photo preview.");
                    if (canceled.get() || disposed) return;
                    ByteArrayOutputStream output = new ByteArrayOutputStream();
                    if (!thumbnail.compress(Bitmap.CompressFormat.JPEG, 85, output)) {
                        throw new IllegalStateException("Cannot encode photo preview.");
                    }
                    if (!canceled.get() && !disposed) result.complete(PlatformPayload.bytes(output.toByteArray()));
                } catch (RuntimeException | OutOfMemoryError error) {
                    if (!canceled.get() && !disposed) {
                        result.fail("thumbnail/decode-failed", "Could not display this photo.", PlatformPayload.nullValue());
                    }
                } finally {
                    if (thumbnail != null) thumbnail.recycle();
                }
            });
            return () -> {
                canceled.set(true);
                work.cancel(false);
            };
        }

        @Override
        public void dispose() {
            disposed = true;
            executor.shutdownNow();
        }
    }
}
