<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# WRONGSEC Unit Test Plan

RFC 8881 Section 2.6.3.1 specifies when NFS4ERR_WRONGSEC must and
must not be returned.  This plan derives one or more unit tests
from each subsection.

## Test Infrastructure

**File**: `lib/nfs4/tests/wrongsec_test.c` (NEW)

The tests build COMPOUND4args/COMPOUND4res directly and call
`dispatch_compound()`, similar to `compound_async.c`.  No live
NFS client or network connection is needed.

### Test Fixture

The fixture needs:
1. Full NFS4 server state (via `nfs4_test_setup` / `nfs4_test_teardown`)
2. Two superblocks with different flavor lists:
   - **root sb** (sb_id=1): flavors = [AUTH_SYS, KRB5]
   - **child sb** (sb_id=42): mounted at `/secure`, flavors = [KRB5]
3. A mock `struct rpc_trans` with controllable `rc_flavor`

### Helper: `make_wrongsec_compound()`

```c
/*
 * Build a compound for WRONGSEC testing.
 *
 * nops:     number of ops in the argarray
 * ops:      array of nfs_opnum4 values to install
 * flavor:   AUTH_SYS, RPCSEC_GSS, etc.
 * gss_svc:  GSS service (RPC_GSS_SVC_NONE, _INTEGRITY, _PRIVACY)
 *
 * Returns a heap-allocated compound.  The rpc_trans has rt_info.ri_cred
 * set to the specified flavor, and rt_fd = -1 (no TLS check needed).
 */
static struct compound *make_wrongsec_compound(
    unsigned int nops, nfs_opnum4 *ops,
    uint32_t flavor, uint32_t gss_svc);
```

### Helper: `set_op_putfh()`

```c
/*
 * Fill argarray[idx] with a PUTFH carrying a network_file_handle
 * pointing at (sb_id, ino).
 */
static void set_op_putfh(struct compound *c, unsigned int idx,
                         uint64_t sb_id, uint64_t ino);
```

Similar helpers for PUTROOTFH, LOOKUP, SAVEFH, RESTOREFH, SECINFO,
SECINFO_NO_NAME, GETATTR, READ, GETFH, LOOKUPP.

### Helper: `compound_op_status()`

```c
/* Return the nfsstat4 from resarray[idx]. */
static nfsstat4 compound_op_status(struct compound *c, unsigned int idx);
```

## Test Cases

Each test case cites the RFC subsection it validates.

### S2.6.3.1.1.1 -- Put FH + SAVEFH (transparent)

**Test 1: `test_putfh_savefh_secinfo_no_wrongsec`**

SAVEFH is transparent.  When the real next op after SAVEFH is
SECINFO, the put-FH must NOT return WRONGSEC.

```
Compound: PUTROOTFH, SAVEFH, SECINFO("secure")
Flavor:   AUTH_SYS
Export:   root (allows AUTH_SYS)
Expected: PUTROOTFH = OK, SAVEFH = OK, SECINFO = OK
```

The server must skip past SAVEFH when determining the "next op"
for SECINFO lookahead.

**Test 2: `test_putfh_savefh_getattr_wrongsec`**

When the real next op after SAVEFH is not SECINFO/LOOKUP/LOOKUPP,
the put-FH MUST return WRONGSEC if flavor doesn't match.

```
Compound: PUTFH(secure_fh), SAVEFH, GETATTR
Flavor:   AUTH_SYS
Export:   /secure (requires KRB5)
Expected: PUTFH = NFS4ERR_WRONGSEC
```

The server looks past SAVEFH, sees GETATTR (Section 2.6.3.1.1.7
applies), so PUTFH must return WRONGSEC.

**Test 3: `test_putfh_multi_savefh_secinfo`**

Multiple SAVEFH ops should all be skipped.

```
Compound: PUTFH(secure_fh), SAVEFH, SAVEFH, SECINFO("foo")
Flavor:   AUTH_SYS
Export:   /secure (requires KRB5)
Expected: PUTFH = OK (next real op is SECINFO)
```

