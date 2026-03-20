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

/* --------------------------------------------------------------------------
 * Allocator
 * -------------------------------------------------------------------------- */

typedef struct {
  void *(*alloc)(size_t size, void *ctx);
  void *(*realloc)(void *ptr, size_t size, void *ctx);
  void (*free)(void *ptr, void *ctx);
  void *ctx;
} shift_allocator_t;

/* --------------------------------------------------------------------------
 * Component info
 * -------------------------------------------------------------------------- */

typedef struct {
  size_t element_size;
  void (*constructor)(void *data, uint32_t count);
  void (*destructor)(void *data, uint32_t count);
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
} shift_metadata_t;

/* --------------------------------------------------------------------------
 * Context (full definition — needed for inline helpers below)
 * -------------------------------------------------------------------------- */

typedef struct shift_s {
  shift_allocator_t         allocator;
  shift_metadata_t         *metadata; /* [max_entities] */
  size_t                    max_entities;
  size_t                    null_front;
  shift_component_info_t   *components; /* [max_components] */
  size_t                    max_components;
  uint32_t                  component_count;
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
} shift_t;

/* --------------------------------------------------------------------------
 * Context lifecycle
 * -------------------------------------------------------------------------- */

shift_result_t shift_context_create(const shift_config_t *config,
                                    shift_t             **out);
void           shift_context_destroy(shift_t *ctx);

/* --------------------------------------------------------------------------
 * Component registration
 * -------------------------------------------------------------------------- */

shift_result_t shift_component_register(shift_t                      *ctx,
                                        const shift_component_info_t *info,
                                        shift_component_id_t         *out_id);

/* --------------------------------------------------------------------------
 * Collection info and registration
 * -------------------------------------------------------------------------- */

typedef struct {
  const shift_component_id_t *comp_ids;
  size_t                      comp_count;
  size_t                      max_capacity; /* leave 0 for dynamic */
  void (*on_enter)(shift_t *ctx, const shift_entity_t *entities,
                   uint32_t count); /* called after entities are placed */
  void (*on_leave)(shift_t *ctx, const shift_entity_t *entities,
                   uint32_t count); /* called before entities are removed */
} shift_collection_info_t;

shift_result_t shift_collection_register(shift_t                       *ctx,
                                         const shift_collection_info_t *info,
                                         shift_collection_id_t         *out_id);

shift_result_t
shift_collection_get_component_array(shift_t *ctx, shift_collection_id_t col_id,
                                     shift_component_id_t comp_id,
                                     void **out_array, size_t *out_count);

shift_result_t shift_collection_get_entities(shift_t              *ctx,
                                             shift_collection_id_t col_id,
                                             shift_entity_t      **out_entities,
                                             size_t               *out_count);

/* --------------------------------------------------------------------------
 * Entity operations
 * -------------------------------------------------------------------------- */

/* Deferred create — reserves entity handles but does not place them until
 * shift_flush(). Components are not accessible until after flush.
 * out_entities points into internal storage; valid only until next
 * shift_flush(). */
shift_result_t shift_entity_create(shift_t *ctx, uint32_t count,
                                   shift_collection_id_t dest_col_id,
                                   shift_entity_t      **out_entities);

shift_result_t shift_entity_create_one(shift_t              *ctx,
                                       shift_collection_id_t dest_col_id,
                                       shift_entity_t       *out_entity);

shift_result_t shift_entity_move(shift_t *ctx, const shift_entity_t *entities,
                                 uint32_t              count,
                                 shift_collection_id_t dest_col_id);

shift_result_t shift_entity_move_one(shift_t *ctx, shift_entity_t entity,
                                     shift_collection_id_t dest_col_id);

shift_result_t shift_entity_get_component(shift_t *ctx, shift_entity_t entity,
                                          shift_component_id_t comp_id,
                                          void               **out_data);

shift_result_t shift_entity_destroy(shift_t              *ctx,
                                    const shift_entity_t *entities,
                                    uint32_t              count);
shift_result_t shift_entity_destroy_one(shift_t *ctx, shift_entity_t entity);

/* --------------------------------------------------------------------------
 * Immediate operations — bypass the deferred queue entirely.
 * Constructors, destructors, on_enter, on_leave all fire inline.
 * Returns shift_error_stale if entity has a pending deferred move.
 * -------------------------------------------------------------------------- */

shift_result_t shift_entity_create_immediate(shift_t              *ctx,
                                              uint32_t              count,
                                              shift_collection_id_t dest_col_id,
                                              shift_entity_t      **out_entities);
shift_result_t shift_entity_create_one_immediate(shift_t              *ctx,
                                                  shift_collection_id_t dest_col_id,
                                                  shift_entity_t       *out_entity);

shift_result_t shift_entity_move_immediate(shift_t              *ctx,
                                            const shift_entity_t *entities,
                                            uint32_t              count,
                                            shift_collection_id_t dest_col_id);
shift_result_t shift_entity_move_one_immediate(shift_t              *ctx,
                                                shift_entity_t        entity,
                                                shift_collection_id_t dest_col_id);

shift_result_t shift_entity_destroy_immediate(shift_t              *ctx,
                                               const shift_entity_t *entities,
                                               uint32_t              count);
