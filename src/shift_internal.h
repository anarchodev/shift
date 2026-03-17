#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "shift.h"

/* --------------------------------------------------------------------------
 * Deferred operation queue
 * -------------------------------------------------------------------------- */

typedef enum {
  SHIFT_OP_CREATE,
  SHIFT_OP_DESTROY,
  SHIFT_OP_MOVE,
} shift_op_kind_t;

typedef struct {
  shift_op_kind_t       kind;
  shift_entity_t        entity;      /* first entity in the run */
  shift_collection_id_t src_col_id;
  shift_collection_id_t dest_col_id;
  uint32_t              count;       /* run length (>= 1) */
} shift_deferred_op_t;

/* --------------------------------------------------------------------------
 * Per-entity metadata
 * -------------------------------------------------------------------------- */

typedef struct {
  uint32_t              generation;
  shift_collection_id_t col_id;
  uint32_t              offset;
  bool                  is_pending_delete;
  bool                  is_pending_create;
  bool                  is_alive;
} shift_metadata_t;

/* --------------------------------------------------------------------------
 * Registered component descriptor
 * -------------------------------------------------------------------------- */

typedef struct {
  shift_component_info_t info;
  shift_component_id_t   id;
} shift_component_entry_t;

/* --------------------------------------------------------------------------
 * Collection (SoA storage)
 * -------------------------------------------------------------------------- */

typedef struct {
  shift_collection_id_t  id;
  size_t                 count;
  size_t                 capacity;
  uint32_t               component_count;
  shift_component_id_t  *component_ids;  /* owned array */
  void                 **columns;         /* one void* per component (owned) */
  shift_entity_t        *entity_ids;      /* back-pointer column (owned) */
} shift_collection_entry_t;

/* --------------------------------------------------------------------------
 * Context (full definition)
 * -------------------------------------------------------------------------- */

struct shift_s {
  shift_allocator_t         allocator;

  shift_metadata_t         *metadata;       /* [max_entities] */
  size_t                    max_entities;
  uint32_t                  entity_count;
  uint32_t                 *free_list;       /* stack of free indices */
  uint32_t                  free_list_count;

  shift_component_entry_t  *components;     /* [max_components] */
  size_t                    max_components;
  uint32_t                  component_count;

  shift_collection_entry_t *collections;    /* [max_collections] */
  size_t                    max_collections;
  size_t                    collection_count;

  shift_deferred_op_t      *deferred_queue; /* [deferred_queue_capacity] */
  size_t                    deferred_queue_capacity;
  size_t                    deferred_queue_count;
};

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

static inline shift_collection_entry_t *find_collection(shift_t *ctx,
                                                          shift_collection_id_t id)
{
  for (size_t i = 0; i < ctx->collection_count; i++) {
    if (ctx->collections[i].id == id) {
      return &ctx->collections[i];
    }
  }
  return NULL;
}

static inline bool entity_is_valid(const shift_t *ctx, shift_entity_t entity)
{
  if (entity.index >= ctx->max_entities) return false;
  const shift_metadata_t *m = &ctx->metadata[entity.index];
  return m->is_alive && !m->is_pending_delete &&
         m->generation == entity.generation;
}
