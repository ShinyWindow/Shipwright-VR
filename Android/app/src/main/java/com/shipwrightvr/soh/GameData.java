package com.shipwrightvr.soh;

import android.content.Context;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

/**
 * The app's data folder, shared with the native side: this is getExternalFilesDir(null), which is
 * also SDL_AndroidGetExternalStoragePath() = Ship::Context::GetAppDirectoryPath() on Android
 * (/storage/emulated/0/Android/data/com.shipwrightvr.soh/files). It needs no storage permission,
 * and `adb push rom.z64 <that folder>` works for testing.
 *
 * Contract with OTRGlobals::RunExtract (Android branch): the game extracts oot.o2r / oot-mq.o2r
 * from any ROM it finds here, without dialogs. If it rejects the ROM it deletes it, writes the
 * reason to setup_error.txt and exits, and the next launch shows SetupActivity with that reason.
 */
final class GameData {
    static final String SETUP_ERROR_FILE = "setup_error.txt";

    private GameData() {
    }

    static File dataDir(Context context) {
        File dir = context.getExternalFilesDir(null);
        if (dir != null && !dir.exists()) {
            dir.mkdirs();
        }
        return dir;
    }

    static boolean hasRomArchives(Context context) {
        File dir = dataDir(context);
        return dir != null && (new File(dir, "oot.o2r").isFile() || new File(dir, "oot-mq.o2r").isFile());
    }

    static boolean isRomFileName(String name) {
        String lower = name.toLowerCase();
        return lower.endsWith(".z64") || lower.endsWith(".n64") || lower.endsWith(".v64");
    }

    static boolean hasRomFile(Context context) {
        File dir = dataDir(context);
        File[] files = dir != null ? dir.listFiles() : null;
        if (files == null) {
            return false;
        }
        for (File f : files) {
            if (f.isFile() && isRomFileName(f.getName())) {
                return true;
            }
        }
        return false;
    }

    /** The game can start: archives exist, or there is a ROM it hasn't rejected yet. */
    static boolean readyToLaunch(Context context) {
        if (hasRomArchives(context)) {
            return true;
        }
        return hasRomFile(context) && readSetupError(context) == null;
    }

    static String readSetupError(Context context) {
        File dir = dataDir(context);
        File f = dir != null ? new File(dir, SETUP_ERROR_FILE) : null;
        if (f == null || !f.isFile()) {
            return null;
        }
        try (FileInputStream in = new FileInputStream(f)) {
            byte[] buf = new byte[(int) Math.min(f.length(), 4096)];
            int n = in.read(buf);
            return n > 0 ? new String(buf, 0, n, StandardCharsets.UTF_8).trim() : "";
        } catch (IOException e) {
            return "";
        }
    }

    static void clearSetupError(Context context) {
        File dir = dataDir(context);
        if (dir != null) {
            new File(dir, SETUP_ERROR_FILE).delete();
        }
    }
}
