#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --------------------------------------------------------------------------
 * Result codes
 * -------------------------------------------------------------------------- */

typedef int shift_result_t;

constexpr shift_result_t shift_ok              = 0;
constexpr shift_result_t shift_error_null      = -1;
constexpr shift_result_t shift_error_oom       = -2;
constexpr shift_result_t shift_error_stale     = -3;
constexpr shift_result_t shift_error_full      = -4;
constexpr shift_result_t shift_error_not_found = -5;
constexpr shift_result_t shift_error_invalid   = -6;

/* --------------------------------------------------------------------------
 * Core handle types
 * -------------------------------------------------------------------------- */

typedef struct {
  uint32_t index;
  uint32_t generation;
} shift_entity_t;

typedef uint32_t shift_component_id_t;
typedef uint32_t shift_collection_id_t;
typedef uint32_t shift_handler_id_t;

typedef struct shift_s shift_t;

/**
 * Collection callback (on_enter / on_leave).
 *
 * @param ctx        The shift context.
 * @param col_id     Collection the entities are entering or leaving.
 * @param entities   Base pointer to the collection's entity array.
 * @param offset     Index of the first affected entity in the array.
 * @param count      Number of contiguous entities starting at offset.
 * @param user_ctx   User pointer passed at handler registration time.
 *
 * Use offset and count to index into any SoA column for this collection:
 *   shift_collection_get_component_array(ctx, col_id, comp, &base, NULL);
 *   MyType *p = (MyType *)base + offset;
 *   // p[0..count-1] are the affected elements
 *
 * **Callback safety rules** (same rules apply to component ctor/dtor):
 *
 * Always safe (any callback context):
 *   - shift_entity_get_component (on affected or unrelated entities)
 *   - shift_collection_get_component_array, shift_collection_get_entities
 *   - shift_entity_is_stale, shift_entity_is_moving,
 *     shift_entity_get_collection
 *   - All introspection (shift_collection_count,
 *     shift_collection_get_components, shift_component_get_collections,
 *     shift_component_get_user_data, shift_collection_entity_count)
 *
 * Safe only in _immediate callbacks (NOT during shift_flush):
 *   - shift_entity_create, shift_entity_move, shift_entity_destroy
 *     (deferred variants — they enqueue for the next flush)
 *
 * Never safe during shift_flush, risky in _immediate callbacks:
 *   - All _immediate operations (create/move/destroy_immediate)
 *   - shift_collection_reserve
 *   These can reallocate columns, corrupt flush iteration state, or
 *   trigger nested callbacks. In _immediate callbacks they are safe
 *   only if they target collections not involved in the current
 *   operation.
 */
typedef void (*shift_collection_callback_t)(shift_t               *ctx,
                                            shift_collection_id_t  col_id,
                                            const shift_entity_t  *entities,
                                            uint32_t               offset,
                                            uint32_t               count,
                                            void                  *user_ctx);

/**
 * Component constructor — called in batch when entities enter a collection
 * that owns this component.
 *
 * @param ctx        The shift context.
 * @param col_id     Destination collection.
 * @param entities   Base pointer to the collection's entity array.
 * @param data       Base pointer to this component's SoA column.
 * @param offset     Index of the first new slot in entities and data.
 * @param count      Number of contiguous slots to initialize.
 * @param user_data  Opaque pointer from shift_component_info_t::user_data.
 *
 * Typical usage: MyType *p = (MyType *)data + offset;
 *
 * See shift_collection_callback_t for callback safety rules.
 */
typedef void (*shift_component_ctor_t)(shift_t               *ctx,
                                       shift_collection_id_t  col_id,
                                       const shift_entity_t  *entities,
                                       void                  *data,
                                       uint32_t               offset,
                                       uint32_t               count,
                                       void                  *user_data);

/**
 * Component destructor — called in batch when entities leave a collection
 * that owns this component.
 *
 * @param ctx        The shift context.
 * @param col_id     Source collection (entity is still addressable).
 * @param entities   Base pointer to the collection's entity array.
 * @param data       Base pointer to this component's SoA column.
 * @param offset     Index of the first slot being torn down.
 * @param count      Number of contiguous slots to tear down.
 * @param user_data  Opaque pointer from shift_component_info_t::user_data.
 *
 * Typical usage: MyType *p = (MyType *)data + offset;
 *
 * See shift_collection_callback_t for callback safety rules.
 */
