/* crash_dump.c -- write a crash report (registers + module/offset) to
 * crash_dump.txt in the install folder when the game faults.
 *
 * Without this, a data/instruction abort in the loaded Android modules just
 * shows Horizon's "an error has occurred" screen and leaves no trace in
 * debug.log. libnx calls __libnx_exception_handler() on the faulting thread;
 * we write the report and return, so the process still ends the same way.
 *
 * The handler's own stack is tiny, so the real work runs on a private 64 KB
 * stack. Everything is formatted by hand (no stdio) and written with write().
 *
 * Send crash_dump.txt back together with debug.log: PC/LR are printed as
 * "libname.so+0xOFFSET", which can be looked up directly in the .so files.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <switch.h>

#include "so_util.h"

#define CRASH_FILE "crash_dump.txt"
#define MAX_FRAMES 24

static uint8_t crash_stack[0x10000] __attribute__((aligned(16)));
static volatile int crash_in_progress = 0;
static int crash_fd = -1;

static void out_str(const char *s) {
  if (crash_fd >= 0)
    write(crash_fd, s, strlen(s));
}

static void out_hex(uint64_t v) {
  char buf[19];
  buf[0] = '0';
  buf[1] = 'x';
  for (int i = 0; i < 16; i++)
    buf[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 0xf];
  buf[18] = 0;
  if (crash_fd >= 0)
    write(crash_fd, buf, 18);
}

static void out_kv(const char *k, uint64_t v) {
  out_str(k);
  out_str(" = ");
  out_hex(v);
  out_str("\n");
}

// true if [addr, addr+len) is mapped readable (so walking a bad frame
// pointer cannot fault again inside the handler)
static int readable(uint64_t addr, uint64_t len) {
  if (addr < 0x1000 || (addr & 7))
    return 0;
  MemoryInfo mi;
  u32 page_info;
  if (R_FAILED(svcQueryMemory(&mi, &page_info, addr)))
    return 0;
  if (!(mi.perm & Perm_R) || mi.type == MemType_Unmapped)
    return 0;
  return addr + len <= mi.addr + mi.size;
}

// "libfoo.so+0x1234" for an address inside a loaded module, else nothing
static void out_where(uint64_t addr) {
  for (so_module *m = so_first(); m; m = m->next) {
    uint64_t base = (uint64_t)m->load_virtbase;
    if (base && addr >= base && addr < base + m->load_size) {
      out_str("  ");
      out_str(m->name);
      out_str("+");
      out_hex(addr - base);
      return;
    }
  }
}

static void out_addr_line(const char *label, uint64_t v) {
  out_str(label);
  out_str(" = ");
  out_hex(v);
  out_where(v);
  out_str("\n");
}

static void dump_report(ThreadExceptionDump *ctx) {
  crash_fd = open(CRASH_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (crash_fd < 0)
    return;

  out_str("hl2_nx crash report\n");
  out_kv("error_desc", (uint64_t)ctx->error_desc);
  // Horizon exception types (libnx ThreadExceptionDesc)
  out_str("  (0x100 instruction abort, 0x101 data abort, 0x102 alignment fault;\n"
          "   others: see ThreadExceptionDesc in libnx)\n");
  // load address of hl2_nx.nro itself, to resolve offsets with nm on hl2_nx.elf
  // (weak: prints 0 instead of failing to link if the linker script lacks it)
  extern char __start__ __attribute__((weak));
  out_kv("hl2_nx.nro base", (uint64_t)(uintptr_t)&__start__);
  out_addr_line("pc", ctx->pc.x);
  out_addr_line("lr", ctx->lr.x);
  out_kv("sp", ctx->sp.x);
  out_kv("fp", ctx->fp.x);
  out_kv("far (fault address)", ctx->far.x);
  out_kv("esr", ctx->esr);
  out_kv("pstate", ctx->pstate);
  out_kv("TPIDR_EL0", ({ uint64_t t; __asm__ volatile("mrs %0, s3_3_c13_c0_2" : "=r"(t)); t; }));

  out_str("\nmodules (virtual base, size):\n");
  for (so_module *m = so_first(); m; m = m->next) {
    out_str("  ");
    out_str(m->name);
    out_str(" ");
    out_hex((uint64_t)m->load_virtbase);
    out_str(" ");
    out_hex((uint64_t)m->load_size);
    out_str("\n");
  }

  out_str("\nregisters:\n");
  for (int i = 0; i < 29; i++) {
    char name[8] = { 'x', 0, 0, 0 };
    if (i < 10) { name[1] = '0' + i; }
    else { name[1] = '0' + i / 10; name[2] = '0' + i % 10; }
    out_addr_line(name, ctx->cpu_gprs[i].x);
  }

  out_str("\nbacktrace (frame-pointer chain):\n");
  out_addr_line("#0 pc", ctx->pc.x);
  out_addr_line("#1 lr", ctx->lr.x);
  uint64_t fp = ctx->fp.x;
  for (int i = 2; i < MAX_FRAMES; i++) {
    if (!readable(fp, 16))
      break;
    uint64_t next = ((uint64_t *)fp)[0];
    uint64_t ret = ((uint64_t *)fp)[1];
    char name[8] = { '#', 0, 0, 0 };
    name[1] = '0' + i / 10;
    name[2] = '0' + i % 10;
    out_addr_line(name, ret);
    if (next <= fp)
      break;
    fp = next;
  }

  out_str("\nstack at sp (first 32 qwords):\n");
  uint64_t sp = ctx->sp.x;
  for (int i = 0; i < 32; i++) {
    if (!readable(sp + 8 * i, 8))
      break;
    out_addr_line("  [sp+..]", ((uint64_t *)sp)[i]);
  }

  close(crash_fd);
  crash_fd = -1;
}

static void __attribute__((used)) crash_worker(ThreadExceptionDump *ctx) {
  dump_report(ctx);
}

// called by libnx on the faulting thread
void __libnx_exception_handler(ThreadExceptionDump *ctx) {
  if (crash_in_progress)
    return; // fault inside the handler: give up quietly
  crash_in_progress = 1;

  // run crash_worker(ctx) on our own stack, then switch back and return
  register uint64_t x0 __asm__("x0") = (uint64_t)ctx;
  register uint64_t x1 __asm__("x1") = (uint64_t)(crash_stack + sizeof(crash_stack));
  register uint64_t x2 __asm__("x2") = (uint64_t)&crash_worker;
  __asm__ volatile(
    "mov x19, sp\n"
    "mov sp, x1\n"
    "blr x2\n"
    "mov sp, x19\n"
    : "+r"(x0), "+r"(x1), "+r"(x2)
    :
    : "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
      "x14", "x15", "x16", "x17", "x18", "x19", "x30", "memory", "cc");
}
