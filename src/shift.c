#include "shift_internal.h"

#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Default allocator (wraps libc)
 * -------------------------------------------------------------------------- */

static void *default_alloc(size_t size, void *ctx)
{
  (void)ctx;
  return malloc(size);
}

static void *default_realloc(void *ptr, size_t size, void *ctx)
{
  (void)ctx;
  return realloc(ptr, size);
}

static void default_free(void *ptr, void *ctx)
{
  (void)ctx;
  free(ptr);
}

static void resolve_allocator(shift_allocator_t *a)
{
  if (!a->alloc)   a->alloc   = default_alloc;
  if (!a->realloc) a->realloc = default_realloc;
  if (!a->free)    a->free    = default_free;
}

#define SALLOC(ctx, sz)        ((ctx)->allocator.alloc((sz), (ctx)->allocator.ctx))
#define SFREE(ctx, ptr)        ((ctx)->allocator.free((ptr), (ctx)->allocator.ctx))
#define SREALLOC(ctx, ptr, sz) ((ctx)->allocator.realloc((ptr), (sz), (ctx)->allocator.ctx))

/* --------------------------------------------------------------------------
 * Context lifecycle
 * -------------------------------------------------------------------------- */

shift_result_t shift_context_create(const shift_config_t *config, shift_t **out)
{
  if (!config || !out) return SHIFT_ERROR_NULL;

  /* Resolve a temporary allocator to allocate the context struct itself */
  shift_allocator_t alloc = config->allocator;
  resolve_allocator(&alloc);

  shift_t *ctx = alloc.alloc(sizeof(shift_t), alloc.ctx);
  if (!ctx) return SHIFT_ERROR_OOM;
  memset(ctx, 0, sizeof(shift_t));

  ctx->allocator = alloc;

  ctx->max_entities              = config->max_entities;
  ctx->max_components            = config->max_components;
  ctx->max_collections           = config->max_collections;
  ctx->deferred_queue_capacity   = config->deferred_queue_capacity;

  /* Allocate sub-arrays */
  ctx->metadata = SALLOC(ctx, sizeof(shift_metadata_t) * ctx->max_entities);
  if (!ctx->metadata) goto oom;
  memset(ctx->metadata, 0, sizeof(shift_metadata_t) * ctx->max_entities);

  ctx->free_list = SALLOC(ctx, sizeof(uint32_t) * ctx->max_entities);
  if (!ctx->free_list) goto oom;

  /* Pre-populate the free list with all indices (highest index at top) */
  for (uint32_t i = 0; i < (uint32_t)ctx->max_entities; i++) {
    ctx->free_list[i] = (uint32_t)ctx->max_entities - 1 - i;
  }
  ctx->free_list_count = (uint32_t)ctx->max_entities;

  ctx->components = SALLOC(ctx, sizeof(shift_component_entry_t) * ctx->max_components);
  if (!ctx->components) goto oom;
  memset(ctx->components, 0, sizeof(shift_component_entry_t) * ctx->max_components);

  ctx->collections = SALLOC(ctx, sizeof(shift_collection_entry_t) * ctx->max_collections);
  if (!ctx->collections) goto oom;
  memset(ctx->collections, 0, sizeof(shift_collection_entry_t) * ctx->max_collections);

  ctx->deferred_queue = SALLOC(ctx, sizeof(shift_deferred_op_t) * ctx->deferred_queue_capacity);
  if (!ctx->deferred_queue) goto oom;

  *out = ctx;
  return SHIFT_OK;

oom:
  /* Best-effort cleanup — some pointers may be NULL (free handles that) */
  if (ctx->metadata)        SFREE(ctx, ctx->metadata);
  if (ctx->free_list)       SFREE(ctx, ctx->free_list);
  if (ctx->components)      SFREE(ctx, ctx->components);
  if (ctx->collections)     SFREE(ctx, ctx->collections);
  if (ctx->deferred_queue)  SFREE(ctx, ctx->deferred_queue);
  alloc.free(ctx, alloc.ctx);
  return SHIFT_ERROR_OOM;
}

