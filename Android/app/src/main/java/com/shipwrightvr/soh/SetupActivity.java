package com.shipwrightvr.soh;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * First-run setup on a 2D panel: the player picks their ROM with the system file picker; it is
 * copied into the app's data folder and the immersive game starts, which converts it (on its own,
 * no dialogs). Only reached from LauncherActivity when there is no game data yet, or after the
 * game rejected a ROM (its reason is shown here).
 */
public class SetupActivity extends Activity {
    private static final String TAG = "ShipwrightVR";
    private static final int PICK_ROM = 1;

    // The ROM sizes Extractor::ValidateRomSize accepts (soh/soh/Extractor/Extract.h).
    private static final long MB = 1024L * 1024L;
    private static final long[] ROM_SIZES = { 32 * MB, 54 * MB, 64 * MB };

    private TextView mStatus;
    private Button mPick;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setGravity(Gravity.CENTER_HORIZONTAL);
        int pad = dp(32);
        col.setPadding(pad, pad, pad, pad);

        TextView title = text("Shipwright VR setup", 28);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        col.addView(title);

        File dir = GameData.dataDir(this);
        col.addView(text("Choose your Ocarina of Time ROM (.z64, .n64 or .v64). It is copied into the "
            + "app's folder, and the game converts it the first time it starts. That takes a few "
            + "minutes; the headset view stays dark meanwhile.\n\n"
            + "Supported ROM versions are listed at ship.equipment.\n\n"
            + "You can also copy the ROM with adb into:\n" + (dir != null ? dir.getAbsolutePath() : "?"), 18));

        String error = GameData.readSetupError(this);
        if (error != null) {
            TextView err = text("The last ROM didn't work: " + (error.isEmpty() ? "unknown reason" : error), 18);
            err.setTextColor(0xFFFF6E6E);
            col.addView(err);
        }

        mPick = new Button(this);
        mPick.setText("Choose ROM file");
        mPick.setTextSize(TypedValue.COMPLEX_UNIT_SP, 22);
        mPick.setOnClickListener(v -> pickRom());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
            LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(24);
        col.addView(mPick, lp);

        mStatus = text("", 18);
        col.addView(mStatus);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(col);
        setContentView(scroll);
    }

    private void pickRom() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*"); // ROMs have no registered MIME type
        try {
            startActivityForResult(intent, PICK_ROM);
        } catch (Exception e) {
            Log.e(TAG, "No file picker", e);
            mStatus.setText("This device has no file picker. Copy the ROM with adb instead (see above).");
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_ROM || resultCode != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        final Uri uri = data.getData();
        mPick.setEnabled(false);
        mStatus.setText("Copying...");
        new Thread(() -> {
            String result = importRom(uri);
            runOnUiThread(() -> {
                if (result == null) {
                    startActivity(new Intent(this, GameActivity.class));
                    finish();
                } else {
                    mStatus.setText(result);
                    mPick.setEnabled(true);
                }
            });
        }).start();
    }

    /** Copies the picked file into the data folder. Null on success, else what went wrong. */
    private String importRom(Uri uri) {
        File dir = GameData.dataDir(this);
        if (dir == null) {
            return "The app's storage folder is unavailable.";
        }
        String name = displayName(uri);
        File tmp = new File(dir, "import.tmp");
        long size = 0;
        byte[] head = new byte[4];
        try (InputStream in = getContentResolver().openInputStream(uri); OutputStream out = new FileOutputStream(tmp)) {
            if (in == null) {
                return "Couldn't open that file.";
            }
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) {
                if (size < 4) {
                    System.arraycopy(buf, 0, head, (int) size, (int) Math.min(4 - size, n));
                }
                out.write(buf, 0, n);
                size += n;
                if (size > 64 * MB) {
                    break;
                }
            }
        } catch (IOException e) {
            Log.e(TAG, "ROM import failed", e);
            tmp.delete();
            return "Copying failed: " + e.getMessage();
        }

        boolean sizeOk = false;
        for (long s : ROM_SIZES) {
            sizeOk |= size == s;
        }
        String ext = romExtension(head);
        if (!sizeOk || ext == null) {
            tmp.delete();
            return "That file isn't an N64 ROM of a supported size (32, 54 or 64 MB). If it is zipped, unzip it first.";
        }
        if (name == null || !GameData.isRomFileName(name)) {
            name = "rom" + ext; // Extractor only looks at .z64 / .n64 / .v64
        }
        File target = new File(dir, name.replace('/', '_'));
        target.delete();
        if (!tmp.renameTo(target)) {
            tmp.delete();
            return "Couldn't save the ROM into the app's folder.";
        }
        GameData.clearSetupError(this);
        return null;
    }

    /** The N64 byte order from the first word: big-endian .z64, byte-swapped .v64, little-endian .n64. */
    private static String romExtension(byte[] h) {
        int w = ((h[0] & 0xFF) << 24) | ((h[1] & 0xFF) << 16) | ((h[2] & 0xFF) << 8) | (h[3] & 0xFF);
        switch (w) {
            case 0x80371240:
                return ".z64";
            case 0x37804012:
                return ".v64";
            case 0x40123780:
                return ".n64";
            default:
                return null;
        }
    }

    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] { OpenableColumns.DISPLAY_NAME }, null, null,
            null)) {
            if (c != null && c.moveToFirst()) {
                return c.getString(0);
            }
        } catch (Exception e) {
            Log.w(TAG, "No display name for " + uri, e);
        }
        return null;
    }

    private TextView text(String s, int sp) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setPadding(0, dp(8), 0, dp(8));
        return t;
    }

    private int dp(int v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }
}
