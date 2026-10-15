/* so_util.c -- utils to load and hook .so modules
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * HL2: loads the 26 Source engine modules side by side, an arbitrary number
 * of PT_LOAD segments, RELR/ANDROID_RELR packed relocations and cross-module
 * symbol resolution (CreateInterface plumbing relies on it).
 */

#include <switch.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <elf.h>

#include "config.h"
#include "so_util.h"
#include "android_reloc.h"
#include "util.h"
#include "error.h"

// not in devkitA64's elf.h
#ifndef DT_RELR
#define DT_RELR    36
#define DT_RELRSZ  35
#define DT_RELRENT 37
#endif
#ifndef R_AARCH64_TLS_TPREL64
#define R_AARCH64_TLS_TPREL64 1030
#endif
#ifndef R_AARCH64_TLSDESC
#define R_AARCH64_TLSDESC 1031
#endif
#define DT_ANDROID_RELR    0x6fffe000
#define DT_ANDROID_RELRSZ  0x6fffe001
#define DT_ANDROID_RELRENT 0x6fffe003

static so_module *so_list = NULL;

// next free byte (relative to TPIDR_EL0) in the per-thread static TLS area
static uint64_t so_tls_next = SO_TLS_BASE_OFF;

static DynLibFunction *extra_funcs = NULL;
static int num_extra_funcs = 0;
static so_missing_import_cb missing_cb = NULL;

void so_set_extra_imports(DynLibFunction *funcs, int num_funcs) {
  extra_funcs = funcs;
  num_extra_funcs = num_funcs;
}

DynLibFunction *so_find_extra_import(const char *name) {
  for (int i = 0; i < num_extra_funcs; i++)
    if (strcmp(extra_funcs[i].symbol, name) == 0)
      return &extra_funcs[i];
  return NULL;
}

void so_set_missing_import_handler(so_missing_import_cb cb) {
  missing_cb = cb;
}

// TLSDESC resolver for static TLS: the descriptor's second word already holds
// the variable's offset from the thread pointer, so just return it. Per the
// AArch64 TLSDESC ABI only x0 (and flags) may be clobbered.
#if defined(__aarch64__)
extern void so_tlsdesc_static_resolver(void);
__asm__(
  ".text\n"
  ".global so_tlsdesc_static_resolver\n"
  ".type so_tlsdesc_static_resolver, %function\n"
  "so_tlsdesc_static_resolver:\n"
  "  ldr x0, [x0, #8]\n"
  "  ret\n"
);
#else
// host-side unit tests only: the address is stored but never called
static void so_tlsdesc_static_resolver(void) {}
#endif

so_module *so_first(void) {
  return so_list;
}

so_module *so_find_module(const char *basename) {
  for (so_module *m = so_list; m; m = m->next) {
    if (strcmp(m->name, basename) == 0)
      return m;
  }
  return NULL;
}

int so_is_module(const void *handle) {
  for (so_module *m = so_list; m; m = m->next)
    if ((const void *)m == handle)
      return 1;
  return 0;
}

void hook_arm64(uintptr_t addr, uintptr_t dst) {
  if (addr == 0)
    return;
  uint32_t *hook = (uint32_t *)addr;
  hook[0] = 0x58000051u; // LDR X17, #0x8
  hook[1] = 0xd61f0220u; // BR X17
  *(uint64_t *)(hook + 2) = dst;
}

void so_flush_caches(so_module *mod) {
  armDCacheFlush(mod->load_virtbase, mod->load_size);
  armICacheInvalidate(mod->load_virtbase, mod->load_size);
}

void so_free_temp(so_module *mod) {
  free(mod->so_base);
  mod->so_base = NULL;
}