### S2.6.3.1.1.2 -- Two or More Put FH Operations

**Test 4: `test_two_putfh_lookup_no_wrongsec_on_first`**

For N put-FH ops in series, only the last one can return WRONGSEC.
The first N-1 MUST NOT.

```
Compound: PUTFH(secure_fh), PUTROOTFH, LOOKUP("secure")
Flavor:   AUTH_SYS
Export:   PUTFH targets /secure (KRB5), PUTROOTFH targets root (SYS+KRB5)
Expected: PUTFH = OK (first of 2 put-FH ops, ignored for WRONGSEC)
          PUTROOTFH = OK (followed by LOOKUP, S2.6.3.1.1.3 applies)
          LOOKUP = NFS4ERR_WRONGSEC (child /secure requires KRB5)
```

**Test 5: `test_two_putfh_getattr_wrongsec_on_last`**

```
Compound: PUTFH(secure_fh), PUTROOTFH, GETATTR
Flavor:   AUTH_SYS
Export:   PUTROOTFH targets root (SYS+KRB5)
Expected: PUTFH = OK (first of 2, ignored)
          PUTROOTFH = OK (root allows SYS)
          GETATTR = OK
```

This is the normal case -- the second put-FH is the effective one
and S2.6.3.1.1.7 is satisfied because root allows AUTH_SYS.

### S2.6.3.1.1.3 -- Put FH + LOOKUP (or OPEN by name)

**Test 6: `test_putrootfh_lookup_wrongsec_on_lookup`**

When LOOKUP crosses into a child export with incompatible flavor,
WRONGSEC comes from LOOKUP, NOT from the put-FH op.

```
Compound: PUTROOTFH, LOOKUP("secure")
Flavor:   AUTH_SYS
Export:   root (SYS+KRB5), /secure (KRB5 only)
Expected: PUTROOTFH = OK
          LOOKUP = NFS4ERR_WRONGSEC
```

RFC: "the put filehandle operation cannot return NFS4ERR_WRONGSEC
when there is a security tuple mismatch.  Instead, it should be
returned from the LOOKUP"

**Test 7: `test_putrootfh_lookup_allowed_flavor`**

```
Compound: PUTROOTFH, LOOKUP("secure")
Flavor:   RPCSEC_GSS (krb5, svc=NONE)
Export:   root (SYS+KRB5), /secure (KRB5)
Expected: PUTROOTFH = OK
          LOOKUP = OK
```

**Test 8: `test_putfh_lookup_no_mount_crossing`**

LOOKUP that does NOT cross a mount point (stays in root sb):

```
Compound: PUTROOTFH, LOOKUP("somedir")  -- somedir in root sb
Flavor:   AUTH_SYS
Export:   root (SYS+KRB5)
Expected: PUTROOTFH = OK
          LOOKUP = OK (or NFS4ERR_NOENT if dir doesn't exist)
```

No WRONGSEC because no mount-point crossing occurs.

### S2.6.3.1.1.4 -- Put FH + LOOKUPP

**Test 9: `test_putfh_lookupp_no_wrongsec_on_putfh`**

A put-FH followed by LOOKUPP MUST NOT return WRONGSEC on the
put-FH.  LOOKUPP crosses back to the parent sb (root), which
allows AUTH_SYS, so LOOKUPP succeeds.

```
Compound: PUTFH(secure_root_fh), LOOKUPP
Flavor:   AUTH_SYS
Export:   /secure (KRB5 only)
Expected: PUTFH = OK (S2.6.3.1.1.4: MUST NOT return WRONGSEC)
          LOOKUPP = OK (crosses to root, root allows SYS)
```

NOTE: The current implementation has a bug here -- LOOKUPP at
dir.c:178 calls nfs4_check_wrongsec() against the current
(child) export BEFORE crossing to the parent.  This will return
WRONGSEC from LOOKUPP when the child export rejects the flavor,
even though the crossing would succeed.  The check should be
against the parent export AFTER crossing, not the child BEFORE.

