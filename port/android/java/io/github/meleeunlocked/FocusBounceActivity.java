// Gives the keys back to the game's screen. Android keeps one focused display, and opening an activity
// on the second screen (ViewerActivity) makes that display the focused one: the controllers' buttons
// then go to the viewer, which takes none of them, and Android reports it frozen. Bringing the game's
// own task forward does not move the focus (it is already in front on its display); an activity newly
// opened on the main display does. This one opens there, invisible, and closes at once, leaving the game
// on top of the focused display.
// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.meleeunlocked;

import android.app.Activity;
import android.app.ActivityOptions;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import android.view.Display;

public class FocusBounceActivity extends Activity {
    static void bounce(Context context) {
        try {
            context.startActivity(new Intent(context, FocusBounceActivity.class)
                            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_NO_ANIMATION | Intent.FLAG_ACTIVITY_MULTIPLE_TASK),
                    ActivityOptions.makeBasic().setLaunchDisplayId(Display.DEFAULT_DISPLAY).toBundle());
        } catch (RuntimeException e) {
            Log.w("MeleeUnlocked", "focus: could not move it back to the main screen: " + e);
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        finish();
        overridePendingTransition(0, 0);
    }
}
