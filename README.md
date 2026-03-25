# shift

A C23 library that provides an entity system and collections with deferred mutations and Structure-of-Arrays storage.

## Overview

`shift` organizes objects as lightweight entity handles that move between named **collections**. Mutations (creates, moves, destroys) are queued and applied in batch on an explicit `shift_flush()` call, enabling safe iteration without invalidating pointers mid-frame.

**Core concepts:**

- **Entities** — `{index, generation}` handles. The generation increments on destruction, making stale handles detectable.
- **Components** — typed data blobs registered with `shift_component_register`. Each component has an `element_size`, optional `alignment` for SIMD-friendly column allocation, optional `constructor`/`destructor` callbacks, and an opaque `user_data` pointer for consumer layers. Constructor/destructor callbacks receive the context, collection ID, entity array, data pointer, offset, count, and the component's `user_data` pointer — so they can rip through SoA arrays with pointer arithmetic and access component metadata without an extra lookup.
- **Collections** — named groups that store a fixed set of components in Structure-of-Arrays layout. Entities live in exactly one collection at a time. Collections support `on_enter`/`on_leave` callbacks and optional fixed capacity.
- **Deferred mutations** — `shift_entity_move`, `shift_entity_create`, and `shift_entity_destroy` enqueue operations. Nothing takes effect until `shift_flush()`. For cases where you need entities live immediately, `_immediate` variants bypass the queue entirely.
- **Generation revocation** — `shift_entity_revoke` bumps an entity's generation, invalidating all existing handles without destroying the entity.

## Building

Requires CMake 3.x and a C23-capable compiler.

```sh
# Debug build (ASan + UBSan enabled)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# Release build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Tests

Tests use the [Unity](https://github.com/ThrowTheSwitch/Unity) framework, fetched automatically via CMake FetchContent.

```sh
cmake --build build && ctest --test-dir build

# Verbose output
ctest --test-dir build -V

# Run the binary directly
./build/tests/shift_tests
```

## Usage

### 1. Create a context

```c
shift_config_t cfg = {
    .max_entities            = 1024,
    .max_components          = 16,
    .max_collections         = 8,
    .deferred_queue_capacity = 256,
    .allocator               = {0},  /* NULL = use libc */
};
shift_t *ctx = NULL;
shift_context_create(&cfg, &ctx);
```

Pass a custom `shift_allocator_t` (alloc/realloc/free + user ctx pointer) to use an arena or pool allocator. For components with custom alignment, you can optionally set `aligned_alloc`/`aligned_realloc`/`aligned_free` on the allocator — if left NULL, shift falls back to over-allocation with manual alignment.

### 2. Register components

```c
/* Verbose form */
shift_component_info_t pos_info = {
    .element_size = sizeof(float) * 3,
    .alignment    = 0,     /* 0 = default (alignof(max_align_t)) */
    .constructor  = NULL,
    .destructor   = NULL,
    .user_data    = NULL,  /* opaque pointer for consumer layers */
};
shift_component_id_t pos_id;
shift_component_register(ctx, &pos_info, &pos_id);

/* Convenience macros */
SHIFT_COMPONENT(ctx, pos_id, float[3]);
SHIFT_COMPONENT(ctx, vel_id, float[3]);
SHIFT_COMPONENT_EX(ctx, hp_id, int, hp_ctor, hp_dtor);  /* with ctor/dtor */
```

Set `alignment` to a power-of-two value when the component will be processed with SIMD intrinsics (e.g. 16 for SSE, 32 for AVX, 64 for AVX-512). The SoA column base pointer returned by `shift_collection_get_component_array` is guaranteed to satisfy this alignment. When left as 0 the alignment defaults to `alignof(max_align_t)`, which is what `malloc` provides.

The `user_data` pointer is stored as-is and retrievable via `shift_component_get_user_data`. Consumer layers can use it to attach type metadata, tag flags, custom copy semantics, or whatever else they need — keyed by component ID without maintaining parallel arrays.

`constructor` and `destructor` follow the same signature pattern as all shift callbacks, with an additional `user_data` parameter:

```c
void my_ctor(shift_t *ctx, shift_collection_id_t col_id,
             const shift_entity_t *entities, void *data,
             uint32_t offset, uint32_t count, void *user_data);
```

They receive the context, collection, entity array, component column base pointer, offset into the arrays, count, and the opaque `user_data` pointer from the component's `shift_component_info_t`. This lets you cross-reference other components or entity handles during init/teardown, and access component metadata without an extra lookup. They are called in batch during `shift_flush()` whenever entities enter or leave a collection that owns the component.

### 3. Register collections

```c
/* Verbose form */
shift_component_id_t moving_comps[] = {pos_id, vel_id};

shift_collection_info_t col_info = {
    .name         = "moving",
    .comp_ids     = moving_comps,
    .comp_count   = 2,
    .max_capacity = 0,       /* 0 = dynamic growth */
};
shift_collection_id_t moving_id;
shift_collection_register(ctx, &col_info, &moving_id);