**Test 10: `test_putfh_lookupp_crosses_back`**

LOOKUPP from child sb root crosses to parent sb.  The parent
allows the flavor, so no WRONGSEC.

```
Compound: PUTFH(secure_root_fh), LOOKUPP
Flavor:   RPCSEC_GSS (krb5, svc=NONE)
Export:   /secure (KRB5)
Expected: PUTFH = OK
          LOOKUPP = OK (crosses to root, root allows KRB5)
```

### S2.6.3.1.1.5 -- Put FH + SECINFO / SECINFO_NO_NAME

**Test 11: `test_putfh_secinfo_no_wrongsec`**

MUST NOT return WRONGSEC on either the put-FH or the SECINFO.

```
Compound: PUTFH(secure_fh), SECINFO("anything")
Flavor:   AUTH_SYS
Export:   /secure (KRB5 only)
Expected: PUTFH = OK
          SECINFO = OK (returns flavor list)
```

**Test 12: `test_putfh_secinfo_no_name_no_wrongsec`**

```
Compound: PUTFH(secure_fh), SECINFO_NO_NAME(CURRENT_FH)
Flavor:   AUTH_SYS
Export:   /secure (KRB5 only)
Expected: PUTFH = OK
          SECINFO_NO_NAME = OK (returns flavor list)
```

**Test 13: `test_putrootfh_secinfo_no_wrongsec`**

Same rule applies to PUTROOTFH:

```
Compound: PUTROOTFH, SECINFO("secure")
Flavor:   AUTH_NONE   (not in any export's list)
Expected: PUTROOTFH = OK
          SECINFO = OK
```

### S2.6.3.1.1.6 -- Put FH + Nothing

**Test 14: `test_putfh_nothing_no_wrongsec`**

A put-FH as the last op in the compound MUST NOT return WRONGSEC.

```
Compound: PUTFH(secure_fh)
Flavor:   AUTH_SYS
Export:   /secure (KRB5 only)
Expected: PUTFH = OK
```

**Test 15: `test_putrootfh_nothing_no_wrongsec`**

```
Compound: PUTROOTFH
Flavor:   AUTH_NONE
Expected: PUTROOTFH = OK
```

### S2.6.3.1.1.7 -- Put FH + Anything Else

**Test 16: `test_putfh_getattr_wrongsec`**

Put-FH followed by a non-special op MUST return WRONGSEC when
flavor mismatches.

```
Compound: PUTFH(secure_fh), GETATTR
Flavor:   AUTH_SYS
Export:   /secure (KRB5 only)
Expected: PUTFH = NFS4ERR_WRONGSEC
```

**Test 17: `test_putfh_read_wrongsec`**

```
Compound: PUTFH(secure_fh), READ
Flavor:   AUTH_SYS
Export:   /secure (KRB5 only)
Expected: PUTFH = NFS4ERR_WRONGSEC
```

**Test 18: `test_putfh_getfh_wrongsec`**

```
Compound: PUTFH(secure_fh), GETFH
Flavor:   AUTH_SYS
Export:   /secure (KRB5 only)
Expected: PUTFH = NFS4ERR_WRONGSEC
```

**Test 19: `test_putrootfh_getattr_allowed`**

Positive test: flavor matches, no WRONGSEC.

```
Compound: PUTROOTFH, GETATTR
Flavor:   AUTH_SYS
Export:   root (SYS+KRB5)
Expected: PUTROOTFH = OK
          GETATTR = OK
```

**Test 20: `test_putpubfh_getattr_wrongsec`**

PUTPUBFH behaves like PUTROOTFH for WRONGSEC purposes.

```
Compound: PUTPUBFH, GETATTR
Flavor:   AUTH_NONE
Export:   root (SYS+KRB5 only)
Expected: PUTPUBFH = NFS4ERR_WRONGSEC
```

### S2.6.3.1.1.8 -- Operations after SECINFO/SECINFO_NO_NAME

**Test 21: `test_secinfo_consumes_fh`**

