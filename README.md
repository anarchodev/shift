# shift

A C23 library that provides an entity system and collections with deferred mutations and Structure-of-Arrays storage.

## Overview

`shift` organizes objects as lightweight entity handles that move between named **collections**. Mutations (creates, moves, destroys) are queued and applied in batch on an explicit `shift_flush()` call, enabling safe iteration without invalidating pointers mid-frame.

**Core concepts:**

- **Entities** — `{index, generation}` handles. The generation increments on destruction, making stale handles detectable.
- **Components** — typed data blobs registered with `shift_component_register`. Each component has an `element_size` and optional `constructor`/`destructor` callbacks invoked in batch on flush.
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

Pass a custom `shift_allocator_t` (alloc/realloc/free + user ctx pointer) to use an arena or pool allocator.

### 2. Register components

```c
/* Verbose form */
shift_component_info_t pos_info = {
    .element_size = sizeof(float) * 3,
    .constructor  = NULL,
    .destructor   = NULL,
};
shift_component_id_t pos_id;
shift_component_register(ctx, &pos_info, &pos_id);

/* Convenience macros */
SHIFT_COMPONENT(ctx, pos_id, float[3]);
SHIFT_COMPONENT(ctx, vel_id, float[3]);
SHIFT_COMPONENT_EX(ctx, hp_id, int, hp_ctor, hp_dtor);  /* with ctor/dtor */
```

`constructor` and `destructor` are `void fn(void *data, uint32_t count)`. They are called in batch during `shift_flush()` whenever entities enter or leave a collection that owns the component.

### 3. Register collections

```c
/* Verbose form */
shift_component_id_t moving_comps[] = {pos_id, vel_id};

shift_collection_info_t col_info = {
    .comp_ids     = moving_comps,
    .comp_count   = 2,
    .max_capacity = 0,       /* 0 = dynamic growth */
    .on_enter     = NULL,
    .on_leave     = NULL,
};
shift_collection_id_t moving_id;
shift_collection_register(ctx, &col_info, &moving_id);

/* Convenience macros */
SHIFT_COLLECTION(ctx, moving_id, pos_id, vel_id);
SHIFT_COLLECTION_EX(ctx, fixed_id, 64, on_enter_fn, on_leave_fn, pos_id);
SHIFT_COLLECTION_EMPTY(ctx, marker_id);   /* zero-component state marker */
```

Set `max_capacity` to a non-zero value for a **fixed-capacity** collection. All storage for that collection is allocated up-front at registration time — no heap activity occurs during `shift_flush`. Attempting to move more entities into the collection than its capacity allows returns `shift_error_full` from `shift_flush`.

Zero-component collections (`comp_count = 0`) are valid and useful as state markers or staging areas. They store only entity handles with no component data.

**on_enter / on_leave contract:**
- `on_enter(ctx, entities, count)` fires after entities are fully placed — components are zero-inited, constructors have been called, and the entity handle is valid inside the callback.
- `on_leave(ctx, entities, count)` fires before entities are removed — components are still accessible via `shift_entity_get_component` inside the callback.
- For moves: on_leave fires on the source, then on_enter fires on the destination.
- For creates: on_enter only. For destroys: on_leave only.

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
static void fd_destructor(void *data, uint32_t count) {
    int *fds = data;
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

## Design notes

- **SoA layout** — each component column is a contiguous `void *` array. Iterating a single component type has no stride overhead.
- **Swap-remove** — when entities are removed from a collection the gap is filled from the tail, keeping arrays dense. Entity metadata tracks the current offset so handles remain valid after swaps.
- **Peek optimization** — the deferred queue merges consecutive moves/creates of contiguous entities to the same destination into a single op, minimising constructor/destructor call overhead.

## License

AGPL-3.0-or-later. See [LICENSE](LICENSE).
