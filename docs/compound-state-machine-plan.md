<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Pausable Compound State Machine — Implementation Plan

**Date:** 2026-03-20
**Goal:** Make NFS4 COMPOUND requests pausable so individual ops can go async
(io_uring file I/O, DS callbacks, client callbacks) and resume cleanly.

---

## Design Summary

Three state machines:
- **Task** — `IDLE → RUNNING → PAUSED → RUNNING → DONE`
- **RPC** — transaction lifecycle (parse → dispatch → encode → send → done)
- **Compound** — op index (`c_curr_op`) + resume callback (`rt_next_action`)

**Ownership transfer:** The op sets `rt->rt_next_action`, atomically marks the task
`TASK_PAUSED` (point of no return — caller cannot touch task/compound after this),
then starts async work. The async completer atomically marks `TASK_RUNNING` and
re-enqueues the task. The worker thread then calls `rpc_protocol_op_call(rt)` again;
`dispatch_compound` sees `rt_next_action != NULL`, calls it, clears it, advances
`c_curr_op`, and continues the loop.

---

## Struct Changes

### `lib/include/reffs/task.h`

Add state enum and new fields:

```c
enum task_state {
    TASK_IDLE    = 0,
    TASK_RUNNING = 1,
    TASK_PAUSED  = 2,
    TASK_DONE    = 3,
};

struct task {
    /* ... existing fields unchanged ... */
    _Atomic enum task_state t_state;
    struct rpc_trans       *t_rt;   /* non-NULL once rpc_trans allocated */
};
```

Add inline primitives:

```c
static inline bool task_pause(struct task *t)
{
    enum task_state expected = TASK_RUNNING;
    return atomic_compare_exchange_strong_explicit(
        &t->t_state, &expected, TASK_PAUSED,
        memory_order_acq_rel, memory_order_relaxed);
}

static inline void task_resume(struct task *t)
{
    enum task_state expected = TASK_PAUSED;
    bool ok = atomic_compare_exchange_strong_explicit(
        &t->t_state, &expected, TASK_RUNNING,
        memory_order_acq_rel, memory_order_relaxed);
    if (ok)
        add_task(t);
}

static inline bool task_is_paused(const struct task *t)
{
    return atomic_load_explicit(&t->t_state, memory_order_acquire)
           == TASK_PAUSED;
}
```

### `lib/include/reffs/rpc.h`

Add to `struct rpc_trans`:

```c
    void           (*rt_next_action)(struct rpc_trans *rt); /* NULL = fresh */
    struct task     *rt_task;       /* owning task */
    struct compound *rt_compound;   /* live compound; NULL when not in dispatch */
```

`rt_task` is set in `rpc_trans_create_from_task`.
`rt_compound` is set by `nfs4_proc_compound` after `compound_alloc` and cleared
by `nfs4_compound_finalize`.

---

## `dispatch_compound` — `lib/nfs4/dispatch.c`

```c
void dispatch_compound(struct compound *compound)
{
    COMPOUND4args *args = compound->c_args;
    COMPOUND4res  *res  = compound->c_res;
    struct task   *t    = compound->c_rt->rt_task;

    /* Resume case: call the registered continuation first */
    if (compound->c_rt->rt_next_action != NULL) {
        void (*action)(struct rpc_trans *) = compound->c_rt->rt_next_action;
        compound->c_rt->rt_next_action = NULL;
        action(compound->c_rt);

        if (task_is_paused(t))
            return;   /* callback itself went async */

        nfs_resop4 *resop = &res->resarray.resarray_val[compound->c_curr_op];
        if (resop->nfs_resop4_u.opillegal.status) {
            res->status = resop->nfs_resop4_u.opillegal.status;
            res->resarray.resarray_len = compound->c_curr_op + 1;
            return;
        }
        compound->c_curr_op++;  /* op complete, advance */
    }

    /* Forward loop (fresh or post-resume) */
    for (; compound->c_curr_op < args->argarray.argarray_len;
           compound->c_curr_op++) {

        nfs_argop4 *argop = &args->argarray.argarray_val[compound->c_curr_op];
        nfs_resop4 *resop = &res->resarray.resarray_val[compound->c_curr_op];

        resop->resop = argop->argop;
        if (argop->argop < OP_MAX && op_table[argop->argop])
            op_table[argop->argop](compound);
        else
            nfs4_op_illegal(compound);

        trace_nfs4_compound_op(compound, __func__, __LINE__);

        if (task_is_paused(t))
            return;   /* op went async; c_curr_op stays here */

        if (resop->nfs_resop4_u.opillegal.status) {
            res->status = resop->nfs_resop4_u.opillegal.status;
            res->resarray.resarray_len = compound->c_curr_op + 1;
            return;
        }
    }

    res->status = NFS4_OK;
    res->resarray.resarray_len = args->argarray.argarray_len;
}
```

---

## `nfs4_proc_compound` — `lib/nfs4/compound.c`

Distinguish fresh vs resumed; skip alloc+validation on resume; return `EINPROGRESS`
when paused; call `nfs4_compound_finalize` on completion.

