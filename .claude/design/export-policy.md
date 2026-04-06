# Per-Client Export Policy

## Problem

The current export model has one flat set of options per export path:
one `clients` string, one `flavors[]`, one `root_squash`, one `access`.
This maps to a single NFS client specification and cannot express
`exports(5)`-style policies where different clients get different options
on the same export path.

## exports(5) Model (Subset)

From `exports(5)`, a line looks like:

```
/path  client1(options)  client2(options)  *(options)
```

Each client specification is matched independently, and the first
match wins.  We implement the following spec types (no netgroups):

| Type | Example | Priority |
|------|---------|----------|
| Single IPv4 host | `192.168.1.5` | 1 (highest) |
| Single IPv6 host | `2001:db8::1` | 1 |
| IPv4 CIDR | `192.168.1.0/24` | 2 |
| IPv6 CIDR | `2001:db8::/32` | 2 |
| Hostname wildcard | `*.lab.example.com` | 3 |
| Anonymous | `*` | 4 (lowest) |

Within the same priority level, list order (first-listed wins).

Per-client options:

| Option | Values | Default |
|--------|--------|---------|
| `access` | `"rw"`, `"ro"` | `"rw"` |
| `root_squash` | `true`, `false` | `true` |
| `all_squash` | `true`, `false` | `false` |
| `flavors` | array of flavor names | `["sys"]` |

## MDS Root Access Probe

A DS does not know it is being used as a DS -- an NFSv3 DS is an
ordinary NFS server with no special configuration.  The root_squash
setting lives in the DS export policy, which is the DS admin's
responsibility.  The MDS cannot enforce policy on another machine.

Instead, the MDS verifies root access at dstore connection time:

```
dstore_probe_root_access(ds):
  1. NFSv3 CREATE of "<mds_uuid>/.root_probe" with uid=0, gid=0
  2. If NFS3ERR_ACCES or NFS3ERR_PERM:
       LOG("DS %s:%s denies root access (root_squash likely set) -- "
           "MDS control-plane will fail; set root_squash=false "
           "for the MDS address on the DS export",
           ds->ds_address, ds->ds_path);
       return -EACCES;
  3. On success: immediately REMOVE the probe file
  4. Return 0
```

This runs in `dstore_alloc()` after the NFSv3 connection is established,
before runway setup.  A -EACCES return marks the dstore as unavailable
and LAYOUTGET skips it.

### Bread-crumb cleanup

The probe file and any runway pool files left by a prior run (crash,
unclean shutdown) are bread crumbs on the DS.  On startup, before
creating the new probe file, the MDS scans `<mds_uuid>/` on each DS:

1. Remove any `.root_probe` file found (from a prior probe that didn't
   clean up)
2. Inventory existing pool files (`pool/*.dat`) -- these are the bread
   crumbs from incomplete runway allocations; add them back to the pool
   rather than creating new ones (this is the existing runway restart
   handling from `mds.md` -- the bread-crumb cleanup unifies the
   probe and runway-restart paths)

The scan happens via NFSv3 READDIR on `<mds_uuid>/` with uid=0.  If
the READDIR itself fails with ACCES/PERM, the root access check has
already logged the error and the dstore is marked unavailable.

## TOML Configuration Format

`[[export.clients]]` is the only accepted form.  The old flat fields
(`clients`, `access`, `root_squash`, `flavors`) are removed from the
parser.  All existing configs must be migrated.

```toml
[[export]]
path = "/"

    [[export.clients]]
    match       = "10.0.0.0/8"
    access      = "rw"
    root_squash = false
    flavors     = ["krb5", "krb5p"]

    [[export.clients]]
    match       = "*.trusted.lab"
    access      = "rw"
    root_squash = false
    flavors     = ["sys", "krb5"]

    [[export.clients]]
    match       = "*"
    access      = "ro"
    root_squash = true
    flavors     = ["sys"]

[[export]]
path = "/ds-data"

    [[export.clients]]
    match       = "*"
    access      = "rw"
    root_squash = false
    flavors     = ["sys"]
```

### BAT config (reffsd-bat.toml after migration)

```toml
[[export]]
path = "/"

    [[export.clients]]
    match       = "*"
    access      = "rw"
    root_squash = false
    flavors     = ["sys", "krb5", "krb5i", "krb5p", "tls"]
```

Per-flavor sub-exports are still created at runtime via probe.

## Data Structures

### New: `struct sb_client_rule`

Defined in `lib/include/reffs/super_block.h`:

