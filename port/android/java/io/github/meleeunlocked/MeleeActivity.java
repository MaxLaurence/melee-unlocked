// The game: SDL's activity, which loads libSDL2.so and libmain.so and runs SDL_main (app/main.cpp) on a
// thread of its own. The disc the launcher remembered is opened here and handed to the game as
// /proc/self/fd/<n> (the game reads it with fopen); a test can name a file instead and add arguments:
//   adb shell am start -n io.github.meleeunlocked/.MeleeActivity --es iso /sdcard/.../melee.iso --es args "--frames 600"
// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.meleeunlocked;

import android.app.ActivityManager;
import android.app.ActivityOptions;
import android.content.Context;
import android.content.Intent;
import android.hardware.display.DisplayManager;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.util.Log;
import android.view.Display;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

public class MeleeActivity extends SDLActivity {
    private static final String TAG = "MeleeUnlocked";

    // libmain.so (port/android/android_input.cpp)
    private static native void nativeBuiltinPad(int vendor, int product);
    private static native void nativeBuiltinSticks(float lx, float ly, float rx, float ry);

    private BottomScreen bottomScreen;   // the companion on the second screen, while the game is in front
    // A viewer (ViewerActivity) on the second screen: the companion's window, a presentation, would
    // sit above it, so it steps aside while the viewer runs and comes back when the viewer ends.
    private final Handler ui = new Handler(Looper.getMainLooper());
    private boolean viewerShown;
    private int viewerPid;   // once seen: followed through /proc (the process list can leave it out for a moment)
    private long viewerDeadline;
    private final Runnable watchViewer = new Runnable() {
        @Override public void run() {
            if (viewerPid == 0) viewerPid = processId(MeleeActivity.this, ":viewer");
            if (viewerPid != 0 ? new java.io.File("/proc/" + viewerPid).exists() : System.currentTimeMillis() < viewerDeadline) {
                ui.postDelayed(this, viewerPid != 0 ? 1000 : 300);
                return;
            }
            viewerShown = false;
            viewerPid = 0;
            openBottomScreen();
        }
    };
    // Opening the viewer on the second screen makes that display the focused one; FocusBounceActivity
    // gives the focus back to this screen (the viewer asks for it once it is up; this is the backstop).
    private final Runnable reclaimFocus = () -> { if (viewerShown) FocusBounceActivity.bounce(this); };

