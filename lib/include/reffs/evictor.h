/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifndef _REFFS_EVICTOR_H
#define _REFFS_EVICTOR_H

enum evictor_mode {
	EVICTOR_SYNC = 0, /* evict on worker thread (stress testing) */
	EVICTOR_ASYNC = 1, /* evict on background thread (production) */
};

/* Start/stop the background evictor thread. */
int evictor_init(void);
void evictor_fini(void);

/* Signal that eviction pressure exists -- wakes the evictor. */
void evictor_signal(void);

/* Synchronous drain -- blocks until one full eviction pass completes. */
void evictor_drain(void);

/* Runtime mode switch (for tests). */
void evictor_set_mode(enum evictor_mode mode);
enum evictor_mode evictor_get_mode(void);

#endif /* _REFFS_EVICTOR_H */