SECINFO consumes the current FH.  The next op gets
NFS4ERR_NOFILEHANDLE, NOT NFS4ERR_WRONGSEC.

```
Compound: PUTFH(secure_fh), SECINFO("anything"), READ
Flavor:   AUTH_SYS
Export:   /secure (KRB5 only)
Expected: PUTFH = OK (next real op is SECINFO)
          SECINFO = OK
          READ = NFS4ERR_NOFILEHANDLE
```

**Test 22: `test_secinfo_no_name_consumes_fh`**

```
Compound: PUTFH(secure_fh), SECINFO_NO_NAME(CURRENT_FH), GETATTR
Flavor:   AUTH_SYS
Export:   /secure (KRB5 only)
Expected: PUTFH = OK
          SECINFO_NO_NAME = OK
          GETATTR = NFS4ERR_NOFILEHANDLE
```

### S2.6.3.1.2 -- LINK and RENAME

**Test 23: `test_rename_cross_export_wrongsec_on_putfh`**

RENAME uses saved + current FH.  If the current FH's export
rejects the flavor, PUTFH returns WRONGSEC (S2.6.3.1.1.7
applies because RENAME is "anything else").

```
Compound: PUTROOTFH, SAVEFH, PUTFH(secure_fh), RENAME("a", "b")
Flavor:   AUTH_SYS
Export:   root (SYS+KRB5), /secure (KRB5)
Expected: PUTROOTFH = OK
          SAVEFH = OK
          PUTFH = NFS4ERR_WRONGSEC (next op is RENAME, not SECINFO/LOOKUP)
```

**Test 24: `test_rename_saved_fh_wrongsec`**

If current FH's export allows the flavor but saved FH's doesn't,
RENAME itself MAY return WRONGSEC per the RFC.

```
Compound: PUTFH(secure_fh), SAVEFH, PUTROOTFH, RENAME("a", "b")
Flavor:   RPCSEC_GSS (krb5)
Export:   /secure (KRB5), root (SYS+KRB5)
Expected: PUTFH = OK (next is SAVEFH, look past to PUTROOTFH -- two
                       put-FH ops, first ignored per S2.6.3.1.1.2)
          SAVEFH = OK
          PUTROOTFH = OK (next is RENAME, root allows KRB5)
          RENAME = OK or NFS4ERR_XDEV (cross-sb rename)
```

Note: reffs returns NFS4ERR_XDEV for cross-sb RENAME, so this
tests the WRONGSEC-does-not-fire path.

**Test 25: `test_rename_no_common_flavor_xdev`**

When saved and current FH exports have NO common flavor, server
MUST NOT return WRONGSEC in an endless loop.  Must return XDEV
or allow the op.

```
Compound: PUTFH(secure_fh), SAVEFH, PUTFH(sys_only_fh), RENAME("a","b")
Flavor:   AUTH_SYS
Export:   /secure (KRB5 only), /sys-only (SYS only)
Expected: PUTFH(secure_fh) = OK (first of two put-FH, ignored)
          SAVEFH = OK (transparent)
          PUTFH(sys_only) = OK (sys-only allows SYS, next is RENAME)
          RENAME = NFS4ERR_XDEV (cross-sb)
```

This requires a third sb (/sys-only with SYS only).

### RESTOREFH

**Test 26: `test_restorefh_wrongsec_check`**

RESTOREFH changes the current FH to a saved FH from a potentially
different export.  Must check WRONGSEC on the restored FH.

```
Compound: PUTROOTFH, SAVEFH, PUTFH(secure_fh), SECINFO("x"),
          RESTOREFH, GETATTR
Flavor:   AUTH_SYS
Export:   root (SYS+KRB5), /secure (KRB5)

Expected: PUTROOTFH = OK
          SAVEFH = OK
          PUTFH = OK (next real op past SECINFO is... well,
                       SECINFO is the next op, so no WRONGSEC)
          SECINFO = OK (consumes FH)
          RESTOREFH = OK (restores root FH; root allows SYS;
                          next is GETATTR, S2.6.3.1.1.7 applies,
                          but root allows SYS so OK)
          GETATTR = OK
```