```c
#define SB_MAX_CLIENT_RULES  32
#define SB_CLIENT_MATCH_MAX  128

struct sb_client_rule {
    char                   scr_match[SB_CLIENT_MATCH_MAX];
    bool                   scr_rw;          /* false = read-only */
    bool                   scr_root_squash;
    bool                   scr_all_squash;
    enum reffs_auth_flavor scr_flavors[REFFS_CONFIG_MAX_FLAVORS];
    unsigned int           scr_nflavors;
};
```

### Changes to `struct super_block`

Remove `sb_flavors[]` / `sb_nflavors`.  Add:

```c
struct sb_client_rule  sb_client_rules[SB_MAX_CLIENT_RULES];
unsigned int           sb_nclient_rules;

/* Derived union of all flavors across all rules -- for SECINFO. */
enum reffs_auth_flavor sb_all_flavors[REFFS_CONFIG_MAX_FLAVORS];
unsigned int           sb_nall_flavors;
```

`sb_all_flavors` is recomputed by `super_block_set_client_rules()`
whenever the rule list changes.  `nfs4_check_wrongsec()` and SECINFO
use `sb_all_flavors` to advertise what flavors the export accepts;
per-rule flavor lists are used for enforcement once the client is
matched.

`super_block_set_flavors()` becomes a thin wrapper that creates a
single `*` rule with the given flavors, `root_squash=true`, `access=rw`.
It is kept only for the `SB_SET_FLAVORS` probe op; no new call sites.

### Changes to `struct reffs_export_config` (settings.h)

```c
struct reffs_client_rule_config {
    char match[SB_CLIENT_MATCH_MAX];
    bool rw;
    bool root_squash;
    bool all_squash;
    enum reffs_auth_flavor flavors[REFFS_CONFIG_MAX_FLAVORS];
    unsigned int nflavors;
};

struct reffs_export_config {
    char path[REFFS_CONFIG_MAX_PATH];
    struct reffs_client_rule_config rules[SB_MAX_CLIENT_RULES];
    unsigned int nrules;
};
```

The old `clients`, `access`, `root_squash`, `flavors` flat fields
are removed.

## Client Matching Logic

New file: `lib/fs/client_match.c`, header `lib/include/reffs/client_match.h`.

```c
/*
 * Match the connecting client against an ordered rule list.
 * Returns the first matching rule, or NULL if nothing matches.
 *
 * Priority: single host (1) > CIDR (2) > hostname wildcard (3) > * (4).
 * Within same priority, list order (first wins).
 */
const struct sb_client_rule *
client_rule_match(const struct sb_client_rule *rules, unsigned int nrules,
                  const struct sockaddr_storage *peer);
```

Implementation:

1. Two-pass: first pass collects best match per priority level; second
   pass returns the lowest (best) priority hit.
2. Single host: `inet_pton()` for IP specs; `getaddrinfo()` + compare
   for exact hostnames (result cached in a small per-server LRU).
3. CIDR: parse `addr/prefix`, mask the peer address and compare.
4. Hostname wildcard: `fnmatch()` on the reverse-DNS of the peer
   (if available; DNS failure skips the rule -- never fail open),
   logged once per peer.
5. Anonymous `*`: always matches.

## Security Enforcement

### Split responsibility

| Where | What |
|-------|------|
| `client_rule_match()` | Which rule applies to this connection |
| `nfs4_check_wrongsec()` | Flavor enforcement using matched rule's `scr_flavors` |
| `rpc_cred_squash()` | Credential transformation (root_squash, all_squash) |
| `nfs4_check_rofs()` (new) | ro/rw enforcement for write ops |

### `rpc_cred_squash()` -- new function in `lib/rpc/`

Called after AUTH_SYS credential parsing, before any op dispatch.
Looks up the matched rule for this connection's peer address:

- `root_squash && rc_uid == 0` --> `rc_uid = 65534, rc_gid = 65534`
- `all_squash` --> `rc_uid = 65534, rc_gid = 65534` unconditionally

The matched rule pointer is cached on the connection so it is not
re-evaluated on every RPC.

### `nfs4_check_wrongsec()` changes

1. Look up matched rule via `client_rule_match(sb->sb_client_rules, ..., peer)`.
2. Check the client's flavor against `rule->scr_flavors[]`.
3. If no rule matches, return `NFS4ERR_ACCESS`.

`sb_all_flavors[]` is used for `SECINFO` / `SECINFO_NO_NAME` responses.

### `nfs3_check_access()` -- new function in `lib/nfs3/`

1. **MOUNT handler**: reject if no rule matches the client, or the
   client's AUTH flavor is not in the matched rule's flavor list.
   Return `MNT3ERR_ACCES`.
2. **Per-op dispatch** (WRITE, CREATE, REMOVE, etc.): check `scr_rw`;
   return `NFS3ERR_ACCES` for writes on ro exports.

The matched rule is cached on the NFSv3 connection.

