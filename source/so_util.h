/* so_util.h -- utils to load and hook .so modules
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __SO_UTIL_H__
#define __SO_UTIL_H__

#include <stdint.h>
#include <stddef.h>
#include <elf.h>

#define ALIGN_MEM(x, align) (((x) + ((align) - 1)) & ~((align) - 1))

#define SO_MAX_SEGMENTS 8

// Static TLS emulation (see so_util.c). Android modules with a PT_TLS segment
// get a slice of every thread's fake bionic TLS block (libc_shim.c:
// fake_tls_install, 0x1000 bytes with TPIDR_EL0 in the middle). Slices are
// handed out at TPIDR_EL0 + [SO_TLS_BASE_OFF, SO_TLS_LIMIT_OFF).
#define SO_TLS_BASE_OFF  0x100
#define SO_TLS_LIMIT_OFF 0x7c0

typedef struct {
  char *symbol;
  uintptr_t func;
} DynLibFunction;

typedef struct so_module {
  struct so_module *next;
  char name[64];

  // entire LOAD zone
  void *load_base, *load_virtbase;
  size_t load_size;
  void *load_memrv; // VirtmemReservation *

  // copy of the unmodified program headers (link-time vaddrs),
  // needed for dl_iterate_phdr / exception unwinding
  Elf64_Phdr phdr[SO_MAX_SEGMENTS * 2];
  int phnum;

  // temporary file image
  void *so_base;
  size_t so_size;

  Elf64_Ehdr *elf_hdr;
  Elf64_Phdr *prog_hdr;
  Elf64_Shdr *sec_hdr;
  Elf64_Sym *syms;
  int num_syms;
  char *shstrtab;
  char *dynstrtab;

  // .init_array location (runtime address), captured at load time so
  // initializers can run after the temp file image is freed
  void (**init_array)(void);
  int num_init;

  // PT_TLS (Android modules built with thread_local); offsets are relative to
  // TPIDR_EL0. tls_vaddr is the link-time address of the initial image.
  uint64_t tls_vaddr, tls_filesz, tls_memsz, tls_align;
  uint64_t tls_offset;
  int finalized;
} so_module;

void hook_arm64(uintptr_t addr, uintptr_t dst);

void so_flush_caches(so_module *mod);
void so_free_temp(so_module *mod);
int so_load(so_module *mod, const char *filename, void *base, size_t max_size);
int so_relocate(so_module *mod);
int so_resolve(so_module *mod, DynLibFunction *funcs, int num_funcs, int taint_missing_imports);
void so_execute_init_array(so_module *mod);
// copies every module's TLS initial image into a thread's fake TLS block;
// `tp` is the value that thread's TPIDR_EL0 holds. No-op until the modules
// are finalized, so it is safe to call from fake_tls_install at any time.
void so_tls_init_thread(void *tp);
// pre-finalize only: address inside the RW mirror, for hooking/patching
uintptr_t so_find_addr(so_module *mod, const char *symbol);
// pre-finalize only: also searches the full, non-dynamic symbol table and
// returns the symbol size when available
uintptr_t so_find_addr_with_size(so_module *mod, const char *symbol, size_t *symbol_size);
uintptr_t so_find_addr_rx(so_module *mod, const char *symbol);
// returns 0 instead of aborting when the symbol is missing
uintptr_t so_try_find_addr_rx(so_module *mod, const char *symbol);
// exported (defined, non-local) symbols only; the dlsym() backend
uintptr_t so_lookup_export(so_module *mod, const char *name);
DynLibFunction *so_find_import(DynLibFunction *funcs, int num_funcs, const char *name);

// Optional second import table, consulted right after the table handed to
// so_resolve (so it can override module exports too). Used by the GDroid
// profile to add its extra libc/Android/OpenXR surface without touching the
// stock HL2 table.
void so_set_extra_imports(DynLibFunction *funcs, int num_funcs);
DynLibFunction *so_find_extra_import(const char *name);

// Optional fallback for imports that resolve nowhere: return an address to
// bind instead (0 = still unresolved). Lets the host bind logging stubs.
typedef uintptr_t (*so_missing_import_cb)(const char *name);
void so_set_missing_import_handler(so_missing_import_cb cb);
void so_finalize(so_module *mod);
int so_unload(so_module *mod);

// loaded module list access for the dl* emulation
so_module *so_first(void);
so_module *so_find_module(const char *basename);
int so_is_module(const void *handle);
// search every loaded module in load order; for dlsym(RTLD_DEFAULT)
uintptr_t so_lookup_export_all(const char *name);

// dl_iterate_phdr() replacement operating on all loaded modules
int so_dl_iterate_phdr(int (*callback)(void *info, size_t size, void *data), void *data);

#endif
