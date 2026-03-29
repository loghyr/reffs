<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Export Management Guide

How to create, configure, and manage exports (superblocks) in reffs.

## Concepts

An **export** is a named subtree of the server namespace backed by a
`struct super_block`. Each export has:

- A unique numeric **sb_id** (1 is the root pseudo-export)
- A **mount path** in the namespace (`/`, `/secure`, `/public`, etc.)
- A **lifecycle state**: CREATED → MOUNTED → UNMOUNTED → DESTROYED
- A **security flavor list** (AUTH_SYS, krb5, krb5i, krb5p, TLS)
- A **storage backend** (RAM or POSIX)

The **root export** (sb_id=1) is auto-created at `/` during
`reffs_ns_init()`. It cannot be unmounted or destroyed.

## Lifecycle

```
            create()
    ───────────────► CREATED
                        │
                        │ mount(path)
                        ▼
                     MOUNTED  ◄── clients can access
                        │
                        │ unmount()
                        ▼
                    UNMOUNTED  ◄── existing FH still works
                        │
                        │ destroy()
                        ▼
                    DESTROYED  ◄── FH → NFS4ERR_STALE
```

### Transition rules

| Transition | Condition | Error on failure |
|-----------|-----------|-----------------|
| → CREATED | `super_block_alloc()` succeeds | NULL return |
| CREATED → MOUNTED | `super_block_mount(sb, path)`, path must be an existing directory | -ENOENT, -ENOTDIR |
| MOUNTED → MOUNTED | prohibited | -EBUSY |
| MOUNTED → UNMOUNTED | `super_block_unmount(sb)`, no child sb mounted | -EBUSY, -EPERM (root) |
| UNMOUNTED → MOUNTED | `super_block_mount(sb, path)` (re-mount, may use different path) | -ENOENT |
| UNMOUNTED → DESTROYED | `super_block_destroy(sb)` | -EPERM (root) |
| CREATED → DESTROYED | `super_block_destroy(sb)`, always OK | — |

## C API Reference

### Creating an export

```c
#include "reffs/super_block.h"
#include "reffs/dirent.h"

/* 1. Allocate the superblock. */
struct super_block *sb = super_block_alloc(
    42,                     /* sb_id — unique, > 2 */
    "/secure",              /* mount path */
    REFFS_STORAGE_RAM,      /* or REFFS_STORAGE_POSIX */
    NULL                    /* backend_path (NULL for RAM) */
);
if (!sb)
    return -ENOMEM;

/* 2. Create the root dirent + inode for this export. */
int ret = super_block_dirent_create(sb, NULL, reffs_life_action_birth);
if (ret) {
    super_block_put(sb);
    return ret;
}

/* 3. Configure security flavors (before mounting). */
enum reffs_auth_flavor flavors[] = { REFFS_AUTH_KRB5 };
super_block_set_flavors(sb, flavors, 1);

/* 4. Mount — makes the export visible to NFS clients. */
ret = super_block_mount(sb, "/secure");
if (ret) {
    super_block_release_dirents(sb);
    super_block_put(sb);
    return ret;
}
/* sb is now MOUNTED. Clients doing LOOKUP("secure") from the
 * root will cross into this export's namespace. */
```

### Modifying the root export's security policy

```c
struct super_block *root = super_block_find(SUPER_BLOCK_ROOT_ID);

/* Allow AUTH_SYS and all Kerberos variants. */
enum reffs_auth_flavor root_flavors[] = {
    REFFS_AUTH_SYS,
    REFFS_AUTH_KRB5,
    REFFS_AUTH_KRB5I,
    REFFS_AUTH_KRB5P,
};
super_block_set_flavors(root, root_flavors, 4);

super_block_put(root);
```

> **Note**: The root export's flavor list should be a superset of
> all child exports' flavors. Use `super_block_lint_flavors()` to
> check for inconsistencies after configuration changes.

### Listing all exports

```c
#include "reffs/super_block.h"

struct cds_list_head *sb_list = super_block_list_head();
struct super_block *sb;

rcu_read_lock();
cds_list_for_each_entry_rcu(sb, sb_list, sb_link) {
    printf("sb_id=%lu path=%s state=%s nflavors=%u\n",
           (unsigned long)sb->sb_id,
           sb->sb_path ? sb->sb_path : "(none)",
           super_block_lifecycle_name(sb->sb_lifecycle),
           sb->sb_nflavors);
}
rcu_read_unlock();
```

### Querying a specific export

