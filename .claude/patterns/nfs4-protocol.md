<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# NFSv4 Protocol Patterns in reffs

## clientid4 partitioning

```
[ boot_seq (16 bits) | incarnation (16 bits) | slot (32 bits) ]
```

Do not compare clientid4 as an opaque integer without extracting fields.
Use accessor macros. Mixing up field positions produces silent correctness bugs
after server restart.

## EXCHANGE_ID decision tree

nfs4_client_alloc_or_find() owns the full decision tree:

1. New clientid (no existing record) → allocate, assign clientid4
2. Same principal, same verifier → return existing clientid4 (idempotent retry)
3. Same principal, new verifier → new incarnation, invalidate old state
4. Different principal, same nii_name → CLID_INUSE
5. Confirmed vs. unconfirmed state transitions

All five cases must be handled. Falling through to case 1 for cases 3-5 produces
state corruption under client restart.

## utf8string validation

All string fields off the wire (client owner, server owner, nii_name, path
components) must go through utf8string_validate() before use:

```c
if (utf8string_validate(&cs->clientowner) < 0)
    return NFS4ERR_INVAL;
```

Do not access .utf8string_val directly on unvalidated input.

## nfstime4 overflow

nseconds must be < 1e9; wire values >= 1e9 are invalid, reject them.
Use nfstime4_to_timespec() and timespec_to_nfstime4(). Do not open-code.

## Persistent state write pattern

```c
/* CORRECT: write temp, fsync, rename */
fd = open(path_tmp, O_WRONLY|O_CREAT|O_TRUNC, 0600);
write(fd, &state, sizeof(state));
fsync(fd);
close(fd);
rename(path_tmp, path_final);
```

Direct overwrite of the final path is not crash-safe.

## bitmap4 operations

Use bitmap4.h helpers. Open-coded bit manipulation fails when bitmaps span
multiple uint32 words (word index off-by-one is the common mistake).

```c
bitmap |= (1U << attr_id);      /* WRONG for attr_id >= 32 */
bitmap4_set(&bm, attr_id);      /* CORRECT */
```
