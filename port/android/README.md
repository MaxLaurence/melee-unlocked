# Android app: Vulkan renderer, arm64 handhelds

```sh
port/android/build-apk.sh /path/to/melee.iso            # build-android/MeleeUnlocked.apk
port/android/build-apk.sh /path/to/melee.iso --install  # and install it on the device adb sees
```

Needs the Android SDK (build-tools, `platforms;android-34`) and NDK 27, a JDK 17, CMake, Ninja, Python 3
and Docker Desktop (the game library is built by GCC 14 for Linux, as for the Mac). No Gradle: the APK is
put together with the SDK's own tools (`package_apk.py`: javac, d8, aapt2, zipalign, apksigner) and
signed with a debug key kept in `build-android/debug.keystore`, so reinstalls keep the app's data.
Developed and checked on an AYN Thor (Snapdragon 8 Gen 2, Adreno 740, Android 13); any arm64 device
with Vulkan 1.3 and Android 11 or later should run it.

**Using it.** The launcher (`LauncherActivity`) asks for the disc once with Android's file picker and
remembers it; a disc copied into the app's own folder (`Android/data/io.github.meleeunlocked/files/`,
reachable over USB) is found without picking. **Sign in to Slippi** opens slippi.gg's own page
(`SlippiLoginActivity`, as in slippi-android): once signed in, the page's user.json download is caught
and saved to the player's folder, where the game looks first. No Slippi keys ship in the app. **Play**
starts the game (`MeleeActivity`, SDL's activity) in a process of its own, so leaving the game returns
to the launcher. In the game the device's **Back** button opens the settings panel (F1 on a keyboard),
which the controller works. Settings, saves, replays, logs and the shader cache are in the app's
private files folder (`files/User/Slippi`); `adb shell run-as io.github.meleeunlocked` reaches them.

**The program** is `libmain.so`: the same host as the Mac and Linux builds (runtime/, app/, the Linux
compatibility layer), loaded by SDL2's Java glue, which calls `SDL_main` (`app/main.cpp`,
`android_launch.h`). Paths the other builds take from beside the program (Sys, Shaders, ui_sources)
come from the package's assets, unpacked into `files/res` once per install (`android_paths.h`).

**The game** is the arm64 Mac's GCC-built Linux ELF with its console address window at 256 GB
(`build-game.sh`): Android's arm64 kernels give a process 39 bits (512 GB), so the Mac's 1 TB does not
fit, and ART's heap fills the low 4 GB. It is installed as `libmelee_game.so` (the native library
folder is the one place an app may map code from) and loaded by the host's own loader
(`port/mac/elf_loader.cpp`), which maps its code straight from that file.

**Rendering** is the Vulkan renderer Windows and Linux offer (`runtime/gx/gx_vulkan.cpp`), on the
SDL window's Android surface (`gx_backend_android.cpp`). The generated HLSL is compiled in the program
by glslang with SPIRV-Tools' legalization (`shader_spirv.cpp`; Linux runs glslangValidator or DXC). Two
places where glslang reads HLSL differently from DXC are put right before compiling: a push constant
on a struct variable, and shaders that name their own bindings. The swapchain is drawn upright at the
window's size and Android's compositor turns it for the panel (the Thor's top screen is a portrait
panel). When the app goes to the background Android takes the surface away; the renderer makes a new
one when it returns. The display rate is the panel's (120 Hz on the Thor), with the game at 60.

**Controllers.** The **GameCube adapter** (WUP-028 and compatibles) is read by the same code as on
Windows, Linux and the Mac (`runtime/host/gc_adapter.cpp`): Android lets an app reach a USB device only
through its USB host API, so `GcAdapterAccess.java` asks the player once (Android's prompt; or choose the
game when Android offers it as the adapter is plugged in, "always" to never be asked again), opens and
claims the adapter and hands its file descriptor to libusb (`libusb_wrap_sys_device`, no device
discovery). Plugging it in or out mid-game is picked up within two seconds. The **built-in controls** of
an AYN handheld are read as slippi-android reads them (`BuiltinSticks.java`, `android_input.cpp`): the
Thor reports its sticks to Android scaled and clipped (full deflection after about a third of the
throw), so the sticks come unclipped from AYN's mapping service (`com.odin.mapping`, `currentRawEvent`,
every 2 ms) and are calibrated as that app does by default: four times outward (the raw lanes reach about
a quarter of their range), a radial deadzone and a response curve (main stick 2% and 1.5, C-stick 10%
and 2.0 so a light flick does not smash). `builtin-sticks.ini` in the player's folder changes them:

    main deadzone=0.02 curve=1.5 scale=4 cap=1 center_x=0 center_y=0
    c deadzone=0.10 curve=2.0 scale=4 cap=1

The buttons and triggers go through the game's own bindings (Settings > Controls), with L1 also a full L
press by default on Android (as slippi-android maps it). Other Bluetooth or USB controllers work as on
Linux.

**Also built in**: Slippi online and rollback netplay (curl and OpenSSL, certificates from Android's
own store), music packs and video backgrounds (a static FFmpeg with the game's formats only), the
settings panel, texture packs, mods, replays.

Not yet on Android: the Thor's second screen, a stick calibration screen (slippi-android's wizard), and
a match through Slippi's matchmaking (untested).