void so_finalize(so_module *mod) {
  Result rc = 0;

  // map the entire thing as code memory
  rc = svcMapProcessCodeMemory(envGetOwnProcessHandle(), (u64)mod->load_virtbase, (u64)mod->load_base, mod->load_size);
  if (R_FAILED(rc)) fatal_error("Error: svcMapProcessCodeMemory failed:\n%08x", rc);

  // the RW mirror at load_base is gone now; rebase the symbol table pointers
  // to the live mapping so runtime dlsym() keeps working
  mod->syms = (Elf64_Sym *)((uintptr_t)mod->syms - (uintptr_t)mod->load_base + (uintptr_t)mod->load_virtbase);
  mod->dynstrtab = (char *)((uintptr_t)mod->dynstrtab - (uintptr_t)mod->load_base + (uintptr_t)mod->load_virtbase);

  // the kernel never allows W -> X transitions on code memory, so the RX
  // segments must be set first, straight from the freshly-mapped state;
  // everything else (data segments AND the alignment gaps lld leaves
  // between segments) becomes RW afterwards. build a page map to emit
  // each permission exactly once per page.
  const size_t num_pages = mod->load_size / 0x1000;
  uint8_t *is_x_page = calloc(num_pages, 1);
  if (!is_x_page) fatal_error("Error: out of memory in so_finalize");

  for (int i = 0; i < mod->phnum; i++) {
    const Elf64_Phdr *p = &mod->phdr[i];
    if (p->p_type != PT_LOAD || (p->p_flags & PF_X) != PF_X)
      continue;
    const size_t first = p->p_vaddr / 0x1000;
    const size_t last = (ALIGN_MEM(p->p_vaddr + p->p_memsz, 0x1000) / 0x1000) - 1;
    for (size_t pg = first; pg <= last && pg < num_pages; pg++)
      is_x_page[pg] = 1;
  }

  // pass 0 sets the executable runs to RX, pass 1 sets the rest to RW
  for (int want_x = 1; want_x >= 0; want_x--) {
    size_t pg = 0;
    while (pg < num_pages) {
      if (is_x_page[pg] != want_x) {
        pg++;
        continue;
      }
      size_t run_end = pg;
      while (run_end < num_pages && is_x_page[run_end] == want_x)
        run_end++;
      const u64 addr = (u64)mod->load_virtbase + pg * 0x1000;
      const u64 size = (run_end - pg) * 0x1000;
      rc = svcSetProcessMemoryPermission(envGetOwnProcessHandle(), addr, size, want_x ? Perm_Rx : Perm_Rw);
      if (R_FAILED(rc)) fatal_error("Error: could not map %u bytes of %s memory at %p:\n%08x", (u32)size, want_x ? "RX" : "RW", (void *)addr, rc);
      pg = run_end;
    }
  }

  free(is_x_page);
  mod->finalized = 1;
}