**Test 27: `test_restorefh_into_restricted_export`**

```
Compound: PUTFH(secure_fh), SAVEFH, PUTROOTFH, RESTOREFH, GETATTR
Flavor:   AUTH_SYS
Export:   /secure (KRB5), root (SYS+KRB5)

Expected: PUTFH = OK (first of two put-FH, ignored per S2.6.3.1.1.2)
          SAVEFH = OK (transparent, look past)
          PUTROOTFH = OK (last put-FH; next real op past SAVEFH is
                          RESTOREFH; RESTOREFH is a put-FH op itself --
                          so two put-FH in a row: PUTROOTFH ignored,
                          RESTOREFH is the effective one)
          RESTOREFH = NFS4ERR_WRONGSEC (restores secure_fh;
                       /secure requires KRB5, next is GETATTR
                       which is "anything else")
```

### S2.6.3.1.1.3 -- OPEN by Component Name

**Test 29: `test_putrootfh_open_by_name_wrongsec`**

S2.6.3.1.1.3 explicitly includes "OPEN of an Existing Name."
OPEN with CLAIM_NULL (component name) follows the same rules as
LOOKUP -- put-FH must NOT return WRONGSEC, OPEN itself returns it.

```
Compound: PUTROOTFH, OPEN(CLAIM_NULL, "secure/file")
Flavor:   AUTH_SYS
Export:   root (SYS+KRB5), /secure (KRB5 only)
Expected: PUTROOTFH = OK (followed by OPEN-by-name, S2.6.3.1.1.3)
          OPEN = NFS4ERR_WRONGSEC (target in restricted export)
```

NOTE: This test may be complex to set up (needs a file to exist
in /secure).  If infeasible for the unit test, defer to CI
integration.  But the rule must be encoded in
`nfs4_putfh_should_check_wrongsec()`.

**Test 30: `test_putfh_open_by_fh_wrongsec`**

OPEN-by-filehandle (CLAIM_FH, CLAIM_DELEG_CUR_FH, etc.) is
"Anything Else" per S2.6.3.1.1.7.  Put-FH MUST return WRONGSEC.

```
Compound: PUTFH(secure_fh), OPEN(CLAIM_FH)
Flavor:   AUTH_SYS
Export:   /secure (KRB5 only)
Expected: PUTFH = NFS4ERR_WRONGSEC
```

The `nfs4_putfh_should_check_wrongsec()` function must inspect
the OPEN claim type to distinguish CLAIM_NULL (skip check) from
CLAIM_FH (enforce check).

### S2.6.3.1.2 -- LINK

**Test 31: `test_link_cross_export_xdev`**

LINK uses saved + current FH across exports.  Same rules as
RENAME per S2.6.3.1.2.

```
Compound: PUTROOTFH, SAVEFH, PUTFH(secure_fh), LINK("newname")
Flavor:   AUTH_SYS
Export:   root (SYS+KRB5), /secure (KRB5)
Expected: PUTROOTFH = OK
          SAVEFH = OK
          PUTFH = NFS4ERR_WRONGSEC (next op is LINK, "anything else")
```

### RESTOREFH as First of Two Put-FH Ops

**Test 32: `test_restorefh_then_putfh_first_ignored`**

RESTOREFH is a put-FH op (RFC 8881 S2.6.3.1.1, line 1441).
When followed by another put-FH, it is the first of two and
MUST NOT return WRONGSEC per S2.6.3.1.1.2.

```
Compound: PUTROOTFH, SAVEFH, PUTFH(secure_fh), SECINFO("x"),
          RESTOREFH, PUTROOTFH, GETATTR
Flavor:   AUTH_SYS
Export:   root (SYS+KRB5), /secure (KRB5)
Expected: PUTROOTFH = OK, SAVEFH = OK, PUTFH = OK (SECINFO next)
          SECINFO = OK, RESTOREFH = OK (first of two put-FH)
          PUTROOTFH = OK (root allows SYS, GETATTR follows)
          GETATTR = OK
```

