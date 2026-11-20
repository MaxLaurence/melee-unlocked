// The first screen: the disc (picked once with Android's file picker, remembered), the Slippi sign-in
// (SlippiLoginActivity) and Play, which starts the game (MeleeActivity). Works with a controller: the
// buttons take focus, A presses, B goes back. It also unpacks the package's assets (Sys, Shaders,
// ui_sources: what the other builds keep beside the program) into the app's files folder, once per install.
// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.meleeunlocked;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.content.res.AssetManager;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

public class LauncherActivity extends Activity {
    static final String PREFS = "melee";
    static final String KEY_DISC_URI = "disc_uri";
    static final String KEY_DISC_NAME = "disc_name";
    static final String KEY_DISC_PATH = "disc_path";
    private static final int PICK_DISC = 1, SIGN_IN = 2;

    private TextView discLine, slippiLine, statusLine;
    private Button play, chooseDisc, signIn;
    private volatile boolean assetsReady = false;

    /** The player's folder (settings, saves, replays): android_launch.h's default, under the files folder. */
    static File userDir(Context context) { return new File(context.getFilesDir(), "User/Slippi"); }
    static File userJson(Context context) { return new File(userDir(context), "user.json"); }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        root.setBackgroundColor(Color.rgb(14, 16, 22));
        int pad = dp(32);
        root.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText("Melee Unlocked");
        title.setTextColor(Color.WHITE);
        title.setTextSize(34);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setGravity(Gravity.CENTER);
        root.addView(title);

        discLine = line(root, 18);
        slippiLine = line(root, 18);
        statusLine = line(root, 14);
        statusLine.setTextColor(Color.rgb(150, 156, 170));

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(24);
        root.addView(buttons, lp);
        play = button(buttons, "Play", v -> startGame());
        chooseDisc = button(buttons, "Choose disc", v -> pickDisc());
        signIn = button(buttons, "Sign in to Slippi", v -> toggleSignIn());
        setContentView(root);
        findDiscInAppFolder();
        refresh();
        focusFirst();
        unpackAssets();
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
        getWindow().setDecorFitsSystemWindows(false);
        android.view.WindowInsetsController bars = getWindow().getInsetsController();
        if (bars != null) {
            bars.setSystemBarsBehavior(android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            bars.hide(android.view.WindowInsets.Type.systemBars());
        }
    }

    private void focusFirst() {
        if (play.isEnabled()) play.requestFocus();
        else chooseDisc.requestFocus();
    }

    /** The app's own folder on the shared storage (Android/data/<package>/files): a disc copied there
     *  over USB is used without picking it, when none was picked. */
    private void findDiscInAppFolder() {
        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        String path = prefs.getString(KEY_DISC_PATH, null);
        if (path != null && !new File(path).isFile())
            prefs.edit().remove(KEY_DISC_PATH).remove(KEY_DISC_NAME).apply();
        if (prefs.getString(KEY_DISC_URI, null) != null || prefs.getString(KEY_DISC_PATH, null) != null) return;
        File folder = getExternalFilesDir(null);
        File[] files = folder != null ? folder.listFiles() : null;
        if (files == null) return;
        for (File f : files) {
            String name = f.getName().toLowerCase();
            if (f.isFile() && (name.endsWith(".iso") || name.endsWith(".gcm"))) {
                prefs.edit().putString(KEY_DISC_PATH, f.getAbsolutePath()).putString(KEY_DISC_NAME, f.getName()).apply();
                return;
            }
        }
    }

    private TextView line(LinearLayout root, int size) {
        TextView t = new TextView(this);
        t.setTextColor(Color.rgb(220, 224, 232));
        t.setTextSize(size);
        t.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(10);
        root.addView(t, lp);
        return t;
    }

