#include "shift.h"
#include "shift_internal.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Default allocator (wraps libc)
 * -------------------------------------------------------------------------- */

static void *default_alloc(size_t size, void *ctx) {
  (void)ctx;
  return malloc(size);
}

static void *default_realloc(void *ptr, size_t size, void *ctx) {
  (void)ctx;
  return realloc(ptr, size);
}

static void default_free(void *ptr, void *ctx) {
  (void)ctx;
  free(ptr);
}

static void resolve_allocator(shift_allocator_t *a) {
  if (!a->alloc)
    a->alloc = default_alloc;
  if (!a->realloc)
    a->realloc = default_realloc;
  if (!a->free)
    a->free = default_free;
}

static inline void *salloc(shift_t *ctx, size_t sz) {
  return ctx->allocator.alloc(sz, ctx->allocator.ctx);
}
static inline void sfree(shift_t *ctx, void *ptr) {
  ctx->allocator.free(ptr, ctx->allocator.ctx);
}
static inline void *srealloc(shift_t *ctx, void *ptr, size_t sz) {
  return ctx->allocator.realloc(ptr, sz, ctx->allocator.ctx);
}

/* --------------------------------------------------------------------------
 * Null-collection on_enter: bumps generation, updates metadata, LIFO push
 * -------------------------------------------------------------------------- */

static void null_collection_on_enter(shift_t              *ctx,
                                     const shift_entity_t *entities,
                                     uint32_t              count) {
  for (uint32_t i = 0; i < count; i++) {
    shift_entity_t    entity = entities[i];
    shift_metadata_t *m      = &ctx->metadata[entity.index];
    m->generation++;
    ctx->collections[0].entity_ids[m->offset] =
        (shift_entity_t){entity.index, m->generation};
  }
}

#ifndef NDEBUG
static void null_col_on_leave(shift_t *ctx, const shift_entity_t *entities,
                              uint32_t count) {
  (void)entities;
  ctx->null_front -= count;
}
#endif

/* --------------------------------------------------------------------------
 * Context lifecycle
 * -------------------------------------------------------------------------- */

shift_result_t shift_context_create(const shift_config_t *config,
                                    shift_t             **out) {
  if (!config || !out)
    return shift_error_null;

  /* Resolve a temporary allocator to allocate the context struct itself */
  shift_allocator_t alloc = config->allocator;
  resolve_allocator(&alloc);

  shift_t *ctx = alloc.alloc(sizeof(shift_t), alloc.ctx);
  if (!ctx)
    return shift_error_oom;
  memset(ctx, 0, sizeof(shift_t));

  ctx->allocator = alloc;

  ctx->max_entities   = config->max_entities;
  ctx->max_components = config->max_components;
  ctx->max_collections =
      config->max_collections + 1; /* +1 for null col at index 0 */
  ctx->deferred_queue_capacity = config->deferred_queue_capacity;

  /* Allocate sub-arrays */
  ctx->metadata = salloc(ctx, sizeof(shift_metadata_t) * ctx->max_entities);
  if (!ctx->metadata)
    goto oom;
  memset(ctx->metadata, 0, sizeof(shift_metadata_t) * ctx->max_entities);

  ctx->components =
      salloc(ctx, sizeof(shift_component_info_t) * ctx->max_components);
  if (!ctx->components)
    goto oom;
  memset(ctx->components, 0,
         sizeof(shift_component_info_t) * ctx->max_components);

  ctx->collections =
      salloc(ctx, sizeof(shift_collection_entry_t) * ctx->max_collections);
  if (!ctx->collections)
    goto oom;
  memset(ctx->collections, 0,
         sizeof(shift_collection_entry_t) * ctx->max_collections);

  /* Null collection occupies collections[0]: permanent free-slot pool */
  ctx->collections[0].component_count = 0;
  ctx->collections[0].component_ids   = NULL;
  ctx->collections[0].columns         = NULL;
  ctx->collections[0].count           = ctx->max_entities;
  ctx->collections[0].capacity        = ctx->max_entities;
  ctx->collections[0].entity_ids =
      salloc(ctx, sizeof(shift_entity_t) * ctx->max_entities);
  if (!ctx->collections[0].entity_ids)
    goto oom;

  /* entity_ids[i] = {i, 0}; entities are reserved from the front; backfilled at
   * flush via col_remove_run. */
  for (uint32_t i = 0; i < (uint32_t)ctx->max_entities; i++) {
    ctx->collections[0].entity_ids[i] = (shift_entity_t){i, 0};
    ctx->metadata[i].col_id           = shift_null_nol_id;
    ctx->metadata[i].offset           = i;
    /* generation, has_pending_move already zeroed by memset */
  }

  ctx->collections[0].on_enter = null_collection_on_enter;
#ifndef NDEBUG
  ctx->collections[0].on_leave = null_col_on_leave;
#endif
  ctx->collection_count = 1;

  ctx->deferred_queue =
      salloc(ctx, sizeof(shift_deferred_op_t) * ctx->deferred_queue_capacity);
  if (!ctx->deferred_queue)
    goto oom;

  ctx->max_src_offset = salloc(ctx, sizeof(uint32_t) * ctx->max_collections);
  if (!ctx->max_src_offset)
    goto oom;
  memset(ctx->max_src_offset, 0, sizeof(uint32_t) * ctx->max_collections);

  *out = ctx;
  return shift_ok;

oom:
  if (ctx->metadata)
    sfree(ctx, ctx->metadata);
  if (ctx->components)
    sfree(ctx, ctx->components);
  if (ctx->collections) {
    if (ctx->collections[0].entity_ids)
      sfree(ctx, ctx->collections[0].entity_ids);
    sfree(ctx, ctx->collections);
  }
  if (ctx->deferred_queue)
    sfree(ctx, ctx->deferred_queue);
  if (ctx->max_src_offset)
    sfree(ctx, ctx->max_src_offset);
  alloc.free(ctx, alloc.ctx);
  return shift_error_oom;
}