### SECINFO_NO_NAME with PARENT Style

**Test 33: `test_putfh_secinfo_no_name_parent_no_wrongsec`**

SECINFO_NO_NAME with SECINFO_STYLE4_PARENT style is the mechanism
for discovering the parent directory's flavor policy (RFC 8881
S2.6.3.1.1.4).  Put-FH must NOT return WRONGSEC.

```
Compound: PUTFH(secure_fh), SECINFO_NO_NAME(SECINFO_STYLE4_PARENT)
Flavor:   AUTH_SYS
Export:   /secure (KRB5 only)
Expected: PUTFH = OK
          SECINFO_NO_NAME = OK (returns parent's flavor list)
```

### No Flavors Configured

**Test 28: `test_no_flavors_allows_all`**

When an export has no configured flavors (nflavors=0), everything
is allowed.

```
Compound: PUTFH(no_flavor_fh), GETATTR
Flavor:   AUTH_NONE
Export:   no flavors configured
Expected: PUTFH = OK
          GETATTR = OK
```

## Implementation Notes

### How dispatch_compound interacts with WRONGSEC

The WRONGSEC check is done inside each put-FH op handler (PUTFH,
PUTROOTFH, PUTPUBFH, RESTOREFH) and inside LOOKUP/LOOKUPP.
`nfs4_check_wrongsec()` calls `next_op_is_secinfo()` which
implements the SAVEFH-transparent lookahead.

The current implementation checks WRONGSEC in each put-FH op
unconditionally.  For S2.6.3.1.1.2 (consecutive put-FH ops, only
last one checks) and S2.6.3.1.1.3 (put-FH + LOOKUP, put-FH skips
check), the `next_op_is_secinfo()` function handles the SECINFO
case, but we need additional logic for:

1. **Put-FH + LOOKUP/LOOKUPP/OPEN-by-name**: The put-FH must NOT
   check WRONGSEC.  Currently PUTFH calls `nfs4_check_wrongsec()`
   before it checks what follows.

2. **Put-FH + Nothing**: The put-FH must NOT check WRONGSEC when
   it is the last op.  `next_op_is_secinfo()` already returns
   false for end-of-compound, but we need to also skip the check
   in this case.

3. **Two consecutive put-FH ops**: First put-FH must NOT check.
   Need `next_op_is_putfh()` check.

4. **OPEN-by-FH vs OPEN-by-name**: The function must inspect the
   OPEN args to check the claim type.  CLAIM_NULL and
   CLAIM_DELEGATE_CUR (component name) follow S2.6.3.1.1.3.
   CLAIM_FH, CLAIM_DELEG_CUR_FH, CLAIM_DELEG_PREV_FH follow
   S2.6.3.1.1.7 ("anything else").

These tests will likely expose that the current implementation
needs a refactored check.  The fix is to replace the inline
`nfs4_check_wrongsec()` call in each put-FH handler with a
smarter dispatcher-level check.

### Implementation bug: PUTFH checks the wrong superblock

**BLOCKER** (from reviewer): In `filehandle.c` line 66, PUTFH
calls `nfs4_check_wrongsec(compound)` BEFORE updating
`compound->c_curr_sb` to the target filehandle's superblock
(lines 70-83).  This means PUTFH checks the PREVIOUS export's
flavor list, not the target export's.

Fix: The refactored `nfs4_putfh_should_check_wrongsec()` must
resolve the target sb from the PUTFH args before checking
flavors, OR the WRONGSEC check in PUTFH must be moved to after
the sb update.  The cleanest approach: move the WRONGSEC call
in PUTFH to after the sb and inode are resolved (after line 95),
guarded by `nfs4_putfh_should_check_wrongsec()`.

For PUTROOTFH and PUTPUBFH, the target sb is always
SUPER_BLOCK_ROOT_ID, so the check can resolve it deterministically.

