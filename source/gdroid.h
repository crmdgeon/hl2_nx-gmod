/* gdroid.h -- "GMod" profile: run the GDroid (Garry's Mod for Android) modules
 *
 * Selected with `gamedir garrysmod` in config.txt. The stock Half-Life 2
 * profile is untouched; this only supplies a different module list, data
 * layout, environment and command line.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __GDROID_H__
#define __GDROID_H__

// modules in load order (dependencies first), without the ones the host
// provides itself (libSDL2) or fakes (libopenxr_loader, libzstd-jni)
extern const char *const gdroid_module_names[];
extern const unsigned int gdroid_num_modules;

// 1 when config.txt selects the GMod profile
int gdroid_requested(void);

// "lib_gmod/<name>"
const char *gdroid_module_path(const char *name, char *path, unsigned int size);

// aborts if the libraries are missing, only warns about game data
void gdroid_check_data(void);

// exports the environment variables the Android launcher contract expects and
// builds the argv for LauncherMainAndroid (static storage); returns argc
int gdroid_setup_environment(char ***argv_out);

// forces LuaJIT into interpreter-only mode (see gdroid.c)
void gdroid_patch_lua(void);

#endif