typedef void (*shift_component_dtor_t)(shift_t               *ctx,
                                       shift_collection_id_t  col_id,
                                       const shift_entity_t  *entities,
                                       void                  *data,
                                       uint32_t               offset,
                                       uint32_t               count,
                                       void                  *user_data);

/* --------------------------------------------------------------------------
 * Allocator
 * -------------------------------------------------------------------------- */

typedef struct {
  void *(*alloc)(size_t size, void *ctx);
  void *(*realloc)(void *ptr, size_t size, void *ctx);
  void (*free)(void *ptr, void *ctx);
  void *(*aligned_alloc)(size_t size, size_t alignment, void *ctx);
  void *(*aligned_realloc)(void *ptr, size_t size, size_t alignment, void *ctx);
  void (*aligned_free)(void *ptr, void *ctx);
  void *ctx;
} shift_allocator_t;

/* --------------------------------------------------------------------------
 * Component info
 * -------------------------------------------------------------------------- */

typedef struct {
  size_t                 element_size;
  size_t                 alignment; /* 0 = default (alignof(max_align_t)) */
  shift_component_ctor_t constructor;
  shift_component_dtor_t destructor;
  void                  *user_data; /* opaque, stored and returned as-is */
} shift_component_info_t;

/* --------------------------------------------------------------------------
 * Context configuration
 * -------------------------------------------------------------------------- */

typedef struct {
  size_t            max_entities;
  size_t            max_components;
  size_t            max_collections;
  size_t            deferred_queue_capacity;
  shift_allocator_t allocator;
} shift_config_t;

/* --------------------------------------------------------------------------
 * Forward declarations for internal types used only as pointers in shift_t
 * -------------------------------------------------------------------------- */

typedef struct shift_collection_entry_s shift_collection_entry_t;
typedef struct shift_deferred_op_s      shift_deferred_op_t;
typedef struct shift_migration_recipe_s shift_migration_recipe_t;

/* --------------------------------------------------------------------------
 * Per-entity metadata
 * -------------------------------------------------------------------------- */

typedef struct {
  uint32_t              generation;
  shift_collection_id_t col_id;
  uint32_t              offset;
  bool                  has_pending_move;
  bool                  constructing; /* between create_begin and create_end */
} shift_metadata_t;

/* --------------------------------------------------------------------------
 * Context (full definition — needed for inline helpers below)
 * -------------------------------------------------------------------------- */

struct shift_s {
  shift_allocator_t         allocator;
  shift_metadata_t         *metadata; /* [max_entities] */
  size_t                    max_entities;
  size_t                    null_front;
  shift_component_info_t   *components;      /* [max_components] */
  size_t                    max_components;
  uint32_t                  component_count;
  void                     *comp_collections; /* [max_components] internal */
  shift_collection_entry_t *collections; /* [max_collections] */
  size_t                    max_collections;
  size_t                    collection_count;
  shift_deferred_op_t      *deferred_queue; /* [deferred_queue_capacity] */
  size_t                    deferred_queue_capacity;
  size_t                    deferred_queue_count;
  uint32_t                 *max_src_offset; /* [max_collections] */
  bool                      needs_sort;
  shift_migration_recipe_t *migration_recipes;
  size_t                    migration_recipe_count;
  size_t                    migration_recipe_capacity;
  uint32_t                  next_handler_id;
};

/* --------------------------------------------------------------------------
 * Context lifecycle
 * -------------------------------------------------------------------------- */

/**
 * Allocate and initialize a new shift context.
 *
 * @param config  Configuration (max entities, components, collections, queue
 *                capacity, optional custom allocator). Zeroed allocator fields
 *                fall back to libc malloc/realloc/free.
 * @param out     Receives the new context pointer on success.
 * @return shift_ok, shift_error_null, or shift_error_oom.
 */
shift_result_t shift_context_create(const shift_config_t *config,
                                    shift_t             **out);

/**
 * Destroy a shift context and free all associated memory.
 *
 * @param ctx  Context to destroy. NULL is a safe no-op.
 */
void           shift_context_destroy(shift_t *ctx);

/* --------------------------------------------------------------------------
 * Component registration
 * -------------------------------------------------------------------------- */

/**
 * Register a new component type.
 *
 * @param ctx     The shift context.
 * @param info    Component descriptor (element_size, optional ctor/dtor).
 * @param out_id  Receives the assigned component ID on success.
 * @return shift_ok, shift_error_null, shift_error_full, or shift_error_invalid.
 */
shift_result_t shift_component_register(shift_t                      *ctx,
                                        const shift_component_info_t *info,
                                        shift_component_id_t         *out_id);