int so_load(so_module *mod, const char *filename, void *base, size_t max_size) {
  int res = 0;

  memset(mod, 0, sizeof(*mod));
  // store the basename so dlopen("/abs/path/libengine.so") can match
  const char *bn = strrchr(filename, '/');
  strncpy(mod->name, bn ? bn + 1 : filename, sizeof(mod->name) - 1);

  FILE *fd = fopen(filename, "rb");
  if (fd == NULL)
    return -1;

  fseek(fd, 0, SEEK_END);
  mod->so_size = ftell(fd);
  fseek(fd, 0, SEEK_SET);

  mod->so_base = malloc(mod->so_size);
  if (!mod->so_base) {
    fclose(fd);
    return -2;
  }

  fread(mod->so_base, mod->so_size, 1, fd);
  fclose(fd);

  if (memcmp(mod->so_base, ELFMAG, SELFMAG) != 0) {
    res = -1;
    goto err_free_so;
  }

  mod->elf_hdr = (Elf64_Ehdr *)mod->so_base;
  mod->prog_hdr = (Elf64_Phdr *)((uintptr_t)mod->so_base + mod->elf_hdr->e_phoff);
  mod->sec_hdr = (Elf64_Shdr *)((uintptr_t)mod->so_base + mod->elf_hdr->e_shoff);
  mod->shstrtab = (char *)((uintptr_t)mod->so_base + mod->sec_hdr[mod->elf_hdr->e_shstrndx].sh_offset);

  if (mod->elf_hdr->e_phnum > SO_MAX_SEGMENTS * 2) {
    debugPrintf("so_load: %s has too many program headers (%d)\n", filename, mod->elf_hdr->e_phnum);
    res = -4;
    goto err_free_so;
  }

  // save a pristine copy of the phdrs for dl_iterate_phdr
  mod->phnum = mod->elf_hdr->e_phnum;
  memcpy(mod->phdr, mod->prog_hdr, mod->phnum * sizeof(Elf64_Phdr));

  // total size of the LOAD zone = highest vaddr+memsz of all LOAD segments
  mod->load_size = 0;
  for (int i = 0; i < mod->elf_hdr->e_phnum; i++) {
    if (mod->prog_hdr[i].p_type == PT_LOAD) {
      const size_t seg_end = mod->prog_hdr[i].p_vaddr + mod->prog_hdr[i].p_memsz;
      if (seg_end > mod->load_size)
        mod->load_size = seg_end;
    }
  }

  // align total size to page size
  mod->load_size = ALIGN_MEM(mod->load_size, 0x1000);
  if (mod->load_size > max_size) {
    res = -3;
    goto err_free_so;
  }

  // thread-local storage segment (pristine phdrs: link-time addresses)
  for (int i = 0; i < mod->phnum; i++) {
    if (mod->phdr[i].p_type == PT_TLS) {
      uint64_t align = mod->phdr[i].p_align ? mod->phdr[i].p_align : 1;
      if (align < 16) align = 16;
      const uint64_t off = ALIGN_MEM(so_tls_next, align);
      if (off + mod->phdr[i].p_memsz > SO_TLS_LIMIT_OFF) {
        debugPrintf("so_load: %s needs too much static TLS (%u bytes)\n", filename, (u32)mod->phdr[i].p_memsz);
        res = -5;
        goto err_free_so;
      }
      mod->tls_vaddr = mod->phdr[i].p_vaddr;
      mod->tls_filesz = mod->phdr[i].p_filesz;
      mod->tls_memsz = mod->phdr[i].p_memsz;
      mod->tls_align = align;
      mod->tls_offset = off;
      so_tls_next = off + mod->phdr[i].p_memsz;
      break;
    }
  }

  mod->load_base = base;
  if (!mod->load_base) goto err_free_so;
  memset(mod->load_base, 0, mod->load_size);

  // reserve virtual memory space for the entire LOAD zone
  virtmemLock();
  mod->load_virtbase = virtmemFindCodeMemory(mod->load_size, 0x1000);
  mod->load_memrv = virtmemAddReservation(mod->load_virtbase, mod->load_size);
  virtmemUnlock();

  debugPrintf("%s: load base = %p, size = %u KB\n", mod->name, mod->load_virtbase, (u32)(mod->load_size / 1024));

  // copy segments to where they belong, then fix up the runtime phdrs to
  // point into the virtual address space
  for (int i = 0; i < mod->elf_hdr->e_phnum; i++) {
    Elf64_Phdr *p = &mod->prog_hdr[i];
    if (p->p_type == PT_LOAD) {
      memcpy((void *)((uintptr_t)mod->load_base + p->p_vaddr),
             (void *)((uintptr_t)mod->so_base + p->p_offset),
             p->p_filesz);
    }
    p->p_vaddr += (Elf64_Addr)mod->load_virtbase;
  }

  mod->syms = NULL;
  mod->dynstrtab = NULL;

  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".dynsym") == 0) {
      mod->syms = (Elf64_Sym *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
      mod->num_syms = mod->sec_hdr[i].sh_size / sizeof(Elf64_Sym);
    } else if (strcmp(sh_name, ".dynstr") == 0) {
      mod->dynstrtab = (char *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
    } else if (strcmp(sh_name, ".init_array") == 0) {
      mod->init_array = (void (**)(void))((uintptr_t)mod->load_virtbase + mod->sec_hdr[i].sh_addr);
      mod->num_init = mod->sec_hdr[i].sh_size / 8;
    }
  }

  if (mod->syms == NULL || mod->dynstrtab == NULL) {
    res = -2;
    goto err_free_load;
  }

  // register in the global module list (append, load order matters for resolution)
  mod->next = NULL;
  if (!so_list) {
    so_list = mod;
  } else {
    so_module *m = so_list;
    while (m->next) m = m->next;
    m->next = mod;
  }

  return 0;

err_free_load:
  virtmemLock();
  virtmemRemoveReservation(mod->load_memrv);
  virtmemUnlock();
err_free_so:
  free(mod->so_base);
  mod->so_base = NULL;

  return res;
}

