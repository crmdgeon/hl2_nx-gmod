<div align=center>
 
</div>
<h1 align=center>A Garry's Mod port for Nintendo Switch based on hl2_nx and GDroid</h1>

An experimental native port of Garry's Mod to the Nintendo Switch, combining code and technologies from the proyects [GDroid](https://github.com/tinsarfal/GDroid) and
[hl2_nx](https://github.com/NaGaa95/hl2_nx).

### Installation

You need:
* the Source Engine `.apk` [HERE](https://arumoon.github.io/source-engine-download/)
* the GDroid ARM64 `.apk` [HERE](https://github.com/tinsarfal/GDroid/releases/download/0.1.29/GDroid-0.1.29.apk)
* Your own Garry's Mod files (/garrysmod, /platform, /sourceengine)

To install, create `/switch/hl2_nx/` on your SD card and lay it out like this:

```
/switch/
 gmod_nx_overlay/        <- the GDroid APK /assets/runtime.zip decompressed
 hl2_nx/
   hl2_nx-gmod.nro
   lib_gmod/             <- the GDroid's APK /lib files, except: "libzstd-jni-1.5.7-6.so",
                            "libSDL2.so" and "libSDL2.so"

   files/                <- the fonts from the Source Engine APK's assets/ (dejavusans.ttf,
   platform/             <- from the game data
   garrysmod/            <- from the game data
   sourceengine/         <- from the game data

   config.txt            <- a .txt with "screen_width -1"
                                        "screen_height -1"
                                        "gamedir garrysmod"
                                        "lang english"
                                        "gamepad 1"

```

### Notes

This will not work in applet/album mode — use a game override (hold R on a title) or a forwarder.
The Switch needs the full application memory pool

Some Garry's Mod addons may be compatible with this port. To install them, you can use a Windows PC and SteamCMD to download them.
* Download the required addons using SteamCMD on your PC.
* Locate the downloaded addon files.
* Copy the addon folders into the garrysmod/addons/ directory on your Nintendo Switch.
* Launch the game and check whether the addons work correctly.

Note: Compatibility is not guaranteed. Addons that rely on unsupported Lua features, external dependencies, platform-specific binaries, or features unavailable in this port may not work properly. Some addons may require additional configuration or dependencies.

### How to build

You need devkitA64 plus the following pacman packages:
* `switch-sdl2`
* `switch-sdl2_image`
* `switch-mesa`
* `switch-libdrm_nouveau`
* `switch-zlib`
* `switch-ffmpeg`

### Credits

* nillerusr for the Android Source engine port this wraps
* TheOfficialFloW & fgsfds for the so-loader lineage
* Masagrator for the hl2_picker

### Support

If you enjoy my work and want to support me :

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/D1D1P2MOG)

### Legal

This project has no affiliation with Valve Corporation. "Half-Life" and "Source" are trademarks
of their respective owners. All Rights Reserved.

No assets or program code from the original game or its Android port are included in this project.
We do not condone piracy in any way, shape or form and encourage users to legally own the original
game.

Unless specified otherwise, the source code provided in this repository is licensed under the MIT
License. Please see the accompanying LICENSE file.
