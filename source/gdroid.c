/* gdroid.c -- "GMod" profile: run the GDroid (Garry's Mod for Android) modules
 *
 * The Android launcher contract here is inferred from strings in liblauncher,
 * libengine and libclient (the Java side is not available), so the exact set
 * of variables may need adjusting; the engine logs what it finds missing.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "gdroid.h"
#include "so_util.h"

// language_env_name() lives in main.c
extern const char *language_env_name(const char *lang);

// dependency order (topological sort of the modules' DT_NEEDED entries): init
// arrays run in this order, so dependencies must come first
const char *const gdroid_module_names[] = {
  "libc++_shared.so",
  "libtier0.so",
  "libvstdlib.so",
  "libsteam_api.so",
  "libphonon.so",
  "libvrxr.so",
  "liblua_shared.so",
  "libtogl.so",
  "libfilesystem_stdio.so",
  "libinputsystem.so",
  "libdatacache.so",
  "libscenefilecache.so",
  "libsoundemittersystem.so",
  "libvideo_services.so",
  "libvaudio_minimp3.so",
  "libvaudio_opus.so",
  "libvaudio_speex.so",
  "libvphysics.so",
  "libstudiorender.so",
  "libmaterialsystem.so",
  "libshaderapidx9.so",
  "libstdshader_dx9.so",
  "libvgui2.so",
  "libvguimatsurface.so",
  "libvtex_dll.so",
  "libengine.so",
  "libGameUI.so",
  "libServerBrowser.so",
  "libclient.so",
  "libserver.so",
  "liblauncher.so",
};
const unsigned int gdroid_num_modules = sizeof(gdroid_module_names) / sizeof(*gdroid_module_names);

int gdroid_requested(void) {
  return strcmp(config.gamedir, "garrysmod") == 0;
}

const char *gdroid_module_path(const char *name, char *path, unsigned int size) {
  snprintf(path, size, "lib_gmod/%s", name);
  return path;
}

void gdroid_check_data(void) {
  struct stat st;

  // without these nothing can start
  const char *libs[] = {
    "lib_gmod/liblauncher.so",
    "lib_gmod/libengine.so",
    "lib_gmod/libclient.so",
    "lib_gmod/libserver.so",
    "lib_gmod/libtogl.so",
    "lib_gmod/liblua_shared.so",
    "lib_gmod/libvrxr.so",
    "lib_gmod/libc++_shared.so",
  };
  for (unsigned int i = 0; i < sizeof(libs) / sizeof(*libs); i++) {
    if (stat(libs[i], &st) < 0)
      fatal_error("Could not find\n%s/%s\nCopy the GDroid .so files to lib_gmod/.", config.install_root, libs[i]);
  }

  // game data: only reported, the file names were not verified against a real install
  const char *data[] = {
    "garrysmod/gameinfo.txt",
    "garrysmod/garrysmod_dir.vpk",
    "sourceengine/hl2_misc_dir.vpk",
    "platform/platform_misc_dir.vpk",
  };
  for (unsigned int i = 0; i < sizeof(data) / sizeof(*data); i++) {
    if (stat(data[i], &st) < 0)
      debugPrintf("gdroid: game data not found: %s/%s\n", config.install_root, data[i]);
  }
}

// ---------------------------------------------------------------- argv

#define MAX_ARGS 64
static char arg_storage[1536];
static size_t arg_used = 0;
static char *argv_list[MAX_ARGS + 1];
static int argc_count = 0;

static void add_arg(const char *arg) {
  const size_t len = strlen(arg) + 1;
  if (argc_count >= MAX_ARGS || arg_used + len > sizeof(arg_storage))
    return;
  memcpy(arg_storage + arg_used, arg, len);
  argv_list[argc_count++] = arg_storage + arg_used;
  arg_used += len;
}

static void add_int_arg(int value) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", value);
  add_arg(buf);
}

// config.txt "args": split on spaces
static void add_user_args(const char *args) {
  char word[128];
  size_t n = 0;
  for (const char *p = args;; p++) {
    if (*p == ' ' || *p == '\0') {
      if (n) {
        word[n] = '\0';
        add_arg(word);
        n = 0;
      }
      if (*p == '\0')
        break;
    } else if (n < sizeof(word) - 1) {
      word[n++] = *p;
    }
  }
}

// ---------------------------------------------------------------- environment

// mkdir -p
static void make_dirs(const char *path) {
  char tmp[384];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      mkdir(tmp, 0777);
      *p = '/';
    }
  }
  mkdir(tmp, 0777);
}

// Optional <install_root>/gmod_env.txt: one NAME=VALUE per line ("#" starts a
// comment, "NAME=" unsets). Applied after the defaults so the launcher contract
// can be tried out without recompiling.
static void apply_env_file(void) {
  FILE *f = fopen("gmod_env.txt", "r");
  if (!f)
    return;

  char line[512];
  while (fgets(line, sizeof(line), f)) {
    char *p = line;
    while (*p == ' ' || *p == '\t')
      p++;
    size_t len = strlen(p);
    while (len && (p[len - 1] == '\n' || p[len - 1] == '\r' || p[len - 1] == ' '))
      p[--len] = '\0';
    if (!len || *p == '#')
      continue;
    char *eq = strchr(p, '=');
    if (!eq || eq == p)
      continue;
    *eq = '\0';
    const char *name = p;
    const char *value = eq + 1;
    if (*value) {
      setenv(name, value, 1);
      debugPrintf("gmod_env.txt: %s=%s\n", name, value);
    } else {
      unsetenv(name);
      debugPrintf("gmod_env.txt: unset %s\n", name);
    }
  }
  fclose(f);
}

int gdroid_setup_environment(char ***argv_out) {
  static char lib_path[384];
  static char overlay_path[384];
  static char extras_path[384];
  struct stat st;

  snprintf(lib_path, sizeof(lib_path), "%s/lib_gmod", config.install_root);
  // private write overlay: unzip runtime.zip from the GDroid APK assets here
  // The engine validates the private write root and refuses ALL writes ("blocked
  // ... under invalid Android storage policy path") when it is nested inside
  // SOURCEVR_SHARED_CONTENT_PATH (checked in libfilesystem_stdio), so it cannot
  // live inside the install folder: use a sibling of it instead.
  {
    char parent[352];
    snprintf(parent, sizeof(parent), "%s", config.install_root);
    char *slash = strrchr(parent, '/');
    if (slash)
      *slash = '\0';
    snprintf(overlay_path, sizeof(overlay_path), "%s/gmod_nx_overlay", parent);

    // earlier builds kept it at <install_root>/gmod_overlay: move it over
    char legacy[384];
    snprintf(legacy, sizeof(legacy), "%s/gmod_overlay", config.install_root);
    if (stat(legacy, &st) == 0 && stat(overlay_path, &st) != 0) {
      if (rename(legacy, overlay_path) == 0)
        debugPrintf("gdroid: moved %s to %s\n", legacy, overlay_path);
      else
        debugPrintf("gdroid: could not move %s to %s\n", legacy, overlay_path);
    }
  }
  snprintf(extras_path, sizeof(extras_path), "%s/assets/extras_dir.vpk", config.install_root);
  mkdir(overlay_path, 0777);

  setenv("APP_DATA_PATH", config.install_root, 1);
  setenv("APP_LIB_PATH", lib_path, 1);
  unsetenv("APP_MOD_LIB");
  setenv("VALVE_GAME_PATH", config.install_root, 1);
  // shared depot: the folder that holds garrysmod/, sourceengine/ and platform/
  setenv("SOURCEVR_SHARED_CONTENT_PATH", config.install_root, 1);
  setenv("SOURCEVR_WRITE_GAME_PATH", overlay_path, 1);
  setenv("SOURCEVR_GAME", "garrysmod", 1);
  if (stat(extras_path, &st) == 0)
    setenv("EXTRAS_VPK_PATH", extras_path, 1);
  else
    unsetenv("EXTRAS_VPK_PATH");
  setenv("LANG", language_env_name(config.lang), 1);
  setenv("HOME", config.install_root, 1);
  setenv("LIBGL_USEVBO", "0", 1);
  setenv("SDL_TOUCH_MOUSE_EVENTS", "0", 1);

  apply_env_file();
  {
    const char *write_root = getenv("SOURCEVR_WRITE_GAME_PATH");
    if (write_root && *write_root)
      make_dirs(write_root);
  }

  argc_count = 0;
  arg_used = 0;
  add_arg("gmod_nx");
  add_arg("-game");
  add_arg("garrysmod");
  add_arg("-w");
  add_int_arg(screen_width);
  add_arg("-h");
  add_int_arg(screen_height);
  add_user_args(config.args);
  if (config.console)
    add_arg("-console");
  add_arg("+gmod_lua_verbose");
  add_arg("1");
  add_arg("+cl_showfps");
  add_arg(config.show_fps ? "1" : "0");
  add_arg("+joystick");
  add_arg(config.gamepad ? "1" : "0");
  add_arg("+touch_enable");
  add_arg(config.touch_hud ? "1" : "0");
  add_arg("-language");
  add_arg(config.lang);
  add_arg("-audiolanguage");
  add_arg(config.lang);
  argv_list[argc_count] = NULL;

  {
    char line[640] = "";
    for (int i = 0; i < argc_count; i++) {
      strncat(line, argv_list[i], sizeof(line) - strlen(line) - 1);
      strncat(line, " ", sizeof(line) - strlen(line) - 1);
    }
    debugPrintf("gdroid command line: %s\n", line);
  }

  *argv_out = argv_list;
  return argc_count;
}

// --- LuaJIT: run in interpreter-only mode --------------------------------------
// LuaJIT writes machine code into memory from our mmap shim (plain heap) and then
// wants it R+X. Horizon refuses to execute heap pages, so the first compiled
// trace crashes (instruction abort in the heap), and refusing mprotect(PROT_EXEC)
// makes LuaJIT raise "runtime code generation failed" as a fatal panic.
// So switch the JIT engine off for every state right after luaL_openlibs()
// (luaopen_jit switches it ON, so it has to be done after, not before).
// The call in liblua_shared goes through the module's own luaL_openlibs@plt stub;
// we overwrite that stub (offsets are for the GDroid release's liblua_shared.so)
// and our wrapper calls the untouched real function.
#define LUASHARED_OPENLIBS_OFF 0xddfecUL // luaL_openlibs
#define LUASHARED_OPENLIBS_PLT 0x245630UL // luaL_openlibs@plt

static void (*real_openlibs)(void *L);
static int (*real_setmode)(void *L, int idx, int mode);

static void openlibs_nojit(void *L) {
  if (!real_openlibs) {
    so_module *m = so_find_module("liblua_shared.so");
    real_openlibs = (void (*)(void *))so_try_find_addr_rx(m, "luaL_openlibs");
    real_setmode = (int (*)(void *, int, int))so_try_find_addr_rx(m, "luaJIT_setmode");
  }
  real_openlibs(L);
  if (real_setmode)
    real_setmode(L, 0, 0); // LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF
}

// call after so_relocate/so_resolve and before so_finalize
void gdroid_patch_lua(void) {
  so_module *m = so_find_module("liblua_shared.so");
  if (!m)
    return;
  const uintptr_t fn = so_find_addr(m, "luaL_openlibs");
  if (!fn) {
    debugPrintf("gdroid: luaL_openlibs not found, JIT not disabled\n");
    return;
  }
  const uintptr_t mirror = fn - LUASHARED_OPENLIBS_OFF;
  uint32_t *plt = (uint32_t *)(mirror + LUASHARED_OPENLIBS_PLT);
  // expect: adrp x16 / ldr x17,[x16,#imm] / add x16,x16,#imm / br x17
  if ((plt[0] & 0x9f00001f) != 0x90000010 || plt[3] != 0xd61f0220) {
    debugPrintf("gdroid: unexpected luaL_openlibs@plt (%08x %08x), JIT not disabled\n",
                plt[0], plt[3]);
    return;
  }
  hook_arm64((uintptr_t)plt, (uintptr_t)&openlibs_nojit);
  debugPrintf("gdroid: LuaJIT will be disabled after luaL_openlibs\n");
}
