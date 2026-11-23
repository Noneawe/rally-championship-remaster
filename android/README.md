# Rally Championship Remaster - Android

SDL2 hosts the port (`port/`), built as `libmain.so`, with GLES2 and a touch
overlay.  No game data is in the APK: you need your own copy of the game.

## Build

Needs the Android SDK, NDK 28.2 (16 KB page support), JDK 17 and Gradle 9.1.

    ./fetch-sdl.sh                         # SDL2 into android/SDL2
    echo sdk.dir=$ANDROID_HOME > local.properties
    gradle :app:assembleRelease            # arm64-v8a and x86_64
    # app/build/outputs/apk/release/app-release.apk (signed with the debug key)

## Game data

Copy `RAL.EXE` and the `FILES` folder from the game's CD or install to

    /sdcard/Android/media/org.rally.championship/game/

for example `adb push /path/to/game /sdcard/Android/media/org.rally.championship/game`
or with a file manager.  Saves go to `.../save/` and the log to `.../rally.log`
next to it.  Names may be in any case.

## What differs from the desktop and handheld builds

- **Resolution:** the game renders at the screen's aspect, up to 720 lines
  (a 20:9 phone gets 1600x720).  A wider-than-4:3 picture keeps the 4:3 view's
  height and shows more to the sides; the menus stay 4:3 in the middle with
  black bars.
- **Controls:** a USB or Bluetooth gamepad works as it does on desktop.
  Without one, an on-screen overlay is drawn: a d-pad, OK and ESC in the menus;
  in a race, a drag anywhere on the left half steers, GAS and BRK on the right,
  UP / DN for the gears, HB the handbrake, CAM the view, II pause.  Touching
  the screen brings the overlay back after a gamepad was used.  Back is Esc.
- **Orientation:** landscape.
