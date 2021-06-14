#ifndef NTIRPC_MACPTHREADS_H_
#define NTIRPC_MACPTHREADS_H_

#include <err.h>
#include <os/lock.h>
#include <pthread.h>
#include <string.h>
#include <sys/errno.h>

typedef os_unfair_lock pthread_spinlock_t;

static inline int pthread_spin_init(os_unfair_lock *sp, int pshared)
{
	if (pshared != PTHREAD_PROCESS_PRIVATE)
		errx(1, "Unsupported spinlock type: %d", pshared);

	memset(sp, 0, sizeof(*sp));
	return 0;
}

static inline int pthread_spin_lock(os_unfair_lock *sp)
{
	os_unfair_lock_lock(sp);
	return 0;
}

static inline int pthread_spin_trylock(os_unfair_lock *sp)
{
	return os_unfair_lock_trylock(sp) ? 0 : EBUSY;
}

static inline int pthread_spin_unlock(os_unfair_lock *sp)
{
	os_unfair_lock_unlock(sp);
	return 0;
}

static inline int pthread_spin_destroy(os_unfair_lock *sp)
{
	/* Nothing to do. */
	return 0;
}

#endif				/* NTIRPC_MACPTHREADS_H_ */