/* Register on_enter / on_leave handlers */
shift_collection_on_enter(ctx, moving_id, my_on_enter, user_ctx, NULL);
shift_collection_on_leave(ctx, moving_id, my_on_leave, user_ctx, NULL);

/* Convenience macros */
SHIFT_COLLECTION(ctx, moving_id, pos_id, vel_id);
SHIFT_COLLECTION_CAP(ctx, fixed_id, 64, pos_id);   /* fixed capacity */
SHIFT_COLLECTION_EMPTY(ctx, marker_id);   /* zero-component state marker */
```

Set `max_capacity` to a non-zero value for a **fixed-capacity** collection. All storage for that collection is allocated up-front at registration time — no heap activity occurs during `shift_flush`. Attempting to move more entities into the collection than its capacity allows returns `shift_error_full` from `shift_flush`.

Zero-component collections (`comp_count = 0`) are valid and useful as state markers or staging areas. They store only entity handles with no component data.

**on_enter / on_leave contract:**

All collection callbacks share one signature:

```c
void my_cb(shift_t *ctx, shift_collection_id_t col_id,
           const shift_entity_t *entities, uint32_t offset,
           uint32_t count, void *user_ctx);
```

- `on_enter` fires after entities are fully placed — components are zero-inited, constructors have been called, and the entity handle is valid inside the callback.
- `on_leave` fires before entities are removed — components are still accessible via `shift_entity_get_component` or directly via the offset into the SoA arrays.
- For moves: on_leave fires on the source, then on_enter fires on the destination.
- For creates: on_enter only. For destroys: on_leave only.

**What you can and cannot call inside callbacks** (applies to `on_enter`, `on_leave`, constructors, and destructors equally):

Callbacks fire in two contexts: during `shift_flush()` (deferred path) and inline during `_immediate` operations. The rules differ:

*Always safe (any callback context):*

| Function | Notes |
|---|---|
| `shift_entity_get_component` | Works on the affected entities and any unrelated entity |
| `shift_collection_get_component_array` | Arrays are stable within the callback |
| `shift_collection_get_entities` | Same |
| `shift_entity_is_stale` / `is_moving` / `get_collection` | Read-only metadata checks |
| All introspection (`shift_collection_count`, `get_components`, `entity_count`, `shift_component_get_collections`, `get_user_data`) | Read-only |

*Safe only in `_immediate` callbacks (NOT during `shift_flush`):*

| Function | Notes |
|---|---|
| `shift_entity_create` | Enqueues for the next flush — the deferred queue is idle |
| `shift_entity_move` | Same |
| `shift_entity_destroy` | Same |

During `shift_flush`, the deferred queue is being consumed. Pushing new ops onto it corrupts the iteration.

*Never safe during `shift_flush`, risky during `_immediate` callbacks:*

| Function | Why |
|---|---|
| `shift_entity_create_immediate` | Can realloc columns the flush is iterating. Manipulates the null pool which flush is also using. In `_immediate` callbacks, safe only if targeting a collection not involved in the current operation. |
| `shift_entity_move_immediate` | `col_remove_run` on the source can swap-remove entities the flush hasn't processed yet. Triggers nested callbacks. In `_immediate` callbacks, safe only if targeting uninvolved collections. |
| `shift_entity_destroy_immediate` | Same as move_immediate (destroy is a move to the null collection) |
| `shift_collection_reserve` | Can realloc columns the flush or immediate op is actively reading/writing |

**The core rule: during a flush, the only safe thing to do in a callback is read. During an `_immediate` callback, you can also enqueue deferred work for the next flush.**

The `offset` and `count` parameters let you index directly into the collection's SoA arrays — the same offset works for the entity array and every component column. See [Pattern D](#pattern-d-fast-batch-processing-in-callbacks) below.

### 4. Create entities

```c
/* Deferred — entities are not accessible until flush */
shift_entity_t e;
shift_entity_create_one(ctx, moving_id, &e);
shift_flush(ctx);  /* entity is now live, components accessible */

/* Immediate — entity is live right away */
shift_entity_t e2;
shift_entity_create_one_immediate(ctx, moving_id, &e2);
/* e2 is immediately live, constructors + on_enter already fired */

/* Batch create */
shift_entity_t *ep;
shift_entity_create(ctx, 100, moving_id, &ep);
shift_flush(ctx);
```

`out_entities` from `shift_entity_create` points into internal storage and is only valid until the next `shift_flush()` — copy the handles if you need them beyond that point.

### 5. Access component data

```c
float *pos = NULL;
shift_entity_get_component(ctx, e, pos_id, (void **)&pos);
pos[0] = 1.0f;  pos[1] = 2.0f;  pos[2] = 3.0f;
```

Returns `shift_error_stale` if the entity handle is stale or if a mutation is pending (between `shift_entity_create`/`shift_entity_move` and `shift_flush`).

For bulk processing, iterate the SoA arrays directly:

```c
void  *positions = NULL;
size_t count     = 0;
shift_collection_get_component_array(ctx, moving_id, pos_id, &positions, &count);

