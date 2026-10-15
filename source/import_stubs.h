/* import_stubs.h -- logging trampolines for imports nobody implemented
 *
 * A module import that resolves to nothing used to be "tainted" so a call
 * crashed at an anonymous address. With this pool the import is bound to a
 * tiny function that writes the symbol's name to the log (the first few times
 * it is called) and returns 0, so the log says exactly what the game asked for.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __IMPORT_STUBS_H__
#define __IMPORT_STUBS_H__

#include <stdint.h>

// Returns the address of a trampoline dedicated to `name` (the same one for
// repeated requests of the same name), or 0 when the pool is exhausted.
uintptr_t import_stub_for(const char *name);

// number of distinct names handed out so far / pool capacity
int import_stub_count(void);
int import_stub_capacity(void);

// copies the names handed out so far into out[] (up to max), returns how many
int import_stub_names(const char **out, int max);

// for tests: how many times trampoline `index` has been called
unsigned import_stub_hits(int index);

#endif