## Probe Protocol Changes

### New XDR types (probe1_xdr.x)

```
const PROBE1_MAX_CLIENT_RULES = 32;
const PROBE1_MAX_MATCH        = 128;

struct probe_client_rule1 {
    string              pcr_match<PROBE1_MAX_MATCH>;
    bool                pcr_rw;
    bool                pcr_root_squash;
    bool                pcr_all_squash;
    probe_auth_flavor1  pcr_flavors<PROBE1_MAX_FLAVORS>;
};
```

`probe_sb_info1` gains a new field (appended for wire compat):

```
struct probe_sb_info1 {
    ...existing fields...
    probe_client_rule1  psi_client_rules<PROBE1_MAX_CLIENT_RULES>;
};
```

### New op: `SB_SET_CLIENT_RULES` (op 26)

```
struct SB_SET_CLIENT_RULES1args {
    unsigned hyper       scra_id;
    probe_client_rule1   scra_rules<PROBE1_MAX_CLIENT_RULES>;
};
/* Returns probe_stat1 directly */
```

CLI:
```bash
reffs-probe.py sb-set-client-rules --id 42 \
    --rule match="10.0.0.0/8",access=rw,root_squash=false,flavors=krb5:krb5p \
    --rule match="*",access=ro,root_squash=true,flavors=sys

reffs_probe1_clnt --op sb-set-client-rules --sb-id 42 \
    --rule "10.0.0.0/8,rw,no_root_squash,krb5:krb5p" \
    --rule "*,ro,root_squash,sys"
```

`SB_SET_FLAVORS` (op 19) is kept as a shim only: it synthesizes a
single `*` catch-all rule.  No new callers; deprecation and removal
is NOT_NOW_BROWN_COW.

### `SB_LIST` / `SB_GET` responses

`fill_sb_info()` populates `psi_client_rules` from `sb->sb_client_rules`.

## Registry Persistence

`sb_registry_entry` does NOT grow.  Client rules are stored in a
separate per-sb file:

```
<state_dir>/sb_<id>.clients
```

Format: a 4-byte rule count followed by `nrules` fixed-size records:

```c
#define SB_REGISTRY_CLIENT_MATCH_MAX 128

struct sb_registry_client_rule {
    char     srcr_match[SB_REGISTRY_CLIENT_MATCH_MAX];
    uint32_t srcr_flags;      /* SRCR_RW, SRCR_ROOT_SQUASH, SRCR_ALL_SQUASH */
    uint32_t srcr_nflavors;
    uint32_t srcr_flavors[SB_REGISTRY_MAX_FLAVORS];
};

#define SRCR_RW          (1u << 0)
#define SRCR_ROOT_SQUASH (1u << 1)
#define SRCR_ALL_SQUASH  (1u << 2)
```

Written with write-temp/fdatasync/rename.  Read during
`sb_registry_load()` after the superblock is restored.  If the
`.clients` file is absent, the sb has no client rules and all
connections are denied until rules are set via probe.

The existing `sre_flavors[]` / `sre_nflavors` in `sb_registry_entry`
are kept and written from `sb_all_flavors` so they remain meaningful
as a human-readable summary, but they are not used for enforcement.

## New `bat_export_setup.sh` usage

```bash
# DS-backed export: admin is responsible for root_squash=false
# MDS probes at startup and logs if this is missing
$PROBE sb-set-client-rules --id "$FFV1_ID" \
    --rule match="*",access=rw,root_squash=false,flavors=sys

# Per-flavor exports
$PROBE sb-set-client-rules --id "$KRB5_ID" \
    --rule match="*",access=rw,root_squash=true,flavors=krb5

$PROBE sb-set-client-rules --id "$SYS_ID" \
    --rule match="10.0.0.0/8",access=rw,root_squash=false,flavors=sys \
    --rule match="*",access=rw,root_squash=true,flavors=sys
```

## Test Plan

### Existing tests affected

| File | Impact |
|------|--------|
| `sb_security_test.c` | **UPDATE** -- adapt to `super_block_set_client_rules()`; test that `sb_all_flavors` is recomputed correctly |
| `sb_persistence_test.c` | **UPDATE** -- add round-trip for `.clients` file; test absent-file behavior (no rules, all denied) |
| `config_test.c` | **UPDATE** -- replace flat-field tests with `[[export.clients]]` tests |
| All other `make check` tests | PASS |

### New unit tests

**`lib/fs/tests/client_match_test.c`** (new):

