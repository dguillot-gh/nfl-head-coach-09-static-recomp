package com.hc09.recomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.net.Uri;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.io.File;

/**
 * Game screen. Runs in its own process (":game"); the native side lives as long as the process, so
 * quitting ends the process. Hands the native side its Surface and, when the game's files are present,
 * starts the game. Over the picture: the on-screen pad (XboxPad), its menu (GameMenu) and an optional
 * frame-rate counter. Bluetooth/USB gamepads are read here and merged with the on-screen pad.
 */
public class GameActivity extends Activity implements SurfaceHolder.Callback, XboxPad.Listener {
    private static final String TAG = "HC09";

    private XboxPad pad;
    private GameMenu menu;
    private SharedPreferences prefs;
    private TextView fps;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private int lastFrame;
    private long lastFpsTime;

    // The real gamepad's state (merged with the on-screen pad's).
    private int gamepadButtons, gamepadLeftTrigger, gamepadRightTrigger, gamepadDpad;
    private float gamepadLeftX, gamepadLeftY, gamepadRightX, gamepadRightY;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        prefs = getSharedPreferences(XboxPad.PREFS, MODE_PRIVATE);
        final FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        SurfaceView view = new SurfaceView(this);
        view.getHolder().addCallback(this);
        root.addView(view);
        pad = new XboxPad(this);
        pad.setListener(this);
        root.addView(pad);
        fps = new TextView(this);
        fps.setTextColor(0xFFE0FFE0);
        fps.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        fps.setBackgroundColor(0x80000000);
        fps.setPadding(12, 4, 12, 4);
        root.addView(fps, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.CENTER_HORIZONTAL));
        setContentView(root);
        menu = new GameMenu(this, pad, prefs, this::updateFps, this::finish);
        updateFps();
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
        else
            pad.releaseAll();
    }

    // ---- frame-rate counter -------------------------------------------------------------------------------------
    private final Runnable fpsTick = new Runnable() {
        @Override
        public void run() {
            final int frame = NativeBridge.frameNumber();
            final long now = System.nanoTime();
            if (lastFpsTime != 0 && now > lastFpsTime)
                fps.setText(String.format(java.util.Locale.US, "%.0f fps", (frame - lastFrame) * 1e9 / (now - lastFpsTime)));
            lastFrame = frame;
            lastFpsTime = now;
            handler.postDelayed(this, 1000);
        }
    };

    private void updateFps() {
        final boolean shown = prefs.getBoolean("fps", false);
        fps.setVisibility(shown ? View.VISIBLE : View.GONE);
        handler.removeCallbacks(fpsTick);
        if (shown) {
            lastFpsTime = 0;
            fps.setText("- fps");
            handler.post(fpsTick);
        }
    }

    // ---- controller: on-screen pad + real gamepad -----------------------------------------------------------------
    @Override
    public void onPadChanged(XboxPad.State state) { sendPad(); }

    @Override
    public void onMenu() { menu.open(); }

    private static int axis(float value) {
        if (Math.abs(value) < 0.15f) return 0;  // dead zone
        return Math.max(-32767, Math.min(32767, Math.round(value * 32767)));
    }

    private static int clampAxis(int a, int b) { return Math.max(-32767, Math.min(32767, a + b)); }

    private void sendPad() {
        final XboxPad.State touch = pad.state();
        NativeBridge.setPad(touch.buttons | gamepadButtons | gamepadDpad,
                Math.max(touch.leftTrigger, gamepadLeftTrigger), Math.max(touch.rightTrigger, gamepadRightTrigger),
                clampAxis(touch.leftX, axis(gamepadLeftX)), clampAxis(touch.leftY, axis(-gamepadLeftY)),
                clampAxis(touch.rightX, axis(gamepadRightX)), clampAxis(touch.rightY, axis(-gamepadRightY)));
    }

    private static boolean fromGamepad(int source) {
        return (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
                || (source & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD;
    }

    private static int buttonFor(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BUTTON_A: return NativeBridge.A;
            case KeyEvent.KEYCODE_BUTTON_B: return NativeBridge.B;
            case KeyEvent.KEYCODE_BUTTON_X: return NativeBridge.X;
            case KeyEvent.KEYCODE_BUTTON_Y: return NativeBridge.Y;
            case KeyEvent.KEYCODE_BUTTON_L1: return NativeBridge.LB;
            case KeyEvent.KEYCODE_BUTTON_R1: return NativeBridge.RB;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return NativeBridge.LEFT_THUMB;
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return NativeBridge.RIGHT_THUMB;
            case KeyEvent.KEYCODE_BUTTON_START: return NativeBridge.START;
            case KeyEvent.KEYCODE_BUTTON_SELECT: return NativeBridge.BACK;
            case KeyEvent.KEYCODE_DPAD_UP: return NativeBridge.UP;
            case KeyEvent.KEYCODE_DPAD_DOWN: return NativeBridge.DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT: return NativeBridge.LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return NativeBridge.RIGHT;
            default: return 0;
        }
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (!fromGamepad(event.getSource()) || menu.isOpen())
            return super.dispatchKeyEvent(event);
        int keyCode = event.getKeyCode();
        // Some controllers report their B button as Back.
        if (keyCode == KeyEvent.KEYCODE_BACK)
            keyCode = KeyEvent.KEYCODE_BUTTON_B;
        final boolean down = event.getAction() == KeyEvent.ACTION_DOWN;
        final int button = buttonFor(keyCode);
        if (keyCode == KeyEvent.KEYCODE_BUTTON_L2) {
            gamepadLeftTrigger = down ? 255 : 0;
        } else if (keyCode == KeyEvent.KEYCODE_BUTTON_R2) {
            gamepadRightTrigger = down ? 255 : 0;
        } else if (keyCode == KeyEvent.KEYCODE_BUTTON_MODE) {
            // The Xbox/Home button opens the menu.
            if (down && event.getRepeatCount() == 0) menu.open();
            return true;
        } else if (button != 0) {
            gamepadButtons = down ? gamepadButtons | button : gamepadButtons & ~button;
        } else {
            return super.dispatchKeyEvent(event);
        }
        pad.stepAside();
        sendPad();
        return true;
    }

    @Override
    public boolean onGenericMotionEvent(MotionEvent event) {
        if ((event.getSource() & InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK
                || event.getAction() != MotionEvent.ACTION_MOVE)
            return super.onGenericMotionEvent(event);
        gamepadLeftX = event.getAxisValue(MotionEvent.AXIS_X);
        gamepadLeftY = event.getAxisValue(MotionEvent.AXIS_Y);
        gamepadRightX = event.getAxisValue(MotionEvent.AXIS_Z);
        gamepadRightY = event.getAxisValue(MotionEvent.AXIS_RZ);
        // Analog triggers (controllers with digital L2/R2 send key events instead).
        final float lt = Math.max(event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_BRAKE));
        final float rt = Math.max(event.getAxisValue(MotionEvent.AXIS_RTRIGGER), event.getAxisValue(MotionEvent.AXIS_GAS));
        gamepadLeftTrigger = Math.round(Math.max(0, Math.min(1, lt)) * 255);
        gamepadRightTrigger = Math.round(Math.max(0, Math.min(1, rt)) * 255);
        // The hat: the D-pad as axes, on most controllers.
        final float hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X), hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y);
        gamepadDpad = (hatX < -0.5f ? NativeBridge.LEFT : 0) | (hatX > 0.5f ? NativeBridge.RIGHT : 0)
                | (hatY < -0.5f ? NativeBridge.UP : 0) | (hatY > 0.5f ? NativeBridge.DOWN : 0);
        if (axis(gamepadLeftX) != 0 || axis(gamepadLeftY) != 0 || axis(gamepadRightX) != 0 || axis(gamepadRightY) != 0
                || gamepadDpad != 0 || lt > 0.2f || rt > 0.2f)
            pad.stepAside();
        sendPad();
        return true;
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
        if (menu.isOpen()) {
            menu.close();
            return;
        }
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