```c
struct super_block *sb = super_block_find(42);
if (!sb) {
    /* sb_id 42 does not exist */
    return -ENOENT;
}

printf("id=%lu state=%s path=%s flavors=%u\n",
       (unsigned long)sb->sb_id,
       super_block_lifecycle_name(sb->sb_lifecycle),
       sb->sb_path,
       sb->sb_nflavors);

super_block_put(sb);  /* always drop the find ref */
```

### Unmounting and destroying an export

```c
struct super_block *sb = super_block_find(42);
if (!sb)
    return -ENOENT;

/* 1. Unmount — clients still holding FHs can access,
 *    but LOOKUP no longer crosses into this export. */
int ret = super_block_unmount(sb);
if (ret == -EBUSY) {
    /* A child export is mounted within this one.
     * Unmount children first (bottom-up). */
    super_block_put(sb);
    return ret;
}

/* 2. Destroy — after this, FHs return NFS4ERR_STALE. */
ret = super_block_destroy(sb);

/* 3. Release dirent tree and drop the sb ref. */
super_block_release_dirents(sb);
super_block_put(sb);
```

### Linting flavor consistency

```c
int warnings = super_block_lint_flavors();
if (warnings > 0)
    printf("%d flavor inconsistencies found\n", warnings);
```

The lint scans all mounted child exports and checks that each
child's required flavors are present in its parent's flavor
list. Warnings are logged via `LOG()`. A warning means a
client authenticated to the parent might not be able to access
the child without re-authenticating — which is valid NFSv4
behavior (WRONGSEC + SECINFO negotiation), but may indicate a
misconfiguration.

### Persisting exports across restarts

```c
#include "reffs/sb_registry.h"

/* Save all non-root exports to the registry. */
int ret = sb_registry_save("/var/lib/reffs/mds");

/* On restart, load the registry to recreate exports. */
ret = sb_registry_load("/var/lib/reffs/mds");
if (ret == -ENOENT)
    /* Fresh start — no registry file. */;

/* Check for orphaned sb directories not in the registry. */
int orphans = sb_registry_detect_orphans("/var/lib/reffs/mds");
```

The registry uses a binary format (header + fixed-size entries)
written via write-temp/fdatasync/rename for crash safety. Each
entry stores the sb_id, lifecycle state, storage type, and mount
path.

## TOML Configuration

Exports can also be configured in the TOML config file:

```toml
[[export]]
path = "/secure"
flavors = ["krb5"]
read_only = false
root_squash = false

[[export]]
path = "/public"
flavors = ["sys", "tls"]
read_only = true

[[export]]
path = "/fast"
flavors = ["sys"]
```

At startup, `reffsd` creates superblocks for each `[[export]]`
entry and copies the first export's flavors to the global
`server_state` as a fallback.

## Security Flavor Reference

| Config name | Enum | Wire value | Description |
|------------|------|-----------|-------------|
| `sys` | `REFFS_AUTH_SYS` | AUTH_SYS (1) | Standard UNIX uid/gid |
| `krb5` | `REFFS_AUTH_KRB5` | RPCSEC_GSS | Kerberos authentication only |
| `krb5i` | `REFFS_AUTH_KRB5I` | RPCSEC_GSS+INTEGRITY | Kerberos with integrity |
| `krb5p` | `REFFS_AUTH_KRB5P` | RPCSEC_GSS+PRIVACY | Kerberos with encryption |
| `tls` | `REFFS_AUTH_TLS` | pseudo-flavor | AUTH_SYS over TLS transport |

### Flavor enforcement

When a client accesses an export, `nfs4_check_wrongsec()` checks
the client's RPC credential against the export's flavor list:

1. If the export has per-sb flavors (`sb->sb_nflavors > 0`), use
   those.
2. Otherwise, fall back to global `server_state->ss_flavors`.
3. If no flavors are configured at all, allow everything.

On mismatch, the server returns `NFS4ERR_WRONGSEC`. The client
then uses `SECINFO` or `SECINFO_NO_NAME` to discover the
required flavors and re-authenticates.

### Flavor inheritance model

There is **no automatic flavor inheritance**. Each export's
flavors are set explicitly. The root export should be configured
with the superset of all child flavors so clients can reach any
child. Use `super_block_lint_flavors()` to verify.

## Mount-Point Semantics

When a child export is mounted at a path (e.g., `/secure`):

- **LOOKUP** into the mount point crosses into the child sb's
  root inode. The compound's `c_curr_sb` switches to the child.
