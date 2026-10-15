# GMod profile (experimental)

Runs the libraries of **GDroid** (Garry's Mod for Android) inside the hl2_nx loader. Select it with
`gamedir garrysmod` in `config.txt`; any other `gamedir` keeps the stock Half-Life 2 behaviour.

Nothing from Valve, Facepunch or the GDroid project is included in this repository. You provide:

* **The GDroid APK** (arm64), from its own releases page.
* **Garry's Mod game data** from your own Steam copy: the `garrysmod/`, `sourceengine/` and
  `platform/` folders, complete, including every numbered `*_000.vpk` part next to its `*_dir.vpk`.

## Setup

1. Put the three game folders inside the hl2_nx install folder (e.g. `/switch/hl2_nx`).
2. Build the library and overlay folders from your APK:

   ```
   python3 tools/prepare_gmod_files.py GDroid-<version>.apk <SD>/switch/hl2_nx
   ```

   This creates `lib_gmod/` inside the install folder and `gmod_nx_overlay/` **next to it**.
   The overlay must not be inside the install folder: the engine validates its private write
   root and refuses every write if it is nested in the shared content root.
3. Add `gamedir garrysmod` to `config.txt` and launch with title override (hold R) so the
   process gets the full RAM.

## Optional files

* `<install>/gmod_env.txt`: `NAME=VALUE` lines applied to the environment before the engine starts.
* `gmod_nx_overlay/cfg/autoexec.cfg`: your own console settings.

## Notes

* Needs `DEBUG_LOG 1` in `config.h` to produce `debug.log`.
* AAudio and OpenXR are reported as unavailable on purpose; Android video decoding is not implemented.
* Experimental: expect crashes and unimplemented features. Please include `debug.log` and, if it
  exists, `crash_dump.txt` in bug reports.

## Credits and legal

Based on [NaGaa95/hl2_nx](https://github.com/NaGaa95/hl2_nx) (MIT) and the Source engine port by
nillerusr. GDroid is the work of its own authors. This project has no affiliation with Valve
Corporation, Facepunch Studios or GDroid; "Half-Life", "Source" and "Garry's Mod" are trademarks of
their owners. You must own the game to use it. Unless stated otherwise, the code in this
repository is MIT licensed (see `LICENSE`).
