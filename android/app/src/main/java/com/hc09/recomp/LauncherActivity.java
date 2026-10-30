package com.hc09.recomp;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * Start menu: shows the selected disc image, lets the user pick one with the system file picker (the
 * image is read in place, never copied), and starts the game screen.
 */
public class LauncherActivity extends Activity {
    static final String PREFS = "hc09";
    static final String KEY_ISO_URI = "isoUri";
    private static final int PICK_ISO = 1;

    private TextView status;
    private Button play;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        root.setBackgroundResource(R.drawable.launcher_field);
        int pad = dp(24);
        root.setPadding(pad, pad, pad, pad);

        ImageView ball = new ImageView(this);
        ball.setImageResource(R.drawable.football);
        root.addView(ball, new LinearLayout.LayoutParams(dp(96), dp(96)));

        TextView title = new TextView(this);
        title.setText("NFL Head Coach 09");
        title.setTextColor(Color.WHITE);
        title.setTextSize(28);
        title.setGravity(Gravity.CENTER);
        root.addView(title);

        status = new TextView(this);
        status.setTextColor(Color.LTGRAY);
        status.setTextSize(14);
        status.setGravity(Gravity.CENTER);
        status.setPadding(0, dp(12), 0, dp(24));
        root.addView(status);

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setGravity(Gravity.CENTER);
        play = new Button(this);
        play.setText("Play");
        play.setOnClickListener(v -> startActivity(new Intent(this, GameActivity.class)));
        Button select = new Button(this);
        select.setText("Select game file");
        select.setOnClickListener(v -> pickIso());
        buttons.addView(play, new LinearLayout.LayoutParams(dp(160), ViewGroup.LayoutParams.WRAP_CONTENT));
        buttons.addView(select, new LinearLayout.LayoutParams(dp(200), ViewGroup.LayoutParams.WRAP_CONTENT));
        root.addView(buttons);

        setContentView(root);
        refresh();
    }

    private void pickIso() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        startActivityForResult(intent, PICK_ISO);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_ISO || resultCode != RESULT_OK || data == null || data.getData() == null)
            return;
        Uri uri = data.getData();
        try {
            getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException e) {
            // Some providers grant no persistable permission; the pick then lasts until the app restarts.
        }
        prefs().edit().putString(KEY_ISO_URI, uri.toString()).apply();
        refresh();
    }

    private void refresh() {
        // The same choice GameActivity makes: the picked image, else HC09.iso in the app's files folder.
        String uri = prefs().getString(KEY_ISO_URI, null);
        java.io.File local = new java.io.File(getExternalFilesDir(null), "HC09.iso");
        if (uri != null)
            status.setText("Disc image: " + displayName(Uri.parse(uri)));
        else if (local.isFile())
            status.setText("Disc image: HC09.iso (app folder)");
        else
            status.setText("No disc image selected (Play opens the graphics test).");
    }

    private String displayName(Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst())
                return cursor.getString(0);
        } catch (Exception e) {
            // fall through
        }
        return uri.getLastPathSegment();
    }

    private SharedPreferences prefs() {
        return getSharedPreferences(PREFS, MODE_PRIVATE);
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
