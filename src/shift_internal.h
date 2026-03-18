#pragma once

#include "shift.h"
#include <stdbool.h>
#include <stdint.h>

/* Null collection is always at index 0. */
constexpr shift_collection_id_t shift_null_nol_id = 0;

/* --------------------------------------------------------------------------
 * Migration recipe cache
 * -------------------------------------------------------------------------- */

typedef struct {
  uint32_t             src_col_idx;
  uint32_t             dst_col_idx;
  shift_component_id_t comp_id;
} shift_recipe_copy_t;

typedef struct {
  uint32_t             col_idx;
  shift_component_id_t comp_id;
} shift_recipe_xtor_t;

typedef struct {
  shift_collection_id_t src_col_id;
  shift_collection_id_t dst_col_id;
  shift_recipe_copy_t  *copy;
  uint32_t              copy_count;
  shift_recipe_xtor_t  *construct;
  uint32_t              construct_count;
  shift_recipe_xtor_t  *destruct;
  uint32_t              destruct_count;
} shift_migration_recipe_t;

/* --------------------------------------------------------------------------
 * Deferred operation queue
 * -------------------------------------------------------------------------- */

typedef struct {
  shift_collection_id_t src_col_id;
  shift_collection_id_t dest_col_id;
  uint32_t              count; /* run length (>= 1) */
  uint32_t              src_offset;
} shift_deferred_op_t;

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
 * Collection (SoA storage)
 * -------------------------------------------------------------------------- */

typedef struct {
  size_t                count;
  size_t                capacity;
  uint32_t              component_count;
  shift_component_id_t *component_ids; /* owned array */
  void                **columns;       /* one void* per component (owned) */
  shift_entity_t       *entity_ids;    /* back-pointer column (owned) */
  void (*on_enter)(shift_t *ctx, const shift_entity_t *entities,
                   uint32_t count);
  void (*on_leave)(shift_t *ctx, const shift_entity_t *entities,
                   uint32_t count);
} shift_collection_entry_t;

/* --------------------------------------------------------------------------
 * Context (full definition)
 * -------------------------------------------------------------------------- */

struct shift_s {
  shift_allocator_t allocator;

  shift_metadata_t *metadata; /* [max_entities] */
  size_t            max_entities;
  // uint32_t          entity_count;

  size_t null_front; /* index of next entity to reserve; 0 after each flush */

  shift_component_info_t *components; /* [max_components] */
  size_t                  max_components;
  uint32_t                component_count;

  shift_collection_entry_t *collections; /* [max_collections] */
  size_t                    max_collections;
  size_t                    collection_count;

  shift_deferred_op_t *deferred_queue; /* [deferred_queue_capacity] */
  size_t               deferred_queue_capacity;
  size_t               deferred_queue_count;

  uint32_t *max_src_offset; /* [max_collections] largest src_offset enqueued per
                               col since last flush */
  bool needs_sort; /* true if queue may be out of ascending src_offset order */

  shift_migration_recipe_t *migration_recipes;
  size_t                    migration_recipe_count;
  size_t                    migration_recipe_capacity;
};

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

static inline shift_collection_entry_t *
find_collection(shift_t *ctx, shift_collection_id_t id) {
  if (id >= ctx->collection_count)
    return NULL;
  return &ctx->collections[id];
}

// entity_is_stale: true if the handle's generation no longer matches — the
// entity has been destroyed and recycled (or was never valid).
static inline bool entity_is_stale(const shift_t *ctx, shift_entity_t entity) {
  if (entity.index >= ctx->max_entities)
    return true;
  return ctx->metadata[entity.index].generation != entity.generation;
}

// entity_is_moving: true if a deferred move is queued for this entity.
// The entity is alive but its destination is not yet committed.
static inline bool entity_is_moving(const shift_t *ctx, shift_entity_t entity) {
  if (entity.index >= ctx->max_entities)
    return false;
  return ctx->metadata[entity.index].has_pending_move;
}