    private Button button(LinearLayout row, String label, View.OnClickListener click) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextSize(18);
        b.setTextColor(Color.WHITE);
        b.setFocusable(true);
        b.setOnClickListener(click);
        StateListDrawable states = new StateListDrawable();
        states.addState(new int[]{android.R.attr.state_focused}, rounded(Color.rgb(210, 40, 52), Color.WHITE));
        states.addState(new int[]{android.R.attr.state_pressed}, rounded(Color.rgb(170, 30, 42), Color.WHITE));
        states.addState(new int[]{}, rounded(Color.rgb(44, 48, 60), Color.rgb(44, 48, 60)));
        b.setBackground(states);
        b.setPadding(dp(22), dp(12), dp(22), dp(12));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.leftMargin = lp.rightMargin = dp(8);
        row.addView(b, lp);
        return b;
    }

    private GradientDrawable rounded(int fill, int stroke) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(fill);
        d.setCornerRadius(dp(10));
        d.setStroke(dp(2), stroke);
        return d;
    }

    private int dp(int v) { return Math.round(v * getResources().getDisplayMetrics().density); }

    private void refresh() {
        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        String name = prefs.getString(KEY_DISC_NAME, null);
        discLine.setText(name != null ? "Disc: " + name : "Disc: none chosen (Melee NTSC 1.02 .iso)");
        String code = connectCode(this);
        slippiLine.setText(code != null ? "Slippi: signed in as " + code : "Slippi: not signed in (offline play works without it)");
        signIn.setText(code != null ? "Sign out" : "Sign in to Slippi");
        play.setEnabled(name != null);
        play.setAlpha(name != null ? 1f : 0.5f);
        if (!assetsReady) statusLine.setText("Preparing game files…");
    }

    /** The connect code in the sign-in file, or null when there is none. */
    static String connectCode(Context context) {
        File f = userJson(context);
        if (!f.isFile()) return null;
        try (InputStream in = new FileInputStream(f)) {
            byte[] bytes = new byte[(int) Math.min(f.length(), 1 << 20)];
            int n = 0, r;
            while (n < bytes.length && (r = in.read(bytes, n, bytes.length - n)) > 0) n += r;
            JSONObject json = new JSONObject(new String(bytes, 0, n, StandardCharsets.UTF_8));
            String code = json.optString("connectCode", "");
            return code.isEmpty() ? null : code;
        } catch (Exception e) {
            return null;
        }
    }

    private void pickDisc() {
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("*/*");
        pick.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        try {
            startActivityForResult(pick, PICK_DISC);
        } catch (Exception e) {
            Toast.makeText(this, "No file picker on this device", Toast.LENGTH_LONG).show();
        }
    }

    private void toggleSignIn() {
        if (connectCode(this) != null) {
            //noinspection ResultOfMethodCallIgnored
            userJson(this).delete();
            Toast.makeText(this, "Signed out of Slippi", Toast.LENGTH_SHORT).show();
            refresh();
            return;
        }
        startActivityForResult(new Intent(this, SlippiLoginActivity.class), SIGN_IN);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == PICK_DISC && result == RESULT_OK && data != null && data.getData() != null) {
            Uri uri = data.getData();
            try {
                getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (SecurityException ignored) {
                // A provider without persistable grants: the disc works until the app restarts.
            }
            String name = displayName(getContentResolver(), uri);
            getSharedPreferences(PREFS, MODE_PRIVATE).edit().remove(KEY_DISC_PATH)
                .putString(KEY_DISC_URI, uri.toString()).putString(KEY_DISC_NAME, name != null ? name : uri.getLastPathSegment()).apply();
        }
        refresh();
        focusFirst();
    }

    static String displayName(ContentResolver resolver, Uri uri) {
        try (Cursor c = resolver.query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0);
        } catch (Exception ignored) {
        }
        return null;
    }

    private void startGame() {
        if (!assetsReady) {
            Toast.makeText(this, "Still preparing the game files…", Toast.LENGTH_SHORT).show();
            return;
        }
        startActivity(new Intent(this, MeleeActivity.class));
    }

    // ---- the package's assets ("res") into <files>/res, once per installed version
    private void unpackAssets() {
        new Thread(() -> {
            String stamp;
            try {
                PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
                stamp = info.versionName + "/" + info.lastUpdateTime;
            } catch (Exception e) {
                stamp = "unknown";
            }
            File res = new File(getFilesDir(), "res");
            File marker = new File(res, ".unpacked");
            boolean current = false;
            try (InputStream in = new FileInputStream(marker)) {
                byte[] b = new byte[256];
                int n = in.read(b);
                current = n > 0 && new String(b, 0, n, StandardCharsets.UTF_8).equals(stamp);
            } catch (IOException ignored) {
            }
            String error = null;
            if (!current) {
                try {
                    copyAssets(getAssets(), "res", res);
                    try (OutputStream out = new FileOutputStream(marker)) { out.write(stamp.getBytes(StandardCharsets.UTF_8)); }
                } catch (IOException e) {
                    error = e.getMessage();
                }
            }
            final String failed = error;
            runOnUiThread(() -> {
                assetsReady = failed == null;
                statusLine.setText(failed == null ? "" : "Could not prepare the game files: " + failed);
            });
        }, "unpack-assets").start();
    }

    private static void copyAssets(AssetManager assets, String path, File target) throws IOException {
        String[] children = assets.list(path);
        if (children != null && children.length > 0) {
            if (!target.isDirectory() && !target.mkdirs()) throw new IOException("cannot create " + target);
            for (String child : children) copyAssets(assets, path + "/" + child, new File(target, child));
            return;
        }
        try (InputStream in = assets.open(path); OutputStream out = new FileOutputStream(target)) {
            byte[] buffer = new byte[1 << 16];
            int n;
            while ((n = in.read(buffer)) > 0) out.write(buffer, 0, n);
        }
    }

    // A controller: A presses the focused button, B leaves.
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        int key = event.getKeyCode();
        if (key == KeyEvent.KEYCODE_BUTTON_A) {
            if (event.getAction() == KeyEvent.ACTION_UP) {
                View focused = getCurrentFocus();
                if (focused == null || !focused.isEnabled()) focused = play.isEnabled() ? play : chooseDisc;
                focused.performClick();
            }
            return true;
        }
        if (key == KeyEvent.KEYCODE_BUTTON_B) {
            if (event.getAction() == KeyEvent.ACTION_UP) finish();
            return true;
        }
        return super.dispatchKeyEvent(event);
    }
}