float (*pos)[3] = positions;
for (size_t i = 0; i < count; i++) {
    pos[i][0] += vel[i][0] * dt;
    /* ... */
}
```

### Pointer lifetime and why entity handles are your stable reference

**Never cache raw pointers from `shift_entity_get_component`,
`shift_collection_get_component_array`, or `shift_collection_get_entities`.
Store `shift_entity_t` handles instead and re-fetch pointers when you need
them.**

To understand why, you need to know what shift does to memory under the hood.

**Swap-remove keeps arrays dense.** When entity B is destroyed or moved out of
a collection that contains [A, B, C, D], shift copies D's data into B's slot
and decrements the count. The result is [A, D, C] — no gap, no tombstone, and
D's metadata is updated to reflect its new offset. But any pointer you held to
B's slot now points at D's data. Any pointer you held to D's slot now points
past the end of the live range.

```
Before:   [A] [B] [C] [D]    count=4
                ↑ destroy B
After:    [A] [D] [C]        count=3
                ↑ D was copied here — old pointer to B now reads D's data
```

**Moves copy data to a new collection.** When an entity moves from collection
X to collection Y, its component data is copied into Y's arrays and removed
from X via swap-remove. A pointer into X that used to reference this entity
now points at whatever the swap-remove put there.

**Growth reallocates the entire column.** When a collection grows (because an
entity was created or moved into it), `realloc` may move the SoA column to a
new address. Every pointer into that column — even for entities that didn't
move — becomes dangling.

All three of these can happen during `shift_flush()`, any `_immediate`
operation, or `shift_collection_reserve()`. None of them produce a runtime
warning. The pointer just silently refers to the wrong data, a different
entity's data, or freed memory.

**Entity handles survive all of this.** A `shift_entity_t` is an
`{index, generation}` pair. The index identifies a metadata slot that always
tracks the entity's current collection and offset, regardless of how many
times it has been swap-removed, migrated, or reallocated around. As long as
the generation matches, the handle is valid and `shift_entity_get_component`
will find the entity's data at its current location.

When an entity is destroyed, its generation is bumped. Any handle with the old
generation will fail `shift_entity_is_stale()` and `shift_entity_get_component`
returns `shift_error_stale`. This is how you detect that something you were
referencing no longer exists — without ever touching a dangling pointer.

**The pattern:**

```c
/* WRONG — caching a component pointer */
typedef struct {
    float *cached_pos;  /* BAD: will dangle after next flush */
} my_system_t;

/* WRONG — caching a collection offset */
typedef struct {
    size_t offset;  /* BAD: swap-remove can change any entity's offset */
} my_ref_t;

/* RIGHT — storing entity handles, re-fetching when needed */
typedef struct {
    shift_entity_t target;  /* stable across flushes */
} my_ref_t;

void use_ref(shift_t *ctx, my_ref_t *ref) {
    if (shift_entity_is_stale(ctx, ref->target)) {
        /* entity was destroyed or revoked — handle it */
        return;
    }
    float *pos = NULL;
    shift_entity_get_component(ctx, ref->target, pos_id, (void **)&pos);
    pos[0] += 1.0f;
    /* don't store pos — let it go, re-fetch next time */
}
```

For **bulk iteration** (where you're processing every entity in a collection
rather than chasing individual handles), grab the column base pointer and
count, iterate, and discard:

```c
void physics_system(shift_t *ctx) {
    void  *pos_base, *vel_base;
    size_t count;
    shift_collection_get_component_array(ctx, moving_id, pos_id,
                                         &pos_base, &count);
    shift_collection_get_component_array(ctx, moving_id, vel_id,
                                         &vel_base, NULL);
    float (*pos)[3] = pos_base;
    float (*vel)[3] = vel_base;
    for (size_t i = 0; i < count; i++) {
        pos[i][0] += vel[i][0] * dt;
        pos[i][1] += vel[i][1] * dt;
        pos[i][2] += vel[i][2] * dt;
    }
    /* pos_base, vel_base are not stored — safe to flush after this */
}
```

This is safe because no mutations happen during the loop. The pointers are
scoped to this function and discarded before the next `shift_flush()`.

In advanced scenarios where you can guarantee no flushes or immediate
operations will occur (e.g. iterating multiple read-only systems between
flushes), holding pointers across those systems is safe as a performance
optimization. But this is fragile — any future code that adds a flush or
immediate op between those systems silently breaks the invariant. Default to
re-fetching.

### 6. Move and destroy entities

```c
/* Deferred move — takes effect at flush */
shift_entity_move_one(ctx, e, static_id);
shift_flush(ctx);

/* Immediate move — fires on_leave, destructors, copy, constructors, on_enter now */
shift_entity_move_one_immediate(ctx, e, other_id);

/* Deferred destroy */
shift_entity_destroy_one(ctx, e);
shift_flush(ctx);

