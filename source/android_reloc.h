/* android_reloc.h -- decoder for Android's packed relocation tables
 *
 * Android NDK linkers can store .rela.dyn in a compressed form (section type
 * SHT_ANDROID_RELA, dynamic tags DT_ANDROID_RELA/DT_ANDROID_RELASZ, payload
 * starting with the magic "APS2"). The stock so_relocate() only understands
 * plain Elf64_Rela tables, so these modules must be expanded first.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __ANDROID_RELOC_H__
#define __ANDROID_RELOC_H__

#include <stdint.h>
#include <stddef.h>
#include <elf.h>

#ifndef DT_ANDROID_REL
#define DT_ANDROID_REL     0x6000000f
#define DT_ANDROID_RELSZ   0x60000010
#endif
#define DT_ANDROID_RELA    0x60000011
#define DT_ANDROID_RELASZ  0x60000012

#define SHT_ANDROID_RELA_TYPE 0x60000002u

enum {
  ANDROID_RELOC_OK          =  0,
  ANDROID_RELOC_BAD_MAGIC   = -1,  // not an "APS2" table
  ANDROID_RELOC_TRUNCATED   = -2,  // ran out of bytes while decoding
  ANDROID_RELOC_BAD_FORMAT  = -3,  // inconsistent group header / count
  ANDROID_RELOC_NO_MEMORY   = -4,
  ANDROID_RELOC_TOO_MANY    = -5,  // count field is not plausible for the data size
};

// Expands an "APS2" packed table of `size` bytes at `data` into a freshly
// malloc()ed array of Elf64_Rela (caller frees). On success returns 0 and
// sets *out / *count; on failure returns a negative ANDROID_RELOC_* code and
// leaves the outputs untouched. Never reads outside [data, data + size).
// If consumed != NULL it receives the number of input bytes actually used.
int android_reloc_unpack(const uint8_t *data, size_t size,
                         Elf64_Rela **out, size_t *count, size_t *consumed);

const char *android_reloc_strerror(int code);

#endif
