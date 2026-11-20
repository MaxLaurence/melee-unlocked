// The GameCube adapter (Nintendo WUP-028, 057E:0337) through Android's USB host API. An app reaches a
// USB device only this way: the player allows it once in Android's prompt (or by choosing this app when
// the adapter is plugged in, AndroidManifest.xml), then the connection is opened here, its interface
// claimed (which takes it from the kernel's HID driver) and its file descriptor handed to the game, whose
// adapter code (runtime/host/gc_adapter.cpp) reads it through libusb as on Linux. Called from the game's
// adapter thread every two seconds while no adapter is open (android_input.cpp).
// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.meleeunlocked;

import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import android.util.Log;

final class GcAdapterAccess {
    private static final String TAG = "MeleeUnlocked";
    private static final int NINTENDO = 0x057E, WUP028 = 0x0337;

    private final Context context;
    private UsbDeviceConnection connection;
    private int askedFor = -1;   // the device id permission was last asked for (a replugged adapter gets a new one)

    GcAdapterAccess(Context context) { this.context = context.getApplicationContext(); }

    /** The adapter's file descriptor, or -1 (none plugged in, or the player has not allowed it yet). */
    synchronized int open() {
        UsbManager usb = (UsbManager) context.getSystemService(Context.USB_SERVICE);
        if (usb == null) return -1;
        UsbDevice adapter = null;
        for (UsbDevice d : usb.getDeviceList().values())
            if (d.getVendorId() == NINTENDO && d.getProductId() == WUP028) { adapter = d; break; }
        if (adapter == null) return -1;
        if (!usb.hasPermission(adapter)) {
            if (askedFor != adapter.getDeviceId()) {
                askedFor = adapter.getDeviceId();
                Intent result = new Intent(context.getPackageName() + ".USB_PERMISSION").setPackage(context.getPackageName());
                PendingIntent pending = PendingIntent.getBroadcast(context, 0, result,
                        PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE);
                usb.requestPermission(adapter, pending);
                Log.i(TAG, "gc adapter: asking for permission to use it");
            }
            return -1;
        }
        close();
        UsbDeviceConnection c = usb.openDevice(adapter);
        if (c == null) { Log.w(TAG, "gc adapter: Android could not open it"); return -1; }
        if (adapter.getInterfaceCount() < 1) { c.close(); return -1; }
        UsbInterface itf = adapter.getInterface(0);
        if (!c.claimInterface(itf, true)) { Log.w(TAG, "gc adapter: could not claim it"); c.close(); return -1; }
        connection = c;
        return c.getFileDescriptor();
    }

    synchronized void close() {
        if (connection != null) { connection.close(); connection = null; }
    }
}
