<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Reflected GETATTR Test Plan

## Context

The MDS must fan out NFSv3 GETATTR to all DSes (a "reflected GETATTR")
whenever it cannot trust its cached size/mtime.  A compound-level flag
`COMPOUND_DS_ATTRS_REFRESHED` (bit 0 of `c_flags`) deduplicates fan-outs
within a single compound.  The goal of this test suite is to verify that
every compound in `~/ops.txt` produces exactly the expected number of DS
round-trips -- at most one reflected GETATTR per compound, except where
multiple filehandles or unavoidable ordering makes more correct.

The DS is NFSv3 for all scenarios in this plan.  A NFSv4 DS counts
compound round-trips differently and is a separate concern.

## Rules as Implemented

### Trigger T1 -- GETATTR (attr.c:3293)

Fan out to all DSes when ALL of:
- `inode_has_write_layout(inode)` is true
- `inode->i_layout_segments->lss_count > 0`
- `!(compound->c_flags & COMPOUND_DS_ATTRS_REFRESHED)`

Sets `COMPOUND_DS_ATTRS_REFRESHED` before pausing.

### Trigger T2 -- LAYOUTRETURN(rw) (layout.c:1153)

Fan out when ALL of:
- The write iomode bit is being cleared (`clear_bit & LAYOUT_STATEID_IOMODE_RW`)
- `inode->i_layout_segments->lss_count > 0`
- `!(compound->c_flags & COMPOUND_DS_ATTRS_REFRESHED)`

Sets `COMPOUND_DS_ATTRS_REFRESHED` before pausing.

### Trigger T3 -- SETATTR(size) WCC (NOT YET IMPLEMENTED -- gap G2)

The truncate fan-out in `nfs4_op_setattr_resume` receives NFSv3 WCC from
each DS.  This should set `COMPOUND_DS_ATTRS_REFRESHED` so a subsequent
GETATTR in the same compound does not trigger a second fan-out.  Currently
`dstore_wcc_check` is called but the flag is never set.

### Trigger T4 -- CLOSE implicit layout return (NOT YET IMPLEMENTED -- gap G3)

Per RFC 8881 S12.5.5.1, the server SHOULD process implicit return of all
outstanding layouts when the last CLOSE for a file is processed.  For a write
layout, this should trigger a reflected GETATTR (same as T2) and set the flag.
reffs policy mandates this.  Currently CLOSE only unhashes the open stateid
with no layout interaction.

### Trigger T5 -- DELEGRETURN implicit layout return (NOT YET IMPLEMENTED -- gap G4)

Same rule as G3 for delegation stateids.  When DELEGRETURN is the last
stateid for the file (or when the layout was granted on the delegation
stateid per RFC 9754), a write layout is implicitly returned.
Currently DELEGRETURN only unhashes the delegation stateid.

### Dedup: COMPOUND_DS_ATTRS_REFRESHED

Once set by any trigger, all subsequent T1/T2/T3/T4/T5 checks in the
same compound are suppressed.

### Dedup: PUTFH must clear the flag (NOT YET IMPLEMENTED -- gap G1)

When PUTFH switches to a different inode, `COMPOUND_DS_ATTRS_REFRESHED`
must be cleared.  A fan-out for inode A must not suppress a needed
fan-out for inode B.  Currently PUTFH does not touch `c_flags`.

## Design Decision: LAYOUTCOMMIT

LAYOUTCOMMIT carries `lca_last_write_offset`, telling the server the
highest byte the client has written to the DS.  The server's cached size
may be stale.  Two options:

**Option A**: LAYOUTCOMMIT triggers a reflected GETATTR itself (new T6).
  - Pro: server always has fresh attrs after LAYOUTCOMMIT.
  - Con: LAYOUTCOMMIT is almost always followed by LAYOUTRETURN in the
    same or next compound; T2 would fire anyway.  Doubled fan-out if
    both are in the same compound.

