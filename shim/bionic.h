#pragma once
#include "../loader/elf_loader.h"
void bionic_init(so_resolver next);      // call on main thread before so_init of any module
void *bionic_resolve(const char *name);  // pass to so_set_resolver
void bionic_thread_enter(void);          // set up fake TCB on the current thread
void bionic_set_root(const char *root);  // /sdcard/... maps to root/...
const char *bionic_path(const char *p);
int bionic_oflags(int f);
void *bionic_dlopen(const char *n, int f);
void *bionic_dlsym(void *h, const char *s);
extern void *(*bionic_gl_proc)(const char *);