// find a dynamic tag in the file image; returns 0 if missing
static Elf64_Xword so_dynamic_tag(so_module *mod, Elf64_Sxword tag) {
  for (int i = 0; i < mod->phnum; i++) {
    if (mod->phdr[i].p_type == PT_DYNAMIC) {
      const Elf64_Dyn *dyn = (const Elf64_Dyn *)((uintptr_t)mod->so_base + mod->phdr[i].p_offset);
      for (; dyn->d_tag != DT_NULL; dyn++)
        if (dyn->d_tag == tag)
          return dyn->d_un.d_val;
    }
  }
  return 0;
}

// RELR/ANDROID_RELR packed relative relocations;
// each location gets the load base added to the value stored in the file
static void so_process_relr(so_module *mod, const Elf64_Xword *relr, size_t relrsz) {
  uintptr_t where = 0;
  const size_t count = relrsz / sizeof(Elf64_Xword);
  for (size_t i = 0; i < count; i++) {
    const Elf64_Xword entry = relr[i];
    if ((entry & 1) == 0) {
      // even entry: address of the next relocation
      where = (uintptr_t)entry;
      *(uint64_t *)((uintptr_t)mod->load_base + where) += (uint64_t)mod->load_virtbase;
      where += 8;
    } else {
      // odd entry: bitmap for the next 63 words
      for (int bit = 1; bit < 64; bit++) {
        if (entry & (1ull << bit))
          *(uint64_t *)((uintptr_t)mod->load_base + where + (bit - 1) * 8) += (uint64_t)mod->load_virtbase;
      }
      where += 63 * 8;
    }
  }
}

// Returns the relocation entries of section `idx`: a view into the load image
// for plain SHT_RELA tables, or a freshly decoded copy for Android's packed
// SHT_ANDROID_RELA tables ("APS2"). *owned != 0 means the caller must free().
static const Elf64_Rela *so_section_relocs(so_module *mod, int idx, size_t *count, int *owned) {
  const Elf64_Shdr *sh = &mod->sec_hdr[idx];
  const void *data = (const void *)((uintptr_t)mod->load_base + sh->sh_addr);

  *owned = 0;
  if (sh->sh_type == SHT_ANDROID_RELA_TYPE) {
    Elf64_Rela *rels = NULL;
    const int rc = android_reloc_unpack((const uint8_t *)data, sh->sh_size, &rels, count, NULL);
    if (rc != ANDROID_RELOC_OK)
      fatal_error("Error: %s: cannot read packed relocations:\n%s", mod->name, android_reloc_strerror(rc));
    *owned = 1;
    return rels;
  }

  *count = sh->sh_size / sizeof(Elf64_Rela);
  return (const Elf64_Rela *)data;
}

// offset of a TLS variable from TPIDR_EL0 for relocation `rel` of module `mod`
static uint64_t so_tls_value(so_module *mod, const Elf64_Sym *sym, const Elf64_Rela *rel) {
  if (mod->tls_memsz == 0)
    fatal_error("Error: %s uses TLS relocations\nbut has no PT_TLS segment", mod->name);
  uint64_t value = mod->tls_offset + (uint64_t)rel->r_addend;
  if (ELF64_R_SYM(rel->r_info) != 0) {
    if (sym->st_shndx == SHN_UNDEF)
      fatal_error("Error: %s imports a TLS symbol:\n%s", mod->name, mod->dynstrtab + sym->st_name);
    value += sym->st_value; // offset inside the TLS segment
  }
  return value;
}

