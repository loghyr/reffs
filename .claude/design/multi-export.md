# Multi-Export Design

## Terminology

- **export**: a named subtree of the server namespace with its own
  security policy, layout policy, and backend configuration.
  Identified by an NFSv4 fsid.  Maps to `struct super_block` internally.
- **dstore**: a DS address + export path pair (backing storage for
  data files).  Hammerspace calls these "volumes".
- **pseudo-root**: the synthetic top-level export that aggregates
  child exports into a single namespace.  Always exists, always
  sb_id=1.

## Current State

reffs has a single export (sb_id=1) plus optionally a DS namespace
(sb_id=2) in combined mode.  The config has one `[export]` section
with one flavor list.  All files share the same security and layout
policy.

## Goal

Multiple exports, each with independent:

- **Security flavors**: `["krb5"]`, `["tls"]`, `["sys"]`, or
  combinations
- **Layout policy**: flex files v1, flex files v2, or none (inband I/O)
- **Backend**: POSIX, RocksDB (future)
- **Dstore set**: which data servers back this export's layouts

## Config

```toml
[[export]]
name = "secure"
path = "/secure"
flavors = ["krb5"]
layout = "ffv1"
data_servers = [1, 2, 3, 4, 5, 6]

[[export]]
name = "public"
path = "/public"
flavors = ["sys", "tls"]
layout = "none"

[[export]]
name = "fast"
path = "/fast"
flavors = ["sys"]
layout = "ffv2"
data_servers = [1, 2, 3, 4, 5, 6]
```

The pseudo-root (`/`) is implicit.  Its flavor list is the union
of all child exports' flavors.  SECINFO on the pseudo-root
advertises all flavors; SECINFO on a child export advertises only
that export's flavors.

## Security Flavor Inheritance Rule

A parent export's flavor set must be a **superset** of every child
export's flavor set.  This ensures a client can always traverse
from parent to child without hitting an unexpected WRONGSEC:

```
/           → ["krb5", "sys", "tls"]   (superset)
/secure     → ["krb5"]                  (subset ✓)
/public     → ["sys", "tls"]            (subset ✓)
/fast       → ["sys"]                   (subset ✓)
```

If a child requires a flavor the parent doesn't have, config
validation rejects it at startup.

### WRONGSEC at export boundaries

When a compound crosses an fsid boundary (LOOKUP into a child
export), the server checks the new export's flavor list against
the client's credential.  If the client authenticated with `sys`
and enters `/secure` (krb5 only), the server returns
NFS4ERR_WRONGSEC.  The client then uses SECINFO to discover krb5
is required and re-authenticates.

This is standard NFSv4 behavior (RFC 8881 §2.6.3.1) and already
works in reffs via `nfs4_check_wrongsec()` — it just needs to
check the **current export's** flavor list rather than a global one.

## Internal Changes

### Per-super_block security state

Currently `server_state` holds one global `ss_flavors[]` and
`ss_nflavors`.  Change to per-super_block:

```c
struct super_block {
    ...
    enum reffs_auth_flavor *sb_flavors;
    unsigned int            sb_nflavors;
    layouttype4             sb_layout_type;  /* or LAYOUT4_NONE */
    uint32_t               *sb_dstore_ids;   /* which dstores */
    unsigned int            sb_ndstores;
};
```

`nfs4_check_wrongsec()` changes from:
```c
struct server_state *ss = server_state_find();
for (i = 0; i < ss->ss_nflavors; i++) ...
```
to:
```c
struct super_block *sb = compound->c_curr_sb;
for (i = 0; i < sb->sb_nflavors; i++) ...
```

### LAYOUTGET per-export

`nfs4_op_layoutget()` currently uses the global layout_width and
dstore set.  Change to consult `compound->c_curr_sb->sb_layout_type`
and `sb_dstore_ids`.  If `sb_layout_type == LAYOUT4_NONE`, return
NFS4ERR_LAYOUTUNAVAILABLE.

### GETDEVICEINFO

Device info is already dstore-based and independent of exports.
No change needed — the client gets device info for dstores
referenced in the layout, regardless of which export issued it.

### Pseudo-filesystem traversal

The pseudo-root already exists (sb_id=1).  Child exports are
created as additional super_blocks with unique sb_ids.  LOOKUP
across an fsid boundary triggers the standard NFSv4 fsid change
handling (client sees different fsid in post-op attrs).

Currently `super_block_find()` looks up by sb_id.  No change
needed.  The config loader creates multiple super_blocks at
startup instead of one.

### MOUNT (NFSv3)

NFSv3 MOUNT returns the root FH of the requested export path.
`find_matching_directory_entry()` already walks the namespace —
it just needs to resolve the correct super_block for the path.
The MOUNT response auth_flavors list comes from the matched
export's `sb_flavors`.

## Implementation Order

1. **Per-sb flavor list** — move `ss_flavors` into `struct super_block`
2. **Config parser** — support `[[export]]` array (TOML array of tables)
3. **Multi-sb creation** — instantiate super_blocks from config
4. **WRONGSEC per-export** — `nfs4_check_wrongsec` uses `c_curr_sb`
5. **SECINFO per-export** — return export-specific flavor list
6. **MOUNT per-export** — return export-specific auth flavors
7. **LAYOUTGET per-export** — check `sb_layout_type`, use `sb_dstore_ids`
8. **Pseudo-root flavor union** — auto-compute from children at startup
9. **Config validation** — reject child flavors not in parent

## Interaction with Identity

Each export can have different identity requirements:

- A krb5 export needs name-aware identity (owner strings with
  `user@domain`)
- A sys-only export works fine with numeric uid/gid
- A TLS export is AUTH_SYS over encrypted transport — numeric
  uid/gid

The identity system (design/identity.md) handles this naturally:
the `reffs_id` type is stored on the inode regardless of which
export it was created through.  Owner string conversion
(`reffs_id` ↔ `user@domain` or numeric) is a presentation-layer
concern handled in GETATTR/SETATTR, not a storage concern.

## Interaction with pNFS Flex Files + Kerberos

Flex Files layouts over krb5-authenticated DSes is an unsolved
problem in the NFS community.  The fundamental issue: the MDS
issues a layout containing DS addresses + filehandles, but the
client needs credentials to access the DS.  With AUTH_SYS, the
layout carries synthetic uid/gid (fencing creds).  With krb5,
the client would need a service ticket for each DS — but the MDS
can't grant those.

Options (all deferred, none implemented anywhere):

1. **Proxy tickets**: MDS uses constrained delegation to obtain
   DS service tickets on behalf of the client.  Requires AD
   trust relationships.
2. **Session-bound tokens**: MDS issues opaque tokens the DS
   validates via a back-channel to the MDS.  Proprietary.
3. **AUTH_SYS fallback for DS I/O**: client uses krb5 to MDS
   but AUTH_SYS (with fencing creds) to DSes.  The DS trusts
   the MDS, not the client.  This is what current implementations
   do in practice.

reffs uses option 3 today.  The krb5 export authenticates the
client to the MDS; DS I/O uses AUTH_SYS with synthetic fencing
credentials from the layout.  This is secure as long as the DS
network is trusted (same DC, VLAN isolation, or TLS to DSes).

## Non-Goals (for now)

- Cross-export hard links (different fsids = different filesystems)
- Export-level quotas
- Per-export ACL policy (mode bits vs NFSv4 ACLs)
- Dynamic export creation (config reload without restart)
