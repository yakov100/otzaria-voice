package org.otzaria.voice;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** The C++ service (libotzaria_voice.so, service/src/jni.cpp) and what it asks Java to do. */
final class NativeBridge {
    static {
        // The service loads ONNX Runtime's C API by name; loading it here first
        // makes sure the copy packed in this APK is the one it finds.
        System.loadLibrary("onnxruntime");
        System.loadLibrary("otzaria_voice");
    }

    private NativeBridge() {}

    /** Runs the HTTP service on 127.0.0.1:47811 until {@link #quit()}. Blocks. */
    static native int serve(String filesDir, String nativeLibDir);

    static native void quit();

    /** False while Android would hand this app silence instead of sound. */
    static native void setMicrophoneAllowed(boolean allowed);

    private static native void nativeProgress(long handle, long done);

    // ------------------------------------------------------------ called from C++

    /** Appends the rest of [url] to [part], resuming from its current size. */
    static void download(String url, String part, long expected, long progress) throws IOException {
        File file = new File(part);
        long have = file.exists() ? file.length() : 0;
        if (have > expected) {
            file.delete();
            have = 0;
        }
        if (have == expected) return;
        HttpURLConnection c;
        try {
            c = (HttpURLConnection) new URL(url).openConnection();
        } catch (IOException e) {
            throw new IOException("אין חיבור לשרת ההורדה");
        }
        c.setConnectTimeout(15000);
        c.setReadTimeout(60000);
        c.setInstanceFollowRedirects(true);
        if (have > 0) c.setRequestProperty("Range", "bytes=" + have + "-");
        int status;
        try {
            status = c.getResponseCode();
        } catch (IOException e) {
            throw new IOException("אין חיבור לאינטרנט");
        }
        if (status == 200 && have > 0) {
            have = 0;  // the server ignored the range: start over
        } else if (status != 200 && status != 206) {
            throw new IOException("שרת ההורדה השיב בשגיאה " + status);
        }
        long got = have;
        try (InputStream in = c.getInputStream();
             OutputStream out = new FileOutputStream(file, have > 0)) {
            byte[] buf = new byte[1 << 16];
            long reported = 0;
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
                got += n;
                if (got - reported >= (1 << 20)) {
                    nativeProgress(progress, got);
                    reported = got;
                }
            }
        } catch (IOException e) {
            throw new IOException("החיבור לאינטרנט נקטע; לחצו שוב והורדה תמשיך מאותה נקודה");
        } finally {
            c.disconnect();
        }
        nativeProgress(progress, got);
        if (got != expected) {
            throw new IOException("ההורדה לא הושלמה; לחצו שוב והיא תמשיך מאותה נקודה");
        }
    }

    /** Unpacks the zip archive [zip] into the existing folder [to]. */
    static void extract(String zip, String to) throws IOException {
        File root = new File(to).getCanonicalFile();
        try (ZipInputStream in = new ZipInputStream(new FileInputStream(zip))) {
            byte[] buf = new byte[1 << 16];
            ZipEntry e;
            while ((e = in.getNextEntry()) != null) {
                File out = new File(root, e.getName()).getCanonicalFile();
                if (!out.getPath().startsWith(root.getPath() + File.separator)) {
                    throw new IOException("נתיב לא חוקי בחבילה");
                }
                if (e.isDirectory()) {
                    out.mkdirs();
                    continue;
                }
                File parent = out.getParentFile();
                if (parent != null) parent.mkdirs();
                try (OutputStream o = new FileOutputStream(out)) {
                    int n;
                    while ((n = in.read(buf)) > 0) o.write(buf, 0, n);
                }
            }
        } catch (IOException e) {
            throw new IOException("פתיחת החבילה נכשלה: " + e.getMessage());
        }
    }
}
