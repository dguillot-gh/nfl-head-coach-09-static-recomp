package com.hc09.recomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.util.Log;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;

import java.io.File;

/**
 * Game screen. Runs in its own process (":game"); the native side lives as long as the process, so
 * quitting ends the process. Hands the native side its Surface and, when the game's files are present,
 * starts the game.
 */
public class GameActivity extends Activity implements SurfaceHolder.Callback {
    private static final String TAG = "HC09";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        SurfaceView view = new SurfaceView(this);
        view.getHolder().addCallback(this);
        setContentView(view);
        hideSystemBars();
        NativeBridge.setAssetManager(getAssets());
        startGameIfPossible();
    }

    // Development layout until the launcher prepares the files itself: the decrypted executable
    // (default.pe) and its XEX (default.xex) in the app's external files folder, pushed with adb.
    private void startGameIfPossible() {
        File dir = getExternalFilesDir(null);
        File image = new File(dir, "default.pe");
        File xex = new File(dir, "default.xex");
        String iso = isoPath(dir);
        if (!image.isFile() || !xex.isFile() || iso == null) {
            Log.i(TAG, "game files missing in " + dir + "; graphics test only");
            return;
        }
        File userData = new File(getFilesDir(), "userdata");
        NativeBridge.startGame(image.getPath(), xex.getPath(), iso, userData.getPath());
    }

    // The disc image picked in the launcher (as /proc/self/fd/<n>), else HC09.iso in the files folder.
    private String isoPath(File dir) {
        String uri = getSharedPreferences(LauncherActivity.PREFS, MODE_PRIVATE).getString(LauncherActivity.KEY_ISO_URI, null);
        if (uri != null) {
            try {
                ParcelFileDescriptor descriptor = getContentResolver().openFileDescriptor(Uri.parse(uri), "r");
                if (descriptor != null)
                    return "/proc/self/fd/" + descriptor.detachFd();  // owned by the native side from here on
            } catch (Exception e) {
                Log.w(TAG, "cannot open the selected disc image", e);
            }
        }
        File local = new File(dir, "HC09.iso");
        return local.isFile() ? local.getPath() : null;
    }

    private void hideSystemBars() {
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN
            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus)
            hideSystemBars();
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        NativeBridge.setSurface(holder.getSurface());
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        NativeBridge.setSurface(holder.getSurface());
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        NativeBridge.setSurface(null);
    }

    @Override
    public void onBackPressed() {
        new AlertDialog.Builder(this)
            .setMessage("Quit the game?")
            .setPositiveButton("Quit", (dialog, which) -> finish())
            .setNegativeButton("Cancel", null)
            .show();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        // The native game cannot restart inside this process.
        System.exit(0);
    }
}