void shift_context_destroy(shift_t *ctx) {
  if (!ctx)
    return;

  /* Free per-collection owned arrays */
  for (size_t i = 0; i < ctx->collection_count; i++) {
    shift_collection_entry_t *col = &ctx->collections[i];
    if (col->component_ids)
      sfree(ctx, col->component_ids);
    if (col->entity_ids)
      sfree(ctx, col->entity_ids);
    if (col->columns) {
      for (uint32_t c = 0; c < col->component_count; c++) {
        if (col->columns[c])
          sfree(ctx, col->columns[c]);
      }
      sfree(ctx, col->columns);
    }
  }

  for (size_t i = 0; i < ctx->migration_recipe_count; i++) {
    shift_migration_recipe_t *r = &ctx->migration_recipes[i];
    if (r->copy)
      sfree(ctx, r->copy);
    if (r->construct)
      sfree(ctx, r->construct);
    if (r->destruct)
      sfree(ctx, r->destruct);
  }
  if (ctx->migration_recipes)
    sfree(ctx, ctx->migration_recipes);

  sfree(ctx, ctx->max_src_offset);
  sfree(ctx, ctx->deferred_queue);
  sfree(ctx, ctx->collections);
  sfree(ctx, ctx->components);
  sfree(ctx, ctx->metadata);

  /* Free the context struct itself using a local copy of the allocator */
  shift_allocator_t alloc = ctx->allocator;
  alloc.free(ctx, alloc.ctx);
}

/* --------------------------------------------------------------------------
 * Component registration
 * -------------------------------------------------------------------------- */

shift_result_t shift_component_register(shift_t                      *ctx,
                                        const shift_component_info_t *info,
                                        shift_component_id_t         *out_id) {
  if (!ctx || !info || !out_id)
    return shift_error_null;
  if (ctx->component_count >= ctx->max_components)
    return shift_error_full;
  if (info->element_size == 0)
    return shift_error_invalid;

  shift_component_id_t id = ctx->component_count;
  ctx->components[id]     = *info;
  ctx->component_count++;

  *out_id = id;
  return shift_ok;
}

