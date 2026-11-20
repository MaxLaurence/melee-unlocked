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
Android answers every present with "suboptimal" because the frame is drawn upright on a turned panel;
that is not a reason to make the swapchain again here (it was made again every frame). Each draw's
bindings go in a descriptor set rather than being pushed (`VK_KHR_push_descriptor`): with them pushed,
Qualcomm's driver (Adreno 740) faulted inside `vkEndCommandBuffer` after a minute or two of a match,
with the Khronos validation layer reporting nothing; with sets the same match runs to its end at 120 fps.

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

**The second screen** (the AYN Thor's bottom panel): while the game runs, `BottomScreen` (a Presentation, Android's
window for another display, owned by the game's activity) opens there, and the game's renderer draws the companion there
(`runtime/gx/companion.h`): a second swapchain on that surface (`gx_vulkan.cpp`, `second_*`), an ImGui
context of its own that shares the panel's fonts (`pc_settings.cpp`, `settings_companion_*`), worked by
touch (`companion_input.h`: a tap clicks, a drag on a slider moves it, a drag elsewhere scrolls). Its
window can never be focused, so the controllers stay with the game; it is drawn at most 60 times a
second and only when an image is free, so the game's frame never waits for it. The pages:

- **Settings**: the settings panel itself, off the game's picture. Back (or the controller shortcut)
  still opens it, and the controller drives it there; opened by touch, the controller stays in the game.
- **Online**: a connect code typed on a touch keyboard (Direct or Teams; recent codes from
  `direct-codes.json`), the search started from Training as the Practice menu starts it; during a match
  the players, ping and a graph of ping and rollbacks; after it, the game's Slippi-style stats and the
  set so far (`app/launcher_replay_data.cpp`, read off the recording just written).
- **Training**: the controller as the game read it, each fighter's action, frame and damage, frame
  advantage, every aerial landing's L-cancel (how early the press was), the Training tools, and Save /
  Load state (the Lab's own D-pad save, pressed for the player).
- **Replays**: every recording with its players, stage, date and length; one replay's stats; watch it on
  the top screen (the game restarts into it and comes back after) or on the bottom screen (a second game,
  `ViewerActivity`, in a process of its own) while you keep playing. A replay playing on the top screen
  gets a timeline: drag to seek, pause, step, slow motion, fast forward.
- **Room**: the warm-up room and arena rooms: where you are, the room's game and score, and the room's
  match on the bottom screen (the arena's viewer as `ViewerActivity`, muted).
- **Sticks**: the built-in sticks' calibration, live: both sticks as read and as the game gets them,
  measure the centre and the range, deadzone, response curve and largest output; Save writes
  `builtin-sticks.ini`.

The launcher's **Bottom screen** button turns it off (then the display keeps AYN's own launcher). On a
Mac the pages can be seen beside the game for development: `MELEE_COMPANION_PREVIEW=1` (and
`MELEE_COMPANION_PAGE=online|training|replays|room`).

**Also built in**: Slippi online and rollback netplay (curl and OpenSSL, certificates from Android's
own store), music packs and video backgrounds (a static FFmpeg with the game's formats only), the
settings panel, texture packs, mods, replays.

Not yet on Android: a match through Slippi's matchmaking (untested).
