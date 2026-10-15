/* import_stubs.c -- logging trampolines for imports nobody implemented
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include <string.h>

#include "import_stubs.h"
#include "import_stub_list.h"
#include "util.h"

#define STUB_NAME_MAX 96
#define STUB_LOG_FIRST_HITS 5   // calls logged per symbol before going quiet

typedef struct {
  char name[STUB_NAME_MAX];
  unsigned hits;
} StubSlot;

static StubSlot slots[IMPORT_STUB_POOL];
static int num_slots = 0;

static long stub_hit(int i) {
  StubSlot *s = &slots[i];
  s->hits++;
  if (s->hits <= STUB_LOG_FIRST_HITS)
    debugPrintf("UNIMPLEMENTED import called: %s (call #%u)\n", s->name, s->hits);
  else if (s->hits == STUB_LOG_FIRST_HITS + 1)
    debugPrintf("UNIMPLEMENTED import %s keeps being called; not logging it again\n", s->name);
  return 0;
}

#define DEFINE_STUB(n) static long stub_##n(void) { return stub_hit(n); }
IMPORT_STUB_LIST(DEFINE_STUB)
#undef DEFINE_STUB

#define STUB_ADDR(n) (uintptr_t)&stub_##n,
static const uintptr_t stub_fns[IMPORT_STUB_POOL] = { IMPORT_STUB_LIST(STUB_ADDR) };
#undef STUB_ADDR

uintptr_t import_stub_for(const char *name) {
  if (!name)
    return 0;
  for (int i = 0; i < num_slots; i++) {
    if (strcmp(slots[i].name, name) == 0)
      return stub_fns[i];
  }
  if (num_slots >= IMPORT_STUB_POOL)
    return 0;
  strncpy(slots[num_slots].name, name, STUB_NAME_MAX - 1);
  slots[num_slots].name[STUB_NAME_MAX - 1] = '\0';
  slots[num_slots].hits = 0;
  return stub_fns[num_slots++];
}

int import_stub_count(void) { return num_slots; }
int import_stub_capacity(void) { return IMPORT_STUB_POOL; }

int import_stub_names(const char **out, int max) {
  int n = num_slots < max ? num_slots : max;
  for (int i = 0; i < n; i++)
    out[i] = slots[i].name;
  return n;
}

unsigned import_stub_hits(int index) {
  return (index >= 0 && index < num_slots) ? slots[index].hits : 0;
}