    private GcAdapterAccess adapter;
    private BuiltinSticks builtinSticks;
    private boolean nativeReady, started;

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
        getSystemService(DisplayManager.class).registerDisplayListener(displays, null);
    }

    @Override
    protected void onDestroy() {
        final boolean finishing = isFinishing();
        getSystemService(DisplayManager.class).unregisterDisplayListener(displays);
        closeBottomScreen();
        super.onDestroy();
        if (finishing) {
            // SDL leaves this process running once the game has returned, and the game's library cannot
            // start twice in one process: the next Play (or a relaunch into a replay) gets a fresh one.
            // A viewer on the second screen does not outlive the game.
            closeViewer();
            android.os.Process.killProcess(android.os.Process.myPid());
        }
    }

    // ---- for the game (android_second_screen.cpp), from its threads
    /** Whether the handheld has a second screen. */
    @SuppressWarnings("unused")
    public boolean hasBottomDisplay() { return bottomDisplay(this) != null; }

    /** A second game on the second screen (ViewerActivity), with these arguments; a running one is closed first. */
    @SuppressWarnings("unused")
    public boolean launchViewer(String[] argv) {
        Display display = bottomDisplay(this);
        if (display == null) return false;
        closeViewer();
        runOnUiThread(() -> {
            viewerShown = true;
            viewerPid = 0;
            viewerDeadline = System.currentTimeMillis() + 15000;
            closeBottomScreen();
            ui.removeCallbacks(watchViewer);
            ui.postDelayed(watchViewer, 500);
            // In case the focus moved without telling this window: look again once the viewer is up.
            ui.postDelayed(reclaimFocus, 1500);
            ui.postDelayed(reclaimFocus, 4000);
        });
        try {
            Intent intent = new Intent(this, ViewerActivity.class).putExtra("argv", argv)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_MULTIPLE_TASK | Intent.FLAG_ACTIVITY_NO_ANIMATION);
            startActivity(intent, ActivityOptions.makeBasic().setLaunchDisplayId(display.getDisplayId()).toBundle());
            Log.i(TAG, "viewer: opening on display " + display.getDisplayId() + " " + Arrays.toString(argv));
            return true;
        } catch (RuntimeException e) {
            Log.w(TAG, "viewer: could not open: " + e);
            return false;
        }
    }

    /** The viewer's process id, or 0. */
    static int processId(Context context, String suffix) {
        ActivityManager am = context.getSystemService(ActivityManager.class);
        List<ActivityManager.RunningAppProcessInfo> running = am != null ? am.getRunningAppProcesses() : null;
        if (running == null) return 0;
        String name = context.getPackageName() + suffix;
        for (ActivityManager.RunningAppProcessInfo p : running) if (name.equals(p.processName)) return p.pid;
        return 0;
    }

    @SuppressWarnings("unused")
    public boolean viewerRunning() { return processId(this, ":viewer") != 0; }

    @SuppressWarnings("unused")
    public void closeViewer() {
        int pid = processId(this, ":viewer");
        if (pid != 0) {
            android.os.Process.killProcess(pid);
            Log.i(TAG, "viewer: closed");
        }
    }

    /** The game's last moment (android_second_screen.cpp before_process_end; the process then ends at
     *  once, without this activity's teardown): a relaunch asked for goes to the launcher, which starts
     *  the game again once this process is gone. Started from here, while the game is still in front. */
    @SuppressWarnings("unused")
    public void gameEnding() {
        if (!LauncherActivity.relaunchFile(this).isFile()) return;
        try {
            startActivity(new Intent(this, LauncherActivity.class)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_NO_ANIMATION));
        } catch (RuntimeException e) {
            Log.w(TAG, "relaunch: could not reach the launcher: " + e);
        }
    }

    /** Leave the game and start it again with these arguments (the launcher does it once this process
     *  has ended); comeBack: start it once more, as usual, when that run ends. The game then exits. */
    @SuppressWarnings("unused")
    public boolean requestRelaunch(String args, boolean comeBack) {
        // A file, not the preferences: those are cached per process, and the launcher's process reads it.
        java.io.File request = LauncherActivity.relaunchFile(this);
        try (java.io.FileOutputStream out = new java.io.FileOutputStream(request)) {
            out.write(((comeBack ? "1" : "0") + "\n" + args).getBytes(java.nio.charset.StandardCharsets.UTF_8));
        } catch (java.io.IOException e) {
            Log.w(TAG, "relaunch: cannot write the request: " + e);
            return false;
        }
        Log.i(TAG, "relaunch: " + args + (comeBack ? " (then back to the game)" : ""));
        return true;
    }

    // ---- the second screen (BottomScreen, a Presentation): open while the game is in front
    @Override
    protected void onStart() {
        super.onStart();
        started = true;
        openBottomScreen();
    }

    @Override
    protected void onStop() {
        started = false;
        closeBottomScreen();
        super.onStop();
    }

    /** The other built-in screen, or null: the first display that is not the main one and not an app's own (private) display. */
    static Display bottomDisplay(android.content.Context context) {
        DisplayManager manager = context.getSystemService(DisplayManager.class);
        for (Display d : manager.getDisplays()) {
            if (d.getDisplayId() == Display.DEFAULT_DISPLAY) continue;
            if ((d.getFlags() & Display.FLAG_PRIVATE) != 0) continue;
            return d;
        }
        return null;
    }

    private void openBottomScreen() {
        if (!nativeReady || !started || bottomScreen != null || viewerShown) return;
        if (!getSharedPreferences(LauncherActivity.PREFS, MODE_PRIVATE).getBoolean(LauncherActivity.KEY_BOTTOM_SCREEN, true)) return;
        // Tests: adb shell am start ... --es bottom off
        if (getIntent() != null && "off".equals(getIntent().getStringExtra("bottom"))) return;
        Display display = bottomDisplay(this);
        if (display == null) return;
        try {
            BottomScreen screen = new BottomScreen(this, display);
            screen.setOnDismissListener(d -> { if (bottomScreen == d) bottomScreen = null; });
            screen.show();
            bottomScreen = screen;
        } catch (RuntimeException e) {   // WindowManager.InvalidDisplayException among them
            Log.w(TAG, "second screen: could not open on display " + display.getDisplayId() + ": " + e);
        }
    }

    private void closeBottomScreen() {
        BottomScreen screen = bottomScreen;
        bottomScreen = null;
        if (screen != null) screen.dismiss();
    }

    private final DisplayManager.DisplayListener displays = new DisplayManager.DisplayListener() {
        @Override public void onDisplayAdded(int id) { openBottomScreen(); }
        @Override public void onDisplayRemoved(int id) {
            if (bottomScreen != null && bottomScreen.getDisplay().getDisplayId() == id) closeBottomScreen();
        }
        @Override public void onDisplayChanged(int id) { }
    };

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
        String[] argv = intent != null ? intent.getStringArrayExtra("argv") : null;
        if (argv != null) args.addAll(Arrays.asList(argv));
        Log.i(TAG, "arguments: " + args);
        return args.toArray(new String[0]);
    }

    private String openRememberedDisc() { return rememberedDisc(this); }

    /** The disc the launcher remembered, as a path the game can open (a picked document: /proc/self/fd/N). */
    static String rememberedDisc(Context context) {
        String path = context.getSharedPreferences(LauncherActivity.PREFS, MODE_PRIVATE).getString(LauncherActivity.KEY_DISC_PATH, null);
        if (path != null && new java.io.File(path).canRead()) return path;
        String uri = context.getSharedPreferences(LauncherActivity.PREFS, MODE_PRIVATE).getString(LauncherActivity.KEY_DISC_URI, null);
        if (uri == null) return null;
        try {
            ParcelFileDescriptor pfd = context.getContentResolver().openFileDescriptor(Uri.parse(uri), "r");
            if (pfd == null) return null;
            int fd = pfd.detachFd();   // stays open for the game's lifetime (the process ends with it)
            return "/proc/self/fd/" + fd;
        } catch (Exception e) {
            Log.w(TAG, "cannot open the disc " + uri + ": " + e);
            return null;
        }
    }
}
