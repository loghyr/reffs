<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Chunk-Collision Track 2: IOR via N Proxy Servers

## Context

`.claude/design/chunk-collision-validation.md` defines three
execution tracks for stressing the CHUNK state machine under
concurrent multi-clientid writes.  Track 1 (N concurrent `ec_demo`
instances) shipped; its harness is
`deploy/benchmark/run_chunk_collision.sh` and the per-sb chunk
counters (BLOCKER 2) landed.

Track 2 is the HPC-canonical arm: `IOR -F 0 -W -R -C` (shared
file, write-then-verify, reorder-tasks) driven through **N Proxy
Server containers**.  Each PS holds its own MDS-facing session, so
N PSes = N distinct `clientid4`s contending on one shared MDS
file -- the same contention surface as Track 1 but exercised
through the client-visible POSIX path (kernel NFS mount -> PS
:4098 listener -> `REFFS_DATA_PROXY` backend -> CHUNK ops) rather
than through `ec_demo` driving CHUNK ops directly.

The original gate ("after PS Phases 3 AND 4") is **satisfied**:
proxy-server.md marks Phase 3 (proxy READ) and Phase 4a+4b (proxy
WRITE) DONE.  This doc supersedes the Track 2 sketch in
chunk-collision-validation.md lines 142-170 and 266-273; that
doc's stale "Gated on PS Phase 4" note is corrected to "ungated"
in the same slice.

This is a **correctness validation, not a benchmark**.  Throughput
numbers from a single-host container run do not transfer (see
chunk-collision-validation.md "Cost model").

## What does NOT exist yet

Surveyed 2026-05-19:

- No `ior` and no MPI (`mpirun`/`mpiexec`) in `Dockerfile`,
  `Dockerfile.ci`, or on any bench host.
- IOR (github.com/hpc/ior) is **not packaged** for Fedora or
  Ubuntu.  It must be built from source against an MPI.
- `deploy/benchmark/run-ps-bench-bringup.sh` brings up exactly
  **2** PSes (A, B) on ports 4098/4099.  Track 2 needs N.

## Scope

1. **IOR + MPI in the dev image.**  Add OpenMPI (packaged), a
   source-built IOR, and `nfs-utils` (the IOR container
   kernel-mounts the PS listeners via `mount -t nfs4`; the Fedora
   image did not previously carry it) to the main `Dockerfile`
   (Fedora `reffs-dev`, the image `deploy/benchmark/` uses).  NOT added to
   `Dockerfile.ci`: the CLAUDE.md "update both Dockerfiles" rule
   covers `configure.ac` `PKG_CHECK_MODULES` link dependencies;
   IOR/MPI are runtime test tools, not link deps of reffsd, and
   Track 2 does not run in GitHub CI.  Decision recorded
   2026-05-19.
2. **Generalize the PS bringup to N.**  `run-ps-bench-bringup.sh`
   parametrized by `NPS` (default keeps 2 for existing callers;
   Track 2 passes `NPS=4`).  N certs, N `[[allowed_ps]]` blocks,
   N PS containers on ports 4098..4098+N-1.
3. **Track 2 harness.**  New `run_chunk_collision_track2.sh`:
   brings up MDS + 10 DSes + N PSes, runs IOR `-F 0 -W -R -C`
   with N MPI ranks (each rank mounts a distinct PS listener),
   reads chunk counters before/after, checks pass criteria.
4. **Compose / orchestration.**  An `ior-client` container image
   layer + a harness that `docker run`s it `--network=host` (same
   pattern run-ps-bench-bringup.sh already uses for PSes).
5. Update `chunk-collision-validation.md` (Track 2 ungated;
   cross-reference this doc) and `BENCHMARKS.md`.

## Tests first

This slice is test infrastructure -- its "tests" are the harness
pass criteria, not C unit tests.  No production C code changes,
so no `make check` impact.

### Existing tests affected