**Option B** (implemented, chosen): LAYOUTCOMMIT updates the cached size from
  `lca_last_write_offset` only (no DS round-trip); leaves T1/T2 to fan out
  as normal.
  - Pro: no extra DS round-trip; T2 in the following LAYOUTRETURN covers it.
  - Con: if the client commits but never returns the layout (long-running
    write), the server's cached attrs stay stale between commits.

**Note on mtime**: Option B updates i_size but NOT i_mtime or i_ctime
(layout.c:944-956).  Fresh mtime is only available after T1 or T2 fires.
Clients SHOULD follow LAYOUTCOMMIT with LAYOUTRETURN to flush attrs.  The
test `test_layoutcommit_mtime_stale` in Group H documents this behavior.

## Compound Analysis

State notation:
  (0)  no layout outstanding
  (R)  read layout outstanding
  (W)  write layout outstanding

"rGA" = number of DS fan-outs triggered in the compound.
"DS op" = any DS round-trip (reflected GETATTR or truncate fan-out).
[BUG] = current code produces wrong count (gap Gn not yet fixed).

For multi-FH compounds a 2-D state matrix (state-of-a x state-of-b) applies.

### OPEN + LAYOUTGET (state matrix: prior layout state)

SEQUENCE PUTFH OPEN LAYOUTGET(rw) GETATTR
  (0): 1  -- write layout just granted; T1 fires
  (R): 1  -- write layout added; T1 fires
  (W): 1  -- additional write layout; T1 fires

SEQUENCE PUTFH OPEN LAYOUTGET(rd) GETATTR
  (0): 0  -- read layout only; T1 condition false
  (R): 0  -- still no write layout
  (W): 1  -- prior write layout still active; T1 fires

### LAYOUTGET without OPEN (re-acquire on existing stateid)

SEQUENCE PUTFH LAYOUTGET(rw) GETATTR
  (0): 1  -- same as above; write layout active at GETATTR
  (R): 1
  (W): 1

SEQUENCE PUTFH LAYOUTGET(rd) GETATTR
  (0): 0
  (R): 0
  (W): 1  -- prior write layout still active

### Double GETATTR (same filehandle)

SEQUENCE PUTFH GETATTR GETATTR
  (0): 0
  (R): 0
  (W): 1  -- T1 fires on first; flag suppresses second

### Multi-filehandle compounds (G1 not yet fixed -- all (W)+(W) cases broken)