int so_relocate(so_module *mod) {
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rela.dyn") == 0 || strcmp(sh_name, ".rela.plt") == 0) {
      size_t num_rels = 0;
      int owned = 0;
      const Elf64_Rela *rels = so_section_relocs(mod, i, &num_rels, &owned);
      for (size_t j = 0; j < num_rels; j++) {
        uintptr_t *ptr = (uintptr_t *)((uintptr_t)mod->load_base + rels[j].r_offset);
        Elf64_Sym *sym = &mod->syms[ELF64_R_SYM(rels[j].r_info)];

        int type = ELF64_R_TYPE(rels[j].r_info);
        switch (type) {
          case R_AARCH64_ABS64:
            if (sym->st_shndx == SHN_UNDEF) {
              // Imported absolute relocations (notably RTTI/type_info vtable
              // references) must be resolved after all modules are loaded.
              *ptr = rels[j].r_addend;
            } else {
              *ptr = (uintptr_t)mod->load_virtbase + sym->st_value + rels[j].r_addend;
            }
            break;

          case R_AARCH64_RELATIVE:
            // sometimes the value of r_addend is also at *ptr
            *ptr = (uintptr_t)mod->load_virtbase + rels[j].r_addend;
            break;

          case R_AARCH64_GLOB_DAT:
          case R_AARCH64_JUMP_SLOT:
          {
            if (sym->st_shndx != SHN_UNDEF)
              *ptr = (uintptr_t)mod->load_virtbase + sym->st_value + rels[j].r_addend;
            break;
          }

          case R_AARCH64_TLS_TPREL64:
            *ptr = (uintptr_t)so_tls_value(mod, sym, &rels[j]);
            break;

          case R_AARCH64_TLSDESC:
            // descriptor = { resolver, argument }; static TLS argument is the
            // variable's offset from the thread pointer
            ptr[0] = (uintptr_t)&so_tlsdesc_static_resolver;
            ptr[1] = (uintptr_t)so_tls_value(mod, sym, &rels[j]);
            break;

          default:
            fatal_error("Error: unknown relocation type:\n%u in %s", (unsigned)type, mod->name);
            break;
        }
      }
      if (owned)
        free((void *)rels);
    }
  }

  // packed relative relocations, if present
  Elf64_Xword relr_off = so_dynamic_tag(mod, DT_RELR);
  Elf64_Xword relr_size = so_dynamic_tag(mod, DT_RELRSZ);
  if (!relr_off) {
    relr_off = so_dynamic_tag(mod, DT_ANDROID_RELR);
    relr_size = so_dynamic_tag(mod, DT_ANDROID_RELRSZ);
  }
  if (relr_off && relr_size)
    so_process_relr(mod, (const Elf64_Xword *)((uintptr_t)mod->load_base + relr_off), relr_size);

  return 0;
}

// look up an exported (defined) symbol in a single module;
// returns its virtual (runtime) address or 0
uintptr_t so_lookup_export(so_module *mod, const char *name) {
  for (int i = 0; i < mod->num_syms; i++) {
    if (mod->syms[i].st_shndx == SHN_UNDEF)
      continue;
    if (ELF64_ST_BIND(mod->syms[i].st_info) == STB_LOCAL)
      continue;
    const char *sname = mod->dynstrtab + mod->syms[i].st_name;
    if (sname[0] == name[0] && strcmp(sname, name) == 0)
      return (uintptr_t)mod->load_virtbase + mod->syms[i].st_value;
  }
  return 0;
}

uintptr_t so_lookup_export_all(const char *name) {
  for (so_module *m = so_list; m; m = m->next) {
    const uintptr_t addr = so_lookup_export(m, name);
    if (addr)
      return addr;
  }
  return 0;
}

static uintptr_t so_resolve_symbol(so_module *mod, DynLibFunction *funcs, int num_funcs, const char *name) {
  // The import table takes priority over module exports so the host can
  // override anything (e.g. SDL_*, libc); everything else falls through to
  // the other Source modules' exports (CreateInterface, tier0 API, ...).
  // the extra table (GMod profile only) overrides the stock one
  {
    DynLibFunction *extra = so_find_extra_import(name);
    if (extra)
      return extra->func;
  }

  for (int k = 0; k < num_funcs; k++) {
    if (strcmp(name, funcs[k].symbol) == 0)
      return funcs[k].func;
  }

  for (so_module *m = so_list; m; m = m->next) {
    if (m == mod)
      continue;
    const uintptr_t addr = so_lookup_export(m, name);
    if (addr)
      return addr;
  }

  return 0;
}

