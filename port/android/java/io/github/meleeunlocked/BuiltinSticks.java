// A handheld's built-in sticks as slippi-android reads them (utils/OdinMappingInput.java there). The AYN
// Thor reports its sticks to Android already scaled and clipped (full deflection after about a third of
// the throw); AYN's mapping service (com.odin.mapping) answers currentRawEvent() with the lower-level
// values, whose first four lanes are the two sticks. They are read here every 2 ms and handed to the game
// (android_input.cpp), which calibrates them as that app does. Without the service (another device) the
// game keeps Android's values, with the same deadzone and curve.
// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.meleeunlocked;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;
import android.util.Log;
import android.view.InputDevice;

final class BuiltinSticks {
    private static final String TAG = "MeleeUnlocked";
    private static final String MAPPING_PACKAGE = "com.odin.mapping";
    private static final String MAPPING_SERVICE = "com.ro.mapping.service.ApiService";
    private static final String MAPPING_ACTION = "com.ro.mapping.action.MAPPING_SERVICE";
    private static final String SERVER_DESCRIPTOR = "com.ro.mapping.sdk.IServerApi";
    private static final int TRANSACTION_CURRENT_RAW_EVENT = 24;
    private static final float RAW_AXIS_MAX = 4096.0f;
    private static final long POLL_MS = 2;
    private static final int FAILURE_LIMIT = 120;

    interface Sink { void sticks(float lx, float ly, float rx, float ry); }

    private final Context context;
    private final Sink sink;
    private final Object lock = new Object();
    private IBinder service;
    private boolean bound, running, unavailable, loggedShape;
    private int failures;
    private HandlerThread thread;
    private Handler handler;

    BuiltinSticks(Context context, Sink sink) {
        this.context = context.getApplicationContext();
        this.sink = sink;
    }

    private static Intent serviceIntent() {
        return new Intent(MAPPING_ACTION).setComponent(new ComponentName(MAPPING_PACKAGE, MAPPING_SERVICE));
    }

    /** Whether this device has AYN's mapping service (the Thor and AYN's other handhelds). */
    static boolean available(Context context) {
        try {
            return context.getPackageManager().resolveService(serviceIntent(), 0) != null;
        } catch (RuntimeException e) {
            return false;
        }
    }

    /** The handheld's own pad, as {vendor, product}: AYN names it "Odin Controller". Null when none. */
    static int[] builtinPad() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null || d.isVirtual()) continue;
            int sources = d.getSources();
            boolean pad = (sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                    || (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
            String name = d.getName() == null ? "" : d.getName().toLowerCase();
            if (pad && name.contains("odin")) return new int[]{d.getVendorId(), d.getProductId()};
        }
        return null;
    }

    private final ServiceConnection connection = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName name, IBinder binder) {
            synchronized (lock) { service = binder; failures = 0; unavailable = false; }
            Log.i(TAG, "built-in sticks: AYN mapping service connected");
        }
        @Override public void onServiceDisconnected(ComponentName name) {
            synchronized (lock) { service = null; }
        }
    };

    private final Runnable poll = new Runnable() {
        @Override public void run() {
            synchronized (lock) { if (!running) return; }
            read();
            synchronized (lock) { if (running && !unavailable && handler != null) handler.postDelayed(this, POLL_MS); }
        }
    };

    void start() {
        synchronized (lock) {
            if (running || unavailable) return;
            running = true;
        }
        if (!bound) {
            try {
                bound = context.bindService(serviceIntent(), connection, Context.BIND_AUTO_CREATE);
            } catch (RuntimeException e) {
                bound = false;
            }
            if (!bound) { synchronized (lock) { running = false; unavailable = true; } return; }
        }
        thread = new HandlerThread("builtin-sticks", Process.THREAD_PRIORITY_URGENT_DISPLAY);
        thread.start();
        handler = new Handler(thread.getLooper());
        handler.post(poll);
    }

    void stop() {
        synchronized (lock) { running = false; }
        if (thread != null) { thread.quitSafely(); thread = null; handler = null; }
        if (bound) {
            try { context.unbindService(connection); } catch (IllegalArgumentException ignored) { }
            bound = false;
            synchronized (lock) { service = null; }
        }
    }

    private void read() {
        IBinder s;
        synchronized (lock) { s = service; }
        if (s == null) return;
        Parcel data = Parcel.obtain(), reply = Parcel.obtain();
        try {
            data.writeInterfaceToken(SERVER_DESCRIPTOR);
            if (!s.transact(TRANSACTION_CURRENT_RAW_EVENT, data, reply, 0)) { fail("rejected"); return; }
            reply.readException();
            double[] v = reply.createDoubleArray();
            if (v == null || v.length < 4) { fail("short reply"); return; }
            if (!loggedShape) {
                loggedShape = true;
                Log.i(TAG, String.format(java.util.Locale.US, "built-in sticks: raw reader live (%d lanes; at start %.0f %.0f %.0f %.0f)",
                        v.length, v[0], v[1], v[2], v[3]));
            }
            synchronized (lock) { failures = 0; }
            // The lanes run opposite to Android's axes (right and down read negative).
            sink.sticks(-unit(v[0]), -unit(v[1]), -unit(v[2]), -unit(v[3]));
        } catch (Exception e) {
            fail(e.getClass().getSimpleName());
        } finally {
            reply.recycle();
            data.recycle();
        }
    }

    private static float unit(double v) {
        if (Double.isNaN(v)) return 0f;
        if (v <= Short.MIN_VALUE) return -1f;
        if (v >= Short.MAX_VALUE) return 1f;
        return Math.max(-1f, Math.min(1f, (float) (v / RAW_AXIS_MAX)));
    }

    private void fail(String why) {
        synchronized (lock) {
            if (++failures < FAILURE_LIMIT) return;
            unavailable = true;
            running = false;
        }
        Log.w(TAG, "built-in sticks: AYN mapping service unusable (" + why + "); using Android's stick values");
    }
}
