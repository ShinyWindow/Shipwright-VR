package com.shipwrightvr.soh;

import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

/** The immersive game: SDL's activity running libsoh.so (SDL_main), which starts OpenXR itself. */
public class GameActivity extends SDLActivity {
    private static final String TAG = "ShipwrightVR";

    @Override
    protected String[] getLibraries() {
        // The last entry is the one SDL calls SDL_main in.
        return new String[] { "SDL2", "soh" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        installBundledArchive();
        super.onCreate(savedInstanceState);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        // A relaunch must start a fresh process: the game's globals are not re-initialisable.
        android.os.Process.killProcess(android.os.Process.myPid());
    }

    // OTRGlobals.cpp kAndroidMsgShowSetup: first-run conversion failed. The native side wrote the
    // reason to setup_error.txt and parked its thread; show the setup panel (its own process,
    // see the manifest) and end this one.
    private static final int MSG_SHOW_SETUP = 0x8001;

    @Override
    protected boolean onUnhandledMessage(int command, Object param) {
        if (command == MSG_SHOW_SETUP) {
            startActivity(new Intent(this, SetupActivity.class));
            new Handler(Looper.getMainLooper()).postDelayed(
                () -> android.os.Process.killProcess(android.os.Process.myPid()), 1000);
            return true;
        }
        return super.onUnhandledMessage(command, param);
    }

    /** soh.o2r ships in the APK; copy it next to the ROM archives once per installed APK. */
    private void installBundledArchive() {
        File dir = GameData.dataDir(this);
        if (dir == null) {
            Log.e(TAG, "No external files dir");
            return;
        }
        File target = new File(dir, "soh.o2r");
        File stamp = new File(dir, "soh.o2r.stamp");
        String want = apkStamp();
        if (target.isFile() && want.equals(readText(stamp))) {
            return;
        }
        File tmp = new File(dir, "soh.o2r.tmp");
        try (InputStream in = getAssets().open("soh.o2r"); OutputStream out = new FileOutputStream(tmp)) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
        } catch (IOException e) {
            Log.e(TAG, "Copying soh.o2r failed", e);
            tmp.delete();
            return;
        }
        if (!tmp.renameTo(target)) {
            Log.e(TAG, "Renaming soh.o2r failed");
            return;
        }
        try (OutputStream out = new FileOutputStream(stamp)) {
            out.write(want.getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            Log.w(TAG, "Writing soh.o2r stamp failed", e);
        }
    }

    private String apkStamp() {
        try {
            PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            return info.versionName + "/" + info.lastUpdateTime;
        } catch (PackageManager.NameNotFoundException e) {
            return "unknown";
        }
    }

    private static String readText(File f) {
        if (!f.isFile()) {
            return "";
        }
        try (FileInputStream in = new FileInputStream(f)) {
            byte[] buf = new byte[256];
            int n = in.read(buf);
            return n > 0 ? new String(buf, 0, n, StandardCharsets.UTF_8) : "";
        } catch (IOException e) {
            return "";
        }
    }
}
