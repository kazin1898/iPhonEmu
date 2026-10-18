/*
 * The slice of Darwin's <pthread.h> the GL front end uses, for building it with no iOS SDK
 * (build-win.sh). The types and initializers are the 32-bit ARM ABI of libSystem's pthreads
 * (sys/_pthread/_pthread_types.h: a signature word, then opaque bytes of the sizes below), which
 * every iOS from 2.0 through 6.x keeps. Nothing here is more than a declaration.
 */
#ifndef GLES_PUBLIC_DARWIN_PTHREAD_H
#define GLES_PUBLIC_DARWIN_PTHREAD_H

typedef unsigned long pthread_key_t;
typedef struct _opaque_pthread_t *pthread_t;
typedef struct { long __sig; char __opaque[36]; } pthread_attr_t;
typedef struct { long __sig; char __opaque[4]; } pthread_once_t;
typedef struct { long __sig; char __opaque[40]; } pthread_mutex_t;
typedef struct { long __sig; char __opaque[8]; } pthread_mutexattr_t;
typedef struct { long __sig; char __opaque[24]; } pthread_cond_t;
typedef struct { long __sig; char __opaque[4]; } pthread_condattr_t;

#define PTHREAD_ONCE_INIT          { 0x30B1BCBA, { 0 } }
#define PTHREAD_MUTEX_INITIALIZER  { 0x32AAABA7, { 0 } }
#define PTHREAD_COND_INITIALIZER   { 0x3CB0B1BB, { 0 } }

int pthread_key_create(pthread_key_t *, void (*)(void *));
void *pthread_getspecific(pthread_key_t);
int pthread_setspecific(pthread_key_t, const void *);
int pthread_once(pthread_once_t *, void (*)(void));
int pthread_mutex_lock(pthread_mutex_t *);
int pthread_mutex_unlock(pthread_mutex_t *);
int pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
int pthread_cond_signal(pthread_cond_t *);
int pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);

#endif
