package com.hc09.recomp;

import android.app.Activity;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.Locale;

/**
 * The pad's ≡ menu: a panel over the right part of the screen with the game still running beside it. A tap outside
 * the panel, ✕ or Back closes it.
 *
 * Display: frame-rate counter. Controls: each group of on-screen controls on or off, opacity, size. Game: quit.
 * (Save states, picture settings and HC27 come later.)
 */
final class GameMenu {
    private static final int ACCENT = 0xFF7FC97F, PANEL = 0xF2161A16, ROW = 0xFF283028, DIM = 0xFF8A968A;

    private final Activity activity;
    private final XboxPad pad;
    private final SharedPreferences prefs;
    private final Runnable onFpsChanged, onQuit;
    private final float density;
    private FrameLayout overlay;
    private ScrollView scroll;
    private LinearLayout content;

    GameMenu(Activity activity, XboxPad pad, SharedPreferences prefs, Runnable onFpsChanged, Runnable onQuit) {
        this.activity = activity;
        this.pad = pad;
        this.prefs = prefs;
        this.onFpsChanged = onFpsChanged;
        this.onQuit = onQuit;
        density = activity.getResources().getDisplayMetrics().density;
    }

    boolean isOpen() { return overlay != null; }

    void open() {
        if (overlay != null) return;
        final ViewGroup host = (ViewGroup) pad.getParent();
        overlay = new FrameLayout(activity);
        overlay.setBackgroundColor(0x66000000);
        overlay.setOnClickListener(v -> close());
        scroll = new ScrollView(activity);
        scroll.setBackgroundColor(PANEL);
        scroll.setOnClickListener(v -> { });  // taps inside the panel do not close it
        content = new LinearLayout(activity);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(18), dp(12), dp(18), dp(24));
        scroll.addView(content);
        fill();
        final int width = activity.getResources().getDisplayMetrics().widthPixels;
        overlay.addView(scroll, new FrameLayout.LayoutParams(Math.min(width, Math.max(dp(330), width * 2 / 5)),
                ViewGroup.LayoutParams.MATCH_PARENT, Gravity.END));
        host.addView(overlay, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    void close() {
        if (overlay == null) return;
        ((ViewGroup) overlay.getParent()).removeView(overlay);
        overlay = null;
    }

    /** Rebuilds the panel after a change, keeping its scroll position. */
    private void refresh() {
        final int y = scroll.getScrollY();
        content.removeAllViews();
        fill();
        scroll.post(() -> scroll.scrollTo(0, y));
    }

    private void fill() {
        final LinearLayout head = new LinearLayout(activity);
        head.setGravity(Gravity.CENTER_VERTICAL);
        final TextView title = text("NFL Head Coach 09", 21, Color.WHITE);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        head.addView(title, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        final TextView close = text("✕", 24, Color.WHITE);
        close.setPadding(dp(12), dp(4), dp(4), dp(4));
        close.setOnClickListener(v -> close());
        head.addView(close);
        content.addView(head);

        section("Display");
        toggle("Frame rate", "The game's frames per second, in the top corner.", prefs.getBoolean("fps", false), on -> {
            prefs.edit().putBoolean("fps", on).apply();
            onFpsChanged.run();
        });

        section("Controls");
        for (String[] group : XboxPad.GROUPS) {
            final String id = group[0];
            toggle(group[1], null, pad.groupShown(id), on -> pad.showGroup(id, on));
        }
        content.addView(label("Opacity", null));
        final String[] opacities = new String[XboxPad.OPACITY.length];
        for (int i = 0; i < opacities.length; i++) opacities[i] = Math.round(XboxPad.OPACITY[i] * 100) + "%";
        choices(opacities, pad.opacityIndex(), i -> {
            pad.setOpacityIndex(i);
            refresh();
        });
        content.addView(label("Size", null));
        choices(XboxPad.SIZE_NAMES, pad.sizeIndex(), i -> {
            pad.setSizeIndex(i);
            refresh();
        });
        final TextView hint = text("A Bluetooth or USB controller works too: the on-screen pad steps aside while you use "
                + "it, and comes back when you touch the screen.", 12, DIM);
        hint.setPadding(0, dp(6), 0, 0);
        content.addView(hint);

        section("Game");
        row("Quit the game", () -> {
            close();
            onQuit.run();
        });

        String version = "?";
        try {
            version = activity.getPackageManager().getPackageInfo(activity.getPackageName(), 0).versionName;
        } catch (Exception ignored) {
        }
        final TextView about = text(String.format(Locale.US, "Version %s. The game's own code runs natively, recompiled "
                + "for this phone.", version), 12, DIM);
        about.setPadding(0, dp(20), 0, 0);
        content.addView(about);
    }

    // ---- widgets ------------------------------------------------------------------------------------------------
    interface Toggle { void set(boolean on); }

    interface Choice { void pick(int index); }

    private int dp(float value) { return Math.round(value * density); }

    private TextView text(String s, float sp, int color) {
        final TextView t = new TextView(activity);
        t.setText(s);
        t.setTextColor(color);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        return t;
    }

    private GradientDrawable rounded(int color) {
        final GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(dp(10));
        return d;
    }

    private void section(String name) {
        final TextView t = text(name.toUpperCase(Locale.US), 13, ACCENT);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setPadding(0, dp(18), 0, dp(4));
        content.addView(t);
    }

    private LinearLayout label(String name, String hint) {
        final LinearLayout box = new LinearLayout(activity);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(0, dp(10), 0, 0);
        box.addView(text(name, 17, Color.WHITE));
        if (hint != null) box.addView(text(hint, 12, DIM));
        return box;
    }

    private void toggle(String name, String hint, boolean on, Toggle onToggle) {
        final LinearLayout line = new LinearLayout(activity);
        line.setGravity(Gravity.CENTER_VERTICAL);
        line.addView(label(name, hint), new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        // An On/Off pill (the app's plain theme draws Switch as tiny text).
        final TextView pill = text(on ? "On" : "Off", 15, on ? Color.BLACK : Color.WHITE);
        pill.setGravity(Gravity.CENTER);
        pill.setTypeface(Typeface.DEFAULT_BOLD);
        pill.setBackground(rounded(on ? ACCENT : ROW));
        line.addView(pill, new LinearLayout.LayoutParams(dp(64), dp(36)));
        line.setOnClickListener(v -> {
            onToggle.set(!on);
            refresh();
        });
        content.addView(line);
    }

    private void row(String name, Runnable action) {
        final TextView t = text(name, 17, Color.WHITE);
        t.setPadding(dp(12), 0, dp(12), 0);
        t.setGravity(Gravity.CENTER_VERTICAL);
        t.setBackground(rounded(ROW));
        t.setOnClickListener(v -> action.run());
        final LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(48));
        lp.topMargin = dp(6);
        content.addView(t, lp);
    }

    private void choices(String[] names, int selected, Choice onChoice) {
        final LinearLayout line = new LinearLayout(activity);
        for (int i = 0; i < names.length; i++) {
            final int index = i;
            final boolean chosen = i == selected;
            final TextView b = text(names[i], 15, chosen ? Color.BLACK : Color.WHITE);
            b.setGravity(Gravity.CENTER);
            b.setTypeface(Typeface.DEFAULT_BOLD);
            b.setBackground(rounded(chosen ? ACCENT : ROW));
            b.setOnClickListener(v -> onChoice.pick(index));
            final LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, dp(42), 1);
            lp.setMargins(i == 0 ? 0 : dp(6), dp(6), 0, dp(2));
            line.addView(b, lp);
        }
        content.addView(line);
    }
}