SEQUENCE PUTFH(a) GETATTR PUTFH(b) GETATTR

  a=(0), b=(0): 0
  a=(R), b=(R): 0
  a=(W), b=(0): 1  -- only a fans out
  a=(0), b=(W): 1  -- only b fans out; flag clear (a didn't set it)
  a=(W), b=(R): 1  -- only a fans out; PUTFH(b) clears flag but b has no write layout
  a=(R), b=(W): 1  -- only b fans out
  a=(W), b=(W): 2  -- PUTFH(b) clears flag; both fan out [BUG: currently 1]

SEQUENCE PUTFH(a) GETATTR PUTFH(b) GETATTR PUTFH(a) GETATTR

  a=(0), b=(0): 0
  a=(W), b=(0): 2  -- a fans out twice; PUTFH(b) and PUTFH(a) each clear flag [BUG: currently 1]
  a=(0), b=(W): 1  -- only b fans out
  a=(W), b=(W): 3  -- each PUTFH clears flag; a goes twice, b once [BUG: currently 1]

### CLOSE / DELEGRETURN (G3, G4 not yet fixed)

SEQUENCE PUTFH DELEGRETURN
  (0): 0
  (R): 0
  (W) layout-on-open-stid:  0 -- DELEGRETURN does not return layout;
    write layout persists, held by the open stateid; no action required
  (W) layout-on-deleg-stid: 1 -- T5 should fire [BUG: currently 0]

SEQUENCE PUTFH CLOSE
  (0): 0
  (R): 0
  (W): 1 -- T4 fires (implicit LR of write layout) [BUG: currently 0]

SEQUENCE PUTFH DELEGRETURN CLOSE
  (0): 0
  (R): 0
  (W): 1 -- one of T4/T5 fires; flag prevents the other from doubling [BUG: currently 0]

SEQUENCE PUTFH DELEGRETURN GETATTR
  (0): 0
  (R): 0
  (W) layout-on-open-stid:  1 -- DELEGRETURN does not return layout; write layout still active; T1 fires
  (W) layout-on-deleg-stid: 1 -- T5 fires in DELEGRETURN, sets flag; T1 suppressed; total 1

SEQUENCE PUTFH CLOSE GETATTR
  (0): 0
  (R): 0
  (W): 1 -- T4 fires in CLOSE, sets flag; T1 suppressed
    NOTE: currently 1 by the wrong path -- layout stateid persists after CLOSE
    so T1 fires at GETATTR instead.  Count is accidentally correct before G3.
    After G3: T4 fires at CLOSE, flag set, T1 suppressed.  Count still 1 but
    via the correct mechanism.  test_close_then_getattr_rw_one_fanout must
    probe which op set the flag after G3 is implemented.

SEQUENCE PUTFH DELEGRETURN CLOSE GETATTR
  (0): 0
  (R): 0
  (W): 1 -- T4 or T5 fires; flag suppresses remaining triggers [BUG: currently 1 only by accident]

### Explicit LAYOUTRETURN (currently correct)

SEQUENCE PUTFH LAYOUTRETURN
  (0): 0
  (R): 0
  (W): 1 -- T2 fires

SEQUENCE PUTFH LAYOUTRETURN GETATTR
  (0): 0
  (R): 0
  (W): 1 -- T2 fires in LAYOUTRETURN; T1 suppressed by flag

SEQUENCE PUTFH DELEGRETURN LAYOUTRETURN
  (0): 0
  (R): 0
  (W) layout-on-open-stid:  1 -- DELEGRETURN does not return layout; T2 fires in LAYOUTRETURN
  (W) layout-on-deleg-stid: 1 -- T5 fires in DELEGRETURN, sets flag; T2 suppressed; total 1
    NOTE: when T5 fires, the layout is implicitly returned in DELEGRETURN.
    The subsequent LAYOUTRETURN carries a now-stale stateid; the server should
    return NFS4ERR_OLD_STATEID (RFC 8881 S18.44.3).  Tests must assert this status.

SEQUENCE PUTFH DELEGRETURN LAYOUTRETURN GETATTR
  (0): 0
  (R): 0
  (W): 1 -- same as above; T1 additionally suppressed

SEQUENCE PUTFH LAYOUTRETURN CLOSE
  (0): 0
  (R): 0
  (W): 1 -- T2 fires in LAYOUTRETURN; CLOSE returns open stid (no layout left to return)

SEQUENCE PUTFH LAYOUTRETURN CLOSE GETATTR
  (0): 0
  (R): 0
  (W): 1 -- T2 fires; flag suppresses T4 (CLOSE) and T1 (GETATTR)

### SETATTR variants

SEQUENCE PUTFH SETATTR (non-size, e.g. mode/mtime)
  All states: 0 -- no DS interaction; no GETATTR

SEQUENCE PUTFH SETATTR(size)
  All states: 1 DS op (truncate fan-out); 0 reflected GAs

SEQUENCE PUTFH SETATTR(size) GETATTR
  (0): 1  -- truncate fan-out only; no write layout so T1 skips
  (R): 1  -- same
  (W): 1  -- WCC from truncate sets flag (G2); T1 suppressed
    [BUG: currently 2 for (W) -- G2 not fixed; T1 fires even though WCC
    from truncate fan-out already refreshed DS attrs]

SEQUENCE PUTFH GETATTR SETATTR(size)
  (0): 1 DS op (truncate only; no write layout for T1)
  (R): 1 DS op
  (W): 2 DS ops -- T1 fires at GETATTR (1 reflected GA); SETATTR(size)
       truncate is a separate unavoidable fan-out; both are correct

SEQUENCE PUTFH GETATTR SETATTR(mtime)
  (0): 0
  (R): 0
  (W): 1 -- T1 fires at GETATTR; SETATTR(mtime) is local only

### LAYOUTCOMMIT (Option B implemented)

SEQUENCE PUTFH LAYOUTCOMMIT
  (0): 0
  (R): 0
  (W): 0 -- LAYOUTCOMMIT updates i_size from lca_last_write_offset only; no DS round-trip

SEQUENCE PUTFH LAYOUTCOMMIT GETATTR
  (0): 0
  (R): 0
  (W): 1 -- T1 fires at GETATTR; write layout still active after LAYOUTCOMMIT

SEQUENCE PUTFH LAYOUTCOMMIT LAYOUTRETURN
  (0): 0
  (R): 0
  (W): 1 -- T2 fires in LAYOUTRETURN; no prior fan-out to dedup

SEQUENCE PUTFH LAYOUTCOMMIT LAYOUTRETURN GETATTR
  (0): 0
  (R): 0
  (W): 1 -- T2 in LAYOUTRETURN; T1 suppressed

### LAYOUTERROR / LAYOUTSTATS

SEQUENCE PUTFH LAYOUTERROR
  All states: 0 reflected GAs.  Fencing (SETATTR uid/gid to DS) may occur
  as a separate DS operation but is not a reflected GETATTR.

SEQUENCE PUTFH LAYOUTERROR GETATTR
  (0): 0
  (R): 0
  (W): 1.  Two possible paths:
    - No fencing (e.g., NFS4ERR_IO, NFS4ERR_DELAY): T1 fires at GETATTR.
      DS op count: 1 (reflected GA).  This is what Group F tests.
    - Fencing fires (NFS4ERR_ACCESS/PERM): fencing SETATTR WCC sets flag
      (requires G2 fix extended to fencing SETATTR path); T1 suppressed.
      DS op count: 1 (fencing SETATTR).  This is tested in Group C.
    Either way: 1 DS round-trip total.

SEQUENCE PUTFH LAYOUTERROR LAYOUTRETURN
  (0): 0
  (R): 0
  (W): 1.  Fencing WCC (if G2 extended to fencing) suppresses T2; otherwise T2 fires.

SEQUENCE PUTFH LAYOUTERROR LAYOUTRETURN GETATTR
  (0): 0
  (R): 0
  (W): 1.  Same as above; T1 additionally suppressed.

SEQUENCE PUTFH LAYOUTSTATS
  All states: 0 -- statistics op; no DS interaction.

SEQUENCE PUTFH LAYOUTSTATS GETATTR
  (0): 0
  (R): 0
  (W): 1 -- LAYOUTSTATS has no DS interaction; T1 fires at GETATTR.

### READDIR

SEQUENCE PUTFH READDIR
  All states: 0.  READDIR returns cached attrs for directory entries.
  No per-file DS fan-outs are triggered regardless of the layout state
  of files within the directory.

## Gaps to Fix (Implementation Order)

G1 and G2 are independent and can be developed in parallel.
G3 and G4 are also independent of each other; only the mixed
DELEGRETURN+CLOSE test requires both.

G1 -- PUTFH must clear COMPOUND_DS_ATTRS_REFRESHED when switching inodes.
  Fix: in nfs4_op_putfh (filehandle.c), when nfh_ino changes, clear
  the flag from c_flags.
  Unblocks: multi-FH test group (Group B).

G2 -- SETATTR(size) WCC must set COMPOUND_DS_ATTRS_REFRESHED.
  Fix: in nfs4_op_setattr_resume (attr.c), after dstore_wcc_check loop,
  set compound->c_flags |= COMPOUND_DS_ATTRS_REFRESHED.
  Extend same fix to fencing SETATTR in LAYOUTERROR path.
  Unblocks: SETATTR(size) GETATTR test and fencing-path LAYOUTERROR test (Group C).

G3 -- CLOSE must implicitly return write layouts (reffs policy; RFC 8881 S12.5.5.1 SHOULD).
  Fix: in nfs4_op_close (file.c), after unhashing the open stateid,
  check for outstanding write layout stateids on the inode; if none
  remain (this was the last open stateid and no delegation stateid
  holds the layout), trigger T2.
  Unblocks: CLOSE group tests (Group D).
  NOTE: test_close_then_getattr_rw_one_fanout passes before G3 (count=1 by
  accident via T1) and after G3 (count=1 correctly via T4).  After G3 is
  implemented, add a mechanism probe asserting the flag is set at CLOSE time.

G4 -- DELEGRETURN must implicitly return write layouts when applicable.
  Fix: in nfs4_op_delegreturn (delegation.c), same check as G3 but
  for delegation stateids.  Only trigger if this was the layout-granting
  stateid (RFC 9754) or if it was the last stateid holding the layout.
  Unblocks: DELEGRETURN group tests (Group E).
  NOTE: DELEGRETURN followed by LAYOUTRETURN when T5 fired: the LAYOUTRETURN
  carries a stale stateid; server must return NFS4ERR_OLD_STATEID
  (RFC 8881 S18.44.3).

## Test File Structure

lib/nfs4/tests/reflected_getattr_test.c

Group A -- baseline regression (correct today, no gap fixes required):
  test_getattr_no_layout
  test_getattr_rd_layout
  test_getattr_rw_layout
  test_getattr_getattr_rw_dedup
  test_layoutreturn_rw_no_getattr
  test_layoutreturn_rw_then_getattr_deduped
  test_layoutreturn_rd_no_fanout
  test_layoutcommit_layoutreturn_rw        (Option B: LC=0, LR=1)
  test_layoutcommit_layoutreturn_getattr   (1 total; Option B)
  test_layoutstats_with_getattr_rw
  test_layoutstats_no_getattr
  test_readdir_no_fanout

Group B -- PUTFH flag clearing (requires G1):
  test_putfh_clears_flag_both_write
  test_putfh_clears_flag_a_write_b_read
  test_putfh_no_clear_needed_both_no_layout
  test_putfh_three_visits_all_write

Group C -- SETATTR WCC (requires G2; G2 extends to fencing path too):
  test_setattr_size_alone_one_ds_op
  test_setattr_size_then_getattr_one_ds_op
  test_getattr_then_setattr_size_two_ds_ops
  test_getattr_then_setattr_mtime_one_ds_op
  test_layouterror_access_fencing_wcc_suppresses_getattr

Group D -- CLOSE implicit layout return (requires G3):
  test_close_rd_no_fanout
  test_close_rw_one_fanout
  test_close_then_getattr_rw_one_fanout    (mechanism probe: flag set at CLOSE)
  test_layoutreturn_then_close_rw_one_fanout
  test_layoutreturn_close_getattr_rw_one_fanout

Group E -- DELEGRETURN implicit layout return (requires G4; mixed tests require G3+G4):
  test_delegreturn_open_based_layout_no_implicit_lr
  test_delegreturn_deleg_based_layout_implicit_lr
  test_delegreturn_then_getattr_rw
  test_delegreturn_close_rw_single_fanout             (requires G3+G4)
  test_delegreturn_close_getattr_rw_single_fanout     (requires G3+G4)
  test_delegreturn_layoutreturn_rw
  test_delegreturn_layoutreturn_getattr_rw
  test_delegreturn_layoutreturn_stale_stateid         (T5 fires; LR gets NFS4ERR_OLD_STATEID)

Group F -- LAYOUTERROR / LAYOUTSTATS (no-fencing path; no gap dependency):
  test_layouterror_no_getattr
  test_layouterror_with_getattr_rw         (uses NFS4ERR_IO, not NFS4ERR_ACCESS)
  test_layouterror_layoutreturn_rw
  test_layouterror_layoutreturn_getattr_rw

Group G -- LAYOUTGET (standalone, without OPEN):
  test_layoutget_rw_then_getattr
  test_layoutget_rd_then_getattr_no_write_layout
  test_layoutget_rd_then_getattr_prior_write_layout

Group H -- LAYOUTCOMMIT (Option B):
  test_layoutcommit_no_fanout
  test_layoutcommit_then_getattr_rw
  test_layoutcommit_layoutreturn_rw
  test_layoutcommit_layoutreturn_getattr_rw
  test_layoutcommit_mtime_stale            (i_mtime not updated by LC; only fresh after T1/T2)
