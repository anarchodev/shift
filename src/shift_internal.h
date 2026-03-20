#pragma once

#include "shift.h"
#include <stdbool.h>
#include <stdint.h>

/* Null collection is always at index 0. */
constexpr shift_collection_id_t shift_null_col_id = 0;

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

typedef struct shift_migration_recipe_s {
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

typedef struct shift_deferred_op_s {
  shift_collection_id_t src_col_id;
  shift_collection_id_t dest_col_id;
  uint32_t              count;      /* run length (>= 1) */
  uint32_t              src_offset; /* offset in source collection */
} shift_deferred_op_t;

/* --------------------------------------------------------------------------
 * Collection (SoA storage)
 * -------------------------------------------------------------------------- */

typedef struct shift_collection_entry_s {
  size_t                count;
  size_t                capacity;
  size_t                max_capacity;
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
 * Internal helpers
 * -------------------------------------------------------------------------- */

static inline shift_collection_entry_t *
find_collection(shift_t *ctx, shift_collection_id_t id) {
  if (id >= ctx->collection_count)
    return NULL;
  return &ctx->collections[id];
}