void shift_context_destroy(shift_t *ctx)
{
  if (!ctx) return;

  /* Free per-collection owned arrays */
  for (size_t i = 0; i < ctx->collection_count; i++) {
    shift_collection_entry_t *col = &ctx->collections[i];
    if (col->component_ids) SFREE(ctx, col->component_ids);
    if (col->entity_ids)    SFREE(ctx, col->entity_ids);
    if (col->columns) {
      for (uint32_t c = 0; c < col->component_count; c++) {
        if (col->columns[c]) SFREE(ctx, col->columns[c]);
      }
      SFREE(ctx, col->columns);
    }
  }

  SFREE(ctx, ctx->deferred_queue);
  SFREE(ctx, ctx->collections);
  SFREE(ctx, ctx->components);
  SFREE(ctx, ctx->free_list);
  SFREE(ctx, ctx->metadata);

  /* Free the context struct itself using a local copy of the allocator */
  shift_allocator_t alloc = ctx->allocator;
  alloc.free(ctx, alloc.ctx);
}

/* --------------------------------------------------------------------------
 * Component registration
 * -------------------------------------------------------------------------- */

shift_result_t shift_component_register(shift_t *ctx,
                                         const shift_component_info_t *info,
                                         shift_component_id_t *out_id)
{
  if (!ctx || !info || !out_id) return SHIFT_ERROR_NULL;
  if (ctx->component_count >= ctx->max_components) return SHIFT_ERROR_FULL;
  if (info->element_size == 0) return SHIFT_ERROR_INVALID;

  shift_component_id_t id = ctx->component_count;
  ctx->components[id].info = *info;
  ctx->components[id].id   = id;
  ctx->component_count++;

  *out_id = id;
  return SHIFT_OK;
}

/* --------------------------------------------------------------------------
 * Collection registration and access
 * -------------------------------------------------------------------------- */

shift_result_t shift_collection_register(shift_t *ctx,
                                          shift_collection_id_t col_id,
                                          const shift_component_id_t *comp_ids,
                                          size_t comp_count)
{
  if (!ctx || !comp_ids) return SHIFT_ERROR_NULL;
  if (comp_count == 0)   return SHIFT_ERROR_INVALID;
  if (ctx->collection_count >= ctx->max_collections) return SHIFT_ERROR_FULL;
  if (find_collection(ctx, col_id)) return SHIFT_ERROR_INVALID;

  /* Validate all component IDs */
  for (size_t i = 0; i < comp_count; i++) {
    if (comp_ids[i] >= ctx->component_count) return SHIFT_ERROR_NOT_FOUND;
  }

  shift_collection_entry_t *col = &ctx->collections[ctx->collection_count];
  memset(col, 0, sizeof(*col));
  col->id              = col_id;
  col->component_count = (uint32_t)comp_count;

  col->component_ids = SALLOC(ctx, sizeof(shift_component_id_t) * comp_count);
  if (!col->component_ids) return SHIFT_ERROR_OOM;
  memcpy(col->component_ids, comp_ids, sizeof(shift_component_id_t) * comp_count);

  col->columns = SALLOC(ctx, sizeof(void *) * comp_count);
  if (!col->columns) {
    SFREE(ctx, col->component_ids);
    return SHIFT_ERROR_OOM;
  }
  memset(col->columns, 0, sizeof(void *) * comp_count);

  /* columns are lazily allocated on first entity creation */

  ctx->collection_count++;
  return SHIFT_OK;
}

shift_result_t shift_collection_get_component_array(shift_t *ctx,
                                                     shift_collection_id_t col_id,
                                                     shift_component_id_t comp_id,
                                                     void **out_array,
                                                     size_t *out_count)
{
  if (!ctx || !out_array || !out_count) return SHIFT_ERROR_NULL;

  shift_collection_entry_t *col = find_collection(ctx, col_id);
  if (!col) return SHIFT_ERROR_NOT_FOUND;

  for (uint32_t i = 0; i < col->component_count; i++) {
    if (col->component_ids[i] == comp_id) {
      *out_array = col->columns[i];
      *out_count = col->count;
      return SHIFT_OK;
    }
  }

  return SHIFT_ERROR_NOT_FOUND;
}

/* --------------------------------------------------------------------------
 * Internal SoA helpers
 * -------------------------------------------------------------------------- */

static int col_find_component_index(const shift_collection_entry_t *col,
                                     shift_component_id_t comp_id)
{
  for (uint32_t i = 0; i < col->component_count; i++) {
    if (col->component_ids[i] == comp_id) return (int)i;
  }
  return -1;
}