int so_resolve(so_module *mod, DynLibFunction *funcs, int num_funcs, int taint_missing_imports) {
  int missing = 0;
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rela.dyn") == 0 || strcmp(sh_name, ".rela.plt") == 0) {
      size_t num_rels = 0;
      int owned = 0;
      const Elf64_Rela *rels = so_section_relocs(mod, i, &num_rels, &owned);
      for (size_t j = 0; j < num_rels; j++) {
        uintptr_t *ptr = (uintptr_t *)((uintptr_t)mod->load_base + rels[j].r_offset);
        Elf64_Sym *sym = &mod->syms[ELF64_R_SYM(rels[j].r_info)];

        int type = ELF64_R_TYPE(rels[j].r_info);
        switch (type) {
          case R_AARCH64_ABS64:
          case R_AARCH64_GLOB_DAT:
          case R_AARCH64_JUMP_SLOT:
          {
            if (sym->st_shndx == SHN_UNDEF) {
              char *name = mod->dynstrtab + sym->st_name;
              uintptr_t addr = so_resolve_symbol(mod, funcs, num_funcs, name);
              if (!addr) {
                missing++;
                debugPrintf("%s: unresolved import: %s\n", mod->name, name);
                if (missing_cb)
                  addr = missing_cb(name);
              }
              if (addr) {
                *ptr = addr + rels[j].r_addend;
              } else if (taint_missing_imports) {
                // taint so an unresolved call crashes at an obvious address
                *ptr = rels[j].r_offset;
              }
            }

            break;
          }

          default:
            break;
        }
      }
      if (owned)
        free((void *)rels);
    }
  }

  if (missing)
    debugPrintf("%s: %d unresolved imports\n", mod->name, missing);

  return 0;
}

void so_execute_init_array(so_module *mod) {
  // uses the location captured at load time: safe to run after so_free_temp
  for (int i = 0; i < mod->num_init; i++) {
    if (mod->init_array[i])
      mod->init_array[i]();
  }
}

void so_tls_init_thread(void *tp) {
  if (!tp)
    return;
  for (so_module *m = so_list; m; m = m->next) {
    if (!m->finalized || m->tls_filesz == 0)
      continue;
    memcpy((uint8_t *)tp + m->tls_offset,
           (const void *)((uintptr_t)m->load_virtbase + m->tls_vaddr),
           m->tls_filesz);
  }
}

// pre-finalize only: address inside the RW mirror, for hooking/patching
uintptr_t so_find_addr(so_module *mod, const char *symbol) {
  for (int i = 0; i < mod->num_syms; i++) {
    char *name = mod->dynstrtab + mod->syms[i].st_name;
    if (strcmp(name, symbol) == 0)
      return (uintptr_t)mod->load_base + mod->syms[i].st_value;
  }

  fatal_error("Error: could not find symbol:\n%s", symbol);
  return 0;
}

uintptr_t so_find_addr_with_size(so_module *mod, const char *symbol, size_t *symbol_size) {
  if (symbol_size)
    *symbol_size = 0;

  for (int i = 0; i < mod->num_syms; i++) {
    const char *name = mod->dynstrtab + mod->syms[i].st_name;
    if (strcmp(name, symbol) == 0) {
      if (symbol_size)
        *symbol_size = mod->syms[i].st_size;
      return (uintptr_t)mod->load_base + mod->syms[i].st_value;
    }
  }

  // Android's GameUI keeps useful local function names in .symtab, but they
  // are not part of .dynsym and therefore are not present in the load image.
  // Consult the temporary ELF file image while it is still available.
  if (!mod->so_base)
    return 0;

  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    const Elf64_Shdr *symtab = &mod->sec_hdr[i];
    if (symtab->sh_type != SHT_SYMTAB ||
        symtab->sh_link >= mod->elf_hdr->e_shnum ||
        symtab->sh_entsize != sizeof(Elf64_Sym) ||
        symtab->sh_offset + symtab->sh_size > mod->so_size)
      continue;

    const Elf64_Shdr *strtab = &mod->sec_hdr[symtab->sh_link];
    if (strtab->sh_type != SHT_STRTAB ||
        strtab->sh_offset + strtab->sh_size > mod->so_size)
      continue;

    const Elf64_Sym *symbols =
        (const Elf64_Sym *)((uintptr_t)mod->so_base + symtab->sh_offset);
    const char *strings = (const char *)mod->so_base + strtab->sh_offset;
    const size_t count = symtab->sh_size / sizeof(*symbols);

    for (size_t j = 0; j < count; j++) {
      if (symbols[j].st_shndx == SHN_UNDEF ||
          symbols[j].st_name >= strtab->sh_size ||
          symbols[j].st_value >= mod->load_size ||
          symbols[j].st_size > mod->load_size - symbols[j].st_value)
        continue;
      if (strcmp(strings + symbols[j].st_name, symbol) == 0) {
        if (symbol_size)
          *symbol_size = symbols[j].st_size;
        return (uintptr_t)mod->load_base + symbols[j].st_value;
      }
    }
  }

  return 0;
}