For RESTOREFH, the target sb is `compound->c_saved_sb`, which is
already known at check time.

### Implementation bug: LOOKUP double WRONGSEC check

LOOKUP at `dir.c` line 54 calls `nfs4_check_wrongsec()` against
the parent directory's export BEFORE the lookup.  The second check
at line 143 (after mount-point crossing) correctly checks the
child export.  The first check at line 54 is wrong per
S2.6.3.1.1.3 -- the RFC says LOOKUP returns WRONGSEC based on
the child's policy, not the parent's.  However, when no mount
crossing occurs, LOOKUP stays in the same export and the parent
check is harmless.  Still, the check at line 54 should be removed
to match the RFC precisely: LOOKUP's WRONGSEC comes from the
child export after crossing, or not at all if no crossing occurs.

### Implementation bug: LOOKUPP checks wrong export

LOOKUPP at `dir.c` line 178 calls `nfs4_check_wrongsec()` against
the current (child) export BEFORE crossing to the parent.  Per
S2.6.3.1.1.4, LOOKUPP can return WRONGSEC, but the check should
be against the parent export AFTER crossing, not the child export
before.  Move the check to after the parent sb is resolved.

### Proposed refactoring: `nfs4_putfh_should_check_wrongsec()`

```c
/*
 * Determine whether the current put-FH op should enforce WRONGSEC.
 *
 * Returns true if WRONGSEC enforcement applies, false if the put-FH
 * must silently succeed per RFC 8881 S2.6.3.1.
 *
 * The "next real op" is determined by skipping all intervening SAVEFH
 * ops (S2.6.3.1.1.1: SAVEFH is transparent).
 *
 * Rules (in priority order):
 * 1. If next real op is SECINFO/SECINFO_NO_NAME: false (S2.6.3.1.1.5)
 * 2. If next real op is LOOKUP or LOOKUPP: false (S2.6.3.1.1.3/4)
 * 3. If next real op is OPEN with CLAIM_NULL or CLAIM_DELEGATE_CUR
 *    (component name open): false (S2.6.3.1.1.3)
 * 4. If next real op is another put-FH (PUTFH, PUTROOTFH, PUTPUBFH,
 *    RESTOREFH): false (S2.6.3.1.1.2)
 * 5. If this is the last op (nothing follows): false (S2.6.3.1.1.6)
 * 6. Otherwise (including OPEN by FH): true (S2.6.3.1.1.7)
 */
static bool nfs4_putfh_should_check_wrongsec(struct compound *compound);
```

This centralizes the RFC logic and is testable in isolation.
The function must inspect OPEN args when the next real op is OPEN,
to distinguish component-name opens (rules like LOOKUP) from
filehandle opens ("anything else").

### Test file structure

The test uses `nfs4_test_setup`/`nfs4_test_teardown` from the
harness.  After setup:

1. Create `/secure` directory in root sb
2. Allocate child sb (id=42), set flavors to [KRB5], mount at /secure
3. For tests needing a third sb: create `/sysonly`, mount sb_id=43
   with flavors=[SYS]

The `make_wrongsec_compound()` helper sets `rt_info.ri_cred.rc_flavor`
to the desired auth flavor, and optionally `rc_gss.gc_svc` for GSS
service level.

### Existing tests affected: NONE

All tests are new.  No existing test file is modified.

## Test Priority

**Phase 1** (core rules, most likely to catch implementation bugs):
- Tests 6, 7, 8 (S2.6.3.1.1.3 -- LOOKUP mount crossing)
- Tests 9, 10 (S2.6.3.1.1.4 -- LOOKUPP, exposes dir.c:178 bug)
- Tests 11, 12 (S2.6.3.1.1.5 -- SECINFO lookahead)
- Tests 14, 15 (S2.6.3.1.1.6 -- put-FH + nothing)
- Tests 16, 17, 18, 19, 20 (S2.6.3.1.1.7 -- put-FH + anything else)
- Tests 26, 27 (RESTOREFH)
- Test 28 (no flavors configured)

