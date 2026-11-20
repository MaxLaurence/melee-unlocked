// The handheld's second screen (the AYN Thor's bottom panel) while the game runs: the companion pages
// (runtime/gx/companion.cpp: settings, online, training, replays, the room's match, the sticks) drawn by
// the game's own renderer into this window's surface, and worked by touch.
//
// A Presentation (Android's window for an app's content on another display), owned by MeleeActivity:
// not an activity of its own, so the second display's focus stays where it was and the controllers keep
// going to the game. (An activity there became the focused app of the display it opened on; with its
// window unfocusable, the next button press found no window and Android reported the game frozen.) Its
// window cannot be focused either, so a touch here never takes the controllers; any key or motion that
// reaches it all the same is handed to the game's window. It closes with the game going to the
// background, and the display shows what it showed before (AYN's own launcher on the Thor).
// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.meleeunlocked;

import android.app.Presentation;
import android.content.Context;
import android.os.Bundle;
import android.util.Log;
import android.view.Display;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

final class BottomScreen extends Presentation implements SurfaceHolder.Callback {
    private static final String TAG = "MeleeUnlocked";

    // libmain.so (port/android/android_second_screen.cpp). nativeSurface(null, ...) returns only once the
    // renderer has let go of the old surface.
    static native void nativeSurface(Surface surface, int width, int height, float density);
    static native void nativeTouch(int action, int pointer, float x, float y);

    private final MeleeActivity game;

    BottomScreen(MeleeActivity game, Display display) {
        super(game, display);
        this.game = game;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Window window = getWindow();
        if (window != null)
            window.addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON
                    | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS);
        setCancelable(false);
        SurfaceView view = new SurfaceView(getContext());
        view.getHolder().addCallback(this);
        view.setFocusable(false);
        view.setOnTouchListener((v, e) -> touch(e));
        setContentView(view);
        // The bars, once the window has its views (before, it has no insets controller).
        if (window != null) {
            window.setDecorFitsSystemWindows(false);
            WindowInsetsController bars = window.getDecorView().getWindowInsetsController();
            if (bars != null) {
                bars.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                bars.hide(WindowInsets.Type.systemBars());
            }
        }
        Log.i(TAG, "second screen: open on display " + getDisplay().getDisplayId() + " (" + getDisplay().getName() + ")");
    }

    float density() {
        Context c = getContext();
        return c.getResources().getDisplayMetrics().density;
    }

    // ---- the surface
    @Override public void surfaceCreated(SurfaceHolder holder) { }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        nativeSurface(holder.getSurface(), width, height, density());
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        nativeSurface(null, 0, 0, 0f);
    }

    // ---- touch: every pointer, in the surface's pixels
    private boolean touch(MotionEvent e) {
        final int action = e.getActionMasked();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                final int i = e.getActionIndex();
                nativeTouch(action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN ? 0 : 2,
                        e.getPointerId(i), e.getX(i), e.getY(i));
                return true;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); ++i) nativeTouch(1, e.getPointerId(i), e.getX(i), e.getY(i));
                return true;
            case MotionEvent.ACTION_CANCEL:
                nativeTouch(3, -1, 0, 0);
                return true;
            default:
                return false;
        }
    }

    // ---- the controllers belong to the game
    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        return game.getWindow().getDecorView().dispatchKeyEvent(e) || super.dispatchKeyEvent(e);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent e) {
        return game.getWindow().getDecorView().dispatchGenericMotionEvent(e) || super.dispatchGenericMotionEvent(e);
    }
}