uintptr_t so_find_addr_rx(so_module *mod, const char *symbol) {
  const uintptr_t addr = so_try_find_addr_rx(mod, symbol);
  if (!addr)
    fatal_error("Error: could not find symbol:\n%s", symbol);
  return addr;
}

uintptr_t so_try_find_addr_rx(so_module *mod, const char *symbol) {
  for (int i = 0; i < mod->num_syms; i++) {
    char *name = mod->dynstrtab + mod->syms[i].st_name;
    if (strcmp(name, symbol) == 0)
      return (uintptr_t)mod->load_virtbase + mod->syms[i].st_value;
  }
  return 0;
}

DynLibFunction *so_find_import(DynLibFunction *funcs, int num_funcs, const char *name) {
  for (int i = 0; i < num_funcs; ++i)
    if (!strcmp(funcs[i].symbol, name))
      return &funcs[i];
  return NULL;
}

int so_unload(so_module *mod) {
  if (mod->load_base == NULL)
    return -1;

  if (mod->so_base) {
    // someone forgot to free the temp data
    so_free_temp(mod);
  }

  // remap everything as RW
  for (int i = 0; i < mod->phnum; i++) {
    const Elf64_Phdr *p = &mod->phdr[i];
    if (p->p_type != PT_LOAD || !(p->p_flags & PF_X))
      continue;
    const u64 seg_start = ((u64)mod->load_virtbase + p->p_vaddr) & ~0xFFFull;
    const u64 seg_end = ALIGN_MEM((u64)mod->load_virtbase + p->p_vaddr + p->p_memsz, 0x1000);
    svcSetProcessMemoryPermission(envGetOwnProcessHandle(), seg_start, seg_end - seg_start, Perm_Rw);
  }
  // unmap everything
  svcUnmapProcessCodeMemory(envGetOwnProcessHandle(), (u64)mod->load_virtbase, (u64)mod->load_base, mod->load_size);

  // release virtual address range
  virtmemLock();
  virtmemRemoveReservation(mod->load_memrv);
  virtmemUnlock();

  // remove from list
  if (so_list == mod) {
    so_list = mod->next;
  } else {
    for (so_module *m = so_list; m; m = m->next) {
      if (m->next == mod) {
        m->next = mod->next;
        break;
      }
    }
  }

  return 0;
}

// matches the layout bionic/libunwind expect
struct so_dl_phdr_info {
  Elf64_Addr dlpi_addr;
  const char *dlpi_name;
  const Elf64_Phdr *dlpi_phdr;
  Elf64_Half dlpi_phnum;
};

int so_dl_iterate_phdr(int (*callback)(void *info, size_t size, void *data), void *data) {
  int ret = 0;
  for (so_module *mod = so_list; mod; mod = mod->next) {
    struct so_dl_phdr_info info;
    info.dlpi_addr = (Elf64_Addr)mod->load_virtbase;
    info.dlpi_name = mod->name;
    info.dlpi_phdr = mod->phdr; // link-time vaddrs + dlpi_addr = runtime
    info.dlpi_phnum = mod->phnum;
    ret = callback(&info, sizeof(info), data);
    if (ret)
      break;
  }
  return ret;
}
