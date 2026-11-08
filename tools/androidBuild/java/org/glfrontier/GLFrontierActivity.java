package org.glfrontier;

import android.app.ActivityManager;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Bundle;
import android.os.PowerManager;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.security.SecureRandom;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.TimeZone;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

import org.libsdl.app.SDLActivity;

/*
 * SDL's own activity; SDL is linked statically into libGLFrontier.so, which
 * also holds SDL_main (the game's main()), so that is the only library.
 */
public class GLFrontierActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "GLFrontier" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    /* Always landscape (either way up): SDL would otherwise pick the
       orientation from the window size when the window is made */
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    /* ---- Send diagnostics (src/diag_upload.c) -------------------------- */

    /* each file sent is cut to its last this many bytes */
    private static final int FILE_TAIL = 4 << 20;

    /* false when the build had no upload address or token
       (build_android.py writes UploadConfig) */
    /* Called from the game's drawing thread every few seconds (src/perf_log.c):
       thermal status (0 none .. 6 shutdown), headroom (1.0 = throttling starts;
       NaN when asked too often or unknown), battery temperature. */
    public String perfState() {
        StringBuilder s = new StringBuilder();
        if (Build.VERSION.SDK_INT >= 29) {
            try {
                PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
                s.append("thermal ").append(pm.getCurrentThermalStatus());
                if (Build.VERSION.SDK_INT >= 30)
                    s.append(" headroom ").append(String.format(java.util.Locale.ROOT, "%.2f", pm.getThermalHeadroom(10)));
                if (pm.isPowerSaveMode())
                    s.append(" power-save");
            } catch (Exception e) {
            }
        }
        try {
            Intent b = registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            if (b != null)
                s.append(s.length() > 0 ? ", " : "").append("battery ").append(b.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) / 10.0)
                 .append(" C ").append(b.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)).append('%');
        } catch (Exception e) {
        }
        return s.toString();
    }

    public boolean diagnosticsAvailable() {
        return !UploadConfig.ENDPOINT.isEmpty() && !UploadConfig.TOKEN.isEmpty();
    }

    /* Called on the game's own upload thread. logDir holds glfrontier.log
       (and .old, frameprof.csv); info is a few lines from the game.
       Returns "OK <id>" or "ERR <why>". */
    public String uploadDiagnostics(String logDir, String info) {
        if (!diagnosticsAvailable())
            return "ERR this build has no upload address";
        try {
            String id = randomId();
            SimpleDateFormat f = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US);
            f.setTimeZone(TimeZone.getTimeZone("UTC"));
            String stamp = f.format(new Date());

            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            ZipOutputStream zip = new ZipOutputStream(bytes);
            addText(zip, "device.txt", deviceText(id, info));
            File dir = new File(logDir);
            for (String name : new String[] { "glfrontier.log", "glfrontier.log.old", "frameprof.csv" })
                addFile(zip, name, new File(dir, name));
            addFile(zip, "glfrontier.cfg", new File(getFilesDir(), "glfrontier.cfg"));
            zip.close();

            String flavour = getPackageName().endsWith(".quest") ? "quest" : "android";
            String name = "glfrontier-" + flavour + "-" + stamp + "-" + id + ".zip";
            URL url = new URL(UploadConfig.ENDPOINT + "?filename=" + URLEncoder.encode(name, "UTF-8"));
            HttpURLConnection c = (HttpURLConnection) url.openConnection();
            c.setConnectTimeout(15000);
            c.setReadTimeout(60000);
            c.setDoOutput(true);
            c.setRequestMethod("POST");
            c.setRequestProperty("Content-Type", "application/zip");
            c.setRequestProperty("X-Upload-Token", UploadConfig.TOKEN);
            byte[] body = bytes.toByteArray();
            c.setFixedLengthStreamingMode(body.length);
            OutputStream out = c.getOutputStream();
            out.write(body);
            out.close();
            int code = c.getResponseCode();
            c.disconnect();
            if (code < 200 || code >= 300)
                return "ERR the server answered " + code;
            return "OK " + id;
        } catch (Exception e) {
            return "ERR " + e.getClass().getSimpleName() + ": " + e.getMessage();
        }
    }

    /* 6 letters/digits, easy to read out (no 0/O, 1/I) */
    private static String randomId() {
        final String a = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        SecureRandom r = new SecureRandom();
        StringBuilder s = new StringBuilder();
        for (int i = 0; i < 6; i++)
            s.append(a.charAt(r.nextInt(a.length())));
        return s.toString();
    }

    private String deviceText(String id, String info) {
        StringBuilder s = new StringBuilder();
        s.append("id ").append(id).append('\n');
        s.append("release ").append(UploadConfig.BUILD).append('\n');
        s.append("package ").append(getPackageName()).append('\n');
        s.append("device ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
         .append(" (").append(Build.DEVICE).append(", ").append(Build.PRODUCT).append(")\n");
        s.append("android ").append(Build.VERSION.RELEASE).append(" sdk ").append(Build.VERSION.SDK_INT)
         .append(", build ").append(Build.DISPLAY).append('\n');
        if (Build.VERSION.SDK_INT >= 31)
            s.append("soc ").append(Build.SOC_MANUFACTURER).append(' ').append(Build.SOC_MODEL).append('\n');
        s.append("cpu cores ").append(Runtime.getRuntime().availableProcessors()).append('\n');
        try {
            ActivityManager am = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
            ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
            am.getMemoryInfo(mi);
            s.append("memory ").append(mi.availMem >> 20).append(" MB free of ").append(mi.totalMem >> 20)
             .append(" MB, low ").append(mi.lowMemory).append('\n');
        } catch (Exception e) {
        }
        try {
            s.append("display refresh ").append(getWindowManager().getDefaultDisplay().getRefreshRate()).append(" Hz\n");
        } catch (Exception e) {
        }
        if (Build.VERSION.SDK_INT >= 29) {
            try {
                PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
                /* 0 none, 1 light, 2 moderate, 3 severe, 4 critical, 5 emergency, 6 shutdown */
                s.append("thermal status ").append(pm.getCurrentThermalStatus()).append('\n');
                s.append("power save ").append(pm.isPowerSaveMode()).append('\n');
            } catch (Exception e) {
            }
        }
        try {
            Intent b = registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            if (b != null) {
                s.append("battery ").append(b.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)).append('/')
                 .append(b.getIntExtra(BatteryManager.EXTRA_SCALE, -1))
                 .append(", ").append(b.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) / 10.0).append(" C")
                 .append(", plugged ").append(b.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0)).append('\n');
            }
        } catch (Exception e) {
        }
        s.append(info);
        return s.toString();
    }

    private static void addText(ZipOutputStream zip, String name, String text) throws Exception {
        zip.putNextEntry(new ZipEntry(name));
        zip.write(text.getBytes("UTF-8"));
        zip.closeEntry();
    }

    /* the file's last FILE_TAIL bytes; missing files are left out */
    private static void addFile(ZipOutputStream zip, String name, File file) throws Exception {
        if (!file.isFile())
            return;
        RandomAccessFile in = new RandomAccessFile(file, "r");
        try {
            long len = in.length();
            long start = Math.max(0, len - FILE_TAIL);
            in.seek(start);
            zip.putNextEntry(new ZipEntry(name));
            byte[] buf = new byte[65536];
            long left = len - start;
            while (left > 0) {
                int n = in.read(buf, 0, (int) Math.min(buf.length, left));
                if (n <= 0)
                    break;
                zip.write(buf, 0, n);
                left -= n;
            }
            zip.closeEntry();
        } finally {
            in.close();
        }
    }
}
