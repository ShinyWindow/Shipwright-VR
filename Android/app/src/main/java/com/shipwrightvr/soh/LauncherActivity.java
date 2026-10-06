package com.shipwrightvr.soh;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;

/**
 * Entry point. Decides before any VR session exists: with game data (or an un-rejected ROM) it
 * starts the immersive game, otherwise the 2D setup panel, where the system file picker can be
 * seen (nothing is visible over a running VR session: no Android dialogs, no ImGui yet).
 */
public class LauncherActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Class<?> next = GameData.readyToLaunch(this) ? GameActivity.class : SetupActivity.class;
        startActivity(new Intent(this, next));
        finish();
    }
}