/* --------------------------------------------------------------------------
 * Collection registration and access
 * -------------------------------------------------------------------------- */

shift_result_t shift_collection_register(shift_t                       *ctx,
                                         const shift_collection_info_t *info,
                                         shift_collection_id_t *out_id) {
  if (!ctx || !info || !info->comp_ids || !out_id)
    return shift_error_null;
  if (info->comp_count == 0)
    return shift_error_invalid;
  if (ctx->collection_count >= ctx->max_collections)
    return shift_error_full;

  /* Validate all component IDs */
  for (size_t i = 0; i < info->comp_count; i++) {
    if (info->comp_ids[i] >= ctx->component_count)
      return shift_error_not_found;
  }

  shift_collection_id_t     id  = (shift_collection_id_t)ctx->collection_count;
  shift_collection_entry_t *col = &ctx->collections[id];
  memset(col, 0, sizeof(*col));
  col->component_count = (uint32_t)info->comp_count;
  col->on_enter        = info->on_enter;
  col->on_leave        = info->on_leave;
  col->max_capacity    = info->max_capacity;

  col->component_ids =
      salloc(ctx, sizeof(shift_component_id_t) * info->comp_count);
  if (!col->component_ids)
    return shift_error_oom;
  memcpy(col->component_ids, info->comp_ids,
         sizeof(shift_component_id_t) * info->comp_count);

  col->columns = salloc(ctx, sizeof(void *) * info->comp_count);
  if (!col->columns) {
    sfree(ctx, col->component_ids);
    return shift_error_oom;
  }
  memset(col->columns, 0, sizeof(void *) * info->comp_count);

  ctx->collection_count++;
  *out_id = id;
  return shift_ok;
}

shift_result_t
shift_collection_get_component_array(shift_t *ctx, shift_collection_id_t col_id,
                                     shift_component_id_t comp_id,
                                     void **out_array, size_t *out_count) {
  if (!ctx || !out_array || !out_count)
    return shift_error_null;

  shift_collection_entry_t *col = find_collection(ctx, col_id);
  if (!col)
    return shift_error_not_found;

  for (uint32_t i = 0; i < col->component_count; i++) {
    if (col->component_ids[i] == comp_id) {
      *out_array = col->columns[i];
      *out_count = col->count;
      return shift_ok;
    }
  }

  return shift_error_not_found;
}

/* --------------------------------------------------------------------------
 * Internal SoA helpers
 * -------------------------------------------------------------------------- */

static int col_find_component_index(const shift_collection_entry_t *col,
                                    shift_component_id_t            comp_id) {
  for (uint32_t i = 0; i < col->component_count; i++) {
    if (col->component_ids[i] == comp_id)
      return (int)i;
  }
  return -1;
}

static shift_result_t col_grow(shift_t *ctx, shift_collection_entry_t *col,
                               size_t needed) {
  if (col->capacity >= needed)
    return shift_ok;

  size_t new_cap;
  if (col->max_capacity) {
    if (needed > col->max_capacity) {
      return shift_error_oom;
    }
    new_cap = col->max_capacity;
  } else {
    new_cap = col->capacity == 0 ? 8 : col->capacity;
    while (new_cap < needed)
      new_cap *= 2;
  }
  shift_entity_t *new_eids =
      srealloc(ctx, col->entity_ids, sizeof(shift_entity_t) * new_cap);
  if (!new_eids)
    return shift_error_oom;
  col->entity_ids = new_eids;

  for (uint32_t i = 0; i < col->component_count; i++) {
    size_t elem    = ctx->components[col->component_ids[i]].element_size;
    void  *new_col = srealloc(ctx, col->columns[i], elem * new_cap);
    if (!new_col)
      return shift_error_oom;
    col->columns[i] = new_col;
  }

  col->capacity = new_cap;
  return shift_ok;
}

/*
 * Remove a contiguous run of `run_count` entities starting at `start_offset`.
 * Fills the gap by copying `min(run_count, entities_after_run)` entities from
 * the tail, then decrements count. No destructors are called — caller handles
 * that before invoking this function.
 */