/**
 * Get the user_data pointer stored at component registration time.
 *
 * @param ctx        The shift context.
 * @param comp_id    Component ID.
 * @param out_data   Receives the user_data pointer.
 * @return shift_ok, shift_error_null, or shift_error_not_found.
 */
shift_result_t shift_component_get_user_data(const shift_t        *ctx,
                                             shift_component_id_t  comp_id,
                                             void                **out_data);

/**
 * Get the list of collections that contain a given component.
 *
 * @param ctx        The shift context.
 * @param comp_id    Component ID.
 * @param out_ids    Receives a read-only pointer to the collection ID array.
 * @param out_count  Receives the number of collection IDs.
 * @return shift_ok, shift_error_null, or shift_error_not_found.
 */
shift_result_t shift_component_get_collections(
    const shift_t *ctx, shift_component_id_t comp_id,
    const shift_collection_id_t **out_ids, size_t *out_count);

/* --------------------------------------------------------------------------
 * Collection info and registration
 * -------------------------------------------------------------------------- */

typedef struct {
  const shift_component_id_t *comp_ids;
  size_t                      comp_count;
  size_t                      max_capacity; /* leave 0 for dynamic */
} shift_collection_info_t;

/**
 * Register a new collection with a fixed set of component types.
 *
 * @param ctx     The shift context.
 * @param info    Collection descriptor (component IDs, count, optional fixed
 *                capacity). Zero comp_count creates an empty collection useful
 *                as a state marker. Zero max_capacity means dynamic growth.
 * @param out_id  Receives the assigned collection ID on success.
 * @return shift_ok, shift_error_null, shift_error_full, or shift_error_oom.
 */
shift_result_t shift_collection_register(shift_t                       *ctx,
                                         const shift_collection_info_t *info,
                                         shift_collection_id_t         *out_id);

/**
 * Get a raw pointer to a component's SoA column in a collection.
 *
 * @param ctx        The shift context.
 * @param col_id     Target collection.
 * @param comp_id    Component to look up in that collection.
 * @param out_array  Receives the base pointer to the contiguous column data.
 * @param out_count  Receives the number of live entities (may be NULL).
 * @return shift_ok, shift_error_null, or shift_error_not_found if the
 *         collection does not contain comp_id.
 *
 * @warning The returned pointer and count are a snapshot. Any subsequent
 * shift_flush(), _immediate operation, or shift_collection_reserve() on this
 * collection can reallocate the column (invalidating the pointer) and change
 * the entity count and ordering via swap-remove. Re-fetch after any mutation.
 * See shift_entity_get_component for the full explanation.
 */
shift_result_t
shift_collection_get_component_array(shift_t *ctx, shift_collection_id_t col_id,
                                     shift_component_id_t comp_id,
                                     void **out_array, size_t *out_count);

/**
 * Get a pointer to a collection's entity handle array.
 *
 * @param ctx           The shift context.
 * @param col_id        Target collection.
 * @param out_entities  Receives the base pointer to the entity array.
 * @param out_count     Receives the number of live entities (may be NULL).
 * @return shift_ok, shift_error_null, or shift_error_not_found.
 *
 * @warning Same lifetime rules as shift_collection_get_component_array.
 * The pointer and count are invalidated by any mutation on this collection.
 */
shift_result_t shift_collection_get_entities(shift_t              *ctx,
                                             shift_collection_id_t col_id,
                                             shift_entity_t      **out_entities,
                                             size_t               *out_count);

/**
 * Pre-allocate storage for at least capacity entities in a collection.
 * Avoids incremental reallocation when bulk-loading a known number of
 * entities.  Respects max_capacity if set at registration time.
 *
 * @param ctx       The shift context.
 * @param col_id    Target collection.
 * @param capacity  Minimum number of entity slots to allocate.
 * @return shift_ok, shift_error_null, shift_error_not_found, shift_error_full,
 *         or shift_error_oom.
 */
shift_result_t shift_collection_reserve(shift_t              *ctx,
                                        shift_collection_id_t col_id,
                                        size_t                capacity);

/* --------------------------------------------------------------------------
 * Collection callbacks — add/remove on_enter and on_leave handlers.
 * Multiple handlers per event per collection; fired in registration order.
 * -------------------------------------------------------------------------- */

