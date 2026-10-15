/* gdroid_imports.c -- extra imports needed by the GDroid (Garry's Mod) modules
 *
 * The GDroid libraries are built against a newer NDK/libc++ than the stock HL2
 * ones, so they import ~230 more symbols. Most map straight onto newlib; the
 * rest are small shims. Anything not covered here lands in the logging stub
 * pool (import_stubs.c) instead of crashing.
 *
 * Conventions: bionic's locale_t variants ignore the locale (the C locale is
 * all the game ever uses); AAudio/OpenXR report "unavailable" so the engine
 * falls back to flat rendering and SDL audio.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>
#include <locale.h>
#include <unistd.h>
#include <zlib.h>

#ifdef GDROID_HOST_TEST
// host unit tests have no SDL/EGL: only the addresses are needed
void SDL_SetMainReady(void);
void SDL_StopTextInput(void);
void eglChooseConfig(void);
void eglCreateContext(void);
void eglCreatePbufferSurface(void);
void eglDestroyContext(void);
void eglDestroySurface(void);
void eglGetCurrentContext(void);
void eglGetDisplay(void);
void eglGetError(void);
void eglInitialize(void);
void eglMakeCurrent(void);
void eglTerminate(void);
#else
#include <SDL2/SDL.h>
#include <EGL/egl.h>
#endif

#include "so_util.h"
#include "libc_shim.h"
#include "jni_fake.h"
#include "imports.h"
#include "import_stubs.h"
#include "util.h"
#include "gdroid_imports.h"

// ---------------------------------------------------------------- trivial

static int gd_ret0(void) { return 0; }

// mmap_fake() hands out plain heap memory, which Horizon will not execute.
// LuaJIT's JIT writes machine code there and then mprotect()s it R+X; with a
// no-op mprotect it jumped into the heap (instruction abort). Refusing PROT_EXEC
// makes LuaJIT abort that trace ("failed to allocate mcode memory") and keep
// running in its bytecode interpreter, which is all we need.
static int gd_mprotect(void *addr, size_t len, int prot) {
  (void)addr; (void)len;
  if (prot & 4) { // PROT_EXEC
    static int logged;
    if (logged++ < 3)
      debugPrintf("mprotect(PROT_EXEC) refused (JIT disabled)\n");
    errno = 12; // ENOMEM
    return -1;
  }
  return 0;
}
static int gd_retm1(void) { return -1; }
// 64-bit results (ssize_t, off_t, pointers) must fill the whole of x0
static long gd_retm1_long(void) { return -1; }

// ---------------------------------------------------------------- stdio data
// bionic >= API 23 exports stdin/stdout/stderr as FILE* variables. They point
// at the same fake FILE blocks the `__sF` import hands out, so fprintf & co.
// recognise them.

static FILE *gd_stdin = (FILE *)fake_sF[0];
static FILE *gd_stdout = (FILE *)fake_sF[1];
static FILE *gd_stderr = (FILE *)fake_sF[2];

// FILE-taking functions: fake FILEs (stdin/out/err) never reach newlib
static void clearerr_gd(FILE *f) { if (!shim_is_fake_file(f)) clearerr(f); }
static int getc_gd(FILE *f) { return shim_is_fake_file(f) ? EOF : getc(f); }
static wint_t getwc_gd(FILE *f) { return shim_is_fake_file(f) ? WEOF : getwc(f); }
static wint_t fputwc_gd(wchar_t c, FILE *f) { return shim_is_fake_file(f) ? (wint_t)c : fputwc(c, f); }
static wint_t ungetwc_gd(wint_t c, FILE *f) { return shim_is_fake_file(f) ? WEOF : ungetwc(c, f); }

static int vprintf_gd(const char *fmt, va_list ap) {
  char buf[1024];
  const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  debugPrintf("%s", buf);
  return n;
}

static int vasprintf_gd(char **out, const char *fmt, va_list ap) {
  va_list ap2;
  va_copy(ap2, ap);
  const int n = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);
  if (n < 0) { *out = NULL; return -1; }
  char *s = malloc((size_t)n + 1);
  if (!s) { *out = NULL; return -1; }
  vsnprintf(s, (size_t)n + 1, fmt, ap);
  *out = s;
  return n;
}

// ---------------------------------------------------------------- locale

static int dummy_locale;  // only its address matters

static void *newlocale_gd(int mask, const char *name, void *base) {
  (void)mask; (void)name;
  return base ? base : (void *)&dummy_locale;
}
static void freelocale_gd(void *loc) { (void)loc; }
static void *uselocale_gd(void *loc) { (void)loc; return (void *)&dummy_locale; }

#define WCTYPE_L(name) static int name##_l_gd(wint_t c, void *loc) { (void)loc; return name(c); }
WCTYPE_L(iswalpha) WCTYPE_L(iswblank) WCTYPE_L(iswcntrl) WCTYPE_L(iswdigit) WCTYPE_L(iswlower)
WCTYPE_L(iswprint) WCTYPE_L(iswpunct) WCTYPE_L(iswspace) WCTYPE_L(iswupper) WCTYPE_L(iswxdigit)
#undef WCTYPE_L

static wint_t towlower_l_gd(wint_t c, void *loc) { (void)loc; return towlower(c); }
static wint_t towupper_l_gd(wint_t c, void *loc) { (void)loc; return towupper(c); }
static int strcoll_l_gd(const char *a, const char *b, void *loc) { (void)loc; return strcoll(a, b); }
static size_t strxfrm_l_gd(char *d, const char *s, size_t n, void *loc) { (void)loc; return strxfrm(d, s, n); }
static size_t strftime_l_gd(char *s, size_t max, const char *fmt, const struct tm *tm, void *loc) { (void)loc; return strftime(s, max, fmt, tm); }
static long double strtold_l_gd(const char *s, char **end, void *loc) { (void)loc; return strtold(s, end); }
static long long strtoll_l_gd(const char *s, char **end, int base, void *loc) { (void)loc; return strtoll(s, end, base); }
static unsigned long long strtoull_l_gd(const char *s, char **end, int base, void *loc) { (void)loc; return strtoull(s, end, base); }
static int wcscoll_l_gd(const wchar_t *a, const wchar_t *b, void *loc) { (void)loc; return wcscoll(a, b); }
static size_t wcsxfrm_l_gd(wchar_t *d, const wchar_t *s, size_t n, void *loc) { (void)loc; return wcsxfrm(d, s, n); }

// ---------------------------------------------------------------- string / posix

static size_t strnlen_gd(const char *s, size_t max) {
  size_t n = 0;
  while (n < max && s[n]) n++;
  return n;
}

static void *memmem_gd(const void *hay, size_t hlen, const void *needle, size_t nlen) {
  if (nlen == 0) return (void *)hay;
  if (hlen < nlen) return NULL;
  const unsigned char *h = hay;
  for (size_t i = 0; i + nlen <= hlen; i++)
    if (h[i] == *(const unsigned char *)needle && memcmp(h + i, needle, nlen) == 0)
      return (void *)(h + i);
  return NULL;
}

// not thread-safe against other users of the same fd, which is fine here
static long pread_gd(int fd, void *buf, size_t n, off_t off) {
  const off_t cur = lseek(fd, 0, SEEK_CUR);
  if (cur < 0 || lseek(fd, off, SEEK_SET) < 0) return -1;
  const long r = (long)read(fd, buf, n);
  lseek(fd, cur, SEEK_SET);
  return r;
}

static long pwrite_gd(int fd, const void *buf, size_t n, off_t off) {
  const off_t cur = lseek(fd, 0, SEEK_CUR);
  if (cur < 0 || lseek(fd, off, SEEK_SET) < 0) return -1;
  const long r = (long)write(fd, buf, n);
  lseek(fd, cur, SEEK_SET);
  return r;
}

// bionic's _SC_* numbering (not glibc's / newlib's)
static long sysconf_gd(int name) {
  switch (name) {
    case 0x0000: return 131072;   // _SC_ARG_MAX
    case 0x0006: return 100;      // _SC_CLK_TCK
    case 0x000b: return 1024;     // _SC_OPEN_MAX
    case 0x0027:                  // _SC_PAGESIZE
    case 0x0028: return 0x1000;   // _SC_PAGE_SIZE
    case 0x0060:                  // _SC_NPROCESSORS_CONF
    case 0x0061: return 4;        // _SC_NPROCESSORS_ONLN
    case 0x0062: return 786432;   // _SC_PHYS_PAGES (3 GiB)
    case 0x0063: return 524288;   // _SC_AVPHYS_PAGES
    default:
      debugPrintf("sysconf(0x%x): unknown name\n", name);
      errno = EINVAL;
      return -1;
  }
}

static int pthread_equal_gd(void *a, void *b) { return a == b; }

// newlib/libnx expose no rmdir; unlink handles empty directories there
static int rmdir_gd(const char *path) { return unlink(path); }

// ---------------------------------------------------------------- AAudio / OpenXR
// Both are "not available" on purpose: with no AAudio stream the engine uses
// its SDL audio path, and with no OpenXR runtime libvrxr stays in flat mode.

#define AAUDIO_ERROR_UNAVAILABLE (-889)
#define XR_ERROR_RUNTIME_UNAVAILABLE (-51)

static int aaudio_unavailable(void) { return AAUDIO_ERROR_UNAVAILABLE; }
static const char *aaudio_result_text(int result) { (void)result; return "AAUDIO_ERROR_UNAVAILABLE"; }
static int xr_runtime_unavailable(void) { return XR_ERROR_RUNTIME_UNAVAILABLE; }

// ---------------------------------------------------------------- Android media
// AMEDIAFORMAT_KEY_* are `const char *` variables exported by libmediandk.
// (the AMediaCodec/AMediaExtractor functions fall through to logging stubs)

static const char *key_AMEDIAFORMAT_KEY_BIT_RATE = "bitrate";
static const char *key_AMEDIAFORMAT_KEY_CHANNEL_COUNT = "channel-count";
static const char *key_AMEDIAFORMAT_KEY_DURATION = "durationUs";
static const char *key_AMEDIAFORMAT_KEY_ENCODER_DELAY = "encoder-delay";
static const char *key_AMEDIAFORMAT_KEY_ENCODER_PADDING = "encoder-padding";
static const char *key_AMEDIAFORMAT_KEY_MIME = "mime";
static const char *key_AMEDIAFORMAT_KEY_SAMPLE_RATE = "sample-rate";

// ---------------------------------------------------------------- getenv
// The Android launcher contract is only known from strings, so record every
// environment variable the game asks for (once per name) and what it got.
// This is what tells us which variables are still missing.

#define GETENV_LOG_MAX 192
static char getenv_seen[GETENV_LOG_MAX][48];
static int getenv_seen_count = 0;

static char *getenv_gd(const char *name) {
  char *value = getenv(name);
  if (name) {
    int known = 0;
    for (int i = 0; i < getenv_seen_count; i++) {
      if (strncmp(getenv_seen[i], name, sizeof(getenv_seen[i]) - 1) == 0) {
        known = 1;
        break;
      }
    }
    if (!known && getenv_seen_count < GETENV_LOG_MAX) {
      strncpy(getenv_seen[getenv_seen_count], name, sizeof(getenv_seen[0]) - 1);
      getenv_seen[getenv_seen_count][sizeof(getenv_seen[0]) - 1] = '\0';
      getenv_seen_count++;
      debugPrintf("getenv(%s) = %s\n", name, value ? value : "(null)");
    }
  }
  return value;
}

// ---------------------------------------------------------------- SDL hooks

static void *sdl_android_get_jnienv(void) { return fake_env; }
// the launcher only checks that an activity object exists
static void *sdl_android_get_activity(void) { static int activity; return &activity; }

// ---------------------------------------------------------------- table

DynLibFunction gdroid_import_functions[] = {
  { "btowc", (uintptr_t)&btowc },
  { "clock", (uintptr_t)&clock },
  { "cosh", (uintptr_t)&cosh },
  { "difftime", (uintptr_t)&difftime },
  { "frexpf", (uintptr_t)&frexpf },
  { "hypotf", (uintptr_t)&hypotf },
  { "iswalnum", (uintptr_t)&iswalnum },
  { "iswcntrl", (uintptr_t)&iswcntrl },
  { "iswdigit", (uintptr_t)&iswdigit },
  { "iswspace", (uintptr_t)&iswspace },
  { "ldexpf", (uintptr_t)&ldexpf },
  { "localeconv", (uintptr_t)&localeconv },
  { "log2", (uintptr_t)&log2 },
  { "log2f", (uintptr_t)&log2f },
  { "mbrlen", (uintptr_t)&mbrlen },
  { "mbrtowc", (uintptr_t)&mbrtowc },
  { "mbsnrtowcs", (uintptr_t)&mbsnrtowcs },
  { "mbsrtowcs", (uintptr_t)&mbsrtowcs },
  { "mbstowcs", (uintptr_t)&mbstowcs },
  { "mbtowc", (uintptr_t)&mbtowc },
  { "mktime", (uintptr_t)&mktime },
  { "remainder", (uintptr_t)&remainder },
  { "sinh", (uintptr_t)&sinh },
  { "strtold", (uintptr_t)&strtold },
  { "tanh", (uintptr_t)&tanh },
  { "tmpfile", (uintptr_t)&tmpfile },
  { "towlower", (uintptr_t)&towlower },
  { "towupper", (uintptr_t)&towupper },
  { "vsscanf", (uintptr_t)&vsscanf },
  { "wcrtomb", (uintptr_t)&wcrtomb },
  { "wcscat", (uintptr_t)&wcscat },
  { "wcschr", (uintptr_t)&wcschr },
  { "wcscoll", (uintptr_t)&wcscoll },
  { "wcslen", (uintptr_t)&wcslen },
  { "wcsncat", (uintptr_t)&wcsncat },
  { "wcsncmp", (uintptr_t)&wcsncmp },
  { "wcsncpy", (uintptr_t)&wcsncpy },
  { "wcsnrtombs", (uintptr_t)&wcsnrtombs },
  { "wcsstr", (uintptr_t)&wcsstr },
  { "wcstod", (uintptr_t)&wcstod },
  { "wcstof", (uintptr_t)&wcstof },
  { "wcstol", (uintptr_t)&wcstol },
  { "wcstold", (uintptr_t)&wcstold },
  { "wcstoll", (uintptr_t)&wcstoll },
  { "wcstombs", (uintptr_t)&wcstombs },
  { "wcstoul", (uintptr_t)&wcstoul },
  { "wcstoull", (uintptr_t)&wcstoull },
  { "wctob", (uintptr_t)&wctob },
  { "wmemchr", (uintptr_t)&wmemchr },
  { "wmemcmp", (uintptr_t)&wmemcmp },
  { "compress2", (uintptr_t)&compress2 },
  { "compressBound", (uintptr_t)&compressBound },
  { "uncompress", (uintptr_t)&uncompress },
  { "fchmod", (uintptr_t)&gd_ret0 },
  { "fchmodat", (uintptr_t)&gd_ret0 },
  { "fchown", (uintptr_t)&gd_ret0 },
  { "fsync", (uintptr_t)&gd_ret0 },
  { "mprotect", (uintptr_t)&gd_mprotect },
  { "prctl", (uintptr_t)&gd_ret0 },
  { "pthread_condattr_destroy", (uintptr_t)&gd_ret0 },
  { "pthread_condattr_init", (uintptr_t)&gd_ret0 },
  { "pthread_condattr_setclock", (uintptr_t)&gd_ret0 },
  { "sigaddset", (uintptr_t)&gd_ret0 },
  { "sigaltstack", (uintptr_t)&gd_ret0 },
  { "sigemptyset", (uintptr_t)&gd_ret0 },
  { "sigprocmask", (uintptr_t)&gd_ret0 },
  { "utimes", (uintptr_t)&gd_ret0 },
  { "openlog", (uintptr_t)&gd_ret0 },
  { "closelog", (uintptr_t)&gd_ret0 },
  { "syslog", (uintptr_t)&gd_ret0 },
  { "setitimer", (uintptr_t)&gd_ret0 },
  { "shutdown", (uintptr_t)&gd_ret0 },
  { "fdopendir", (uintptr_t)&gd_ret0 },
  { "open_memstream", (uintptr_t)&gd_ret0 },
  { "ftruncate", (uintptr_t)&gd_retm1 },
  { "truncate", (uintptr_t)&gd_retm1 },
  { "link", (uintptr_t)&gd_retm1 },
  { "symlink", (uintptr_t)&gd_retm1 },
  { "memfd_create", (uintptr_t)&gd_retm1 },
  { "mkstemp", (uintptr_t)&gd_retm1 },
  { "openat", (uintptr_t)&gd_retm1 },
  { "unlinkat", (uintptr_t)&gd_retm1 },
  { "pipe2", (uintptr_t)&gd_retm1 },
  { "statvfs", (uintptr_t)&gd_retm1 },
  { "malloc_info", (uintptr_t)&gd_retm1 },
  { "readlink", (uintptr_t)&gd_retm1_long },
  { "sendfile", (uintptr_t)&gd_retm1_long },
  { "pathconf", (uintptr_t)&gd_retm1_long },
  { "mremap", (uintptr_t)&gd_retm1_long },
  { "clearerr", (uintptr_t)&clearerr_gd },
  { "getc", (uintptr_t)&getc_gd },
  { "getwc", (uintptr_t)&getwc_gd },
  { "fputwc", (uintptr_t)&fputwc_gd },
  { "ungetwc", (uintptr_t)&ungetwc_gd },
  { "freelocale", (uintptr_t)&freelocale_gd },
  { "newlocale", (uintptr_t)&newlocale_gd },
  { "uselocale", (uintptr_t)&uselocale_gd },
  { "iswalpha_l", (uintptr_t)&iswalpha_l_gd },
  { "iswblank_l", (uintptr_t)&iswblank_l_gd },
  { "iswcntrl_l", (uintptr_t)&iswcntrl_l_gd },
  { "iswdigit_l", (uintptr_t)&iswdigit_l_gd },
  { "iswlower_l", (uintptr_t)&iswlower_l_gd },
  { "iswprint_l", (uintptr_t)&iswprint_l_gd },
  { "iswpunct_l", (uintptr_t)&iswpunct_l_gd },
  { "iswspace_l", (uintptr_t)&iswspace_l_gd },
  { "iswupper_l", (uintptr_t)&iswupper_l_gd },
  { "iswxdigit_l", (uintptr_t)&iswxdigit_l_gd },
  { "strcoll_l", (uintptr_t)&strcoll_l_gd },
  { "strftime_l", (uintptr_t)&strftime_l_gd },
  { "strtold_l", (uintptr_t)&strtold_l_gd },
  { "strtoll_l", (uintptr_t)&strtoll_l_gd },
  { "strtoull_l", (uintptr_t)&strtoull_l_gd },
  { "strxfrm_l", (uintptr_t)&strxfrm_l_gd },
  { "towlower_l", (uintptr_t)&towlower_l_gd },
  { "towupper_l", (uintptr_t)&towupper_l_gd },
  { "wcscoll_l", (uintptr_t)&wcscoll_l_gd },
  { "wcsxfrm_l", (uintptr_t)&wcsxfrm_l_gd },
  { "memmem", (uintptr_t)&memmem_gd },
  { "strnlen", (uintptr_t)&strnlen_gd },
  { "vasprintf", (uintptr_t)&vasprintf_gd },
  { "vprintf", (uintptr_t)&vprintf_gd },
  { "pread", (uintptr_t)&pread_gd },
  { "pwrite", (uintptr_t)&pwrite_gd },
  { "sysconf", (uintptr_t)&sysconf_gd },
  { "pthread_equal", (uintptr_t)&pthread_equal_gd },
  { "rmdir", (uintptr_t)&rmdir_gd },
  { "pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock_fake },
  { "pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock_fake },
  { "pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock_fake },
  { "stdin", (uintptr_t)&gd_stdin },
  { "stdout", (uintptr_t)&gd_stdout },
  { "stderr", (uintptr_t)&gd_stderr },
  { "AAudioStreamBuilder_delete", (uintptr_t)&aaudio_unavailable },
  { "AAudioStreamBuilder_openStream", (uintptr_t)&aaudio_unavailable },
  { "AAudioStreamBuilder_setChannelCount", (uintptr_t)&aaudio_unavailable },
  { "AAudioStreamBuilder_setContentType", (uintptr_t)&aaudio_unavailable },
  { "AAudioStreamBuilder_setDataCallback", (uintptr_t)&aaudio_unavailable },
  { "AAudioStreamBuilder_setDirection", (uintptr_t)&aaudio_unavailable },
  { "AAudioStreamBuilder_setErrorCallback", (uintptr_t)&aaudio_unavailable },
  { "AAudioStreamBuilder_setFormat", (uintptr_t)&aaudio_unavailable },
  { "AAudioStreamBuilder_setPerformanceMode", (uintptr_t)&aaudio_unavailable },
  { "AAudioStreamBuilder_setSampleRate", (uintptr_t)&aaudio_unavailable },
  { "AAudioStreamBuilder_setSharingMode", (uintptr_t)&aaudio_unavailable },
  { "AAudioStreamBuilder_setUsage", (uintptr_t)&aaudio_unavailable },
  { "AAudioStream_close", (uintptr_t)&aaudio_unavailable },
  { "AAudioStream_getBufferCapacityInFrames", (uintptr_t)&aaudio_unavailable },
  { "AAudioStream_getBufferSizeInFrames", (uintptr_t)&aaudio_unavailable },
  { "AAudioStream_getFramesPerBurst", (uintptr_t)&aaudio_unavailable },
  { "AAudioStream_getPerformanceMode", (uintptr_t)&aaudio_unavailable },
  { "AAudioStream_getSampleRate", (uintptr_t)&aaudio_unavailable },
  { "AAudioStream_getSharingMode", (uintptr_t)&aaudio_unavailable },
  { "AAudioStream_getXRunCount", (uintptr_t)&aaudio_unavailable },
  { "AAudioStream_requestStart", (uintptr_t)&aaudio_unavailable },
  { "AAudioStream_requestStop", (uintptr_t)&aaudio_unavailable },
  { "AAudioStream_setBufferSizeInFrames", (uintptr_t)&aaudio_unavailable },
  { "AAudio_convertResultToText", (uintptr_t)&aaudio_result_text },
  { "AAudio_createStreamBuilder", (uintptr_t)&aaudio_unavailable },
  { "xrAcquireSwapchainImage", (uintptr_t)&xr_runtime_unavailable },
  { "xrApplyHapticFeedback", (uintptr_t)&xr_runtime_unavailable },
  { "xrAttachSessionActionSets", (uintptr_t)&xr_runtime_unavailable },
  { "xrBeginFrame", (uintptr_t)&xr_runtime_unavailable },
  { "xrBeginSession", (uintptr_t)&xr_runtime_unavailable },
  { "xrCreateAction", (uintptr_t)&xr_runtime_unavailable },
  { "xrCreateActionSet", (uintptr_t)&xr_runtime_unavailable },
  { "xrCreateActionSpace", (uintptr_t)&xr_runtime_unavailable },
  { "xrCreateInstance", (uintptr_t)&xr_runtime_unavailable },
  { "xrCreateReferenceSpace", (uintptr_t)&xr_runtime_unavailable },
  { "xrCreateSession", (uintptr_t)&xr_runtime_unavailable },
  { "xrCreateSwapchain", (uintptr_t)&xr_runtime_unavailable },
  { "xrDestroyActionSet", (uintptr_t)&xr_runtime_unavailable },
  { "xrDestroyInstance", (uintptr_t)&xr_runtime_unavailable },
  { "xrDestroySession", (uintptr_t)&xr_runtime_unavailable },
  { "xrDestroySpace", (uintptr_t)&xr_runtime_unavailable },
  { "xrDestroySwapchain", (uintptr_t)&xr_runtime_unavailable },
  { "xrEndFrame", (uintptr_t)&xr_runtime_unavailable },
  { "xrEndSession", (uintptr_t)&xr_runtime_unavailable },
  { "xrEnumerateBoundSourcesForAction", (uintptr_t)&xr_runtime_unavailable },
  { "xrEnumerateInstanceExtensionProperties", (uintptr_t)&xr_runtime_unavailable },
  { "xrEnumerateReferenceSpaces", (uintptr_t)&xr_runtime_unavailable },
  { "xrEnumerateSwapchainFormats", (uintptr_t)&xr_runtime_unavailable },
  { "xrEnumerateSwapchainImages", (uintptr_t)&xr_runtime_unavailable },
  { "xrEnumerateViewConfigurationViews", (uintptr_t)&xr_runtime_unavailable },
  { "xrGetActionStateBoolean", (uintptr_t)&xr_runtime_unavailable },
  { "xrGetActionStateFloat", (uintptr_t)&xr_runtime_unavailable },
  { "xrGetActionStatePose", (uintptr_t)&xr_runtime_unavailable },
  { "xrGetActionStateVector2f", (uintptr_t)&xr_runtime_unavailable },
  { "xrGetCurrentInteractionProfile", (uintptr_t)&xr_runtime_unavailable },
  { "xrGetInputSourceLocalizedName", (uintptr_t)&xr_runtime_unavailable },
  { "xrGetInstanceProcAddr", (uintptr_t)&xr_runtime_unavailable },
  { "xrGetInstanceProperties", (uintptr_t)&xr_runtime_unavailable },
  { "xrGetReferenceSpaceBoundsRect", (uintptr_t)&xr_runtime_unavailable },
  { "xrGetSystem", (uintptr_t)&xr_runtime_unavailable },
  { "xrGetSystemProperties", (uintptr_t)&xr_runtime_unavailable },
  { "xrLocateSpace", (uintptr_t)&xr_runtime_unavailable },
  { "xrLocateViews", (uintptr_t)&xr_runtime_unavailable },
  { "xrPathToString", (uintptr_t)&xr_runtime_unavailable },
  { "xrPollEvent", (uintptr_t)&xr_runtime_unavailable },
  { "xrReleaseSwapchainImage", (uintptr_t)&xr_runtime_unavailable },
  { "xrRequestExitSession", (uintptr_t)&xr_runtime_unavailable },
  { "xrResultToString", (uintptr_t)&xr_runtime_unavailable },
  { "xrStopHapticFeedback", (uintptr_t)&xr_runtime_unavailable },
  { "xrStringToPath", (uintptr_t)&xr_runtime_unavailable },
  { "xrSuggestInteractionProfileBindings", (uintptr_t)&xr_runtime_unavailable },
  { "xrSyncActions", (uintptr_t)&xr_runtime_unavailable },
  { "xrWaitFrame", (uintptr_t)&xr_runtime_unavailable },
  { "xrWaitSwapchainImage", (uintptr_t)&xr_runtime_unavailable },
  { "eglChooseConfig", (uintptr_t)&eglChooseConfig },
  { "eglCreateContext", (uintptr_t)&eglCreateContext },
  { "eglCreatePbufferSurface", (uintptr_t)&eglCreatePbufferSurface },
  { "eglDestroyContext", (uintptr_t)&eglDestroyContext },
  { "eglDestroySurface", (uintptr_t)&eglDestroySurface },
  { "eglGetCurrentContext", (uintptr_t)&eglGetCurrentContext },
  { "eglGetDisplay", (uintptr_t)&eglGetDisplay },
  { "eglGetError", (uintptr_t)&eglGetError },
  { "eglGetProcAddress", (uintptr_t)&eglGetProcAddress_fake },
  { "eglInitialize", (uintptr_t)&eglInitialize },
  { "eglMakeCurrent", (uintptr_t)&eglMakeCurrent },
  { "eglTerminate", (uintptr_t)&eglTerminate },
  { "AMEDIAFORMAT_KEY_BIT_RATE", (uintptr_t)&key_AMEDIAFORMAT_KEY_BIT_RATE },
  { "AMEDIAFORMAT_KEY_CHANNEL_COUNT", (uintptr_t)&key_AMEDIAFORMAT_KEY_CHANNEL_COUNT },
  { "AMEDIAFORMAT_KEY_DURATION", (uintptr_t)&key_AMEDIAFORMAT_KEY_DURATION },
  { "AMEDIAFORMAT_KEY_ENCODER_DELAY", (uintptr_t)&key_AMEDIAFORMAT_KEY_ENCODER_DELAY },
  { "AMEDIAFORMAT_KEY_ENCODER_PADDING", (uintptr_t)&key_AMEDIAFORMAT_KEY_ENCODER_PADDING },
  { "AMEDIAFORMAT_KEY_MIME", (uintptr_t)&key_AMEDIAFORMAT_KEY_MIME },
  { "AMEDIAFORMAT_KEY_SAMPLE_RATE", (uintptr_t)&key_AMEDIAFORMAT_KEY_SAMPLE_RATE },
  { "getenv", (uintptr_t)&getenv_gd },
  { "SDL_AndroidGetActivity", (uintptr_t)&sdl_android_get_activity },
  { "SDL_AndroidGetJNIEnv", (uintptr_t)&sdl_android_get_jnienv },
  { "SDL_SetMainReady", (uintptr_t)&SDL_SetMainReady },
  { "SDL_StopTextInput", (uintptr_t)&SDL_StopTextInput },
};
const size_t gdroid_import_count = sizeof(gdroid_import_functions) / sizeof(*gdroid_import_functions);

uintptr_t gdroid_missing_import(const char *name) {
  if (name[0] == 'g' && name[1] == 'l' && name[2] >= 'A' && name[2] <= 'Z') {
    void *proc = eglGetProcAddress_fake(name);
    if (proc)
      return (uintptr_t)proc;
  }
  return import_stub_for(name);
}