static void col_remove_run(shift_t *ctx, shift_collection_entry_t *col,
                           uint32_t start_offset, uint32_t run_count) {
  uint32_t N = (uint32_t)col->count;
  if (start_offset + run_count > N)
    return; /* safety */

  uint32_t after_run  = N - start_offset - run_count;
  uint32_t copy_count = run_count < after_run ? run_count : after_run;

  if (copy_count > 0) {
    uint32_t src_start = N - copy_count;
    for (uint32_t c = 0; c < col->component_count; c++) {
      size_t elem = ctx->components[col->component_ids[c]].element_size;
      memcpy((char *)col->columns[c] + start_offset * elem,
             (char *)col->columns[c] + src_start * elem, elem * copy_count);
    }
    for (uint32_t r = 0; r < copy_count; r++) {
      col->entity_ids[start_offset + r] = col->entity_ids[src_start + r];
      ctx->metadata[col->entity_ids[start_offset + r].index].offset =
          start_offset + r;
    }
  }

  col->count -= run_count;
}

/* --------------------------------------------------------------------------
 * Entity operations
 * -------------------------------------------------------------------------- */

static shift_result_t shift_entity_reserve(shift_t *ctx, uint32_t count,
                                           shift_entity_t **out_entities) {
  if (!ctx || !out_entities)
    return shift_error_null;
  if (count == 0)
    return shift_error_invalid;
  if (ctx->collections[0].count - ctx->null_front < (size_t)count)
    return shift_error_full;

  *out_entities = &ctx->collections[0].entity_ids[ctx->null_front];
  for (uint32_t i = 0; i < count; i++) {
    shift_entity_t e = ctx->collections[0].entity_ids[ctx->null_front + i];
    ctx->metadata[e.index].has_pending_move = false;
  }
  ctx->null_front += count;
  return shift_ok;
}

shift_result_t shift_entity_move(shift_t *ctx, const shift_entity_t *entities,
                                 size_t                count,
                                 shift_collection_id_t dest_col_id) {
  if (!ctx || !entities)
    return shift_error_null;
  if (count == 0)
    return shift_error_invalid;

  shift_collection_entry_t *dest = find_collection(ctx, dest_col_id);
  if (!dest)
    return shift_error_not_found;

  for (size_t i = 0; i < count; i++) {
    shift_entity_t entity = entities[i];
    if (entity_is_stale(ctx, entity))
      return shift_error_stale;
    assert(!ctx->metadata[entity.index].has_pending_move);

    shift_metadata_t *m = &ctx->metadata[entity.index];

    bool can_peek =
        (ctx->deferred_queue_count > 0 &&
         ctx->deferred_queue[ctx->deferred_queue_count - 1].src_col_id ==
             m->col_id &&
         ctx->deferred_queue[ctx->deferred_queue_count - 1].dest_col_id ==
             dest_col_id &&
         ctx->deferred_queue[ctx->deferred_queue_count - 1].src_offset +
                 ctx->deferred_queue[ctx->deferred_queue_count - 1].count ==
             m->offset);

    if (!can_peek && ctx->deferred_queue_count >= ctx->deferred_queue_capacity)
      return shift_error_full;

    if (can_peek) {
      ctx->deferred_queue[ctx->deferred_queue_count - 1].count++;
    } else {
      shift_deferred_op_t *op =
          &ctx->deferred_queue[ctx->deferred_queue_count++];
      op->src_col_id  = m->col_id;
      op->dest_col_id = dest_col_id;
      op->count       = 1;
      op->src_offset  = m->offset;

      uint32_t *slot = &ctx->max_src_offset[m->col_id];
      if (m->offset < *slot) {
        ctx->needs_sort = true;
      } else {
        *slot = m->offset;
      }
    }

    m->has_pending_move = true;
  }
  return shift_ok;
}

shift_result_t shift_entity_move_one(shift_t *ctx, shift_entity_t entity,
                                     shift_collection_id_t dest_col_id) {
  return shift_entity_move(ctx, &entity, 1, dest_col_id);
}