```c
int nfs4_proc_compound(struct rpc_trans *rt)
{
    bool is_resume = (rt->rt_compound != NULL);
    struct compound *compound;

    if (!is_resume) {
        compound = compound_alloc(rt);
        if (!compound) return NFS4ERR_DELAY;
        rt->rt_compound = compound;
        /* ... minorversion check, tag copy, resarray alloc, SEQUENCE check ... */
    } else {
        compound = rt->rt_compound;
    }

    dispatch_compound(compound);

    if (task_is_paused(rt->rt_task))
        return EINPROGRESS;  /* still in-flight; do not encode or free */

    nfs4_compound_finalize(compound);  /* slot cache + compound_free + wire send */
    rt->rt_compound = NULL;
    return compound->c_res->status;
}
```

`nfs4_compound_finalize` (new static function) contains the slot-caching block
currently following `dispatch_compound`, then calls `compound_free`. It also
calls `rt->rt_cb(rt)` + `rpc_protocol_free(rt)` — taking final ownership of `rt`.

---

## Worker Thread — `lib/io/worker.c`

Distinguish fresh task from resumed task:

```c
if (t->t_rt != NULL) {
    /* Resumed task: re-enter dispatch directly */
    rpc_protocol_op_call(t->t_rt);
    if (task_is_paused(t))
        continue;    /* went async again; task re-enqueued */
    /* Compound complete: finalize called inside nfs4_proc_compound */
    free(t->t_buffer);
    free(t);
} else {
    /* Fresh task */
    t->t_cb = io_rpc_trans_cb;
    int rc = rpc_process_task(t);
    if (rc == ENOMEM) { add_task(t); continue; }
    if (rc == EINPROGRESS) continue;  /* do NOT free; owned by async path */
    free(t->t_buffer);
    free(t);
}
```

---

## Async Op Pattern

Every op that can go async follows this template:

```c
void nfs4_op_foo(struct compound *compound)
{
    struct rpc_trans *rt = compound->c_rt;
    struct task      *t  = rt->rt_task;

    /* ... validate, prepare async context ... */

    struct foo_ctx *ctx = calloc(1, sizeof(*ctx));
    ctx->compound = compound;
    /* ... fill in ctx ... */

    rt->rt_next_action = nfs4_op_foo_cb;   /* register callback */

    if (!task_pause(t)) {                   /* POINT OF NO RETURN */
        rt->rt_next_action = NULL;
        free(ctx);
        /* set NFS4ERR_SERVERFAULT in resop */
        return;
    }

    /* submit async work — do NOT touch rt/compound/t after this */
    submit_async_work(ctx);
}
```

Callback:

```c
void nfs4_op_foo_cb(struct rpc_trans *rt)
{
    struct compound *compound = rt->rt_compound;
    struct foo_ctx  *ctx = /* retrieve */;

    /* fill in resop from async result */

    free(ctx);
    /* return; dispatch_compound advances c_curr_op */
}
```

Async completer (e.g., io_uring CQE handler):

```c
void io_handle_foo_cqe(struct io_uring_cqe *cqe)
{
    struct foo_ctx *ctx = io_uring_cqe_get_data(cqe);
    ctx->result = cqe->res;
    task_resume(ctx->compound->c_rt->rt_task);  /* PAUSED → RUNNING + add_task */
}
```

---

## Session Slot Invariant

Slot stays `NFS4_SLOT_IN_USE` for the entire async duration.
`NFS4_SLOT_CACHED` is set only inside `nfs4_compound_finalize`, which runs only
when the compound completes. A duplicate request arriving while the compound is
paused correctly receives `NFS4ERR_DELAY` from `nfs4_op_sequence`.

---

## Implementation Order (9 commits)

1. Add `task_state` enum, `t_state` and `t_rt` to `struct task` — no behavior change
2. Add `rt_next_action`, `rt_task`, `rt_compound` to `struct rpc_trans` — no behavior change
3. Add `task_pause`/`task_resume`/`task_is_paused` primitives + unit tests (`lib/nfs4/tests/task_state.c`)
4. Extract `nfs4_compound_finalize` from `nfs4_proc_compound` — pure refactor
5. Add `rt->rt_compound` lifecycle and `EINPROGRESS` return to `nfs4_proc_compound`
6. Update `dispatch_compound` with resume block and `task_is_paused` early-return
7. Update `rpc_process_task` in `rpc.c` to handle `EINPROGRESS`
8. Update worker loop in `worker.c` for resumed tasks
9. Add integration tests (`lib/nfs4/tests/compound_async.c`) with mock async op

Each commit must leave the server compiling and all existing tests passing.

---

## Known Risks / Deferred Items

- **`add_task` blocks when queue is full:** `task_resume` calls `add_task`, which
  holds `task_queue_mutex`. If called from the io_uring CQE thread with a full queue,
  it could block io_uring progress. Acceptable for initial correctness; needs a
  lock-free resume queue for production.

- **Session destroy with live async compounds:** If DESTROY_SESSION arrives while a
  compound is paused, the session ref (`c_session`) keeps it alive, but teardown must
  drain in-flight compounds. Deferred.

- **Stats:** Latency stats only cover the first synchronous segment. Async latency
  is invisible. Note in code; proper async timing is a future enhancement.

- **`rpc_protocol_free` on resume path:** `nfs4_compound_finalize` takes ownership
  of `rt` and calls `rpc_protocol_free`. The caller must not touch `rt` after this.