- **LOOKUPP** from the child sb's root crosses back to the
  parent sb's mounted-on directory.
- **READDIR** entries at mount points show the child sb's fsid
  (so the client detects the filesystem boundary).
- **GETATTR** on the child root's `mounted_on_fileid` returns
  the covered directory's fileid from the parent fs (RFC 8881
  §5.8.2.19).
- **RENAME/LINK** across export boundaries returns
  `NFS4ERR_XDEV`.
- **rmdir** on a mounted directory returns `-EBUSY`.

## Probe Protocol (Runtime Admin)

The probe protocol (program 211768, port 20490) provides runtime
export management without restarting the server.  Both Python and
C clients are available.

### Python CLI (`reffs-probe.py`) — Primary Admin Tool

```bash
# List all exports (shows ID, UUID, path, state, flavors)
reffs-probe.py sb-list

# Create a new export (server assigns ID + UUID; mkdir -p on the path)
reffs-probe.py sb-create --path /secure --storage ram

# Configure security flavors
reffs-probe.py sb-set-flavors --id 42 --flavors krb5 krb5p

# Mount (makes export visible to NFS clients)
reffs-probe.py sb-mount --id 42 --path /secure

# Query export details (shows UUID)
reffs-probe.py sb-get --id 42

# Check flavor consistency across all exports
reffs-probe.py sb-lint-flavors

# Unmount (stops new traversals, existing FHs still work)
reffs-probe.py sb-unmount --id 42

# Destroy (FH → NFS4ERR_STALE after this)
reffs-probe.py sb-destroy --id 42
```

### C CLI (`reffs_probe1_clnt`)

```bash
reffs_probe1_clnt --op sb-list
reffs_probe1_clnt --op sb-create --sb-path /secure --storage-type 0
reffs_probe1_clnt --op sb-mount --sb-id 42 --sb-path /secure
reffs_probe1_clnt --op sb-get --sb-id 42
reffs_probe1_clnt --op sb-unmount --sb-id 42
reffs_probe1_clnt --op sb-destroy --sb-id 42
reffs_probe1_clnt --op sb-lint-flavors
```

For flavor management, use the Python CLI (`--flavors` parsing
is Python-only).

### Per-SB Stats in Existing Commands

The existing stats commands now include per-sb breakdowns:

```bash
# FS usage with per-sb breakdown
reffs-probe.py fs-usage

# NFS4 per-op stats with per-sb breakdown
reffs-probe.py nfs4-op-stats

# Layout errors (per-sb array populated from sb_layout_errors)
reffs-probe.py layout-errors
```

### Python Integration Test

```bash
# Exercises full SB lifecycle: create → set-flavors → mount →
# list → per-sb stats → lint → unmount → destroy
python3 scripts/test_sb_probe.py [--host HOST] [--port PORT]
```

## Common Patterns

### Multi-tenant with separate security policies

```c
/* Root accepts everything. */
enum reffs_auth_flavor root_f[] = {
    REFFS_AUTH_SYS, REFFS_AUTH_KRB5, REFFS_AUTH_KRB5P
};
super_block_set_flavors(root_sb, root_f, 3);

/* /secure requires Kerberos with encryption. */
enum reffs_auth_flavor secure_f[] = { REFFS_AUTH_KRB5P };
super_block_set_flavors(secure_sb, secure_f, 1);

/* /public allows AUTH_SYS. */
enum reffs_auth_flavor public_f[] = { REFFS_AUTH_SYS };
super_block_set_flavors(public_sb, public_f, 1);
```

### Graceful export removal

```c
/* 1. Unmount to stop new traversals. */
super_block_unmount(sb);

/* 2. Wait for active clients to drain (polling). */
/* NOT_NOW_BROWN_COW: DRAINING state for graceful teardown. */

/* 3. Destroy when no open files remain. */
ret = super_block_destroy(sb);
if (ret == -EBUSY)
    /* Still has open files — retry later. */;

/* 4. Clean up. */
super_block_release_dirents(sb);
super_block_put(sb);

/* 5. Persist the change. */
sb_registry_save(state_dir);
```

### Bottom-up unmount for nested exports

```c
/* /data has /data/archive mounted inside it.
 * Must unmount /data/archive first. */
super_block_unmount(archive_sb);  /* child first */
super_block_unmount(data_sb);     /* then parent */
```

Attempting to unmount `/data` while `/data/archive` is still
mounted returns `-EBUSY` (NOT_NOW_BROWN_COW: this check is
deferred — currently the unmount succeeds without checking for
child mounts).