/**
 * Register a callback that fires when entities enter a collection.
 * Called after components are zero-inited and constructors have run.
 *
 * @param ctx       The shift context.
 * @param col_id    Collection to watch.
 * @param fn        Callback function.
 * @param user_ctx  Opaque pointer forwarded to fn on each invocation.
 * @param out_id    Receives the handler ID for later removal (may be NULL).
 * @return shift_ok, shift_error_null, shift_error_not_found, or
 *         shift_error_oom.
 */
shift_result_t shift_collection_on_enter(shift_t                     *ctx,
                                         shift_collection_id_t        col_id,
                                         shift_collection_callback_t  fn,
                                         void                        *user_ctx,
                                         shift_handler_id_t          *out_id);

/**
 * Register a callback that fires when entities leave a collection.
 * Called before removal — components are still accessible inside the callback.
 *
 * @param ctx       The shift context.
 * @param col_id    Collection to watch.
 * @param fn        Callback function.
 * @param user_ctx  Opaque pointer forwarded to fn on each invocation.
 * @param out_id    Receives the handler ID for later removal (may be NULL).
 * @return shift_ok, shift_error_null, shift_error_not_found, or
 *         shift_error_oom.
 */
shift_result_t shift_collection_on_leave(shift_t                     *ctx,
                                         shift_collection_id_t        col_id,
                                         shift_collection_callback_t  fn,
                                         void                        *user_ctx,
                                         shift_handler_id_t          *out_id);

/**
 * Remove a previously registered on_enter or on_leave handler.
 * Searches both handler lists for the given collection.
 *
 * @param ctx         The shift context.
 * @param col_id      Collection the handler was registered on.
 * @param handler_id  ID returned by shift_collection_on_enter/on_leave.
 * @return shift_ok, shift_error_null, or shift_error_not_found.
 */
shift_result_t shift_collection_remove_handler(shift_t              *ctx,
                                               shift_collection_id_t col_id,
                                               shift_handler_id_t    handler_id);

/* --------------------------------------------------------------------------
 * Entity operations (deferred)
 * -------------------------------------------------------------------------- */

/**
 * Queue creation of count entities in dest_col_id. Entities are reserved
 * immediately but not placed until shift_flush(). Components are not
 * accessible until after flush.
 *
 * @param ctx           The shift context.
 * @param count         Number of entities to create (must be >= 1).
 * @param dest_col_id   Destination collection.
 * @param out_entities  Receives a pointer into internal storage holding the
 *                      new entity handles. Valid only until the next
 *                      shift_flush() — copy handles if needed beyond that.
 * @return shift_ok, shift_error_null, shift_error_invalid, shift_error_full,
 *         or shift_error_not_found.
 */
shift_result_t shift_entity_create(shift_t *ctx, uint32_t count,
                                   shift_collection_id_t dest_col_id,
                                   shift_entity_t      **out_entities);

/**
 * Queue creation of a single entity. Convenience wrapper around
 * shift_entity_create with count=1.
 *
 * @param ctx          The shift context.
 * @param dest_col_id  Destination collection.
 * @param out_entity   Receives the new entity handle.
 * @return Same as shift_entity_create.
 */
shift_result_t shift_entity_create_one(shift_t              *ctx,
                                       shift_collection_id_t dest_col_id,
                                       shift_entity_t       *out_entity);

/**
 * Queue a move of count entities to dest_col_id. Takes effect at
 * shift_flush(). Shared components are copied, new components are
 * constructed, dropped components are destructed.
 *
 * @param ctx          The shift context.
 * @param entities     Array of entity handles to move.
 * @param count        Number of entities.
 * @param dest_col_id  Destination collection.
 * @return shift_ok, shift_error_null, shift_error_stale, shift_error_invalid,
 *         or shift_error_not_found.
 */
shift_result_t shift_entity_move(shift_t *ctx, const shift_entity_t *entities,
                                 uint32_t              count,
                                 shift_collection_id_t dest_col_id);

/**
 * Queue a move of a single entity. Convenience wrapper around
 * shift_entity_move with count=1.
 *
 * @param ctx          The shift context.
 * @param entity       Entity handle to move.
 * @param dest_col_id  Destination collection.
 * @return Same as shift_entity_move.
 */
shift_result_t shift_entity_move_one(shift_t *ctx, shift_entity_t entity,
                                     shift_collection_id_t dest_col_id);

