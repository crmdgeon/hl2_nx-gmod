/* android_reloc.c -- decoder for Android's packed relocation tables ("APS2")
 *
 * Format reference: bionic's linker (for_all_packed_relocs) and the Android
 * relocation packer. Layout, all values SLEB128:
 *
 *   "APS2"                       4 byte magic
 *   total_count                  number of relocations in the table
 *   r_offset                     initial r_offset
 *   then groups until total_count relocations were produced:
 *     group_size
 *     group_flags                1  grouped by r_info
 *                                2  grouped by r_offset delta
 *                                4  grouped by r_addend
 *                                8  group has r_addend
 *     [offset_delta]             if flags & 2
 *     [r_info]                   if flags & 1
 *     [addend_delta]             if flags & 8 and flags & 4
 *     per relocation:            [offset delta if !(flags & 2)]
 *                                [r_info       if !(flags & 1)]
 *                                [addend delta if (flags & (8|4)) == 8]
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <string.h>

#include "android_reloc.h"

#define GROUPED_BY_INFO          1u
#define GROUPED_BY_OFFSET_DELTA  2u
#define GROUPED_BY_ADDEND        4u
#define GROUP_HAS_ADDEND         8u

typedef struct {
  const uint8_t *p;
  const uint8_t *end;
  int error;
} sleb_reader;

// signed LEB128, up to 64 bits; sets error on truncation or overlong input
static int64_t sleb_read(sleb_reader *r) {
  uint64_t value = 0;
  unsigned shift = 0;
  uint8_t byte;

  do {
    if (r->p >= r->end || shift >= 70) {
      r->error = 1;
      return 0;
    }
    byte = *r->p++;
    if (shift < 64)
      value |= (uint64_t)(byte & 0x7f) << shift;
    shift += 7;
  } while (byte & 0x80);

  if (shift < 64 && (byte & 0x40))
    value |= ~(uint64_t)0 << shift; // sign extend

  return (int64_t)value;
}

int android_reloc_unpack(const uint8_t *data, size_t size,
                         Elf64_Rela **out, size_t *count, size_t *consumed) {
  if (!data || size < 4 || memcmp(data, "APS2", 4) != 0)
    return ANDROID_RELOC_BAD_MAGIC;

  sleb_reader r = { data + 4, data + size, 0 };

  const int64_t total_signed = sleb_read(&r);
  if (r.error)
    return ANDROID_RELOC_TRUNCATED;
  if (total_signed < 0)
    return ANDROID_RELOC_BAD_FORMAT;

  const uint64_t total = (uint64_t)total_signed;
  // every relocation costs at least one byte in the stream except fully
  // grouped runs; this bound only rejects absurd counts before allocating
  if (total > (uint64_t)size * 64u)
    return ANDROID_RELOC_TOO_MANY;
  if (total > SIZE_MAX / sizeof(Elf64_Rela))
    return ANDROID_RELOC_TOO_MANY;

  Elf64_Rela *rels = malloc((total ? total : 1) * sizeof(Elf64_Rela));
  if (!rels)
    return ANDROID_RELOC_NO_MEMORY;

  Elf64_Rela cur;
  memset(&cur, 0, sizeof(cur));
  cur.r_offset = (Elf64_Addr)sleb_read(&r);
  if (r.error) {
    free(rels);
    return ANDROID_RELOC_TRUNCATED;
  }

  uint64_t produced = 0;
  while (produced < total) {
    const uint64_t group_size = (uint64_t)sleb_read(&r);
    const uint64_t flags = (uint64_t)sleb_read(&r);
    if (r.error) {
      free(rels);
      return ANDROID_RELOC_TRUNCATED;
    }
    if (group_size == 0 || group_size > total - produced) {
      free(rels);
      return ANDROID_RELOC_BAD_FORMAT;
    }

    uint64_t group_offset_delta = 0;
    if (flags & GROUPED_BY_OFFSET_DELTA)
      group_offset_delta = (uint64_t)sleb_read(&r);
    if (flags & GROUPED_BY_INFO)
      cur.r_info = (Elf64_Xword)sleb_read(&r);

    const uint64_t addend_mode = flags & (GROUP_HAS_ADDEND | GROUPED_BY_ADDEND);
    if (addend_mode == GROUP_HAS_ADDEND) {
      // addend comes with every relocation
    } else if (addend_mode == (GROUP_HAS_ADDEND | GROUPED_BY_ADDEND)) {
      cur.r_addend += (Elf64_Sxword)sleb_read(&r);
    } else if (addend_mode == 0) {
      cur.r_addend = 0;
    } else {
      free(rels);
      return ANDROID_RELOC_BAD_FORMAT;
    }

    if (r.error) {
      free(rels);
      return ANDROID_RELOC_TRUNCATED;
    }

    for (uint64_t i = 0; i < group_size; i++) {
      if (flags & GROUPED_BY_OFFSET_DELTA)
        cur.r_offset += (Elf64_Addr)group_offset_delta;
      else
        cur.r_offset += (Elf64_Addr)sleb_read(&r);

      if (!(flags & GROUPED_BY_INFO))
        cur.r_info = (Elf64_Xword)sleb_read(&r);

      if (addend_mode == GROUP_HAS_ADDEND)
        cur.r_addend += (Elf64_Sxword)sleb_read(&r);

      if (r.error) {
        free(rels);
        return ANDROID_RELOC_TRUNCATED;
      }

      rels[produced + i] = cur;
    }
    produced += group_size;
  }

  *out = rels;
  *count = (size_t)total;
  if (consumed)
    *consumed = (size_t)(r.p - data);
  return ANDROID_RELOC_OK;
}

const char *android_reloc_strerror(int code) {
  switch (code) {
    case ANDROID_RELOC_OK:         return "ok";
    case ANDROID_RELOC_BAD_MAGIC:  return "not an APS2 packed relocation table";
    case ANDROID_RELOC_TRUNCATED:  return "packed relocation table is truncated";
    case ANDROID_RELOC_BAD_FORMAT: return "packed relocation table is malformed";
    case ANDROID_RELOC_NO_MEMORY:  return "out of memory";
    case ANDROID_RELOC_TOO_MANY:   return "implausible relocation count";
    default:                       return "unknown error";
  }
}
