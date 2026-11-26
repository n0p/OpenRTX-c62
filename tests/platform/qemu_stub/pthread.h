/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Bare-metal stand-in for <pthread.h>, for the QEMU cost harnesses that link
 * firmware drivers without an RTOS.  The drivers only touch a mutex when the
 * device descriptor has one; the harnesses pass NULL, so these never run.
 *
 * Point the harness build at this directory with -Itests/platform/qemu_stub.
 */
#ifndef QEMU_STUB_PTHREAD_H
#define QEMU_STUB_PTHREAD_H

typedef int pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER 0

static inline int pthread_mutex_lock(pthread_mutex_t *m)
{
    (void)m;
    return 0;
}

static inline int pthread_mutex_trylock(pthread_mutex_t *m)
{
    (void)m;
    return 0;
}

static inline int pthread_mutex_unlock(pthread_mutex_t *m)
{
    (void)m;
    return 0;
}

static inline int pthread_mutex_init(pthread_mutex_t *m, const void *attr)
{
    (void)m;
    (void)attr;
    return 0;
}

static inline int pthread_mutex_destroy(pthread_mutex_t *m)
{
    (void)m;
    return 0;
}

#endif /* QEMU_STUB_PTHREAD_H */
