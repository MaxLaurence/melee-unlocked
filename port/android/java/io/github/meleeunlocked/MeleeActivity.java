// The game: SDL's activity, which loads libSDL2.so and libmain.so and runs SDL_main (app/main.cpp) on a
// thread of its own. The disc the launcher remembered is opened here and handed to the game as
// /proc/self/fd/<n> (the game reads it with fopen); a test can name a file instead and add arguments:
//   adb shell am start -n io.github.meleeunlocked/.MeleeActivity --es iso /sdcard/.../melee.iso --es args "--frames 600"
// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.meleeunlocked;

import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.util.Log;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.util.ArrayList;
import java.util.List;

public class MeleeActivity extends SDLActivity {
    private static final String TAG = "MeleeUnlocked";

    // libmain.so (port/android/android_input.cpp)
    private static native void nativeBuiltinPad(int vendor, int product);
    private static native void nativeBuiltinSticks(float lx, float ly, float rx, float ry);

    private GcAdapterAccess adapter;
    private BuiltinSticks builtinSticks;
    private boolean nativeReady;

    @Override
    protected String[] getLibraries() {
        return new String[]{"SDL2", "main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams attributes = getWindow().getAttributes();
            attributes.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(attributes);
        }
        immersive();
        adapter = new GcAdapterAccess(this);
        // The handheld's own pad (AYN's "Odin Controller"): its sticks read unclipped and calibrated
        // as slippi-android does (BuiltinSticks, android_input.cpp).
        nativeReady = !mBrokenLibraries;
        int[] pad = BuiltinSticks.builtinPad();
        if (nativeReady && pad != null) {
            nativeBuiltinPad(pad[0], pad[1]);
            if (BuiltinSticks.available(this)) builtinSticks = new BuiltinSticks(this, MeleeActivity::nativeBuiltinSticks);
        }
    }

    // The game's adapter thread (android_input.cpp): the GameCube adapter's file descriptor, or -1.
    @SuppressWarnings("unused")
    public int gcAdapterOpen() { return adapter != null ? adapter.open() : -1; }
    @SuppressWarnings("unused")
    public int gcAdapterClose() { if (adapter != null) adapter.close(); return 0; }

    @Override
    protected void onPause() {
        if (builtinSticks != null) builtinSticks.stop();
        super.onPause();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) immersive();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (builtinSticks != null) builtinSticks.start();
        immersive();
        // Coming back from the background the system shows its bars after the activity resumes: hide
        // them again once it has.
        getWindow().getDecorView().postDelayed(this::immersive, 500);
    }

    /** The whole screen for the game: no status or navigation bar (a swipe from an edge shows them for a moment). */
    private void immersive() {
        getWindow().setDecorFitsSystemWindows(false);
        WindowInsetsController bars = getWindow().getInsetsController();
        if (bars != null) {
            bars.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            bars.hide(WindowInsets.Type.systemBars());
        }
    }

    @Override
    protected String[] getArguments() {
        List<String> args = new ArrayList<>();
        args.add("--android-files");
        args.add(getFilesDir().getAbsolutePath());
        Intent intent = getIntent();
        String iso = intent != null ? intent.getStringExtra("iso") : null;
        if (iso == null) iso = openRememberedDisc();
        if (iso != null) {
            args.add("--iso");
            args.add(iso);
        }
        args.add("--fullscreen");
        String extra = intent != null ? intent.getStringExtra("args") : null;
        if (extra != null && !extra.trim().isEmpty())
            for (String a : extra.trim().split("\\s+")) args.add(a);
        Log.i(TAG, "arguments: " + args);
        return args.toArray(new String[0]);
    }

    private String openRememberedDisc() {
        String path = getSharedPreferences(LauncherActivity.PREFS, MODE_PRIVATE).getString(LauncherActivity.KEY_DISC_PATH, null);
        if (path != null && new java.io.File(path).canRead()) return path;
        String uri = getSharedPreferences(LauncherActivity.PREFS, MODE_PRIVATE).getString(LauncherActivity.KEY_DISC_URI, null);
        if (uri == null) return null;
        try {
            ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(Uri.parse(uri), "r");
            if (pfd == null) return null;
            int fd = pfd.detachFd();   // stays open for the game's lifetime (the process ends with it)
            return "/proc/self/fd/" + fd;
        } catch (Exception e) {
            Log.w(TAG, "cannot open the disc " + uri + ": " + e);
            return null;
        }
    }
}