shift_result_t shift_entity_create(shift_t *ctx, uint32_t count,
                                   shift_collection_id_t dest_col_id,
                                   shift_entity_t      **out_entities) {
  shift_result_t r = shift_entity_reserve(ctx, count, out_entities);
  if (r != shift_ok)
    return r;
  return shift_entity_move(ctx, *out_entities, (size_t)count, dest_col_id);
}

shift_result_t shift_entity_create_one(shift_t              *ctx,
                                       shift_collection_id_t dest_col_id,
                                       shift_entity_t       *out_entity) {
  shift_entity_t *ep;
  shift_result_t  r = shift_entity_create(ctx, 1, dest_col_id, &ep);
  if (r != shift_ok)
    return r;
  *out_entity = ep[0];
  return shift_ok;
}

shift_result_t shift_entity_get_component(shift_t *ctx, shift_entity_t entity,
                                          shift_component_id_t comp_id,
                                          void               **out_data) {
  if (!ctx || !out_data)
    return shift_error_null;
  if (entity_is_stale(ctx, entity) || entity_is_moving(ctx, entity))
    return shift_error_stale;
  if (comp_id >= ctx->component_count)
    return shift_error_not_found;

  shift_metadata_t *m = &ctx->metadata[entity.index];
  if (m->col_id == shift_null_nol_id)
    return shift_error_invalid;

  shift_collection_entry_t *col = find_collection(ctx, m->col_id);
  if (!col)
    return shift_error_not_found;

  int col_idx = col_find_component_index(col, comp_id);
  if (col_idx < 0)
    return shift_error_not_found;

  size_t elem = ctx->components[comp_id].element_size;
  *out_data   = (char *)col->columns[col_idx] + m->offset * elem;
  return shift_ok;
}

shift_result_t shift_entity_destroy(shift_t              *ctx,
                                    const shift_entity_t *entities,
                                    size_t                count) {
  return shift_entity_move(ctx, entities, count, shift_null_nol_id);
}

shift_result_t shift_entity_destroy_one(shift_t *ctx, shift_entity_t entity) {
  return shift_entity_destroy(ctx, &entity, 1);
}

/* --------------------------------------------------------------------------
 * Flush — pre-pass + main pass
 * -------------------------------------------------------------------------- */

/*
 * Sort deferred ops so that within each source collection, higher src_offsets
 * are processed first. This ensures col_remove_run's swap-remove only ever
 * relocates entities that have no pending ops (they sit above all remaining
 * ops for that source), preventing later ops from referencing stale offsets.
 */
static int cmp_ops_for_flush(const void *a, const void *b) {
  const shift_deferred_op_t *oa = (const shift_deferred_op_t *)a;
  const shift_deferred_op_t *ob = (const shift_deferred_op_t *)b;
  if (oa->src_col_id != ob->src_col_id)
    return (oa->src_col_id < ob->src_col_id) ? -1 : 1;
  /* Within same source: ascending offset (queue is processed in reverse). */
  if (oa->src_offset != ob->src_offset)
    return (oa->src_offset < ob->src_offset) ? -1 : 1;
  return 0;
}

/* Batch state for deferred constructor calls. */
typedef struct {
  shift_migration_recipe_t *recipe;
  shift_collection_entry_t *dst;
  uint32_t                  base;
  uint32_t                  count;
} shift_batch_t;

static inline void flush_batch(shift_t *ctx, shift_batch_t *b) {
  if (b->count == 0)
    return;
  for (uint32_t c = 0; c < b->recipe->construct_count; c++) {
    shift_recipe_xtor_t    *e  = &b->recipe->construct[c];
    shift_component_info_t *ce = &ctx->components[e->comp_id];
    if (ce->constructor)
      ce->constructor((char *)b->dst->columns[e->col_idx] +
                          b->base * ce->element_size,
                      b->count);
  }
  b->recipe = NULL;
  b->dst    = NULL;
  b->count  = 0;
}

