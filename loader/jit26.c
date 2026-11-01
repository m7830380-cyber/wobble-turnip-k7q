// iOS 26 TXM JIT: executable memory must come from the debugger (StikDebug universal.js protocol)
#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <libkern/OSCacheControl.h>

__attribute__((noinline, naked)) static void *JIT26PrepareRegion(void *addr, size_t len) {
  __asm__("mov x16, #1\n brk #0xf00d\n ret");
}

__attribute__((noinline, naked)) static void JIT26Detach(void) {
  __asm__("mov x16, #0\n brk #0xf00d\n ret");
}
int so_jit_traced(void);
void so_jit_detach(void) {
  if (so_jit_mode() != 2 || !so_jit_traced() || getenv("GMOD_KEEP_DEBUGGER")) return;
  JIT26Detach();
  fprintf(stderr, "[jit] detached from debugger (traced %d)\n", so_jit_traced());
}
int so_jit_traced(void) {
  struct kinfo_proc kp; size_t n = sizeof kp; int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
  memset(&kp, 0, sizeof kp);
  if (sysctl(mib, 4, &kp, &n, NULL, 0)) return 0;
  return (kp.kp_proc.p_flag & P_TRACED) != 0;
}

/* 1 = mprotect RX works (CS_DEBUGGED, pre-TXM), 2 = TXM: need debugger-prepared RX + RW mirror */
int so_jit_mode(void) {
  static int mode = -1;
  if (mode >= 0) return mode;
  void *p = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  *(uint32_t *)p = 0xd65f03c0;
  int ok = mprotect(p, 16384, PROT_READ | PROT_EXEC) == 0;
  if (ok) { /* TXM can accept the call yet leave the page non-executable: read back real protection */
    vm_address_t a = (vm_address_t)p; vm_size_t sz = 0; vm_region_basic_info_data_64_t bi; mach_msg_type_number_t c = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t o;
    if (vm_region_64(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&bi, &c, &o) == KERN_SUCCESS && !(bi.protection & VM_PROT_EXECUTE)) ok = 0;
  }
  mode = ok ? 1 : 2;
  munmap(p, 16384);
  { char v[32] = {0}; size_t n = sizeof v; if (!sysctlbyname("kern.osproductversion", v, &n, NULL, 0) && atoi(v) >= 26 && mode == 1)
      fprintf(stderr, "[jit] iOS %s but mprotect RX accepted\n", v); }
  if (getenv("GMOD_FORCE_TXM")) mode = 2;
  fprintf(stderr, "[jit] mode %d (traced %d)\n", mode, so_jit_traced());
  return mode;
}

/* addr = RX mapping (or NULL to let debugger allocate); returns RX, *rw = writable mirror */
void *so_jit_prepare(void *addr, size_t len, uint8_t **rw) {
  void *rx = JIT26PrepareRegion(addr, len);
  if (!rx || (uintptr_t)rx == (uintptr_t)-1 || (addr && rx != addr)) {
    fprintf(stderr, "[jit] PrepareRegion(%p,%zx) -> %p failed (is StikDebug script = universal?)\n", addr, len, rx); return NULL; }
  vm_address_t w = 0; vm_prot_t cur, max;
  kern_return_t kr = vm_remap(mach_task_self(), &w, len, 0, VM_FLAGS_ANYWHERE, mach_task_self(),
                              (vm_address_t)rx, FALSE, &cur, &max, VM_INHERIT_NONE);
  if (kr != KERN_SUCCESS) { fprintf(stderr, "[jit] vm_remap %d\n", kr); return NULL; }
  kr = vm_protect(mach_task_self(), w, len, FALSE, VM_PROT_READ | VM_PROT_WRITE);
  if (kr != KERN_SUCCESS) { fprintf(stderr, "[jit] vm_protect RW %d\n", kr); return NULL; }
  *rw = (uint8_t *)w;
  return rx;
}
#endif
