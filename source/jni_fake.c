/* jni_fake.c -- minimal fake JNIEnv / JavaVM
 *
 * HL2: the launcher's setArgs/setenv JNI exports only call GetStringUTFChars
 * (slot 169) on jstrings we hand them. The jstrings are plain C strings, so
 * GetStringUTFChars just returns the pointer itself.
 *
 * GDroid (Garry's Mod): GameUI / ServerBrowser / client also reach Java
 * (master-server queries, Workshop HTTP) through env->GetJavaVM() and
 * JavaVM->AttachCurrentThread()/DetachCurrentThread(). There is no Java here,
 * so the fake env answers "no class / no method" (FindClass, GetMethodID...
 * return NULL, which the game handles as an error) but the bookkeeping calls
 * (GetJavaVM, Attach/Detach, exceptions, local refs) must really work: a
 * stubbed GetJavaVM returned 0 with a NULL out-pointer and the game crashed
 * dereferencing it right after "Create new game".
 *
 * Any other slot logs "JNI: unimplemented <name> (slot N) called" (first few
 * calls only) and returns 0.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <string.h>

#include "jni_fake.h"
#include "jni_names.h"
#include "jni_stub_list.h"
#include "util.h"

#define JNI_TABLE_SIZE 256 // standard JNINativeInterface has 233 entries
#define JNI_LOG_FIRST_HITS 3

static void *jni_functions[JNI_TABLE_SIZE];
static void **jni_env_obj = jni_functions; // JNIEnv = JNINativeInterface**

void *fake_env = &jni_env_obj;

// --- JavaVM (JNIInvokeInterface) -------------------------------------------
// layout: reserved0..2, DestroyJavaVM, AttachCurrentThread,
// DetachCurrentThread, GetEnv, AttachCurrentThreadAsDaemon
static void *jni_invoke[8];
static void **jni_vm_obj = jni_invoke;

static int vm_DestroyJavaVM(void *vm) {
  (void)vm;
  return 0;
}

static int vm_AttachCurrentThread(void *vm, void **penv, void *args) {
  (void)vm; (void)args;
  if (penv)
    *penv = fake_env;
  return 0; // JNI_OK
}

static int vm_DetachCurrentThread(void *vm) {
  (void)vm;
  return 0;
}

static int vm_GetEnv(void *vm, void **penv, int version) {
  (void)vm; (void)version;
  if (penv)
    *penv = fake_env;
  return 0;
}

// slot 219
static int GetJavaVM_fake(void *env, void **vm) {
  (void)env;
  if (vm)
    *vm = &jni_vm_obj;
  return 0;
}

// --- unimplemented slots -----------------------------------------------------
static unsigned jni_hits[JNI_TABLE_SIZE];

static long jni_unimplemented_slot(int i) {
  const unsigned hits = ++jni_hits[i];
  if (hits <= JNI_LOG_FIRST_HITS)
    debugPrintf("JNI: unimplemented %s (slot %d) called\n",
                i < 233 ? jni_slot_names[i] : "?", i);
  return 0;
}

#define JNI_DEFINE_STUB(n) static long jni_stub_##n(void) { return jni_unimplemented_slot(n); }
JNI_STUB_LIST(JNI_DEFINE_STUB)
#undef JNI_DEFINE_STUB

#define JNI_STUB_ADDR(n) (void *)&jni_stub_##n,
static void *const jni_stub_fns[JNI_TABLE_SIZE] = { JNI_STUB_LIST(JNI_STUB_ADDR) };
#undef JNI_STUB_ADDR

// --- simple real implementations ---------------------------------------------
static int jni_ret0(void) { return 0; }
static void jni_noop(void) {}

static int GetVersion_fake(void *env) {
  (void)env;
  return 0x00010006; // JNI_VERSION_1_6
}

// NewGlobalRef / NewLocalRef / NewWeakGlobalRef: refs are never freed here
static void *jni_ret_arg(void *env, void *obj) {
  (void)env;
  return obj;
}

// PopLocalFrame(env, result)
static void *PopLocalFrame_fake(void *env, void *result) {
  (void)env;
  return result;
}

// jstring is a plain C string in this fake env
// slot 167
static const char *NewStringUTF_fake(void *env, const char *utf) {
  (void)env;
  return utf ? strdup(utf) : NULL;
}

// slots 164 (GetStringLength) and 168 (GetStringUTFLength)
static int GetStringLength_fake(void *env, const char *jstr) {
  (void)env;
  return jstr ? (int)strlen(jstr) : 0;
}

// slot 169
static const char *GetStringUTFChars_fake(void *env, const char *jstr, unsigned char *is_copy) {
  (void)env;
  if (is_copy)
    *is_copy = 0;
  return jstr;
}

// slot 170
static void ReleaseStringUTFChars_fake(void *env, const char *jstr, const char *utf) {
  (void)env; (void)jstr; (void)utf;
}

void jni_init(void) {
  for (int i = 0; i < JNI_TABLE_SIZE; i++)
    jni_functions[i] = jni_stub_fns[i];

  jni_functions[4] = (void *)GetVersion_fake;
  // exceptions: there is never a pending one
  jni_functions[15] = (void *)jni_ret0;  // ExceptionOccurred
  jni_functions[16] = (void *)jni_noop;  // ExceptionDescribe
  jni_functions[17] = (void *)jni_noop;  // ExceptionClear
  jni_functions[228] = (void *)jni_ret0; // ExceptionCheck
  // references
  jni_functions[19] = (void *)jni_ret0;  // PushLocalFrame
  jni_functions[20] = (void *)PopLocalFrame_fake;
  jni_functions[21] = (void *)jni_ret_arg; // NewGlobalRef
  jni_functions[22] = (void *)jni_noop;    // DeleteGlobalRef
  jni_functions[23] = (void *)jni_noop;    // DeleteLocalRef
  jni_functions[25] = (void *)jni_ret_arg; // NewLocalRef
  jni_functions[26] = (void *)jni_ret0;    // EnsureLocalCapacity
  jni_functions[226] = (void *)jni_ret_arg; // NewWeakGlobalRef
  jni_functions[227] = (void *)jni_noop;    // DeleteWeakGlobalRef
  // threads / VM
  jni_functions[217] = (void *)jni_ret0; // MonitorEnter
  jni_functions[218] = (void *)jni_ret0; // MonitorExit
  jni_functions[219] = (void *)GetJavaVM_fake;
  // strings
  jni_functions[164] = (void *)GetStringLength_fake;
  jni_functions[167] = (void *)NewStringUTF_fake;
  jni_functions[168] = (void *)GetStringLength_fake; // GetStringUTFLength
  jni_functions[169] = (void *)GetStringUTFChars_fake;
  jni_functions[170] = (void *)ReleaseStringUTFChars_fake;

  jni_invoke[3] = (void *)vm_DestroyJavaVM;
  jni_invoke[4] = (void *)vm_AttachCurrentThread;
  jni_invoke[5] = (void *)vm_DetachCurrentThread;
  jni_invoke[6] = (void *)vm_GetEnv;
  jni_invoke[7] = (void *)vm_AttachCurrentThread; // AttachCurrentThreadAsDaemon
}