/* Immediate destroy — on_leave, destructors fire now, generation bumped */
shift_entity_destroy_one_immediate(ctx, e2);
/* e2 is now stale */
```

Note: immediate operations return `shift_error_stale` if the entity has a pending deferred move. Flush first, then use immediate ops.

### 6b. Revoke entity handles

```c
/* Invalidate all existing handles without destroying the entity */
shift_entity_t new_handle;
shift_entity_revoke(ctx, e, &new_handle);
/* e is now stale, new_handle is valid, entity data is unchanged */
```

### 7. Tear down

```c
shift_context_destroy(ctx);
```

## API Reference

### Result codes

| Code | Value | Meaning |
|------|-------|---------|
| `shift_ok` | 0 | Success |
| `shift_error_null` | -1 | NULL argument |
| `shift_error_oom` | -2 | Allocation failed |
| `shift_error_stale` | -3 | Entity handle is stale or pending |
| `shift_error_full` | -4 | Entity pool or fixed-capacity collection is full |
| `shift_error_not_found` | -5 | Component not in collection |
| `shift_error_invalid` | -6 | Invalid argument (e.g. count=0) |

### Context

```c
shift_result_t shift_context_create(const shift_config_t *config, shift_t **out);
void           shift_context_destroy(shift_t *ctx);
```

### Components

```c
shift_result_t shift_component_register(shift_t *ctx,
                                         const shift_component_info_t *info,
                                         shift_component_id_t *out_id);

shift_result_t shift_component_get_user_data(const shift_t *ctx,
                                              shift_component_id_t comp_id,
                                              void **out_data);

shift_result_t shift_component_get_collections(const shift_t *ctx,
                                                shift_component_id_t comp_id,
                                                const shift_collection_id_t **out_ids,
                                                size_t *out_count);
```

### Collections

```c
shift_result_t shift_collection_register(shift_t *ctx,
                                          const shift_collection_info_t *info,
                                          shift_collection_id_t *out_id);

shift_result_t shift_collection_get_component_array(shift_t *ctx,
                                                     shift_collection_id_t col_id,
                                                     shift_component_id_t comp_id,
                                                     void **out_array,
                                                     size_t *out_count);

shift_result_t shift_collection_get_entities(shift_t *ctx,
                                              shift_collection_id_t col_id,
                                              shift_entity_t **out_entities,
                                              size_t *out_count);

shift_result_t shift_collection_get_components(const shift_t *ctx,
                                                shift_collection_id_t col_id,
                                                const shift_component_id_t **out_ids,
                                                uint32_t *out_count);

shift_result_t shift_collection_reserve(shift_t *ctx,
                                         shift_collection_id_t col_id,
                                         size_t capacity);

size_t shift_collection_count(const shift_t *ctx);            /* inline */
size_t shift_collection_entity_count(const shift_t *ctx,
                                      shift_collection_id_t col_id);
```

### Entities (deferred)

```c
shift_result_t shift_entity_create(shift_t *ctx, uint32_t count,
                                    shift_collection_id_t dest_col_id,
                                    shift_entity_t **out_entities);
shift_result_t shift_entity_create_one(shift_t *ctx,
                                        shift_collection_id_t dest_col_id,
                                        shift_entity_t *out_entity);

shift_result_t shift_entity_move(shift_t *ctx, const shift_entity_t *entities,
                                  uint32_t count, shift_collection_id_t dest_col_id);
shift_result_t shift_entity_move_one(shift_t *ctx, shift_entity_t entity,
                                      shift_collection_id_t dest_col_id);

shift_result_t shift_entity_destroy(shift_t *ctx, const shift_entity_t *entities,
                                     uint32_t count);
shift_result_t shift_entity_destroy_one(shift_t *ctx, shift_entity_t entity);
```

### Entities (immediate)

```c
shift_result_t shift_entity_create_immediate(shift_t *ctx, uint32_t count,
                                              shift_collection_id_t dest,
                                              shift_entity_t **out_entities);
shift_result_t shift_entity_create_one_immediate(shift_t *ctx,
                                                  shift_collection_id_t dest,
                                                  shift_entity_t *out_entity);

shift_result_t shift_entity_move_immediate(shift_t *ctx, const shift_entity_t *entities,
                                            uint32_t count, shift_collection_id_t dest);
shift_result_t shift_entity_move_one_immediate(shift_t *ctx, shift_entity_t entity,
                                                shift_collection_id_t dest);

shift_result_t shift_entity_destroy_immediate(shift_t *ctx, const shift_entity_t *entities,
                                               uint32_t count);
shift_result_t shift_entity_destroy_one_immediate(shift_t *ctx, shift_entity_t entity);
```

### Entities (two-phase create)

```c
shift_result_t shift_entity_create_begin(shift_t *ctx, uint32_t count,
                                         shift_collection_id_t dest,
                                         shift_entity_t **out_entities);
shift_result_t shift_entity_create_one_begin(shift_t *ctx,
                                             shift_collection_id_t dest,
                                             shift_entity_t *out_entity);

