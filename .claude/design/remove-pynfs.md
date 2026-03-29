<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Design: Remove pynfs, wire in reply

## Goal

Remove the GPL-licensed pynfs subdirectory from reffs and replace it
with the reply package (pip install from git@github.com:loghyr/reply.git).

## Changes Required

### 1. Dockerfiles — replace ply with reply-xdr
- `Dockerfile`: remove `ply`, add `reply-xdr` (from git or PyPI)
- `Dockerfile.ci`: remove `python3-ply`, add `reply-xdr`

### 2. configure.ac — remove pynfs references
- Remove AC_CONFIG_FILES for pynfs/*
- Remove Makefile entries for pynfs subdirectories
- Keep AM_PATH_PYTHON and RPCGEN checks

### 3. scripts/reffs/Makefile.am — use xdr-parser from reply
- Remove `SUBDIRS = pynfs`
- Change xdrgen.py calls to `xdr-parser` (installed via pip)

### 4. scripts/reffs/protocol_client.py.in — change import
- `from .pynfs.rpc import rpc` → `from reply.rpc import rpc`
  (reply.rpc.rpc provides Client, and reply.rpc.rpc_security
  provides security — but the import needs to stay as `rpc`
  since protocol_client references `rpc.Client` and
  `rpc.security.CredInfo()`)

### 5. Delete scripts/reffs/pynfs/ entirely

### 6. check_license.sh — remove pynfs exclusion

### 7. reffs.spec.in — add Python deps
- BuildRequires: python3-pip (or reply-xdr)
- Requires: python3-reply-xdr (or pip install)

### 8. lib/xdr/Makefile.am — optionally use xdr-parser for C too
- Currently uses system rpcgen for C generation
- Could switch to `xdr-parser --lang c` but rpcgen works fine
- Leave as-is for now (rpcgen is the standard tool)

### 9. Review agent — add Python guidelines
- Update .claude/agents/review.md to check Python files
- Add Python style checks from reply/.claude/standards.md

## Import Path Design

reply installs as a Python package with this structure:
```
reply/
  xdr_parser.py   (also installed as xdr-parser CLI)
  rpc/
    rpc.py
    rpclib.py
    rpc_security.py
    rpc_const.py, rpc_type.py, rpc_pack.py
    gss_const.py, gss_type.py, gss_pack.py
```

The key import change in protocol_client.py.in:
```python
# OLD:
from .pynfs.rpc import rpc

# NEW:
from rpc import rpc
```

This works because the reply package installs `rpc` as a top-level
package. `rpc.rpc` exports Client, and imports `rpc_security` as
`security`, so `rpc.security.CredInfo()` still works.

The XDR code generation changes from:
```makefile
# OLD:
$(top_builddir)/scripts/reffs/pynfs/xdr/xdrgen.py foo.x

# NEW:
xdr-parser --lang python foo.x
```
