/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include "reffs/rpc.h"
#include "nfs4/compound.h"

#include "ps_security.h"

bool ps_proxy_compound_is_gss(const struct compound *compound)
{
	if (!compound || !compound->c_rt)
		return false;
	return compound->c_rt->rt_info.ri_cred.rc_flavor == RPCSEC_GSS;
}