static shift_result_t recipe_create(shift_t *ctx, shift_collection_entry_t *src,
                                    shift_collection_entry_t *dst,
                                    shift_migration_recipe_t *out) {
  /* Temp buffers sized to worst case */
  uint32_t max_copy      = dst->component_count < src->component_count
                               ? dst->component_count
                               : src->component_count;
  uint32_t max_construct = dst->component_count;
  uint32_t max_destruct  = src->component_count;

  shift_recipe_copy_t *copy      = NULL;
  shift_recipe_xtor_t *construct = NULL;
  shift_recipe_xtor_t *destruct  = NULL;

  if (max_copy > 0) {
    copy = salloc(ctx, sizeof(shift_recipe_copy_t) * max_copy);
    if (!copy)
      goto oom;
  }
  if (max_construct > 0) {
    construct = salloc(ctx, sizeof(shift_recipe_xtor_t) * max_construct);
    if (!construct)
      goto oom;
  }
  if (max_destruct > 0) {
    destruct = salloc(ctx, sizeof(shift_recipe_xtor_t) * max_destruct);
    if (!destruct)
      goto oom;
  }

  uint32_t copy_count = 0, construct_count = 0, destruct_count = 0;

  /* Iterate dst components: found in src → copy; not found → construct */
  for (uint32_t di = 0; di < dst->component_count; di++) {
    shift_component_id_t dc = dst->component_ids[di];
    int                  si = col_find_component_index(src, dc);
    if (si >= 0) {
      copy[copy_count++] = (shift_recipe_copy_t){(uint32_t)si, di, dc};
    } else {
      construct[construct_count++] = (shift_recipe_xtor_t){di, dc};
    }
  }

  /* Iterate src components: not found in dst → destruct */
  for (uint32_t si = 0; si < src->component_count; si++) {
    shift_component_id_t sc = src->component_ids[si];
    if (col_find_component_index(dst, sc) < 0) {
      destruct[destruct_count++] = (shift_recipe_xtor_t){si, sc};
    }
  }

  out->copy            = copy;
  out->copy_count      = copy_count;
  out->construct       = construct;
  out->construct_count = construct_count;
  out->destruct        = destruct;
  out->destruct_count  = destruct_count;
  return shift_ok;

oom:
  if (copy)
    sfree(ctx, copy);
  if (construct)
    sfree(ctx, construct);
  if (destruct)
    sfree(ctx, destruct);
  return shift_error_oom;
}

static shift_migration_recipe_t *
find_or_create_recipe(shift_t *ctx, shift_collection_id_t src_col_id,
                      shift_collection_id_t dst_col_id) {
  /* Linear scan for existing entry */
  for (size_t i = 0; i < ctx->migration_recipe_count; i++) {
    shift_migration_recipe_t *r = &ctx->migration_recipes[i];
    if (r->src_col_id == src_col_id && r->dst_col_id == dst_col_id)
      return r;
  }

  /* Grow if needed */
  if (ctx->migration_recipe_count >= ctx->migration_recipe_capacity) {
    size_t                    new_cap = ctx->migration_recipe_capacity == 0
                                            ? 8
                                            : ctx->migration_recipe_capacity * 2;
    shift_migration_recipe_t *new_arr =
        srealloc(ctx, ctx->migration_recipes,
                 sizeof(shift_migration_recipe_t) * new_cap);
    if (!new_arr)
      return NULL;
    ctx->migration_recipes         = new_arr;
    ctx->migration_recipe_capacity = new_cap;
  }

  shift_migration_recipe_t *entry =
      &ctx->migration_recipes[ctx->migration_recipe_count];
  memset(entry, 0, sizeof(*entry));
  entry->src_col_id = src_col_id;
  entry->dst_col_id = dst_col_id;

  shift_collection_entry_t *src = find_collection(ctx, src_col_id);
  shift_collection_entry_t *dst = find_collection(ctx, dst_col_id);

  shift_result_t r = recipe_create(ctx, src, dst, entry);
  if (r != shift_ok)
    return NULL;

  ctx->migration_recipe_count++;
  return entry;
}

