/* gdroid_imports.h -- extra imports needed by the GDroid (Garry's Mod) modules
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __GDROID_IMPORTS_H__
#define __GDROID_IMPORTS_H__

#include <stddef.h>
#include <stdint.h>

#include "so_util.h"

// libc/locale/wide-char shims, AAudio and OpenXR "unavailable" answers, EGL
// entry points, stdio data symbols and a few SDL/Android hooks. Handed to
// so_set_extra_imports().
extern DynLibFunction gdroid_import_functions[];
extern const size_t gdroid_import_count;

// fallback for anything still unresolved: GL entry points are looked up in
// the real driver, everything else gets a logging stub (see import_stubs.h)
uintptr_t gdroid_missing_import(const char *name);

#endif
