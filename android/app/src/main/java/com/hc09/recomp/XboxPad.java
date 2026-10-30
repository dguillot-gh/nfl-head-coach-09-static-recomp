package com.hc09.recomp;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.util.SparseArray;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;

import java.util.ArrayList;
import java.util.List;

/**
 * The on-screen Xbox 360 controller, drawn over the game: left stick and D-pad on the left, A B X Y and the right
 * stick on the right, LB/LT and RB/RT in the top corners, Back and Start at the bottom, small stick-click buttons, and
 * the ≡ button that opens the menu. Every finger is tracked on its own; a finger can slide from one face button to
 * another; the sticks are analog (their centre stays put, the knob follows the finger).
 *
 * Places and sizes are in units of the screen's height, measured from the left or right edge, so the pad keeps its
 * physical size on any phone. Groups of controls can be switched off in the menu; opacity and size are settings.
 * When a real controller is used the pad steps aside (only ≡ stays) until the screen is touched again.
 */
public final class XboxPad extends View {
    static final String PREFS = "pad";

    /** State of the controller the pad describes; GameActivity merges it with any real gamepad. */
    static final class State {
        int buttons, leftTrigger, rightTrigger, leftX, leftY, rightX, rightY;
    }

    interface Listener {
        void onPadChanged(State state);
        void onMenu();
    }

    private static final int BUTTON = 0, DPAD = 1, STICK = 2, SHOULDER = 3, TRIGGER = 4, MENU = 5;
    private static final int LEFT = -1, RIGHT = 1;
    private static final float HIT = 1.3f;  // thumbs land off-centre: hit areas are larger than the drawings

    /* The menu's groups: preference key, label, shown by default. */
    static final String[][] GROUPS = {
            {"sticks", "Analog sticks", "1"},
            {"dpad", "D-pad", "1"},
            {"face", "A B X Y", "1"},
            {"shoulders", "Bumpers and triggers (LB RB LT RT)", "1"},
            {"startback", "Start and Back", "1"},
            {"clicks", "Stick clicks (LS RS)", "0"},
    };
    static final float[] OPACITY = {0.8f, 0.6f, 0.4f, 0.25f};
    static final float[] SIZE = {0.8f, 1.0f, 1.2f};
    static final String[] SIZE_NAMES = {"Small", "Normal", "Large"};

    private static final class Control {
        final int kind, bits, side;
        final String label, group;
        final float x, y, r;  // centre from the side's edge and from the top, radius: in screen heights

        Control(int kind, int bits, String label, String group, int side, float x, float y, float r) {
            this.kind = kind;
            this.bits = bits;
            this.label = label;
            this.group = group;
            this.side = side;
            this.x = x;
            this.y = y;
            this.r = r;
        }
    }