/**
 * Get a pointer to an entity's data for a specific component.
 *
 * @param ctx       The shift context.
 * @param entity    Entity handle.
 * @param comp_id   Component to look up.
 * @param out_data  Receives a pointer to the entity's component data.
 * @return shift_ok, shift_error_null, shift_error_stale (handle expired or
 *         entity has a pending move), or shift_error_not_found (component not
 *         in entity's collection).
 *
 * @warning The returned pointer is a borrowed reference, valid only until the
 * next shift_flush(), _immediate operation, or shift_collection_reserve() that
 * touches the entity's collection. Any of these can trigger swap-remove
 * (which overwrites the slot with tail data), reallocation (which moves the
 * entire column to new memory), or migration (which moves the entity to a
 * different collection entirely). After any such operation the pointer may
 * reference a different entity's data, freed memory, or reallocated memory.
 *
 * Never cache the returned pointer. Store the shift_entity_t handle instead
 * and call this function again when you need the data. The handle remains
 * valid across flushes (check with shift_entity_is_stale if unsure); the
 * pointer does not.
 */
shift_result_t shift_entity_get_component(shift_t *ctx, shift_entity_t entity,
                                          shift_component_id_t comp_id,
                                          void               **out_data);

/**
 * Queue destruction of count entities. Takes effect at shift_flush().
 * Internally this moves entities to the null collection, firing on_leave and
 * destructors.
 *
 * @param ctx       The shift context.
 * @param entities  Array of entity handles to destroy.
 * @param count     Number of entities.
 * @return shift_ok, shift_error_null, shift_error_stale, or
 *         shift_error_invalid.
 */
shift_result_t shift_entity_destroy(shift_t              *ctx,
                                    const shift_entity_t *entities,
                                    uint32_t              count);

/**
 * Queue destruction of a single entity. Convenience wrapper around
 * shift_entity_destroy with count=1.
 *
 * @param ctx     The shift context.
 * @param entity  Entity handle to destroy.
 * @return Same as shift_entity_destroy.
 */
shift_result_t shift_entity_destroy_one(shift_t *ctx, shift_entity_t entity);

/* --------------------------------------------------------------------------
 * Immediate operations — bypass the deferred queue entirely.
 * Constructors, destructors, on_enter, on_leave all fire inline.
 * Returns shift_error_stale if entity has a pending deferred move.
 * -------------------------------------------------------------------------- */

/**
 * Create count entities immediately in dest_col_id. Constructors and
 * on_enter fire inline before this function returns.
 *
 * @param ctx           The shift context.
 * @param count         Number of entities to create (must be >= 1).
 * @param dest_col_id   Destination collection.
 * @param out_entities  Receives a pointer to the new entity handles in the
 *                      collection's entity array. Valid until the next
 *                      mutation that touches this collection.
 * @return shift_ok, shift_error_null, shift_error_invalid, shift_error_full,
 *         shift_error_not_found, or shift_error_oom.
 */
shift_result_t shift_entity_create_immediate(shift_t              *ctx,
                                              uint32_t              count,
                                              shift_collection_id_t dest_col_id,
                                              shift_entity_t      **out_entities);

/**
 * Create a single entity immediately. Convenience wrapper around
 * shift_entity_create_immediate with count=1.
 *
 * @param ctx          The shift context.
 * @param dest_col_id  Destination collection.
 * @param out_entity   Receives the new entity handle.
 * @return Same as shift_entity_create_immediate.
 */
shift_result_t shift_entity_create_one_immediate(shift_t              *ctx,
                                                  shift_collection_id_t dest_col_id,
                                                  shift_entity_t       *out_entity);

/**
 * Move count entities to dest_col_id immediately. Fires on_leave on the
 * source, destructors for dropped components, constructors for new
 * components, and on_enter on the destination — all inline.
 *
 * @param ctx          The shift context.
 * @param entities     Array of entity handles to move.
 * @param count        Number of entities.
 * @param dest_col_id  Destination collection.
 * @return shift_ok, shift_error_null, shift_error_stale (entity has a pending
 *         deferred move), shift_error_invalid, shift_error_not_found, or
 *         shift_error_oom.
 */
shift_result_t shift_entity_move_immediate(shift_t              *ctx,
                                            const shift_entity_t *entities,
                                            uint32_t              count,
                                            shift_collection_id_t dest_col_id);

/**
 * Move a single entity immediately. Convenience wrapper around
 * shift_entity_move_immediate with count=1.
 *
 * @param ctx          The shift context.
 * @param entity       Entity handle to move.
 * @param dest_col_id  Destination collection.
 * @return Same as shift_entity_move_immediate.
 */
shift_result_t shift_entity_move_one_immediate(shift_t              *ctx,
                                                shift_entity_t        entity,
                                                shift_collection_id_t dest_col_id);