static shift_result_t col_grow(shift_t *ctx, shift_collection_entry_t *col,
                                size_t needed)
{
  if (col->capacity >= needed) return SHIFT_OK;

  size_t new_cap = col->capacity == 0 ? 8 : col->capacity;
  while (new_cap < needed) new_cap *= 2;

  shift_entity_t *new_eids = SREALLOC(ctx, col->entity_ids,
                                       sizeof(shift_entity_t) * new_cap);
  if (!new_eids) return SHIFT_ERROR_OOM;
  col->entity_ids = new_eids;

  for (uint32_t i = 0; i < col->component_count; i++) {
    size_t elem = ctx->components[col->component_ids[i]].info.element_size;
    void *new_col = SREALLOC(ctx, col->columns[i], elem * new_cap);
    if (!new_col) return SHIFT_ERROR_OOM;
    col->columns[i] = new_col;
  }

  col->capacity = new_cap;
  return SHIFT_OK;
}

/*
 * Remove a contiguous run of `run_count` entities starting at `start_offset`.
 * Fills the gap by copying `min(run_count, entities_after_run)` entities from
 * the tail, then decrements count. No destructors are called — caller handles
 * that before invoking this function.
 */
static void col_remove_run(shift_t *ctx, shift_collection_entry_t *col,
                            uint32_t start_offset, uint32_t run_count)
{
  uint32_t N = (uint32_t)col->count;
  if (start_offset + run_count > N) return; /* safety */

  uint32_t after_run  = N - start_offset - run_count;
  uint32_t copy_count = run_count < after_run ? run_count : after_run;

  if (copy_count > 0) {
    uint32_t src_start = N - copy_count;
    for (uint32_t c = 0; c < col->component_count; c++) {
      size_t elem = ctx->components[col->component_ids[c]].info.element_size;
      memcpy((char *)col->columns[c] + start_offset * elem,
             (char *)col->columns[c] + src_start   * elem,
             elem * copy_count);
    }
    for (uint32_t r = 0; r < copy_count; r++) {
      col->entity_ids[start_offset + r] = col->entity_ids[src_start + r];
      ctx->metadata[col->entity_ids[start_offset + r].index].offset = start_offset + r;
    }
  }

  col->count -= run_count;
}

/* --------------------------------------------------------------------------
 * Entity operations
 * -------------------------------------------------------------------------- */

shift_result_t shift_entity_create(shift_t *ctx,
                                    shift_collection_id_t col_id,
                                    shift_entity_t *out_entity)
{
  if (!ctx || !out_entity) return SHIFT_ERROR_NULL;

  shift_collection_entry_t *col = find_collection(ctx, col_id);
  if (!col) return SHIFT_ERROR_NOT_FOUND;

  if (ctx->free_list_count == 0) return SHIFT_ERROR_FULL;
  if (ctx->deferred_queue_count >= ctx->deferred_queue_capacity) return SHIFT_ERROR_FULL;

  uint32_t index = ctx->free_list[--ctx->free_list_count];
  shift_metadata_t *m = &ctx->metadata[index];
  m->col_id            = col_id;
  m->is_alive          = true;
  m->is_pending_create = true;
  m->is_pending_delete = false;
  m->offset            = 0;

  shift_entity_t entity = { .index = index, .generation = m->generation };

  shift_deferred_op_t *op = &ctx->deferred_queue[ctx->deferred_queue_count++];
  op->kind        = SHIFT_OP_CREATE;
  op->entity      = entity;
  op->src_col_id  = col_id;
  op->dest_col_id = col_id;
  op->count       = 1;

  *out_entity = entity;
  return SHIFT_OK;
}

shift_result_t shift_entity_destroy(shift_t *ctx, shift_entity_t entity)
{
  if (!ctx) return SHIFT_ERROR_NULL;
  if (!entity_is_valid(ctx, entity)) return SHIFT_ERROR_STALE;

  shift_metadata_t *m = &ctx->metadata[entity.index];

  /* Peek optimisation: extend an existing contiguous DESTROY run rather than
   * pushing a new entry. Only applies to entities already in a collection
   * (not pending-create) so their offsets are meaningful. */
  if (!m->is_pending_create && ctx->deferred_queue_count > 0) {
    shift_deferred_op_t *last = &ctx->deferred_queue[ctx->deferred_queue_count - 1];
    if (last->kind == SHIFT_OP_DESTROY && last->src_col_id == m->col_id) {
      shift_metadata_t *first_m = &ctx->metadata[last->entity.index];
      if (first_m->offset + last->count == m->offset) {
        last->count++;
        m->is_pending_delete = true;
        return SHIFT_OK;
      }
    }
  }

  if (ctx->deferred_queue_count >= ctx->deferred_queue_capacity) return SHIFT_ERROR_FULL;

  m->is_pending_delete = true;

  shift_deferred_op_t *op = &ctx->deferred_queue[ctx->deferred_queue_count++];
  op->kind        = SHIFT_OP_DESTROY;
  op->entity      = entity;
  op->src_col_id  = m->col_id;
  op->dest_col_id = m->col_id;
  op->count       = 1;
  return SHIFT_OK;
}