| File | Impact |
|------|--------|
| All `make check` tests | PASS -- no production code touched |
| `run_chunk_collision.sh` (Track 1) | PASS -- untouched; Track 2 is a sibling script |
| `run-ps-bench-bringup.sh` callers (exps #5/#10/#11/#13) | PASS -- `NPS` defaults to 2, identical behaviour |

### Track 2 pass criteria (the harness IS the test)

Mirrors chunk-collision-validation.md "Verification methodology":

1. **IOR reports zero verify mismatches.**  `-W` (write-phase
   verify) and `-R` (read-back verify) both clean across all
   ranks; `-C` (reorderTasks) makes each rank read another
   rank's data.
2. **Counter deltas consistent.**  `sb_chunk_writes` matches the
   expected fan-out arithmetic; `sb_chunk_pending_displaced` is
   positive (proves real contention happened);
   `sb_chunk_finalize_crc_fail` is zero (no CRC anomaly slipped
   through).  Read via `reffs-probe.py` chunk-stats before and
   after.
3. **Zero ASAN / UBSAN errors** in any MDS / DS / PS container
   log.
4. **Every PS logged `PROXY_REGISTRATION ok`** -- the bringup's
   existing assertion, generalized to N.

### Test inventory (harness sub-cases)

| Case | Configuration | Pass criteria |
|------|--------------|---------------|
| `coll_t2_ior_basic` | `mpirun -np 4 ior -F 0 -w -r -W -R -e -k` via 4 PSes | IOR zero verify mismatches; counters consistent |
| `coll_t2_ior_reorder` | adds `-C` (reorderTasks) | zero mismatches; cross-rank read pairs clean |
| `coll_t2_ior_scaled` | scale to 8 ranks / 8 PSes | same; architecture scales |

`coll_t2_ps_encoding_translate` (mixed-encoding in-flight translation)
is deferred -- it needs a per-file encoding-shift trigger that does
not exist yet.  Tracked as NOT_NOW_BROWN_COW.

## Design

### Topology (single-host, container)

```
        reffs-bench-mds  (1)   <- docker compose, :2049
        reffs-bench-ds0..9 (10) <- docker compose
        reffs-ps-0..N-1   (N)  <- docker run --network=host,
                                   ports 4098..4098+N-1
        reffs-ior-client  (1)  <- docker run --network=host,
                                   --privileged (kernel NFS mounts)
```

A single IOR container runs `mpirun -np N` locally -- all N ranks
in one container.  MPI multi-host (`--hostfile`) is unnecessary
for a correctness validation; the contention is between PS
clientids on the MDS, not between hosts.  Each rank `r` mounts
`127.0.0.1:4098+r` at `/mnt/ps-r` and IOR targets
`/mnt/ps-r/ior_shared.dat` -- all N mounts are the *same* MDS
file proxied through N different PSes.

### IOR I/O path

`ior -a POSIX` does ordinary `open`/`write`/`read` on the NFS
mount.  The kernel NFS client talks NFSv4.2 to the PS :4098
listener; the PS's `REFFS_DATA_PROXY` backend runs LAYOUTGET +
CHUNK I/O + encoding transform against the real MDS+DSes.  This is
the Phase 3/4 proxy data path, now driven by a real kernel client
instead of `ec_demo`.

### IOR + MPI packaging

OpenMPI is packaged on Fedora (`openmpi`, `openmpi-devel`).
Fedora installs the MPI wrappers under `/usr/lib64/openmpi/bin`
and the libs under `/usr/lib64/openmpi/lib`; rather than rely on
`module load mpi/openmpi-x86_64`, the Dockerfile puts those on
`PATH` / `LD_LIBRARY_PATH` via `ENV` so `mpicc` (build) and
`mpirun` (harness) resolve directly.  IOR is not packaged
anywhere; the Dockerfile clones a pinned IOR release tag and
builds it:

```dockerfile
# IOR (HPC parallel-I/O test) -- not packaged; build from source.
RUN git clone --depth 1 --branch 4.0.0 \
        https://github.com/hpc/ior /tmp/ior && \
    cd /tmp/ior && ./bootstrap && \
    ./configure MPICC=mpicc \
        CFLAGS="-O2 -std=gnu17 -Wno-error=implicit-function-declaration -Wno-error=implicit-int -Wno-error=incompatible-pointer-types" && \
    make -j"$(nproc)" && make install && rm -rf /tmp/ior
```

Pin to a release tag (4.0.0) so the image is reproducible.

IOR 4.0.0 predates the C23 toolchain default.  Fedora 43 ships
GCC 15, which builds `-std=gnu23`; that promotes implicit
function declarations, implicit int, and incompatible pointer
types from warnings to hard errors, and IOR 4.0.0 does not
compile clean against it.  The `CFLAGS` above pin the IOR build
to `gnu17` and downgrade the new-default errors -- a build-time
accommodation for the legacy C tag, not a reffs code change.
(Discovered on the first image rebuild, 2026-05-19.)

### Generalized bringup: `run-ps-bench-bringup.sh`

Current script hardcodes `for tag in A B`.  Generalize:

- `NPS=${NPS:-2}` environment variable.  Existing callers
  (experiments #5/#10/#11/#13) get 2, unchanged.
- PS `r` (0-indexed): cert `ps-$r.{crt,key,fpr}`, container
  `reffs-ps-$r`, port `4098+r`, state/data dirs
  `/tmp/ps_${r}_{state,data}`.
- MDS `[[allowed_ps]]` allowlist gets N fingerprint blocks.
- The MDS-grant assertion expects N grants, not 2.
- `ps-tls-A.toml` / `ps-tls-B.toml` collapse to one template
  `ps-tls.toml` with `REPLACE_WITH_PS_PORT` spliced per PS.

The script keeps its existing two-name behaviour reachable
(`NPS=2` -> ports 4098/4099) so nothing downstream breaks; the
A/B-suffixed config files are replaced by an indexed template
but the experiments that consume the *topology* (not the config
filenames) are unaffected.

### Harness: `run_chunk_collision_track2.sh`

```
run_chunk_collision_track2.sh [--n N] [--ranks N] [--reorder]
                              [--size BYTES] [--block BYTES]
                              [--segments N]
```

Steps:
1. `NPS=N run-ps-bench-bringup.sh` -- MDS + 10 DSes + N PSes.
2. Snapshot chunk counters via `reffs-probe.py chunk-stats`
   (or `nfs4-op-stats`) against the MDS probe port.
3. `docker run --network=host --privileged` the IOR client;
   inside: for `r` in 0..N-1, `mount -t nfs4 -o vers=4.2
   127.0.0.1:$((4098+r)) /mnt/ps-$r`; then
   `mpirun --allow-run-as-root -np N ior -a POSIX -F 0
   -w -r -W -R [-C] -e -k -t 64k -b 4m -s 4 -i 3
   -o /mnt/ps-${rank}/ior_shared.dat` (rank-indexed -o via an
   `ior` wrapper or `-o` with the OMPI rank env var).
4. Snapshot counters again; compute deltas.
5. Check pass criteria 1-4; scan all container logs for ASAN /
   UBSAN; exit non-zero on any failure, keep logs on failure.

`mpirun --allow-run-as-root` because the container runs as root
(needed for `mount`).  IOR's `-o` per-rank file: IOR substitutes
no rank token itself, so the harness wraps `ior` in a one-line
shell that reads `$OMPI_COMM_WORLD_RANK` and rewrites `-o`.

### Why not a compose service

The PSes already run via `docker run --network=host` (not
compose) because they need host networking to expose 4098+.  The
IOR client needs the same (to reach 127.0.0.1:4098+r and to do
kernel NFS mounts).  Keeping Track 2 orchestration in the harness
script -- consistent with `run-ps-bench-bringup.sh` -- is cleaner
than splitting half into compose and half into `docker run`.  A
`bench-collision-t2` compose service is therefore NOT added; the
harness owns the IOR container lifecycle.

## RFC / spec compliance

No protocol surface.  Track 2 exercises existing production
paths: NFSv4.2 client <-> PS :4098 listener, PS proxy data path,
CHUNK ops.  All specified in draft-haynes-nfsv4-flexfiles-v2 and
draft-haynes-nfsv4-flexfiles-v2-proxy-server.

## State machines / persistence / security

None introduced -- pure test orchestration.  The IOR container
runs `--privileged` (kernel NFS mount needs it), same as the
existing `reffs-bench-*` containers.

## Admin interface

None.  The chunk counters consumed by the harness are already
surfaced on the probe protocol (BLOCKER 2 of
chunk-collision-validation.md, shipped).

## Implementation order

1. Dockerfile + Dockerfile.ci: add OpenMPI + source-built IOR.
   `make image` to rebuild; smoke-test `ior -h` + `mpirun
   --version` inside the image.
2. Generalize `run-ps-bench-bringup.sh` to `NPS`; collapse
   `ps-tls-{A,B}.toml` to one indexed template.  Verify `NPS=2`
   still brings up the old A/B topology.
3. Write `run_chunk_collision_track2.sh`.
4. Run `coll_t2_ior_basic` (4 ranks) end-to-end on a bench host.
5. Add `--reorder` and the 8-rank scaled case.
6. Update `chunk-collision-validation.md` (Track 2 ungated) and
   `BENCHMARKS.md`.
7. `make -f Makefile.reffs license` + `style` on the new shell
   scripts.

## Risk

Medium.  No production C code changes -- the risk is entirely in
test infrastructure and the image rebuild.  Concrete risks:

- IOR-from-source build is the gnarliest Dockerfile addition;
  pinned tag + `./bootstrap` mitigates.
- The PS proxy data path has been exercised by `ec_demo` and the
  cross-PS experiments but not by a Linux *kernel* NFS client
  doing IOR-style buffered I/O on a shared file -- Track 2 may
  surface PS-side bugs.  That is the point of the validation;
  any such bug is a finding, logged and triaged, not a harness
  defect.
- `mount -t nfs4` inside a container needs `--privileged` and a
  host kernel with the NFS client module; the bench hosts
  already satisfy this (the CI integration tests mount NFS).

## Deferred / NOT_NOW_BROWN_COW

- `coll_t2_ps_encoding_translate` -- needs an in-flight per-file
  encoding-shift trigger that does not exist.
- Multi-host MPI (`--hostfile`) -- a single-host container run is
  sufficient for correctness; multi-host is a perf-scaling
  concern for the deploy/benchmark side.
- HACC-IO / VPIC-IO real-app proxies (already deferred in
  chunk-collision-validation.md).
- Bare-metal Track 2 (Tier 5 in goals.md).