/**
 * Destroy count entities immediately. Fires on_leave and destructors inline.
 * Entity handles become stale before this function returns.
 *
 * @param ctx       The shift context.
 * @param entities  Array of entity handles to destroy.
 * @param count     Number of entities.
 * @return shift_ok, shift_error_null, shift_error_stale, or
 *         shift_error_invalid.
 */
shift_result_t shift_entity_destroy_immediate(shift_t              *ctx,
                                               const shift_entity_t *entities,
                                               uint32_t              count);

/**
 * Destroy a single entity immediately. Convenience wrapper around
 * shift_entity_destroy_immediate with count=1.
 *
 * @param ctx     The shift context.
 * @param entity  Entity handle to destroy.
 * @return Same as shift_entity_destroy_immediate.
 */
shift_result_t shift_entity_destroy_one_immediate(shift_t        *ctx,
                                                   shift_entity_t  entity);

/* --------------------------------------------------------------------------
 * Two-phase create
 * -------------------------------------------------------------------------- */

/**
 * Begin a two-phase create: allocate count entities in dest_col_id, zero-init
 * components and run constructors, but do NOT fire on_enter. Between begin
 * and end the caller may write dynamic state into components via
 * shift_entity_get_component. Entities in the begun state are excluded from
 * collection iteration and cannot be moved, destroyed, or revoked.
 *
 * @param ctx           The shift context.
 * @param count         Number of entities to create (must be >= 1).
 * @param dest_col_id   Destination collection.
 * @param out_entities  Receives a pointer to the new entity handles.
 * @return shift_ok, shift_error_null, shift_error_invalid, shift_error_full,
 *         shift_error_not_found, or shift_error_oom.
 */
shift_result_t shift_entity_create_begin(shift_t              *ctx,
                                         uint32_t              count,
                                         shift_collection_id_t dest_col_id,
                                         shift_entity_t      **out_entities);

/**
 * Begin a two-phase create for a single entity. Convenience wrapper around
 * shift_entity_create_begin with count=1.
 *
 * @param ctx          The shift context.
 * @param dest_col_id  Destination collection.
 * @param out_entity   Receives the new entity handle.
 * @return Same as shift_entity_create_begin.
 */
shift_result_t shift_entity_create_one_begin(shift_t              *ctx,
                                             shift_collection_id_t dest_col_id,
                                             shift_entity_t       *out_entity);

/**
 * Finish a two-phase create: promote entities from the constructing state to
 * fully live. Fires on_enter with all entities fully initialized. After this
 * call the entities are visible to collection iteration.
 *
 * @param ctx       The shift context.
 * @param entities  Array of entity handles returned by create_begin.
 * @param count     Number of entities (must match the begin call).
 * @return shift_ok, shift_error_null, shift_error_invalid, or
 *         shift_error_not_found.
 */
shift_result_t shift_entity_create_end(shift_t              *ctx,
                                       const shift_entity_t *entities,
                                       uint32_t              count);

/**
 * Finish a two-phase create for a single entity. Convenience wrapper around
 * shift_entity_create_end with count=1.
 *
 * @param ctx     The shift context.
 * @param entity  Entity handle returned by create_one_begin.
 * @return Same as shift_entity_create_end.
 */
shift_result_t shift_entity_create_one_end(shift_t        *ctx,
                                           shift_entity_t  entity);

/* --------------------------------------------------------------------------
 * Generation revocation
 * -------------------------------------------------------------------------- */

/**
 * Bump an entity's generation, invalidating all existing handles without
 * destroying the entity or moving its data. The entity remains in its current
 * collection with all component data intact.
 *
 * @param ctx      The shift context.
 * @param entity   Current (soon-to-be-stale) entity handle.
 * @param out_new  Receives the new valid handle with the bumped generation.
 * @return shift_ok, shift_error_null, shift_error_stale, or
 *         shift_error_invalid.
 */
shift_result_t shift_entity_revoke(shift_t        *ctx,
                                    shift_entity_t  entity,
                                    shift_entity_t *out_new);

/* --------------------------------------------------------------------------
 * Flush
 * -------------------------------------------------------------------------- */

/**
 * Execute all queued deferred operations (creates, moves, destroys) in order.
 * Consecutive contiguous operations targeting the same destination are merged
 * into a single batch. Constructors, destructors, on_enter, and on_leave
 * callbacks all fire during this call.
 *
 * @param ctx  The shift context.
 * @return shift_ok, shift_error_null, or shift_error_oom.
 */