shift_result_t shift_entity_move(shift_t *ctx,
                                  shift_entity_t entity,
                                  shift_collection_id_t dest_col_id)
{
  if (!ctx) return SHIFT_ERROR_NULL;
  if (!entity_is_valid(ctx, entity)) return SHIFT_ERROR_STALE;

  shift_collection_entry_t *dest = find_collection(ctx, dest_col_id);
  if (!dest) return SHIFT_ERROR_NOT_FOUND;

  if (ctx->deferred_queue_count >= ctx->deferred_queue_capacity) return SHIFT_ERROR_FULL;

  shift_deferred_op_t *op = &ctx->deferred_queue[ctx->deferred_queue_count++];
  op->kind        = SHIFT_OP_MOVE;
  op->entity      = entity;
  op->src_col_id  = ctx->metadata[entity.index].col_id;
  op->dest_col_id = dest_col_id;
  op->count       = 1;
  (void)dest;
  return SHIFT_OK;
}

shift_result_t shift_entity_get_component(shift_t *ctx,
                                           shift_entity_t entity,
                                           shift_component_id_t comp_id,
                                           void **out_data)
{
  if (!ctx || !out_data) return SHIFT_ERROR_NULL;
  if (!entity_is_valid(ctx, entity)) return SHIFT_ERROR_STALE;
  if (comp_id >= ctx->component_count) return SHIFT_ERROR_NOT_FOUND;

  shift_metadata_t *m = &ctx->metadata[entity.index];
  if (m->is_pending_create) return SHIFT_ERROR_INVALID;

  shift_collection_entry_t *col = find_collection(ctx, m->col_id);
  if (!col) return SHIFT_ERROR_NOT_FOUND;

  int col_idx = col_find_component_index(col, comp_id);
  if (col_idx < 0) return SHIFT_ERROR_NOT_FOUND;

  size_t elem = ctx->components[comp_id].info.element_size;
  *out_data = (char *)col->columns[col_idx] + m->offset * elem;
  return SHIFT_OK;
}

/* --------------------------------------------------------------------------
 * Flush
 * -------------------------------------------------------------------------- */