shift_result_t shift_entity_create_end(shift_t *ctx, const shift_entity_t *entities,
                                       uint32_t count);
shift_result_t shift_entity_create_one_end(shift_t *ctx, shift_entity_t entity);
```

### Entity queries and revocation

```c
shift_result_t shift_entity_get_component(shift_t *ctx, shift_entity_t entity,
                                           shift_component_id_t comp_id,
                                           void **out_data);

shift_result_t shift_entity_revoke(shift_t *ctx, shift_entity_t entity,
                                    shift_entity_t *out_new);

bool shift_entity_is_stale(const shift_t *ctx, shift_entity_t entity);   /* inline */
bool shift_entity_is_moving(const shift_t *ctx, shift_entity_t entity);  /* inline */
```

### Flush

```c
shift_result_t shift_flush(shift_t *ctx);
```

Executes all queued operations in order: removes from source collections (calling `on_leave` / destructors), inserts into destination collections (calling constructors / `on_enter`). Consecutive contiguous operations targeting the same destination are merged into a single batch call.

### Metrics

```c
shift_result_t shift_metrics_begin(shift_t *ctx);
shift_result_t shift_metrics_end(shift_t *ctx, const shift_metrics_t **out);
```

Optional per-tick instrumentation. Bracket your application loop with `shift_metrics_begin` / `shift_metrics_end` to track the high water mark of entity count in each collection during that tick. Zero overhead when not used — the metrics struct is only allocated on first call.

```c
/* At the top of your loop */
shift_metrics_begin(ctx);

/* ... game logic, creates, moves, destroys, flushes ... */

/* At the end of your loop */
const shift_metrics_t *met;
shift_metrics_end(ctx, &met);

/* Iterate collections (skip 0, the internal null collection) */
for (size_t i = 1; i < met->collection_capacity; i++) {
    if (met->collections[i].name)
        printf("%-20s max_count=%zu\n",
               met->collections[i].name,
               met->collections[i].max_count);
}
```

The returned pointer is valid until the next `shift_metrics_begin` or `shift_context_destroy`. Each `shift_collection_metrics_t` contains:

| Field       | Description                                              |
|-------------|----------------------------------------------------------|
| `name`      | The human-readable name from collection registration     |
| `max_count` | Highest entity count observed in the collection this tick |


## Recommended Usage

Collections are much more powerful than you think. For example I have found using a
collection per state of my objects works wonderfully to represent huge numbers of
objects that can be in one of many states. This initially seems like it wouldn't
work well since these collections are modelled on ECS archtypes so people
immediately think they are only for things you want to rip through all at once.

They are certainly good for that, but they are also really good at representing
state in state machines. Think about what it typically means to be in a particular
state. Take this example:

```
typedef enum {
  STATE1.
  STATE2,
  STATE3
} state_t;

struct thing {
  state_t state;
  foo_t*foo;
  bar_t *bar;
};
```

Ok quick which fields are valid for STATE1? foo? Nope, memory access violation. How
about bar? Yes valid. How do you move to STATE2? just set state? Did you forget to
allocate foo? Did you remember to free bar? You did? Hah! you just double freed bar!

All that garbage goes away if you just used collections.

But I just need a static array of things indexed by some id that I use to indirect
to the right object. Oh? You mean like how you can use an entity ID to indirect
into a collection?

But I really do need a flag to tell me when a particular object is in the state I
mean it! Oh, you mean like shift_entity_get_collection?

Trust me. Try it.

But I am writing a direct threaded interpreter of bytecode and every cycle counts!
Ok, use the switch statement.

### Resist the `_one` temptation

If you are coming from a traditional OOP or entity-component background, your
instinct will be to reach for `shift_entity_create_one_immediate`,
`shift_entity_move_one_immediate`, and `shift_entity_destroy_one_immediate` for
everything. They feel familiar — do a thing, get a result, move on. Fight that
instinct.

The real power of shift comes from **batching**. The deferred API
(`shift_entity_create`, `shift_entity_move`, `shift_entity_destroy`) queues
operations and executes them all at once during `shift_flush`. This isn't just an
API quirk — it is the whole point:

- **Constructors and destructors fire once per batch**, not once per entity. If
  you move 500 entities into the same collection, that's one constructor call
  with `count=500`, not 500 individual calls.
- **Migration recipes are cached per (src, dst) pair.** The first flush computes
  which components to copy, construct, and destruct. Every subsequent flush
  between the same pair reuses that work.
- **Contiguous operations merge automatically.** Queue moves for entities 7, 8,
  9, 10 to the same destination and shift collapses them into a single op. You
  don't have to think about it.
- **Iteration is safe.** You can iterate a collection, decide to move or destroy
  entities, and nothing actually changes until you flush. No iterator
  invalidation, no skipped entities, no off-by-one from a mid-loop swap-remove.

Even if you are processing entities one at a time — say, iterating a collection
and moving each entity based on some condition — use the deferred `_one` calls
(without `_immediate`). They still enqueue into the deferred queue and benefit
from the batching, merging, and safe-iteration guarantees above. The `_immediate`
variants bypass all of that.

The `_immediate` variants exist for the cases where you genuinely need an entity
live *right now* — bootstrapping at startup, responding to a one-off external
event, or debugging. They are the escape hatch, not the default.

Think in terms of phases: **read** your collections, **decide** what moves,
**enqueue** the mutations, **flush**. That's Data Oriented Design. That's where
shift shines.

### Pattern A: Resource management

A destructor on an `fd_t` component plus a "pending_close" collection eliminates
double-free gymnastics entirely:

```c
static void fd_destructor(shift_t *ctx, shift_collection_id_t col_id,
                          const shift_entity_t *entities, void *data,
                          uint32_t offset, uint32_t count,
                          void *user_data) {
    (void)ctx; (void)col_id; (void)entities; (void)user_data;
    int *fds = (int *)data + offset;
    for (uint32_t i = 0; i < count; i++)
        if (fds[i] >= 0) close(fds[i]);
}