shift_result_t shift_flush(shift_t *ctx);

/* --------------------------------------------------------------------------
 * Collection introspection
 * -------------------------------------------------------------------------- */

/**
 * Get the number of registered collections (including the internal null
 * collection at index 0).  User-created collections have IDs 1 through
 * shift_collection_count(ctx) - 1.
 *
 * @param ctx  The shift context (NULL returns 0).
 * @return Total collection count, or 0 if ctx is NULL.
 */
static inline size_t shift_collection_count(const shift_t *ctx) {
  return ctx ? ctx->collection_count : 0;
}

/**
 * Get the number of live entities in a collection.
 *
 * @param ctx     The shift context.
 * @param col_id  Target collection.
 * @return Entity count, or 0 if ctx is NULL or col_id is out of range.
 */
size_t shift_collection_entity_count(const shift_t         *ctx,
                                     shift_collection_id_t  col_id);

/**
 * Get the sorted component ID list for a collection.
 *
 * @param ctx       The shift context.
 * @param col_id    Target collection.
 * @param out_ids   Receives a read-only pointer to the sorted component ID
 *                  array.  Valid for the lifetime of the context.
 * @param out_count Receives the number of component IDs.
 * @return shift_ok, shift_error_null, or shift_error_not_found.
 */
shift_result_t shift_collection_get_components(
    const shift_t *ctx, shift_collection_id_t col_id,
    const shift_component_id_t **out_ids, uint32_t *out_count);

/* --------------------------------------------------------------------------
 * Inline entity state queries
 * -------------------------------------------------------------------------- */

/**
 * Check if an entity handle is stale (destroyed, recycled, or never valid).
 *
 * @param ctx     The shift context.
 * @param entity  Entity handle to check.
 * @return true if the handle's generation does not match current metadata.
 */
static inline bool shift_entity_is_stale(const shift_t *ctx,
                                         shift_entity_t entity) {
  if (entity.index >= ctx->max_entities)
    return true;
  return ctx->metadata[entity.index].generation != entity.generation;
}

/**
 * Check if an entity has a deferred move pending. The entity is alive but
 * its destination is not yet committed — shift_entity_get_component will
 * return shift_error_stale until shift_flush() completes.
 *
 * @param ctx     The shift context.
 * @param entity  Entity handle to check.
 * @return true if a deferred move is queued for this entity.
 */
static inline bool shift_entity_is_moving(const shift_t *ctx,
                                          shift_entity_t entity) {
  if (entity.index >= ctx->max_entities)
    return false;
  return ctx->metadata[entity.index].has_pending_move;
}

/**
 * Look up which collection an entity currently belongs to.
 *
 * @param ctx     The shift context.
 * @param entity  Entity handle.
 * @param id      Receives the collection ID on success.
 * @return shift_ok, shift_error_invalid, or shift_error_stale.
 */
static inline shift_result_t
shift_entity_get_collection(const shift_t *ctx, shift_entity_t entity,
                            shift_collection_id_t *id) {
  if (entity.index >= ctx->max_entities)
    return shift_error_invalid;
  if (shift_entity_is_stale(ctx, entity))
    return shift_error_stale;
  *id = ctx->metadata[entity.index].col_id;

  return shift_ok;
}

/* --------------------------------------------------------------------------
 * Convenience registration functions
 * -------------------------------------------------------------------------- */

/**
 * Register a component with no constructor or destructor.
 *
 * @param ctx           The shift context.
 * @param element_size  Size in bytes of one component element.
 * @param err           Receives the result code (may be NULL).
 * @return The assigned component ID (0 on failure).
 */
static inline shift_component_id_t
shift_component_add(shift_t *ctx, size_t element_size, shift_result_t *err) {
  shift_component_id_t   id = 0;
  shift_component_info_t info = {.element_size = element_size};
  shift_result_t         r = shift_component_register(ctx, &info, &id);
  if (err)
    *err = r;
  return id;
}

/**
 * Register a component with a constructor and/or destructor.
 *
 * @param ctx           The shift context.
 * @param element_size  Size in bytes of one component element.
 * @param constructor   Called in batch when entities enter a collection with
 *                      this component (may be NULL).
 * @param destructor    Called in batch when entities leave a collection with
 *                      this component (may be NULL).
 * @param err           Receives the result code (may be NULL).
 * @return The assigned component ID (0 on failure).
 */
