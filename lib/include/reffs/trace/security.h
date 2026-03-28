/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifndef _REFFS_TRACE_SECURITY_H
#define _REFFS_TRACE_SECURITY_H

#include <stdint.h>

#include "reffs/trace/types.h"
#include "reffs/log.h"

static inline void trace_security_gss_init(int fd, uint32_t xid,
					   const char *event, int line)
{
	reffs_trace_event(REFFS_TRACE_CAT_SECURITY, event, line,
			  "GSS INIT fd=%d xid=0x%08x", fd, xid);
}

static inline void trace_security_gss_accept(uint32_t major, uint32_t minor,
					     uint32_t token_in,
					     uint32_t token_out,
					     const char *event, int line)
{
	reffs_trace_event(REFFS_TRACE_CAT_SECURITY, event, line,
			  "GSS accept major=%u minor=%u "
			  "token_in=%u token_out=%u",
			  major, minor, token_in, token_out);
}

static inline void trace_security_gss_data(uint32_t xid, uint32_t seq,
					   uint32_t svc, uint32_t handle_len,
					   const char *event, int line)
{
	reffs_trace_event(REFFS_TRACE_CAT_SECURITY, event, line,
			  "GSS DATA xid=0x%08x seq=%u svc=%u handle_len=%u",
			  xid, seq, svc, handle_len);
}

static inline void trace_security_gss_error(const char *msg, uint32_t major,
					    const char *event, int line)
{
	reffs_trace_event(REFFS_TRACE_CAT_SECURITY, event, line,
			  "GSS %s major=%u", msg, major);
}

static inline void trace_security_gss_map(const char *principal, uint32_t uid,
					  uint32_t gid, const char *event,
					  int line)
{
	reffs_trace_event(REFFS_TRACE_CAT_SECURITY, event, line,
			  "GSS map %s uid=%u gid=%u", principal, uid, gid);
}

static inline void trace_security_wrongsec(uint32_t flavor, int tls,
					   const char *event, int line)
{
	reffs_trace_event(REFFS_TRACE_CAT_SECURITY, event, line,
			  "WRONGSEC flavor=%u tls=%d", flavor, tls);
}

static inline void trace_security_tls(int fd, const char *msg,
				      const char *event, int line)
{
	reffs_trace_event(REFFS_TRACE_CAT_SECURITY, event, line, "TLS fd=%d %s",
			  fd, msg);
}

#endif /* _REFFS_TRACE_SECURITY_H */