shift_result_t shift_flush(shift_t *ctx) {
  if (!ctx)
    return shift_error_null;

  if (ctx->deferred_queue_count > 1 && ctx->needs_sort)
    qsort(ctx->deferred_queue, ctx->deferred_queue_count,
          sizeof(shift_deferred_op_t), cmp_ops_for_flush);

  shift_batch_t batch = {0};

  for (size_t i = ctx->deferred_queue_count; i-- > 0;) {
    shift_deferred_op_t *op = &ctx->deferred_queue[i];

    shift_collection_entry_t *src = find_collection(ctx, op->src_col_id);
    shift_collection_entry_t *dst = find_collection(ctx, op->dest_col_id);
    if (!src || !dst)
      continue;

    /* Validate src bounds. */
    if (op->src_offset + op->count > src->count)
      continue;

    shift_migration_recipe_t *recipe =
        find_or_create_recipe(ctx, op->src_col_id, op->dest_col_id);
    if (!recipe)
      return shift_error_oom;

    /* Flush and restart batch when recipe (or dst) changes. */
    if (recipe != batch.recipe) {
      flush_batch(ctx, &batch);
      batch.recipe = recipe;
      batch.dst    = dst;
      batch.base   = (uint32_t)dst->count;
    }

    /* Grow dst storage (no-op for null dst: 0 components). */
    if (dst->component_count > 0) {
      shift_result_t gr = col_grow(ctx, dst, dst->count + op->count);
      if (gr != shift_ok)
        return gr;
    }

    uint32_t dest_base = (uint32_t)dst->count;

    /* Copy shared components */
    for (uint32_t c = 0; c < recipe->copy_count; c++) {
      shift_recipe_copy_t    *e  = &recipe->copy[c];
      shift_component_info_t *ce = &ctx->components[e->comp_id];
      memcpy((char *)dst->columns[e->dst_col_idx] +
                 dest_base * ce->element_size,
             (char *)src->columns[e->src_col_idx] +
                 op->src_offset * ce->element_size,
             ce->element_size * op->count);
    }

    /* Zero-init new components (constructors deferred to flush_batch) */
    for (uint32_t c = 0; c < recipe->construct_count; c++) {
      shift_recipe_xtor_t    *e  = &recipe->construct[c];
      shift_component_info_t *ce = &ctx->components[e->comp_id];
      memset((char *)dst->columns[e->col_idx] + dest_base * ce->element_size, 0,
             ce->element_size * op->count);
    }

    /* Destruct dropped components */
    for (uint32_t c = 0; c < recipe->destruct_count; c++) {
      shift_recipe_xtor_t    *e  = &recipe->destruct[c];
      shift_component_info_t *ce = &ctx->components[e->comp_id];
      if (ce->destructor)
        ce->destructor((char *)src->columns[e->col_idx] +
                           op->src_offset * ce->element_size,
                       op->count);
    }

    /* Entity placement — uniform for all destinations. */
    for (uint32_t r = 0; r < op->count; r++) {
      shift_entity_t    e            = src->entity_ids[op->src_offset + r];
      shift_metadata_t *m            = &ctx->metadata[e.index];
      dst->entity_ids[dest_base + r] = e;
      m->col_id           = (shift_collection_id_t)(dst - ctx->collections);
      m->offset           = dest_base + r;
      m->has_pending_move = false;
    }
    if (dst->on_enter)
      dst->on_enter(ctx, &dst->entity_ids[dest_base], op->count);

    /* Count update — uniform for all destinations. */
    dst->count += op->count;

    /* Accumulate batch size. */
    batch.count += op->count;

    /* Remove from src. */
    if (src->on_leave)
      src->on_leave(ctx, &src->entity_ids[op->src_offset], op->count);
    col_remove_run(ctx, src, op->src_offset, op->count);
  }

  /* Emit constructors for any trailing batch. */
  flush_batch(ctx, &batch);

  ctx->deferred_queue_count = 0;
  ctx->needs_sort           = false;
  memset(ctx->max_src_offset, 0, sizeof(uint32_t) * ctx->max_collections);
  assert(ctx->null_front ==
         0); /* all reserved entities must be moved before flush */
  ctx->null_front = 0;
  return shift_ok;
}