static inline shift_component_id_t
shift_component_add_ex(shift_t *ctx, size_t element_size,
                       shift_component_ctor_t constructor,
                       shift_component_dtor_t destructor,
                       shift_result_t *err) {
  shift_component_id_t   id = 0;
  shift_component_info_t info = {.element_size = element_size,
                                 .constructor  = constructor,
                                 .destructor   = destructor};
  shift_result_t         r = shift_component_register(ctx, &info, &id);
  if (err)
    *err = r;
  return id;
}

/**
 * Register a collection from an explicit array of component IDs.
 *
 * @param ctx         The shift context.
 * @param comp_count  Number of components in comp_ids.
 * @param comp_ids    Array of component IDs this collection stores.
 * @param err         Receives the result code (may be NULL).
 * @return The assigned collection ID (0 on failure).
 */
static inline shift_collection_id_t
shift_collection_add(shift_t *ctx, size_t comp_count,
                     const shift_component_id_t *comp_ids,
                     shift_result_t *err) {
  shift_collection_id_t   id = 0;
  shift_collection_info_t info = {.comp_ids   = comp_ids,
                                  .comp_count = comp_count};
  shift_result_t          r = shift_collection_register(ctx, &info, &id);
  if (err)
    *err = r;
  return id;
}

/**
 * Register an empty (zero-component) collection, useful as a state marker
 * or staging area. Stores only entity handles with no component data.
 *
 * @param ctx  The shift context.
 * @param err  Receives the result code (may be NULL).
 * @return The assigned collection ID (0 on failure).
 */
static inline shift_collection_id_t
shift_collection_add_empty(shift_t *ctx, shift_result_t *err) {
  shift_collection_id_t   id = 0;
  shift_collection_info_t info = {0};
  shift_result_t          r = shift_collection_register(ctx, &info, &id);
  if (err)
    *err = r;
  return id;
}

/** Register a collection with inline varargs component IDs. */
#define shift_collection_add_of(ctx, err, ...)                                 \
  shift_collection_add(                                                        \
      (ctx),                                                                   \
      sizeof((shift_component_id_t[]){__VA_ARGS__}) /                          \
          sizeof(shift_component_id_t),                                        \
      (shift_component_id_t[]){__VA_ARGS__}, (err))

/* --------------------------------------------------------------------------
 * Convenience macros for component/collection registration
 * -------------------------------------------------------------------------- */

/** Declares shift_component_id_t NAME and registers it (no ctor/dtor). */
#define SHIFT_COMPONENT(ctx, name, type)                                      \
  shift_component_id_t name;                                                  \
  shift_component_register(                                                   \
      (ctx), &(shift_component_info_t){.element_size = sizeof(type)}, &(name))

/** Like SHIFT_COMPONENT but with constructor/destructor. */
#define SHIFT_COMPONENT_EX(ctx, name, type, ctor, dtor)                       \
  shift_component_id_t name;                                                  \
  shift_component_register(                                                   \
      (ctx),                                                                  \
      &(shift_component_info_t){.element_size = sizeof(type),                 \
                                .constructor  = (ctor),                       \
                                .destructor   = (dtor)},                      \
      &(name))

/** Declares shift_collection_id_t NAME and registers with listed components. */
#define SHIFT_COLLECTION(ctx, name, ...)                                      \
  shift_collection_id_t name;                                                 \
  do {                                                                        \
    shift_component_id_t _comps[] = {__VA_ARGS__};                            \
    shift_collection_register(                                                \
        (ctx),                                                                \
        &(shift_collection_info_t){                                           \
            .comp_ids   = _comps,                                             \
            .comp_count = sizeof(_comps) / sizeof(_comps[0])},                \
        &(name));                                                             \
  } while (0)

/** Like SHIFT_COLLECTION but with a fixed max_capacity. */
#define SHIFT_COLLECTION_CAP(ctx, name, cap, ...)                             \
  shift_collection_id_t name;                                                 \
  do {                                                                        \
    shift_component_id_t _comps[] = {__VA_ARGS__};                            \
    shift_collection_register(                                                \
        (ctx),                                                                \
        &(shift_collection_info_t){                                           \
            .comp_ids     = _comps,                                           \
            .comp_count   = sizeof(_comps) / sizeof(_comps[0]),               \
            .max_capacity = (cap)},                                           \
        &(name));                                                             \
  } while (0)

/** Zero-component collection (state marker / staging area). */
#define SHIFT_COLLECTION_EMPTY(ctx, name)                                     \
  shift_collection_id_t name;                                                 \
  shift_collection_register((ctx), &(shift_collection_info_t){0}, &(name))