SHIFT_COMPONENT_EX(ctx, fd_id, int, NULL, fd_destructor);
SHIFT_COLLECTION(ctx, open_id, fd_id, metadata_id);
SHIFT_COLLECTION(ctx, pending_close_id, fd_id);

/* To close: move to pending_close, flush. Destructor runs exactly once. */
shift_entity_move_one(ctx, conn, pending_close_id);
shift_flush(ctx);  /* fd_destructor fires, conn handle is stale */
```

No manual close tracking. No double-free. Move to the collection, flush, done.

### Pattern B: State machine

Model each state as a collection with only the components valid for that state:

```c
SHIFT_COMPONENT(ctx, sock_id, int);           /* socket fd */
SHIFT_COMPONENT(ctx, handshake_id, tls_ctx_t); /* TLS handshake state */
SHIFT_COMPONENT(ctx, stream_id, buffer_t);     /* application data buffer */

SHIFT_COLLECTION(ctx, connecting_id, sock_id);
SHIFT_COLLECTION(ctx, handshaking_id, sock_id, handshake_id);
SHIFT_COLLECTION(ctx, established_id, sock_id, stream_id);

/* State transitions are just moves */
shift_entity_move_one(ctx, conn, handshaking_id);  /* connecting → handshaking */
shift_flush(ctx);
/* handshake_id component auto-constructed, ready to use */

shift_entity_move_one(ctx, conn, established_id);  /* handshaking → established */
shift_flush(ctx);
/* handshake_id auto-destructed, stream_id auto-constructed */
```

Each state guarantees exactly the right components exist. Invalid access is
impossible — asking for `stream_id` on a connecting entity returns
`shift_error_not_found`.

### Pattern C: Two-phase create with dynamic context

Sometimes a component needs to be initialized with runtime state that only the
creator knows — an external connection ID, a file descriptor from an accept
call, a pointer into a foreign library's handle table. Constructors can't
provide this because they only see zero-initialized memory.

The two-phase `create_begin` / `create_end` pattern solves this:

```c
SHIFT_COMPONENT(ctx, sock_id, int);
SHIFT_COMPONENT(ctx, conn_id, uint64_t);   /* external connection handle */
SHIFT_COLLECTION(ctx, connections_id, sock_id, conn_id);

/* Phase 1: begin — entity is allocated, constructors run, components
 * are accessible, but the entity is NOT yet visible to collection
 * iteration and on_enter has NOT fired. */
shift_entity_t conn;
shift_entity_create_one_begin(ctx, connections_id, &conn);

/* Write dynamic state that only we know at creation time. */
uint64_t *cid;
shift_entity_get_component(ctx, conn, conn_id, (void **)&cid);
*cid = external_library_accept();

int *fd;
shift_entity_get_component(ctx, conn, sock_id, (void **)&fd);
*fd = accepted_socket_fd;

/* Phase 2: end — entity becomes visible, on_enter fires.
 * Any on_enter handler now sees fully initialized components. */
shift_entity_create_one_end(ctx, conn);
```

Between `begin` and `end` the entity is in a **constructing** state:
- `shift_entity_get_component` works — the creator can read and write components.
- Collection iteration (`shift_collection_get_entities`, `shift_collection_get_component_array`) does **not** include the entity.
- `shift_entity_move`, `shift_entity_destroy`, and `shift_entity_revoke` all reject it with `shift_error_stale`.
- `on_enter` fires only at `create_end`, so other code reacting to the collection sees a fully formed entity, never a half-initialized one.

Batch creates work the same way:

```c
shift_entity_t *ents;
shift_entity_create_begin(ctx, 10, connections_id, &ents);

for (int i = 0; i < 10; i++) {
    uint64_t *cid;
    shift_entity_get_component(ctx, ents[i], conn_id, (void **)&cid);
    *cid = pending_connections[i].external_id;
}