shift_result_t shift_entity_destroy_one_immediate(shift_t        *ctx,
                                                   shift_entity_t  entity);

/* --------------------------------------------------------------------------
 * Generation revocation — invalidates all existing handles without destroying
 * the entity. Returns the new valid handle via out_new.
 * -------------------------------------------------------------------------- */

shift_result_t shift_entity_revoke(shift_t        *ctx,
                                    shift_entity_t  entity,
                                    shift_entity_t *out_new);

/* --------------------------------------------------------------------------
 * Flush
 * -------------------------------------------------------------------------- */

shift_result_t shift_flush(shift_t *ctx);

/* --------------------------------------------------------------------------
 * Inline entity state queries
 * -------------------------------------------------------------------------- */

/* Returns true if the handle's generation no longer matches — the entity has
 * been destroyed/recycled or was never valid. */
static inline bool shift_entity_is_stale(const shift_t *ctx,
                                         shift_entity_t entity) {
  if (entity.index >= ctx->max_entities)
    return true;
  return ctx->metadata[entity.index].generation != entity.generation;
}

/* Returns true if a deferred move is queued for this entity. The entity is
 * alive but its destination is not yet committed. */
static inline bool shift_entity_is_moving(const shift_t *ctx,
                                          shift_entity_t entity) {
  if (entity.index >= ctx->max_entities)
    return false;
  return ctx->metadata[entity.index].has_pending_move;
}

static inline shift_result_t
shift_entity_get_collection(const shift_t *ctx, shift_entity_t entity,
                            shift_collection_id_t *id) {
  if (entity.index >= ctx->max_entities)
    return shift_error_invalid;
  *id = ctx->metadata[entity.index].col_id;

  return shift_ok;
}

/* --------------------------------------------------------------------------
 * Convenience registration functions
 * -------------------------------------------------------------------------- */

/* Register a component, returning its ID. Stores error in *err (may be NULL). */
static inline shift_component_id_t
shift_component_add(shift_t *ctx, size_t element_size, shift_result_t *err) {
  shift_component_id_t   id = 0;
  shift_component_info_t info = {.element_size = element_size};
  shift_result_t         r = shift_component_register(ctx, &info, &id);
  if (err)
    *err = r;
  return id;
}

/* Register a component with constructor/destructor. */
static inline shift_component_id_t
shift_component_add_ex(shift_t *ctx, size_t element_size,
                       void (*constructor)(void *, uint32_t),
                       void (*destructor)(void *, uint32_t),
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

/* Register a collection from an explicit array of component IDs. */
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

/* Register an empty (zero-component) collection. */
static inline shift_collection_id_t
shift_collection_add_empty(shift_t *ctx, shift_result_t *err) {
  shift_collection_id_t   id = 0;
  shift_collection_info_t info = {0};
  shift_result_t          r = shift_collection_register(ctx, &info, &id);
  if (err)
    *err = r;
  return id;
}

/* Register a collection with inline varargs component IDs. */
#define shift_collection_add_of(ctx, err, ...)                                 \
  shift_collection_add(                                                        \
      (ctx),                                                                   \
      sizeof((shift_component_id_t[]){__VA_ARGS__}) /                          \
          sizeof(shift_component_id_t),                                        \
      (shift_component_id_t[]){__VA_ARGS__}, (err))

/* --------------------------------------------------------------------------
 * Convenience macros for component/collection registration
 * -------------------------------------------------------------------------- */

/* Declares shift_component_id_t NAME and registers it. */
#define SHIFT_COMPONENT(ctx, name, type)                                      \
  shift_component_id_t name;                                                  \
  shift_component_register(                                                   \
      (ctx), &(shift_component_info_t){.element_size = sizeof(type)}, &(name))

/* Like SHIFT_COMPONENT but with constructor/destructor. */
#define SHIFT_COMPONENT_EX(ctx, name, type, ctor, dtor)                       \
  shift_component_id_t name;                                                  \
  shift_component_register(                                                   \
      (ctx),                                                                  \
      &(shift_component_info_t){.element_size = sizeof(type),                 \
                                .constructor  = (ctor),                       \
                                .destructor   = (dtor)},                      \
      &(name))

/* Declares shift_collection_id_t NAME and registers with listed components. */
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

/* Like SHIFT_COLLECTION but with max_capacity, on_enter, on_leave. */
#define SHIFT_COLLECTION_EX(ctx, name, cap, enter, leave, ...)                \
  shift_collection_id_t name;                                                 \
  do {                                                                        \
    shift_component_id_t _comps[] = {__VA_ARGS__};                            \
    shift_collection_register(                                                \
        (ctx),                                                                \
        &(shift_collection_info_t){                                           \
            .comp_ids      = _comps,                                          \
            .comp_count    = sizeof(_comps) / sizeof(_comps[0]),              \
            .max_capacity  = (cap),                                           \
            .on_enter      = (enter),                                         \
            .on_leave      = (leave)},                                        \
        &(name));                                                             \
  } while (0)

/* Zero-component collection (state marker / staging area). */
#define SHIFT_COLLECTION_EMPTY(ctx, name)                                     \
  shift_collection_id_t name;                                                 \
  shift_collection_register((ctx), &(shift_collection_info_t){0}, &(name))