shift_result_t shift_flush(shift_t *ctx)
{
  if (!ctx) return SHIFT_ERROR_NULL;

  /* Pass 1: DESTROY */
  for (size_t i = 0; i < ctx->deferred_queue_count; i++) {
    shift_deferred_op_t *op = &ctx->deferred_queue[i];
    if (op->kind != SHIFT_OP_DESTROY) continue;

    shift_metadata_t *m0 = &ctx->metadata[op->entity.index];
    if (!m0->is_alive || m0->generation != op->entity.generation) continue;

    if (!m0->is_pending_create) {
      shift_collection_entry_t *col = find_collection(ctx, m0->col_id);
      if (col) {
        uint32_t start_offset = m0->offset;

        /* Reclaim all slots in the run (read entity_ids before removal). */
        for (uint32_t r = 0; r < op->count; r++) {
          shift_entity_t re = col->entity_ids[start_offset + r];
          shift_metadata_t *rm = &ctx->metadata[re.index];
          if (rm->is_alive && rm->generation == re.generation) {
            rm->is_alive          = false;
            rm->is_pending_create = false;
            rm->is_pending_delete = false;
            rm->generation++;
            ctx->free_list[ctx->free_list_count++] = re.index;
          }
        }

        /* Call destructors on the entire run in one batch call per component. */
        for (uint32_t c = 0; c < col->component_count; c++) {
          shift_component_entry_t *ce = &ctx->components[col->component_ids[c]];
          if (ce->info.destructor) {
            void *start_ptr = (char *)col->columns[c] + start_offset * ce->info.element_size;
            ce->info.destructor(start_ptr, op->count);
          }
        }

        col_remove_run(ctx, col, start_offset, op->count);
      }
    } else {
      /* Pending-create entity: never entered a collection, just reclaim. */
      m0->is_alive          = false;
      m0->is_pending_create = false;
      m0->is_pending_delete = false;
      m0->generation++;
      ctx->free_list[ctx->free_list_count++] = op->entity.index;
    }
  }

  /* Pass 2: MOVE */
  for (size_t i = 0; i < ctx->deferred_queue_count; i++) {
    shift_deferred_op_t *op = &ctx->deferred_queue[i];
    if (op->kind != SHIFT_OP_MOVE) continue;

    shift_metadata_t *m = &ctx->metadata[op->entity.index];
    if (!m->is_alive || m->generation != op->entity.generation) continue;

    shift_collection_id_t dest_col_id = op->dest_col_id;

    if (m->is_pending_create) {
      m->col_id = dest_col_id;
      continue;
    }

    shift_collection_entry_t *src = find_collection(ctx, m->col_id);
    shift_collection_entry_t *dst = find_collection(ctx, dest_col_id);
    if (!src || !dst || src->id == dst->id) continue;

    shift_result_t gr = col_grow(ctx, dst, dst->count + 1);
    if (gr != SHIFT_OK) return gr;

    uint32_t dest_slot = (uint32_t)dst->count;
    uint32_t old_offset = m->offset;

    for (uint32_t di = 0; di < dst->component_count; di++) {
      shift_component_id_t dc = dst->component_ids[di];
      shift_component_entry_t *ce = &ctx->components[dc];
      void *dest_data = (char *)dst->columns[di] + dest_slot * ce->info.element_size;
      int si = col_find_component_index(src, dc);
      if (si >= 0) {
        memcpy(dest_data,
               (char *)src->columns[si] + old_offset * ce->info.element_size,
               ce->info.element_size);
      } else {
        memset(dest_data, 0, ce->info.element_size);
        if (ce->info.constructor) ce->info.constructor(dest_data, 1);
      }
    }

    for (uint32_t si = 0; si < src->component_count; si++) {
      shift_component_id_t sc = src->component_ids[si];
      if (col_find_component_index(dst, sc) < 0) {
        shift_component_entry_t *ce = &ctx->components[sc];
        if (ce->info.destructor) {
          void *data = (char *)src->columns[si] + old_offset * ce->info.element_size;
          ce->info.destructor(data, 1);
        }
      }
    }

    dst->entity_ids[dest_slot] = op->entity;
    dst->count++;

    m->col_id  = dest_col_id;
    m->offset  = dest_slot;

    col_remove_run(ctx, src, old_offset, 1);
  }

  /* Pass 3: CREATE — scan-ahead to batch consecutive creates for the same
   * collection into a single constructor call per component. */
  for (size_t i = 0; i < ctx->deferred_queue_count; ) {
    shift_deferred_op_t *op = &ctx->deferred_queue[i];
    if (op->kind != SHIFT_OP_CREATE) { i++; continue; }

    shift_metadata_t *m0 = &ctx->metadata[op->entity.index];
    if (!m0->is_alive || m0->generation != op->entity.generation) { i++; continue; }

    shift_collection_id_t target_col_id = m0->col_id;
    shift_collection_entry_t *col = find_collection(ctx, target_col_id);
    if (!col) { i++; continue; }

    /* Count consecutive valid CREATEs targeting the same collection. */
    size_t   j         = i + 1;
    uint32_t run_count = 1;
    while (j < ctx->deferred_queue_count) {
      shift_deferred_op_t *next = &ctx->deferred_queue[j];
      if (next->kind != SHIFT_OP_CREATE) break;
      shift_metadata_t *mj = &ctx->metadata[next->entity.index];
      if (!mj->is_alive || mj->generation != next->entity.generation) break;
      if (mj->col_id != target_col_id) break;
      run_count++;
      j++;
    }

    shift_result_t gr = col_grow(ctx, col, col->count + run_count);
    if (gr != SHIFT_OK) return gr;

    uint32_t base_slot = (uint32_t)col->count;

    /* Zero-init and construct the entire run in one call per component. */
    for (uint32_t c = 0; c < col->component_count; c++) {
      shift_component_entry_t *ce = &ctx->components[col->component_ids[c]];
      void *start_ptr = (char *)col->columns[c] + base_slot * ce->info.element_size;
      memset(start_ptr, 0, ce->info.element_size * run_count);
      if (ce->info.constructor) ce->info.constructor(start_ptr, run_count);
    }

    /* Write entity_ids and fix up metadata for each entity in the run. */
    for (uint32_t r = 0; r < run_count; r++) {
      shift_deferred_op_t *op_r = &ctx->deferred_queue[i + r];
      shift_metadata_t    *mr   = &ctx->metadata[op_r->entity.index];
      uint32_t slot = base_slot + r;
      col->entity_ids[slot] = op_r->entity;
      mr->offset            = slot;
      mr->is_pending_create = false;
    }
    col->count += run_count;

    i = j;
  }

  ctx->deferred_queue_count = 0;
  return SHIFT_OK;
}