**Phase 2** (SAVEFH transparency, consecutive put-FH, OPEN):
- Tests 1, 2, 3 (S2.6.3.1.1.1 -- SAVEFH transparent)
- Tests 4, 5 (S2.6.3.1.1.2 -- consecutive put-FH)
- Tests 29, 30 (OPEN by name vs OPEN by FH)
- Test 32 (RESTOREFH as first of two put-FH)
- Test 33 (SECINFO_NO_NAME PARENT style)

**Phase 3** (SECINFO FH consumption, cross-export ops):
- Tests 21, 22 (S2.6.3.1.1.8 -- SECINFO consumes FH)
- Tests 23, 24, 25 (S2.6.3.1.2 -- RENAME cross-export)
- Test 31 (LINK cross-export)

## Verification

1. `make -j$(nproc)` -- zero errors, zero warnings
2. `make check` -- all existing + new tests pass
3. `make -f Makefile.reffs fix-style` -- clean

## Key Files

| File | Action |
|------|--------|
| `lib/nfs4/tests/wrongsec_test.c` | NEW |
| `lib/nfs4/tests/Makefile.am` | ADD wrongsec_test |
| `lib/nfs4/server/security.c` | MODIFY (refactor check) |
| `lib/nfs4/server/filehandle.c` | MODIFY (use new check) |
| `lib/nfs4/server/dir.c` | MODIFY (remove LOOKUP line 54 check, move LOOKUPP check) |
| `lib/nfs4/server/file.c` | VERIFY (OPEN WRONGSEC check) |

## RFC References

- RFC 8881 S2.6.3.1: Using NFS4ERR_WRONGSEC, SECINFO, and SECINFO_NO_NAME
- RFC 8881 S2.6.3.1.1.1: Put FH + SAVEFH
- RFC 8881 S2.6.3.1.1.2: Two or More Put FH Operations
- RFC 8881 S2.6.3.1.1.3: Put FH + LOOKUP (or OPEN by name)
- RFC 8881 S2.6.3.1.1.4: Put FH + LOOKUPP
- RFC 8881 S2.6.3.1.1.5: Put FH + SECINFO/SECINFO_NO_NAME
- RFC 8881 S2.6.3.1.1.6: Put FH + Nothing
- RFC 8881 S2.6.3.1.1.7: Put FH + Anything Else
- RFC 8881 S2.6.3.1.1.8: Operations after SECINFO and SECINFO_NO_NAME
- RFC 8881 S2.6.3.1.2: LINK and RENAME

## Reviewer Findings (2026-04-04)

The reviewer validated this plan against the RFC and found:

### BLOCKERs (fixed in this revision)

1. **PUTFH checks the wrong superblock**: `filehandle.c` line 66
   calls `nfs4_check_wrongsec()` before `c_curr_sb` is updated.
   Fix documented in Implementation Notes.

2. **Missing OPEN-by-name coverage**: S2.6.3.1.1.3 explicitly
   includes OPEN.  Tests 29 and 30 added.

3. **`nfs4_putfh_should_check_wrongsec()` must handle OPEN claim
   types**: CLAIM_NULL = skip check (like LOOKUP), CLAIM_FH =
   enforce check ("anything else").  Updated in proposal.

### WARNINGs (fixed in this revision)

4. **LOOKUP double WRONGSEC check**: `dir.c` line 54 checks the
   parent's flavor, which is wrong per S2.6.3.1.1.3.  Fix noted.

5. **LOOKUPP checks wrong export**: `dir.c` line 178 checks the
   child export before crossing.  Should check parent after
   crossing.  Tests 9/10 added to Phase 1.

6. **Missing LINK coverage**: Test 31 added.

7. **Missing RESTOREFH-as-first-put-FH test**: Test 32 added.

8. **Test 9 expected result was ambiguous**: Made definitive (OK).

### NOTEs

9. SECINFO_NO_NAME PARENT style test added (Test 33).
10. OPEN-by-FH test added (Test 30).
11. Test priority updated to include LOOKUPP in Phase 1.