shift_entity_create_end(ctx, ents, 10);
/* single on_enter callback with count=10, all entities fully initialized */
```

### Pattern D: Fast batch processing in callbacks

Every callback — `on_enter`, `on_leave`, constructors, destructors — receives
the collection's entity array, an offset, and a count. The offset is the same
index into every SoA column in that collection. This means you never need to
call `shift_entity_get_component` in a loop. You grab the column base pointer
once and rip through it with pointer arithmetic.

Here is a concrete example. Suppose you have a network server where each
connection has a socket fd and a per-connection statistics struct. When
connections enter the `established` collection you want to register them with
epoll and zero their stats:

```c
typedef struct { uint64_t bytes_in; uint64_t bytes_out; } conn_stats_t;

SHIFT_COMPONENT(ctx, sock_id, int);
SHIFT_COMPONENT(ctx, stats_id, conn_stats_t);
SHIFT_COLLECTION(ctx, established_id, sock_id, stats_id);
```

Without the offset-based signature you would write the on_enter callback like
this — one `shift_entity_get_component` call per entity, per component:

```c
/* Slow: two indirect lookups per entity per iteration */
void on_establish_slow(shift_t *ctx, shift_collection_id_t col_id,
                       const shift_entity_t *entities, uint32_t offset,
                       uint32_t count, void *user_ctx) {
    int epfd = *(int *)user_ctx;
    for (uint32_t i = 0; i < count; i++) {
        int *fd;
        shift_entity_get_component(ctx, entities[offset + i], sock_id,
                                   (void **)&fd);
        conn_stats_t *st;
        shift_entity_get_component(ctx, entities[offset + i], stats_id,
                                   (void **)&st);
        struct epoll_event ev = {.events = EPOLLIN, .data.fd = *fd};
        epoll_ctl(epfd, EPOLL_CTL_ADD, *fd, &ev);
        *st = (conn_stats_t){0};
    }
}
```

With the offset you grab each column base pointer once and iterate with
straight pointer arithmetic — no hash lookups, no handle validation, just
sequential memory access:

```c
/* Fast: two column fetches, then a tight loop over contiguous memory */
void on_establish(shift_t *ctx, shift_collection_id_t col_id,
                  const shift_entity_t *entities, uint32_t offset,
                  uint32_t count, void *user_ctx) {
    int epfd = *(int *)user_ctx;

    void *fd_base, *st_base;
    shift_collection_get_component_array(ctx, col_id, sock_id,
                                         &fd_base, NULL);
    shift_collection_get_component_array(ctx, col_id, stats_id,
                                         &st_base, NULL);

    int          *fds   = (int *)fd_base + offset;
    conn_stats_t *stats = (conn_stats_t *)st_base + offset;

    for (uint32_t i = 0; i < count; i++) {
        struct epoll_event ev = {.events = EPOLLIN, .data.fd = fds[i]};
        epoll_ctl(epfd, EPOLL_CTL_ADD, fds[i], &ev);
        stats[i] = (conn_stats_t){0};
    }
}

shift_collection_on_enter(ctx, established_id, on_establish, &epfd, NULL);
```

The same pattern works in destructors. Here is an on_leave that deregisters
fds from epoll when connections leave the established collection:

```c
void on_disconnect(shift_t *ctx, shift_collection_id_t col_id,
                   const shift_entity_t *entities, uint32_t offset,
                   uint32_t count, void *user_ctx) {
    int epfd = *(int *)user_ctx;

    void *fd_base;
    shift_collection_get_component_array(ctx, col_id, sock_id,
                                         &fd_base, NULL);
    int *fds = (int *)fd_base + offset;

    for (uint32_t i = 0; i < count; i++)
        epoll_ctl(epfd, EPOLL_CTL_DEL, fds[i], NULL);
}

shift_collection_on_leave(ctx, established_id, on_disconnect, &epfd, NULL);
```

The key insight: `offset` and `count` describe a contiguous slice that is the
same across the entity array and every component column. Fetch the base, add
the offset, loop over count. That's it. No per-entity indirection, no handle
validation overhead, just linear memory access — which is exactly what the
hardware prefetcher wants to see.

This matters when your callbacks fire on hundreds or thousands of entities per
flush. A constructor that zero-inits 4096 stats structs at `data + offset` is
a single `memset`. An on_enter that registers 200 fds with epoll is a tight
loop with no branches except the syscall. The batching in `shift_flush` groups
contiguous operations together so you get these large runs for free.

### Pattern E: SIMD-aligned components

When processing components with SIMD intrinsics, the SoA column must be aligned
to the SIMD register width. Set `alignment` at registration time and shift
guarantees the column base pointer satisfies it — through growth, migration, and
reallocation:

```c
/* 32-byte aligned for AVX */
shift_component_info_t pos_info = {
    .element_size = sizeof(float) * 8,  /* 8 floats = 32 bytes */
    .alignment    = 32,
};
shift_component_id_t pos_id;
shift_component_register(ctx, &pos_info, &pos_id);

SHIFT_COLLECTION(ctx, particles_id, pos_id);
shift_collection_reserve(ctx, particles_id, 4096);

