#pragma once
#include <stdint.h>
typedef union jvalue { uint8_t z; int8_t b; uint16_t c; int16_t s; int32_t i; int64_t j; float f; double d; void *l; } jvalue;
typedef jvalue (*jni_impl)(void *env, void *thiz, jvalue *args);  // args==NULL for field reads
typedef struct { const char *cls, *name, *sig; jni_impl fn; } jni_impl_entry; // cls/sig NULL = wildcard
void jni_init(void);
void *jni_vm(void);
void *jni_env(void);
void *jni_activity(void);
void jni_set_impls(jni_impl_entry *t, int n);
void jni_register_natives(const char *cls, const void *m, int n);
void *jni_native(const char *cls, const char *name);
typedef struct jobj jobj;
jobj *jni_new_string(const char *s);
jobj *jni_new_array(int n, int esz, const char *cls);
const char *jni_str(void *p);
void *jni_new_object(const char *cls);
const char *jni_class_of(void *o);