| Test | Intent |
|------|--------|
| `test_match_star_all` | `*` matches any address |
| `test_match_ipv4_exact` | Single IPv4 host matches itself, not neighbor |
| `test_match_cidr_v4` | `/24` matches hosts in subnet, not outside |
| `test_match_cidr_v6` | IPv6 `/48` prefix |
| `test_match_wildcard_hostname` | `*.lab` matches `a.lab`, not `b.other` |
| `test_match_priority_host_beats_cidr` | Host rule wins over CIDR even if CIDR listed first |
| `test_match_priority_cidr_beats_star` | CIDR wins over `*` |
| `test_match_no_match` | No rule matches --> NULL return |
| `test_match_first_of_same_type` | Two CIDR rules both match; first listed wins |

**`lib/fs/tests/sb_persistence_test.c`** (extend):

| Test | Intent |
|------|--------|
| `test_client_rules_persisted` | Set 3 rules, save, reload, verify all fields match |
| `test_client_rules_absent_no_access` | Load sb with no `.clients` file; verify `sb_nclient_rules == 0` |

**`lib/nfs4/dstore/tests/dstore_root_probe_test.c`** (new):

| Test | Intent |
|------|--------|
| `test_root_probe_success` | Probe CREATE succeeds; probe file removed; dstore available |
| `test_root_probe_acces` | Probe CREATE returns NFS3ERR_ACCES; dstore marked unavailable |
| `test_root_probe_breadcrumb_cleanup` | Prior `.root_probe` found; removed before new probe |

## Implementation Order

1. `struct sb_client_rule` + constants
2. `client_match.c` + unit tests (TDD)
3. Update `struct super_block`: add `sb_client_rules[]`, `sb_all_flavors[]`; remove `sb_flavors[]`
4. `super_block_set_client_rules()`; `super_block_set_flavors()` as shim wrapper
5. Config parsing: `[[export.clients]]` only; remove flat fields from parser and structs
6. `rpc_cred_squash()` in `lib/rpc/`
7. `nfs4_check_wrongsec()` -- per-rule flavors, `sb_all_flavors` for SECINFO
8. `nfs3_check_access()` -- MOUNT rejection + per-op write check
9. Registry persistence: `.clients` file save/load; absent = no rules
10. `dstore_probe_root_access()` + bread-crumb cleanup in `dstore_alloc()`
11. Probe XDR: `probe_client_rule1`, `SB_SET_CLIENT_RULES` op 26
12. Probe server handler
13. Probe Python client + CLI
14. Update `bat_export_setup.sh` and all example TOML files
15. Integration test in `scripts/test_sb_probe.py`

## Deferred / NOT_NOW_BROWN_COW

- Netgroup support (`@group` specs)
- Per-rule `xprtsec=` transport security policy
- DNS result caching with TTL
- `SB_SET_FLAVORS` removal (kept as shim)
- `secure` option (require source port < 1024)
- Dynamic re-export without client remount

## Key Files

| File | Change |
|------|--------|
| `lib/include/reffs/super_block.h` | `struct sb_client_rule`, new sb fields, remove `sb_flavors` |
| `lib/include/reffs/settings.h` | `struct reffs_client_rule_config`, rewrite `reffs_export_config` |
| `lib/include/reffs/client_match.h` | NEW |
| `lib/fs/client_match.c` | NEW |
| `lib/fs/super_block.c` | `super_block_set_client_rules()` |
| `lib/fs/sb_registry.c` | `.clients` file save/load |
| `lib/include/reffs/sb_registry.h` | `struct sb_registry_client_rule`, new constants |
| `lib/config/config.c` | Parse `[[export.clients]]` only |
| `lib/rpc/rpc.c` | `rpc_cred_squash()` |
| `lib/nfs4/server/security.c` | `nfs4_check_wrongsec()` per-rule flavors |
| `lib/nfs3/server.c` | `nfs3_check_access()` |
| `lib/nfs4/dstore/dstore.c` | `dstore_probe_root_access()`, bread-crumb cleanup |
| `lib/xdr/probe1_xdr.x` | `probe_client_rule1`, `SB_SET_CLIENT_RULES` op 26 |
| `lib/probe1/probe1_server.c` | New handler |
| `lib/probe1/probe1_client.c` | New client wrapper |
| `scripts/reffs/probe_client.py.in` | `sb_set_client_rules()` |
| `scripts/reffs-probe.py.in` | `sb-set-client-rules` subcommand |
| `scripts/bat_export_setup.sh` | Use `sb-set-client-rules` |
| `examples/reffsd-bat.toml` | `[[export.clients]]` format |
| `lib/fs/tests/client_match_test.c` | NEW |
| `lib/fs/tests/sb_persistence_test.c` | Extend |
| `lib/fs/tests/sb_security_test.c` | Update |
| `lib/config/tests/config_test.c` | Update |
| `lib/nfs4/dstore/tests/dstore_root_probe_test.c` | NEW |