    private final SharedPreferences prefs;
    private final List<Control> controls = new ArrayList<>();
    private final Control leftStick, rightStick;
    private final SparseArray<Control> held = new SparseArray<>();      // pointer id -> control
    private final SparseArray<float[]> positions = new SparseArray<>();  // pointer id -> last x, y
    private final State state = new State();
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), stroke = new Paint(Paint.ANTI_ALIAS_FLAG),
            text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path cross = new Path();
    private Listener listener;
    private boolean steppedAside;

    public XboxPad(Context context) {
        super(context);
        prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        // Left side.
        controls.add(new Control(TRIGGER, 0, "LT", "shoulders", LEFT, 0.13f, 0.08f, 0.05f));
        controls.add(new Control(SHOULDER, NativeBridge.LB, "LB", "shoulders", LEFT, 0.13f, 0.21f, 0.05f));
        leftStick = new Control(STICK, 0, "", "sticks", LEFT, 0.19f, 0.47f, 0.12f);
        controls.add(leftStick);
        controls.add(new Control(DPAD, 0, "", "dpad", LEFT, 0.33f, 0.79f, 0.12f));
        controls.add(new Control(BUTTON, NativeBridge.LEFT_THUMB, "LS", "clicks", LEFT, 0.05f, 0.66f, 0.035f));
        controls.add(new Control(BUTTON, NativeBridge.BACK, "BACK", "startback", LEFT, 0.56f, 0.93f, 0.04f));
        controls.add(new Control(MENU, 0, "≡", null, LEFT, 0.32f, 0.08f, 0.045f));
        // Right side: A B X Y in the Xbox pad's places (Y top, B right, A bottom, X left).
        controls.add(new Control(TRIGGER, 0, "RT", "shoulders", RIGHT, 0.13f, 0.08f, 0.05f));
        controls.add(new Control(SHOULDER, NativeBridge.RB, "RB", "shoulders", RIGHT, 0.13f, 0.21f, 0.05f));
        controls.add(new Control(BUTTON, NativeBridge.Y, "Y", "face", RIGHT, 0.19f, 0.34f, 0.055f));
        controls.add(new Control(BUTTON, NativeBridge.B, "B", "face", RIGHT, 0.07f, 0.46f, 0.055f));
        controls.add(new Control(BUTTON, NativeBridge.A, "A", "face", RIGHT, 0.19f, 0.58f, 0.055f));
        controls.add(new Control(BUTTON, NativeBridge.X, "X", "face", RIGHT, 0.31f, 0.46f, 0.055f));
        rightStick = new Control(STICK, 0, "", "sticks", RIGHT, 0.33f, 0.79f, 0.11f);
        controls.add(rightStick);
        controls.add(new Control(BUTTON, NativeBridge.RIGHT_THUMB, "RS", "clicks", RIGHT, 0.05f, 0.75f, 0.035f));
        controls.add(new Control(BUTTON, NativeBridge.START, "START", "startback", RIGHT, 0.56f, 0.93f, 0.04f));
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
        stroke.setStyle(Paint.Style.STROKE);
        setHapticFeedbackEnabled(true);
    }

    void setListener(Listener listener) { this.listener = listener; }

    // ---- settings (the menu changes them) ------------------------------------------------------------------------
    boolean groupShown(String group) {
        if (group == null) return true;  // ≡ is always there
        for (String[] g : GROUPS)
            if (g[0].equals(group)) return prefs.getBoolean("show_" + group, g[2].equals("1"));
        return true;
    }

    void showGroup(String group, boolean shown) {
        prefs.edit().putBoolean("show_" + group, shown).apply();
        releaseAll();
    }

    int opacityIndex() { return clamp(prefs.getInt("opacity", 1), OPACITY.length); }

    void setOpacityIndex(int index) {
        prefs.edit().putInt("opacity", index).apply();
        invalidate();
    }

    int sizeIndex() { return clamp(prefs.getInt("size", 1), SIZE.length); }

    void setSizeIndex(int index) {
        prefs.edit().putInt("size", index).apply();
        invalidate();
    }

    private static int clamp(int value, int count) { return Math.max(0, Math.min(count - 1, value)); }

    // ---- geometry -----------------------------------------------------------------------------------------------
    private float unit() { return Math.min(getWidth(), getHeight()); }

    private float scale(Control c) { return c.kind == MENU ? 1f : SIZE[sizeIndex()]; }

    private float radius(Control c) { return c.r * unit() * scale(c); }

    private float cx(Control c) {
        final float offset = c.x * unit() * scale(c);
        return c.side == LEFT ? offset : getWidth() - offset;
    }

    private float cy(Control c) {
        // Scaled around the bottom for the lower controls and the top for the upper ones, so they stay on screen.
        final float s = scale(c), h = getHeight();
        return c.y < 0.5f ? c.y * h * s : h - (1f - c.y) * h * s;
    }

    private boolean visible(Control c) { return groupShown(c.group) && (!steppedAside || c.kind == MENU); }

    private Control controlAt(float x, float y) {
        Control best = null;
        float bestDistance = Float.MAX_VALUE;
        for (Control c : controls) {
            if (!visible(c)) continue;
            final float d = (float) Math.hypot(x - cx(c), y - cy(c));
            final float wide = c.kind == SHOULDER || c.kind == TRIGGER ? 1.7f : 1f;
            if (d <= radius(c) * wide * HIT && d < bestDistance) {
                best = c;
                bestDistance = d;
            }
        }
        return best;
    }

    // ---- drawing ------------------------------------------------------------------------------------------------
    @Override
    protected void onDraw(Canvas canvas) {
        final int alpha = Math.round(255 * OPACITY[opacityIndex()]);
        for (Control c : controls) {
            if (!visible(c)) continue;
            switch (c.kind) {
                case DPAD: drawDpad(canvas, c, alpha); break;
                case STICK: drawStick(canvas, c, alpha); break;
                case SHOULDER:
                case TRIGGER: drawWide(canvas, c, isHeld(c), alpha); break;
                default: drawRound(canvas, c, isHeld(c), alpha); break;
            }
        }
    }

    private boolean isHeld(Control c) {
        for (int i = 0; i < held.size(); i++)
            if (held.valueAt(i) == c) return true;
        return false;
    }

    private void paints(boolean pressed, int alpha) {
        fill.setColor(pressed ? 0xFFFFFFFF : 0xFF1C1C1C);
        fill.setAlpha(pressed ? alpha * 3 / 4 : alpha * 2 / 3);
        stroke.setColor(0xFFFFFFFF);
        stroke.setAlpha(alpha);
        stroke.setStrokeWidth(Math.max(2f, unit() * 0.004f));
        text.setColor(pressed ? 0xFF000000 : 0xFFFFFFFF);
        text.setAlpha(alpha);
    }

    private void label(Canvas canvas, String s, float x, float y, float size) {
        text.setTextSize(size);
        canvas.drawText(s, x, y - (text.descent() + text.ascent()) / 2, text);
    }

    private void drawRound(Canvas canvas, Control c, boolean pressed, int alpha) {
        final float x = cx(c), y = cy(c), r = radius(c);
        paints(pressed, alpha);
        canvas.drawCircle(x, y, r, fill);
        canvas.drawCircle(x, y, r, stroke);
        // The face buttons' letters in the Xbox colours.
        if (!pressed && c.kind == BUTTON) {
            if (c.bits == NativeBridge.A) text.setColor(0xFF6BD36B);
            else if (c.bits == NativeBridge.B) text.setColor(0xFFF06060);
            else if (c.bits == NativeBridge.X) text.setColor(0xFF6AA8FF);
            else if (c.bits == NativeBridge.Y) text.setColor(0xFFF2D25C);
            text.setAlpha(alpha);
        }
        label(canvas, c.label, x, y, r * (c.label.length() > 2 ? 0.5f : c.label.length() == 2 ? 0.75f : 1.05f));
    }

    private void drawWide(Canvas canvas, Control c, boolean pressed, int alpha) {
        final float x = cx(c), y = cy(c), r = radius(c);
        paints(pressed, alpha);
        final RectF box = new RectF(x - r * 1.7f, y - r, x + r * 1.7f, y + r);
        canvas.drawRoundRect(box, r * (c.kind == TRIGGER ? 0.4f : 1f), r, fill);
        canvas.drawRoundRect(box, r * (c.kind == TRIGGER ? 0.4f : 1f), r, stroke);
        label(canvas, c.label, x, y, r * 0.9f);
    }

    private void drawStick(Canvas canvas, Control c, int alpha) {
        final float x = cx(c), y = cy(c), r = radius(c);
        paints(false, alpha);
        canvas.drawCircle(x, y, r, fill);
        canvas.drawCircle(x, y, r, stroke);
        // The knob, where the stick points.
        final boolean left = c == leftStick;
        final float kx = (left ? state.leftX : state.rightX) / 32767f, ky = -(left ? state.leftY : state.rightY) / 32767f;
        paints(isHeld(c), alpha);
        canvas.drawCircle(x + kx * r * 0.55f, y + ky * r * 0.55f, r * 0.45f, fill);
        canvas.drawCircle(x + kx * r * 0.55f, y + ky * r * 0.55f, r * 0.45f, stroke);
    }

    private void drawDpad(Canvas canvas, Control c, int alpha) {
        final float x = cx(c), y = cy(c), r = radius(c), w = r * 0.34f;
        paints(false, alpha);
        cross.reset();
        cross.addRect(x - w, y - r, x + w, y + r, Path.Direction.CW);
        cross.addRect(x - r, y - w, x + r, y + w, Path.Direction.CW);
        cross.setFillType(Path.FillType.WINDING);
        canvas.drawPath(cross, fill);
        canvas.drawRect(x - w, y - r, x + w, y + r, stroke);
        canvas.drawRect(x - r, y - w, x + r, y + w, stroke);
        fill.setColor(0xFFFFFFFF);
        fill.setAlpha(alpha * 3 / 4);
        final int d = state.buttons;
        if ((d & NativeBridge.UP) != 0) canvas.drawRect(x - w, y - r, x + w, y - w, fill);
        if ((d & NativeBridge.DOWN) != 0) canvas.drawRect(x - w, y + w, x + w, y + r, fill);
        if ((d & NativeBridge.LEFT) != 0) canvas.drawRect(x - r, y - w, x - w, y + w, fill);
        if ((d & NativeBridge.RIGHT) != 0) canvas.drawRect(x + w, y - w, x + r, y + w, fill);
    }

    // ---- touch --------------------------------------------------------------------------------------------------
    @Override
    public boolean onTouchEvent(MotionEvent e) {
        final int action = e.getActionMasked();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                final int i = e.getActionIndex();
                final Control c = controlAt(e.getX(i), e.getY(i));
                if (c == null) break;
                if (steppedAside && c.kind != MENU) steppedAside = false;
                performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
                if (c.kind == MENU) {
                    releaseAll();
                    if (listener != null) listener.onMenu();
                    return true;
                }
                held.put(e.getPointerId(i), c);
                positions.put(e.getPointerId(i), new float[] {e.getX(i), e.getY(i)});
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++) {
                    final int id = e.getPointerId(i);
                    final Control c = held.get(id);
                    if (c == null) continue;
                    positions.put(id, new float[] {e.getX(i), e.getY(i)});
                    if (c.kind == BUTTON && c.group.equals("face")) {
                        // A finger sliding from one face button onto another presses the new one.
                        final Control under = controlAt(e.getX(i), e.getY(i));
                        if (under != null && under != c && under.kind == BUTTON && "face".equals(under.group)) {
                            held.put(id, under);
                            performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
                        }
                    }
                }
                break;
            case MotionEvent.ACTION_POINTER_UP: {
                final int id = e.getPointerId(e.getActionIndex());
                held.remove(id);
                positions.remove(id);
                break;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                held.clear();
                positions.clear();
                break;
            default:
                break;
        }
        update();
        return true;
    }

    /** Recomputes the controller state from the fingers on the pad and reports it when it changed. */
    private void update() {
        int buttons = 0, lt = 0, rt = 0;
        float lx = 0, ly = 0, rx = 0, ry = 0;
        for (int i = 0; i < held.size(); i++) {
            final Control c = held.valueAt(i);
            final float[] p = positions.get(held.keyAt(i));
            switch (c.kind) {
                case BUTTON:
                case SHOULDER:
                    buttons |= c.bits;
                    break;
                case TRIGGER:
                    if (c.side == LEFT) lt = 255;
                    else rt = 255;
                    break;
                case DPAD:
                    buttons |= dpadBits(c, p[0], p[1]);
                    break;
                case STICK: {
                    float dx = (p[0] - cx(c)) / radius(c), dy = (p[1] - cy(c)) / radius(c);
                    final float length = (float) Math.hypot(dx, dy);
                    if (length > 1f) {
                        dx /= length;
                        dy /= length;
                    }
                    if (c == leftStick) {
                        lx = dx;
                        ly = -dy;
                    } else {
                        rx = dx;
                        ry = -dy;
                    }
                    break;
                }
                default:
                    break;
            }
        }
        final State s = new State();
        s.buttons = buttons;
        s.leftTrigger = lt;
        s.rightTrigger = rt;
        s.leftX = Math.round(lx * 32767);
        s.leftY = Math.round(ly * 32767);
        s.rightX = Math.round(rx * 32767);
        s.rightY = Math.round(ry * 32767);
        if (s.buttons != state.buttons || s.leftTrigger != state.leftTrigger || s.rightTrigger != state.rightTrigger
                || s.leftX != state.leftX || s.leftY != state.leftY || s.rightX != state.rightX || s.rightY != state.rightY) {
            state.buttons = s.buttons;
            state.leftTrigger = s.leftTrigger;
            state.rightTrigger = s.rightTrigger;
            state.leftX = s.leftX;
            state.leftY = s.leftY;
            state.rightX = s.rightX;
            state.rightY = s.rightY;
            if (listener != null) listener.onPadChanged(state);
        }
        invalidate();
    }

    /* The D-pad direction (one of eight) under a finger; nothing in the dead centre. */
    private int dpadBits(Control c, float x, float y) {
        final float dx = x - cx(c), dy = y - cy(c);
        if (Math.hypot(dx, dy) < radius(c) * 0.25f) return 0;
        final double a = Math.toDegrees(Math.atan2(dy, dx));  // 0 right, 90 down
        int bits = 0;
        if (a > -67.5 && a < 67.5) bits |= NativeBridge.RIGHT;
        if (a > 112.5 || a < -112.5) bits |= NativeBridge.LEFT;
        if (a > 22.5 && a < 157.5) bits |= NativeBridge.DOWN;
        if (a < -22.5 && a > -157.5) bits |= NativeBridge.UP;
        return bits;
    }

    State state() { return state; }

    /** Lets go of every control (the app lost focus, or the menu opened). */
    void releaseAll() {
        held.clear();
        positions.clear();
        update();
    }

    /** A real controller is in use: the pad steps aside (≡ stays) until the screen is touched. */
    void stepAside() {
        if (steppedAside) return;
        steppedAside = true;
        releaseAll();
    }
}
