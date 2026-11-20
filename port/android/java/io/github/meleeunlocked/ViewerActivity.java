// A second game on the handheld's second screen: an arena room's match (runtime/hle/slippi_arena.cpp,
// the room's viewer) or a replay picked on the companion's Replays page, played while the game itself
// goes on on the main screen. The same libmain.so in a process of its own (":viewer"), opened by
// MeleeActivity.launchViewer onto the other display with the arguments the game chose (--replay ...).
//
// It never takes the controllers and never the GameCube adapter; it is muted by its arguments. Opening
// it makes its display the focused one; MeleeActivity takes the focus back (with this window
// unfocusable, a button press would otherwise find no window and Android would report it frozen).
// MeleeActivity closes it by ending its process, its Close button by finishing, and it ends with its
// replay.
// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.meleeunlocked;

import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.view.Gravity;
import android.widget.Button;
import android.widget.RelativeLayout;
import android.util.Log;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

public class ViewerActivity extends SDLActivity {
    private static final String TAG = "MeleeUnlocked";

    @Override
    protected String[] getLibraries() {
        return new String[]{"SDL2", "main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        super.onCreate(savedInstanceState);
        addCloseButton();
        getWindow().setDecorFitsSystemWindows(false);
        WindowInsetsController bars = getWindow().getInsetsController();
        if (bars != null) {
            bars.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            bars.hide(WindowInsets.Type.systemBars());
        }
    }

    /** A Close in the corner: the second screen goes back to the companion's pages. */
    private void addCloseButton() {
        if (mLayout == null) return;
        Button close = new Button(this);
        close.setText("Close");
        close.setAllCaps(false);
        close.setTextColor(Color.WHITE);
        close.setFocusable(false);
        GradientDrawable shape = new GradientDrawable();
        shape.setColor(Color.argb(170, 14, 16, 22));
        shape.setCornerRadius(18f);
        shape.setStroke(2, Color.argb(200, 210, 40, 52));
        close.setBackground(shape);
        close.setOnClickListener(v -> finish());
        RelativeLayout.LayoutParams lp = new RelativeLayout.LayoutParams(RelativeLayout.LayoutParams.WRAP_CONTENT, RelativeLayout.LayoutParams.WRAP_CONTENT);
        lp.addRule(RelativeLayout.ALIGN_PARENT_TOP);
        lp.addRule(RelativeLayout.ALIGN_PARENT_RIGHT);
        lp.topMargin = lp.rightMargin = 16;
        close.setGravity(Gravity.CENTER);
        mLayout.addView(close, lp);
    }

    private boolean focusReturned;

    @Override
    protected void onResume() {
        super.onResume();
        // Up on the second screen, which Android made the focused display: the focus goes back to the
        // game's screen (FocusBounceActivity), once its window is there.
        if (!focusReturned) {
            focusReturned = true;
            getWindow().getDecorView().postDelayed(() -> FocusBounceActivity.bounce(this), 300);
        }
    }

    @Override
    protected String[] getArguments() {
        List<String> args = new ArrayList<>();
        args.add("--android-files");
        args.add(getFilesDir().getAbsolutePath());
        String iso = MeleeActivity.rememberedDisc(this);
        if (iso != null) {
            args.add("--iso");
            args.add(iso);
        }
        args.add("--fullscreen");
        // Its own log and crash report, beside the game's (which it would otherwise write over).
        args.add("--log-file");
        args.add(new java.io.File(LauncherActivity.userDir(this), "viewer.log").getAbsolutePath());
        String[] argv = getIntent() != null ? getIntent().getStringArrayExtra("argv") : null;
        if (argv != null) args.addAll(Arrays.asList(argv));
        Log.i(TAG, "viewer arguments: " + args);
        return args.toArray(new String[0]);
    }

    // The game's adapter thread asks the activity for the GameCube adapter: the viewer never has it.
    @SuppressWarnings("unused")
    public int gcAdapterOpen() { return -1; }
    @SuppressWarnings("unused")
    public int gcAdapterClose() { return 0; }

    @Override
    protected void onDestroy() {
        final boolean finishing = isFinishing();
        super.onDestroy();
        // SDL leaves the process running after the game returns; the game's library cannot start twice in one.
        if (finishing) android.os.Process.killProcess(android.os.Process.myPid());
    }
}