/* Column pointer is guaranteed 32-byte aligned */
void  *base;
size_t count;
shift_collection_get_component_array(ctx, particles_id, pos_id, &base, &count);

__m256 *positions = (__m256 *)base;  /* safe — properly aligned */
for (size_t i = 0; i < count; i++)
    positions[i] = _mm256_add_ps(positions[i], velocity_vec);
```

If your allocator already provides aligned allocation (e.g. `mimalloc_aligned_alloc`),
set `aligned_alloc`, `aligned_realloc`, and `aligned_free` on the
`shift_allocator_t` to avoid the overhead of shift's manual alignment fallback.

### Pattern F: Building a query system on top of shift

Shift is designed as a storage foundation. A consumer building an archetype-style
query system needs two things from the storage layer: "which collections contain
component X?" and "what components does collection Y have?" Both are available
through the introspection APIs:

```c
/* Find all collections that have both position and velocity */
const shift_collection_id_t *pos_cols, *vel_cols;
size_t pos_count, vel_count;
shift_component_get_collections(ctx, pos_id, &pos_cols, &pos_count);
shift_component_get_collections(ctx, vel_id, &vel_cols, &vel_count);

/* Intersect the two sorted lists to find matching collections */
size_t pi = 0, vi = 0;
while (pi < pos_count && vi < vel_count) {
    if (pos_cols[pi] == vel_cols[vi]) {
        shift_collection_id_t col = pos_cols[pi];

        /* Iterate this collection's position and velocity arrays */
        void  *pos_base, *vel_base;
        size_t count;
        shift_collection_get_component_array(ctx, col, pos_id, &pos_base, &count);
        shift_collection_get_component_array(ctx, col, vel_id, &vel_base, NULL);

        float (*pos)[3] = pos_base;
        float (*vel)[3] = vel_base;
        for (size_t i = 0; i < count; i++) {
            pos[i][0] += vel[i][0] * dt;
            pos[i][1] += vel[i][1] * dt;
            pos[i][2] += vel[i][2] * dt;
        }
        pi++; vi++;
    } else if (pos_cols[pi] < vel_cols[vi]) {
        pi++;
    } else {
        vi++;
    }
}
```

The reverse index (`shift_component_get_collections`) and collection introspection
(`shift_collection_get_components`) are maintained internally. A consumer never
needs to iterate all collections and inspect them manually.

### Pattern G: Capacity reservation for bulk loading

When you know how many entities you'll create — loading a level, spawning a wave
of particles, accepting a batch of connections — pre-allocate with
`shift_collection_reserve` to avoid incremental reallocation during the load:

```c
SHIFT_COLLECTION(ctx, particles_id, pos_id, vel_id, life_id);

/* Pre-allocate for 10000 particles — one allocation, no growth during spawn */
shift_collection_reserve(ctx, particles_id, 10000);

shift_entity_t *ep;
shift_entity_create(ctx, 10000, particles_id, &ep);
shift_flush(ctx);
/* Constructor fires once with count=10000 on pre-allocated memory */
```

Without the reserve call, shift would grow the collection through 8 → 16 → 32 →
... → 16384, performing a reallocation (and memcpy of all existing data) at each
step. With the reserve, it's a single allocation up front.

### Pattern H: Component metadata for consumer layers

A consumer layer (ECS framework, editor, serialization system) often needs to
attach metadata to components — type names, serialization functions, editor
widgets, tag flags. The `user_data` pointer on `shift_component_info_t` provides
a hook without requiring parallel arrays:

```c
typedef struct {
    const char *type_name;
    void (*serialize)(const void *data, FILE *out);
    void (*deserialize)(void *data, FILE *in);
    bool is_tag;  /* zero-size tag component */
} component_meta_t;

component_meta_t pos_meta = {
    .type_name   = "Position",
    .serialize   = serialize_vec3,
    .deserialize = deserialize_vec3,
};

shift_component_info_t pos_info = {
    .element_size = sizeof(float) * 3,
    .user_data    = &pos_meta,
};
shift_component_id_t pos_id;
shift_component_register(ctx, &pos_info, &pos_id);

/* Later, in the serializer: */
void *meta;
shift_component_get_user_data(ctx, pos_id, &meta);
component_meta_t *m = meta;
m->serialize(data, file);
```

This avoids the pattern where every consumer layer maintains a
`component_meta_t metas[MAX_COMPONENTS]` array indexed by component ID. The
metadata lives right next to the component and follows it through the entire API.

## Design notes

- **SoA layout** — each component column is a contiguous `void *` array. Iterating a single component type has no stride overhead.
- **Swap-remove** — when entities are removed from a collection the gap is filled from the tail, keeping arrays dense. Entity metadata tracks the current offset so handles remain valid after swaps.
- **Peek optimization** — the deferred queue merges consecutive moves/creates of contiguous entities to the same destination into a single op, minimising constructor/destructor call overhead.

## License

AGPL-3.0-or-later. See [LICENSE](LICENSE).
