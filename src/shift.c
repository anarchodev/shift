#include "shift.h"
#include "shift_internal.h"

#include <assert.h>
#include <stdalign.h>
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
 * Aligned allocation helpers
 *
 * If the user provides aligned_alloc/aligned_realloc/aligned_free, use them.
 * Otherwise, over-allocate and manually align.  We stash the original pointer
 * in the bytes immediately before the aligned pointer.
 * -------------------------------------------------------------------------- */

static inline size_t resolve_alignment(size_t requested) {
  return requested ? requested : alignof(max_align_t);
}

/* Manual fallback: over-allocate, align, store original pointer. */
static void *manual_aligned_alloc(shift_t *ctx, size_t sz, size_t align) {
  /* Need space for the original pointer plus up to (align-1) padding. */
  size_t overhead = sizeof(void *) + align - 1;
  void  *raw      = salloc(ctx, sz + overhead);
  if (!raw)
    return NULL;
  uintptr_t addr    = (uintptr_t)raw + sizeof(void *);
  uintptr_t aligned = (addr + align - 1) & ~(align - 1);
  ((void **)aligned)[-1] = raw;
  return (void *)aligned;
}

static void manual_aligned_free(shift_t *ctx, void *ptr) {
  if (!ptr)
    return;
  void *raw = ((void **)ptr)[-1];
  sfree(ctx, raw);
}

static inline void *salloc_aligned(shift_t *ctx, size_t sz, size_t align) {
  if (ctx->allocator.aligned_alloc)
    return ctx->allocator.aligned_alloc(sz, align, ctx->allocator.ctx);
  return manual_aligned_alloc(ctx, sz, align);
}

static inline void sfree_aligned(shift_t *ctx, void *ptr, size_t align) {
  (void)align;
  if (ctx->allocator.aligned_free) {
    ctx->allocator.aligned_free(ptr, ctx->allocator.ctx);
    return;
  }
  manual_aligned_free(ctx, ptr);
}

/* --------------------------------------------------------------------------
 * Null-collection on_enter: bumps generation, updates metadata in the
 * entity's null-pool slot
 * -------------------------------------------------------------------------- */

/* --------------------------------------------------------------------------
 * Handler list helpers
 * -------------------------------------------------------------------------- */

static shift_result_t handler_list_add(shift_t *ctx, shift_handler_list_t *list,
                                       shift_collection_callback_t fn,
                                       void *user_ctx,
                                       shift_handler_id_t *out_id) {
  if (list->count == list->capacity) {
    uint32_t new_cap = list->capacity == 0 ? 2 : list->capacity * 2;
    shift_handler_entry_t *new_entries =
        srealloc(ctx, list->entries, sizeof(shift_handler_entry_t) * new_cap);
    if (!new_entries)
      return shift_error_oom;
    list->entries  = new_entries;
    list->capacity = new_cap;
  }
  shift_handler_id_t id = ctx->next_handler_id++;
  list->entries[list->count++] =
      (shift_handler_entry_t){.id = id, .fn = fn, .user_ctx = user_ctx};
  if (out_id)
    *out_id = id;
  return shift_ok;
}

static shift_result_t handler_list_remove(shift_t *ctx,
                                          shift_handler_list_t *list,
                                          shift_handler_id_t id) {
  (void)ctx;
  for (uint32_t i = 0; i < list->count; i++) {
    if (list->entries[i].id == id) {
      /* Shift down to preserve order. */
      memmove(&list->entries[i], &list->entries[i + 1],
              sizeof(shift_handler_entry_t) * (list->count - i - 1));
      list->count--;
      return shift_ok;
    }
  }
  return shift_error_not_found;
}

static void handler_list_fire(const shift_handler_list_t *list,
                               shift_t *ctx,
                               shift_collection_id_t col_id,
                               const shift_entity_t *entities,
                               uint32_t offset,
                               uint32_t count) {
  for (uint32_t i = 0; i < list->count; i++)
    list->entries[i].fn(ctx, col_id, entities, offset, count,
                        list->entries[i].user_ctx);
}

static void handler_list_free(shift_t *ctx, shift_handler_list_t *list) {
  if (list->entries)
    sfree(ctx, list->entries);
  list->entries  = NULL;
  list->count    = 0;
  list->capacity = 0;
}

/* --------------------------------------------------------------------------
 * Null-collection on_enter: bumps generation, updates metadata in the
 * entity's null-pool slot
 * -------------------------------------------------------------------------- */

static void null_collection_on_enter(shift_t               *ctx,
                                     shift_collection_id_t  col_id,
                                     const shift_entity_t  *entities,
                                     uint32_t               offset,
                                     uint32_t               count,
                                     void                  *user_ctx) {
  (void)col_id;
  (void)user_ctx;
  for (uint32_t i = 0; i < count; i++) {
    shift_entity_t    entity = entities[offset + i];
    shift_metadata_t *m      = &ctx->metadata[entity.index];
    m->generation++;
    ctx->collections[0].entity_ids[m->offset] =
        (shift_entity_t){entity.index, m->generation};
  }
}


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

  ctx->comp_collections =
      salloc(ctx, sizeof(shift_comp_collections_t) * ctx->max_components);
  if (!ctx->comp_collections)
    goto oom;
  memset(ctx->comp_collections, 0,
         sizeof(shift_comp_collections_t) * ctx->max_components);

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
  ctx->collections[0].max_capacity    = ctx->max_entities;
  ctx->collections[0].entity_ids =
      salloc(ctx, sizeof(shift_entity_t) * ctx->max_entities);
  if (!ctx->collections[0].entity_ids)
    goto oom;

  /* entity_ids[i] = {i, 0}; entities are reserved from the front; backfilled at
   * flush via col_remove_run. */
  for (uint32_t i = 0; i < (uint32_t)ctx->max_entities; i++) {
    ctx->collections[0].entity_ids[i] = (shift_entity_t){i, 0};
    ctx->metadata[i].col_id           = shift_null_col_id;
    ctx->metadata[i].offset           = i;
    /* generation, has_pending_move already zeroed by memset */
  }

  ctx->next_handler_id = 1;
  handler_list_add(ctx, &ctx->collections[0].on_enter_handlers,
                   null_collection_on_enter, ctx, NULL);
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
  if (ctx->comp_collections)
    sfree(ctx, ctx->comp_collections);
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

  /* Free per-collection owned arrays — columns before component_ids since
   * freeing aligned columns needs to look up alignment via component_ids. */
  for (size_t i = 0; i < ctx->collection_count; i++) {
    shift_collection_entry_t *col = &ctx->collections[i];
    if (col->columns) {
      for (uint32_t c = 0; c < col->component_count; c++) {
        if (col->columns[c]) {
          size_t align = ctx->components[col->component_ids[c]].alignment;
          if (align > alignof(max_align_t))
            sfree_aligned(ctx, col->columns[c], align);
          else
            sfree(ctx, col->columns[c]);
        }
      }
      sfree(ctx, col->columns);
    }
    if (col->component_ids)
      sfree(ctx, col->component_ids);
    if (col->entity_ids)
      sfree(ctx, col->entity_ids);
    handler_list_free(ctx, &col->on_enter_handlers);
    handler_list_free(ctx, &col->on_leave_handlers);
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

  if (ctx->comp_collections) {
    shift_comp_collections_t *cc = ctx->comp_collections;
    for (uint32_t i = 0; i < ctx->component_count; i++) {
      if (cc[i].ids)
        sfree(ctx, cc[i].ids);
    }
    sfree(ctx, ctx->comp_collections);
  }

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
  /* Resolve alignment: 0 means default. Must be a power of two. */
  if (ctx->components[id].alignment == 0)
    ctx->components[id].alignment = alignof(max_align_t);
  ctx->component_count++;

  *out_id = id;
  return shift_ok;
}

/* --------------------------------------------------------------------------
 * Collection registration and access
 * -------------------------------------------------------------------------- */

static int cmp_component_ids(const void *a, const void *b) {
  shift_component_id_t ia = *(const shift_component_id_t *)a;
  shift_component_id_t ib = *(const shift_component_id_t *)b;
  return (ia > ib) - (ia < ib);
}

static int            col_find_component_index(const shift_collection_entry_t *col,
                                               shift_component_id_t            comp_id);
static shift_result_t col_grow(shift_t *ctx, shift_collection_entry_t *col,
                               size_t needed);
static void           col_remove_run(shift_t *ctx, shift_collection_entry_t *col,
                                     uint32_t start_offset, uint32_t run_count);
static shift_migration_recipe_t *
find_or_create_recipe(shift_t *ctx, shift_collection_id_t src_col_id,
                      shift_collection_id_t dst_col_id);

shift_result_t shift_collection_register(shift_t                       *ctx,
                                         const shift_collection_info_t *info,
                                         shift_collection_id_t *out_id) {
  if (!ctx || !info || !out_id)
    return shift_error_null;
  if (info->comp_count > 0 && !info->comp_ids)
    return shift_error_null;
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
  col->max_capacity    = info->max_capacity;

  if (info->comp_count > 0) {
    col->component_ids =
        salloc(ctx, sizeof(shift_component_id_t) * info->comp_count);
    if (!col->component_ids)
      return shift_error_oom;
    memcpy(col->component_ids, info->comp_ids,
           sizeof(shift_component_id_t) * info->comp_count);
    qsort(col->component_ids, info->comp_count, sizeof(shift_component_id_t),
          cmp_component_ids);

    /* Reject duplicate component IDs (adjacent after sort). */
    for (size_t i = 1; i < info->comp_count; i++) {
      if (col->component_ids[i] == col->component_ids[i - 1]) {
        sfree(ctx, col->component_ids);
        return shift_error_invalid;
      }
    }

    col->columns = salloc(ctx, sizeof(void *) * info->comp_count);
    if (!col->columns) {
      sfree(ctx, col->component_ids);
      return shift_error_oom;
    }
    memset(col->columns, 0, sizeof(void *) * info->comp_count);
  }

  if (info->max_capacity > 0) {
    shift_result_t gr = col_grow(ctx, col, info->max_capacity);
    if (gr != shift_ok) {
      if (col->entity_ids)
        sfree(ctx, col->entity_ids);
      if (col->columns) {
        for (uint32_t i = 0; i < col->component_count; i++)
          if (col->columns[i])
            sfree(ctx, col->columns[i]);
        sfree(ctx, col->columns);
      }
      if (col->component_ids)
        sfree(ctx, col->component_ids);
      return gr;
    }
  }

  /* Update reverse index: component -> collections */
  shift_comp_collections_t *cc = ctx->comp_collections;
  for (uint32_t i = 0; i < col->component_count; i++) {
    shift_comp_collections_t *entry = &cc[col->component_ids[i]];
    if (entry->count == entry->capacity) {
      uint32_t new_cap = entry->capacity == 0 ? 4 : entry->capacity * 2;
      size_t bytes = sizeof(shift_collection_id_t) * new_cap;
      shift_collection_id_t *new_ids;
      if (entry->ids)
        new_ids = srealloc(ctx, entry->ids, bytes);
      else
        new_ids = salloc(ctx, bytes);
      if (!new_ids)
        return shift_error_oom;
      entry->ids      = new_ids;
      entry->capacity = new_cap;
    }
    entry->ids[entry->count++] = id;
  }

  ctx->collection_count++;
  *out_id = id;
  return shift_ok;
}

shift_result_t
shift_collection_get_component_array(shift_t *ctx, shift_collection_id_t col_id,
                                     shift_component_id_t comp_id,
                                     void **out_array, size_t *out_count) {
  if (!ctx || !out_array)
    return shift_error_null;

  shift_collection_entry_t *col = find_collection(ctx, col_id);
  if (!col)
    return shift_error_not_found;

  int idx = col_find_component_index(col, comp_id);
  if (idx < 0)
    return shift_error_not_found;

  *out_array = col->columns[idx];
  if (out_count)
    *out_count = col->count;
  return shift_ok;
}

shift_result_t
shift_collection_get_entities(shift_t *ctx, shift_collection_id_t col_id,
                               shift_entity_t **out_entities, size_t *out_count) {
  if (!ctx || !out_entities)
    return shift_error_null;

  shift_collection_entry_t *col = find_collection(ctx, col_id);
  if (!col)
    return shift_error_not_found;

  *out_entities = col->entity_ids;
  if (out_count)
    *out_count = col->count;
  return shift_ok;
}

/* --------------------------------------------------------------------------
 * Collection callback registration
 * -------------------------------------------------------------------------- */

shift_result_t shift_collection_on_enter(shift_t                     *ctx,
                                         shift_collection_id_t        col_id,
                                         shift_collection_callback_t  fn,
                                         void                        *user_ctx,
                                         shift_handler_id_t          *out_id) {
  if (!ctx || !fn)
    return shift_error_null;
  shift_collection_entry_t *col = find_collection(ctx, col_id);
  if (!col)
    return shift_error_not_found;
  return handler_list_add(ctx, &col->on_enter_handlers, fn, user_ctx, out_id);
}

shift_result_t shift_collection_on_leave(shift_t                     *ctx,
                                         shift_collection_id_t        col_id,
                                         shift_collection_callback_t  fn,
                                         void                        *user_ctx,
                                         shift_handler_id_t          *out_id) {
  if (!ctx || !fn)
    return shift_error_null;
  shift_collection_entry_t *col = find_collection(ctx, col_id);
  if (!col)
    return shift_error_not_found;
  return handler_list_add(ctx, &col->on_leave_handlers, fn, user_ctx, out_id);
}

shift_result_t shift_collection_remove_handler(shift_t              *ctx,
                                               shift_collection_id_t col_id,
                                               shift_handler_id_t    handler_id) {
  if (!ctx)
    return shift_error_null;
  shift_collection_entry_t *col = find_collection(ctx, col_id);
  if (!col)
    return shift_error_not_found;
  shift_result_t r = handler_list_remove(ctx, &col->on_enter_handlers,
                                         handler_id);
  if (r == shift_ok)
    return shift_ok;
  return handler_list_remove(ctx, &col->on_leave_handlers, handler_id);
}

/* --------------------------------------------------------------------------
 * New introspection / foundation APIs
 * -------------------------------------------------------------------------- */

size_t shift_collection_entity_count(const shift_t         *ctx,
                                     shift_collection_id_t  col_id) {
  if (!ctx || col_id >= ctx->collection_count)
    return 0;
  return ctx->collections[col_id].count;
}

shift_result_t shift_collection_get_components(
    const shift_t *ctx, shift_collection_id_t col_id,
    const shift_component_id_t **out_ids, uint32_t *out_count) {
  if (!ctx || !out_ids || !out_count)
    return shift_error_null;
  if (col_id >= ctx->collection_count)
    return shift_error_not_found;
  const shift_collection_entry_t *col = &ctx->collections[col_id];
  *out_ids   = col->component_ids;
  *out_count = col->component_count;
  return shift_ok;
}

shift_result_t shift_component_get_user_data(const shift_t        *ctx,
                                             shift_component_id_t  comp_id,
                                             void                **out_data) {
  if (!ctx || !out_data)
    return shift_error_null;
  if (comp_id >= ctx->component_count)
    return shift_error_not_found;
  *out_data = ctx->components[comp_id].user_data;
  return shift_ok;
}

shift_result_t shift_component_get_collections(
    const shift_t *ctx, shift_component_id_t comp_id,
    const shift_collection_id_t **out_ids, size_t *out_count) {
  if (!ctx || !out_ids || !out_count)
    return shift_error_null;
  if (comp_id >= ctx->component_count)
    return shift_error_not_found;
  const shift_comp_collections_t *cc =
      &((const shift_comp_collections_t *)ctx->comp_collections)[comp_id];
  *out_ids   = cc->ids;
  *out_count = cc->count;
  return shift_ok;
}

shift_result_t shift_collection_reserve(shift_t              *ctx,
                                        shift_collection_id_t col_id,
                                        size_t                capacity) {
  if (!ctx)
    return shift_error_null;
  shift_collection_entry_t *col = find_collection(ctx, col_id);
  if (!col)
    return shift_error_not_found;
  return col_grow(ctx, col, capacity);
}

/* --------------------------------------------------------------------------
 * Internal SoA helpers
 * -------------------------------------------------------------------------- */

static int col_find_component_index(const shift_collection_entry_t *col,
                                    shift_component_id_t            comp_id) {
  uint32_t lo = 0, hi = col->component_count;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (col->component_ids[mid] == comp_id)
      return (int)mid;
    if (col->component_ids[mid] < comp_id)
      lo = mid + 1;
    else
      hi = mid;
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
      return shift_error_full;
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
  /* Commit entity_ids first. If a subsequent column realloc fails, entity_ids
   * will be larger than col->capacity indicates, but access is always bounded
   * by col->capacity so no out-of-bounds occurs. State self-heals on the next
   * col_grow call. */
  col->entity_ids = new_eids;

  for (uint32_t i = 0; i < col->component_count; i++) {
    shift_component_info_t *ci  = &ctx->components[col->component_ids[i]];
    size_t                  elem  = ci->element_size;
    size_t                  align = ci->alignment;
    if (align > alignof(max_align_t)) {
      /* Need aligned allocation — alloc new, copy old, free old. */
      void *new_col = salloc_aligned(ctx, elem * new_cap, align);
      if (!new_col)
        return shift_error_oom;
      if (col->columns[i] && col->capacity > 0)
        memcpy(new_col, col->columns[i], elem * col->capacity);
      if (col->columns[i])
        sfree_aligned(ctx, col->columns[i], align);
      col->columns[i] = new_col;
    } else {
      void *new_col = srealloc(ctx, col->columns[i], elem * new_cap);
      if (!new_col)
        return shift_error_oom;
      col->columns[i] = new_col;
    }
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
  assert(start_offset + run_count <= N);

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


shift_result_t shift_entity_move(shift_t *ctx, const shift_entity_t *entities,
                                 uint32_t              count,
                                 shift_collection_id_t dest_col_id) {
  if (!ctx || !entities)
    return shift_error_null;
  if (count == 0)
    return shift_error_invalid;

  shift_collection_entry_t *dest = find_collection(ctx, dest_col_id);
  if (!dest)
    return shift_error_not_found;

  /* Pass 1: validate all entities before touching any state. A stale entity
   * mid-batch must not leave partial ops enqueued for earlier entities. */
  for (uint32_t i = 0; i < count; i++) {
    if (shift_entity_is_stale(ctx, entities[i]))
      return shift_error_stale;
    shift_metadata_t *mv = &ctx->metadata[entities[i].index];
    if (mv->has_pending_move || mv->constructing)
      return shift_error_stale;
  }

  /* Pass 2: enqueue ops (peek + push + set has_pending_move). */
  for (uint32_t i = 0; i < count; i++) {
    shift_metadata_t *m = &ctx->metadata[entities[i].index];

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
  if (!ctx || !out_entities)
    return shift_error_null;
  if (count == 0)
    return shift_error_invalid;

  shift_collection_entry_t *null_col = &ctx->collections[shift_null_col_id];
  if (null_col->count - ctx->null_front < (size_t)count)
    return shift_error_full;

  shift_collection_entry_t *dest = find_collection(ctx, dest_col_id);
  if (!dest)
    return shift_error_not_found;

  /* Check deferred queue capacity before mutating any state.  Peek may allow
   * extending the previous op instead of pushing a new one. */
  uint32_t null_base = (uint32_t)ctx->null_front;
  bool     can_peek  = (ctx->deferred_queue_count > 0 &&
                    ctx->deferred_queue[ctx->deferred_queue_count - 1].src_col_id ==
                        shift_null_col_id &&
                    ctx->deferred_queue[ctx->deferred_queue_count - 1].dest_col_id ==
                        dest_col_id &&
                    ctx->deferred_queue[ctx->deferred_queue_count - 1].src_offset +
                            ctx->deferred_queue[ctx->deferred_queue_count - 1].count ==
                        null_base);

  if (!can_peek && ctx->deferred_queue_count >= ctx->deferred_queue_capacity)
    return shift_error_full;

  /* Reserve entities from null pool — mark them pending so stale checks
   * prevent access before flush. */
  for (uint32_t i = 0; i < count; i++) {
    shift_entity_t    e = null_col->entity_ids[null_base + i];
    shift_metadata_t *m = &ctx->metadata[e.index];
    assert(!m->has_pending_move);
    m->has_pending_move = true;
  }

  ctx->null_front += count;

  /* Enqueue (or extend) — create is just a move from null to dest. */
  if (can_peek) {
    ctx->deferred_queue[ctx->deferred_queue_count - 1].count += count;
  } else {
    shift_deferred_op_t *op = &ctx->deferred_queue[ctx->deferred_queue_count++];
    op->src_col_id  = shift_null_col_id;
    op->dest_col_id = dest_col_id;
    op->src_offset  = null_base;
    op->count       = count;
  }

  /* Return pointer into null pool's entity_ids — valid until next flush. */
  *out_entities = &null_col->entity_ids[null_base];
  return shift_ok;
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
  if (shift_entity_is_stale(ctx, entity) || shift_entity_is_moving(ctx, entity))
    return shift_error_stale;
  if (comp_id >= ctx->component_count)
    return shift_error_not_found;

  shift_metadata_t *m = &ctx->metadata[entity.index];

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
                                    uint32_t              count) {
  return shift_entity_move(ctx, entities, count, shift_null_col_id);
}

shift_result_t shift_entity_destroy_one(shift_t *ctx, shift_entity_t entity) {
  return shift_entity_destroy(ctx, &entity, 1);
}

/* --------------------------------------------------------------------------
 * Immediate operations
 * -------------------------------------------------------------------------- */

shift_result_t shift_entity_create_immediate(shift_t              *ctx,
                                              uint32_t              count,
                                              shift_collection_id_t dest_col_id,
                                              shift_entity_t      **out_entities) {
  if (!ctx || !out_entities)
    return shift_error_null;
  if (count == 0)
    return shift_error_invalid;

  shift_collection_entry_t *null_col = &ctx->collections[shift_null_col_id];
  if (null_col->count - ctx->null_front < (size_t)count)
    return shift_error_full;

  shift_collection_entry_t *dest = find_collection(ctx, dest_col_id);
  if (!dest)
    return shift_error_not_found;

  uint32_t null_base = (uint32_t)ctx->null_front;
  uint32_t dest_base = (uint32_t)dest->count;

  /* Grow dest collection to hold count new entities. */
  shift_result_t gr = col_grow(ctx, dest, dest->count + count);
  if (gr != shift_ok)
    return gr;

  /* Zero-init new component slots. */
  for (uint32_t c = 0; c < dest->component_count; c++) {
    shift_component_info_t *ci = &ctx->components[dest->component_ids[c]];
    memset((char *)dest->columns[c] + dest_base * ci->element_size, 0,
           ci->element_size * count);
  }

  /* Transfer entities from null pool: place in dest, update metadata. */
  for (uint32_t i = 0; i < count; i++) {
    shift_entity_t    e = null_col->entity_ids[null_base + i];
    shift_metadata_t *m = &ctx->metadata[e.index];
    dest->entity_ids[dest_base + i] = e;
    m->has_pending_move = false;
    m->col_id = dest_col_id;
    m->offset = dest_base + i;
  }

  ctx->null_front += count;
  dest->count     += count;

  /* Call constructors eagerly. */
  for (uint32_t c = 0; c < dest->component_count; c++) {
    shift_component_info_t *ci = &ctx->components[dest->component_ids[c]];
    if (ci->constructor)
      ci->constructor(ctx, dest_col_id, dest->entity_ids,
                      dest->columns[c], dest_base, count, ci->user_data);
  }

  /* Fire on_enter immediately. */
  handler_list_fire(&dest->on_enter_handlers, ctx, dest_col_id,
                    dest->entity_ids, dest_base, count);

  /* Remove from null pool immediately. */
  col_remove_run(ctx, null_col, null_base, count);
  /* Reset null_front since we already cleaned up. Deferred creates that
   * preceded this call may have advanced null_front, so only subtract what
   * we consumed. */
  ctx->null_front -= count;

  *out_entities = &dest->entity_ids[dest_base];
  return shift_ok;
}

shift_result_t shift_entity_create_one_immediate(shift_t              *ctx,
                                                  shift_collection_id_t dest_col_id,
                                                  shift_entity_t       *out_entity) {
  shift_entity_t *ep;
  shift_result_t  r = shift_entity_create_immediate(ctx, 1, dest_col_id, &ep);
  if (r != shift_ok)
    return r;
  *out_entity = ep[0];
  return shift_ok;
}

shift_result_t shift_entity_move_immediate(shift_t              *ctx,
                                            const shift_entity_t *entities,
                                            uint32_t              count,
                                            shift_collection_id_t dest_col_id) {
  if (!ctx || !entities)
    return shift_error_null;
  if (count == 0)
    return shift_error_invalid;

  shift_collection_entry_t *dest = find_collection(ctx, dest_col_id);
  if (!dest)
    return shift_error_not_found;

  /* Validate all entities. */
  for (uint32_t i = 0; i < count; i++) {
    if (shift_entity_is_stale(ctx, entities[i]))
      return shift_error_stale;
    shift_metadata_t *mv = &ctx->metadata[entities[i].index];
    if (mv->has_pending_move || mv->constructing)
      return shift_error_stale;
  }

  /* Process one entity at a time (each may come from a different source). */
  for (uint32_t i = 0; i < count; i++) {
    shift_metadata_t *m = &ctx->metadata[entities[i].index];
    shift_collection_entry_t *src = find_collection(ctx, m->col_id);

    shift_migration_recipe_t *recipe =
        find_or_create_recipe(ctx, m->col_id, dest_col_id);
    if (!recipe)
      return shift_error_oom;

    /* Grow dst. */
    {
      shift_result_t gr = col_grow(ctx, dest, dest->count + 1);
      if (gr != shift_ok)
        return gr;
    }

    uint32_t dest_base = (uint32_t)dest->count;
    uint32_t src_offset = m->offset;

    /* Copy shared components. */
    for (uint32_t c = 0; c < recipe->copy_count; c++) {
      shift_recipe_copy_t    *e  = &recipe->copy[c];
      shift_component_info_t *ce = &ctx->components[e->comp_id];
      memcpy((char *)dest->columns[e->dst_col_idx] +
                 dest_base * ce->element_size,
             (char *)src->columns[e->src_col_idx] +
                 src_offset * ce->element_size,
             ce->element_size);
    }

    /* Zero-init + construct new components. */
    for (uint32_t c = 0; c < recipe->construct_count; c++) {
      shift_recipe_xtor_t    *e  = &recipe->construct[c];
      shift_component_info_t *ce = &ctx->components[e->comp_id];
      memset((char *)dest->columns[e->col_idx] + dest_base * ce->element_size,
             0, ce->element_size);
    }

    /* Destruct dropped components. */
    shift_collection_id_t src_col_id = m->col_id;
    for (uint32_t c = 0; c < recipe->destruct_count; c++) {
      shift_recipe_xtor_t    *e  = &recipe->destruct[c];
      shift_component_info_t *ce = &ctx->components[e->comp_id];
      if (ce->destructor)
        ce->destructor(ctx, src_col_id, src->entity_ids,
                       src->columns[e->col_idx], src_offset, 1,
                       ce->user_data);
    }

    /* Fire on_leave (entity still addressable in src). */
    handler_list_fire(&src->on_leave_handlers, ctx, src_col_id,
                      src->entity_ids, src_offset, 1);

    /* Place entity in dest + update metadata. */
    dest->entity_ids[dest_base] = src->entity_ids[src_offset];
    m->col_id = dest_col_id;
    m->offset = dest_base;
    dest->count++;

    /* Call constructors for new components. */
    for (uint32_t c = 0; c < recipe->construct_count; c++) {
      shift_recipe_xtor_t    *e  = &recipe->construct[c];
      shift_component_info_t *ce = &ctx->components[e->comp_id];
      if (ce->constructor)
        ce->constructor(ctx, dest_col_id, dest->entity_ids,
                        dest->columns[e->col_idx], dest_base, 1,
                        ce->user_data);
    }

    /* Fire on_enter. */
    handler_list_fire(&dest->on_enter_handlers, ctx, dest_col_id,
                      dest->entity_ids, dest_base, 1);

    /* Remove from src (swap-remove). */
    col_remove_run(ctx, src, src_offset, 1);
  }

  return shift_ok;
}

shift_result_t shift_entity_move_one_immediate(shift_t              *ctx,
                                                shift_entity_t        entity,
                                                shift_collection_id_t dest_col_id) {
  return shift_entity_move_immediate(ctx, &entity, 1, dest_col_id);
}

shift_result_t shift_entity_destroy_immediate(shift_t              *ctx,
                                               const shift_entity_t *entities,
                                               uint32_t              count) {
  return shift_entity_move_immediate(ctx, entities, count, shift_null_col_id);
}

shift_result_t shift_entity_destroy_one_immediate(shift_t        *ctx,
                                                   shift_entity_t  entity) {
  return shift_entity_destroy_immediate(ctx, &entity, 1);
}

/* --------------------------------------------------------------------------
 * Two-phase create (begin / end)
 * -------------------------------------------------------------------------- */

shift_result_t shift_entity_create_begin(shift_t              *ctx,
                                         uint32_t              count,
                                         shift_collection_id_t dest_col_id,
                                         shift_entity_t      **out_entities) {
  if (!ctx || !out_entities)
    return shift_error_null;
  if (count == 0)
    return shift_error_invalid;

  shift_collection_entry_t *null_col = &ctx->collections[shift_null_col_id];
  if (null_col->count - ctx->null_front < (size_t)count)
    return shift_error_full;

  shift_collection_entry_t *dest = find_collection(ctx, dest_col_id);
  if (!dest)
    return shift_error_not_found;

  uint32_t null_base = (uint32_t)ctx->null_front;
  uint32_t dest_base = (uint32_t)(dest->count + dest->begun_count);

  /* Grow dest to hold both existing + already-begun + new entities. */
  shift_result_t gr = col_grow(ctx, dest, dest_base + count);
  if (gr != shift_ok)
    return gr;

  /* Zero-init new component slots. */
  for (uint32_t c = 0; c < dest->component_count; c++) {
    shift_component_info_t *ci = &ctx->components[dest->component_ids[c]];
    memset((char *)dest->columns[c] + dest_base * ci->element_size, 0,
           ci->element_size * count);
  }

  /* Transfer entities from null pool: place in dest past count+begun_count,
   * update metadata, mark as constructing. */
  for (uint32_t i = 0; i < count; i++) {
    shift_entity_t    e = null_col->entity_ids[null_base + i];
    shift_metadata_t *m = &ctx->metadata[e.index];
    dest->entity_ids[dest_base + i] = e;
    m->col_id       = dest_col_id;
    m->offset        = dest_base + i;
    m->constructing  = true;
  }

  ctx->null_front   += count;
  dest->begun_count += count;

  /* Call constructors eagerly. */
  for (uint32_t c = 0; c < dest->component_count; c++) {
    shift_component_info_t *ci = &ctx->components[dest->component_ids[c]];
    if (ci->constructor)
      ci->constructor(ctx, dest_col_id, dest->entity_ids,
                      dest->columns[c], dest_base, count, ci->user_data);
  }

  /* Remove from null pool immediately. */
  col_remove_run(ctx, null_col, null_base, count);
  ctx->null_front -= count;

  *out_entities = &dest->entity_ids[dest_base];
  return shift_ok;
}

shift_result_t shift_entity_create_one_begin(shift_t              *ctx,
                                             shift_collection_id_t dest_col_id,
                                             shift_entity_t       *out_entity) {
  shift_entity_t *ep;
  shift_result_t  r = shift_entity_create_begin(ctx, 1, dest_col_id, &ep);
  if (r != shift_ok)
    return r;
  *out_entity = ep[0];
  return shift_ok;
}

shift_result_t shift_entity_create_end(shift_t              *ctx,
                                       const shift_entity_t *entities,
                                       uint32_t              count) {
  if (!ctx || !entities)
    return shift_error_null;
  if (count == 0)
    return shift_error_invalid;

  /* Validate: all entities must be in the constructing state. */
  for (uint32_t i = 0; i < count; i++) {
    if (shift_entity_is_stale(ctx, entities[i]))
      return shift_error_stale;
    if (!ctx->metadata[entities[i].index].constructing)
      return shift_error_invalid;
  }

  /* Commit: clear constructing flag, bump collection count, fire on_enter.
   * All entities passed must be a contiguous begun batch at the boundary
   * of count in the same collection. */
  shift_metadata_t         *m0  = &ctx->metadata[entities[0].index];
  shift_collection_id_t     col_id = m0->col_id;
  shift_collection_entry_t *col = find_collection(ctx, col_id);
  if (!col)
    return shift_error_not_found;

  /* The begun entities should sit right at col->count (the first uncommitted
   * slots). Verify they are contiguous there. */
  uint32_t base = (uint32_t)col->count;
  for (uint32_t i = 0; i < count; i++) {
    shift_metadata_t *m = &ctx->metadata[entities[i].index];
    if (m->col_id != col_id || m->offset != base + i)
      return shift_error_invalid;
  }

  /* Clear constructing flags. */
  for (uint32_t i = 0; i < count; i++)
    ctx->metadata[entities[i].index].constructing = false;

  col->count       += count;
  col->begun_count -= count;

  /* Fire on_enter now that entities are fully visible. */
  handler_list_fire(&col->on_enter_handlers, ctx, col_id,
                    col->entity_ids, base, count);

  return shift_ok;
}

shift_result_t shift_entity_create_one_end(shift_t        *ctx,
                                           shift_entity_t  entity) {
  return shift_entity_create_end(ctx, &entity, 1);
}

/* --------------------------------------------------------------------------
 * Generation revocation
 * -------------------------------------------------------------------------- */

shift_result_t shift_entity_revoke(shift_t        *ctx,
                                    shift_entity_t  entity,
                                    shift_entity_t *out_new) {
  if (!ctx || !out_new)
    return shift_error_null;
  if (shift_entity_is_stale(ctx, entity))
    return shift_error_stale;
  shift_metadata_t *m = &ctx->metadata[entity.index];
  if (m->has_pending_move || m->constructing)
    return shift_error_stale;

  m->generation++;

  /* Update entity_ids in the entity's current collection. */
  shift_collection_entry_t *col = find_collection(ctx, m->col_id);
  col->entity_ids[m->offset] =
      (shift_entity_t){entity.index, m->generation};

  *out_new = (shift_entity_t){entity.index, m->generation};
  return shift_ok;
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
  uint32_t                  recipe_idx; /* UINT32_MAX = no active batch */
  shift_collection_entry_t *dst;
  uint32_t                  base;
  uint32_t                  count;
} shift_batch_t;

static inline void flush_batch(shift_t *ctx, shift_batch_t *b) {
  if (b->count == 0)
    return;
  shift_migration_recipe_t *recipe = &ctx->migration_recipes[b->recipe_idx];
  shift_collection_id_t dst_col_id =
      (shift_collection_id_t)(b->dst - ctx->collections);
  for (uint32_t c = 0; c < recipe->construct_count; c++) {
    shift_recipe_xtor_t    *e  = &recipe->construct[c];
    shift_component_info_t *ce = &ctx->components[e->comp_id];
    if (ce->constructor)
      ce->constructor(ctx, dst_col_id, b->dst->entity_ids,
                      b->dst->columns[e->col_idx], b->base, b->count,
                      ce->user_data);
  }
  b->recipe_idx = UINT32_MAX;
  b->dst        = NULL;
  b->count      = 0;
}

/*
 * Cancel all unprocessed ops (indices 0..unprocessed_count-1) and reset all
 * flush-related state.  Called on error paths so that a subsequent flush
 * call sees a clean slate.
 */
static void flush_cleanup(shift_t *ctx, size_t unprocessed_count) {
  for (size_t j = 0; j < unprocessed_count; j++) {
    shift_deferred_op_t      *op  = &ctx->deferred_queue[j];
    shift_collection_entry_t *src = find_collection(ctx, op->src_col_id);
    if (!src)
      continue;
    for (uint32_t r = 0; r < op->count; r++) {
      if (op->src_offset + r >= src->count)
        continue;
      shift_entity_t e = src->entity_ids[op->src_offset + r];
      ctx->metadata[e.index].has_pending_move = false;
    }
  }
  ctx->deferred_queue_count = 0;
  ctx->needs_sort           = false;
  memset(ctx->max_src_offset, 0, sizeof(uint32_t) * ctx->max_collections);
  ctx->null_front           = 0;
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

  /* Linear merge-join over both sorted component_ids arrays: O(n+m). */
  uint32_t si = 0, di = 0;
  while (si < src->component_count && di < dst->component_count) {
    if (src->component_ids[si] == dst->component_ids[di]) {
      copy[copy_count++] =
          (shift_recipe_copy_t){si, di, src->component_ids[si]};
      si++;
      di++;
    } else if (src->component_ids[si] < dst->component_ids[di]) {
      destruct[destruct_count++] = (shift_recipe_xtor_t){si, src->component_ids[si]};
      si++;
    } else {
      construct[construct_count++] = (shift_recipe_xtor_t){di, dst->component_ids[di]};
      di++;
    }
  }
  /* Drain remainder: src-only → destruct, dst-only → construct */
  while (si < src->component_count) {
    destruct[destruct_count++] = (shift_recipe_xtor_t){si, src->component_ids[si]};
    si++;
  }
  while (di < dst->component_count) {
    construct[construct_count++] = (shift_recipe_xtor_t){di, dst->component_ids[di]};
    di++;
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

  shift_batch_t batch = {.recipe_idx = UINT32_MAX};

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
    if (!recipe) {
      flush_cleanup(ctx, i + 1);
      return shift_error_oom;
    }

    /* Compute index immediately — valid even after a realloc. */
    uint32_t recipe_idx = (uint32_t)(recipe - ctx->migration_recipes);

    /* Flush and restart batch when recipe changes. */
    if (recipe_idx != batch.recipe_idx) {
      flush_batch(ctx, &batch);
      batch.recipe_idx = recipe_idx;
      batch.dst        = dst;
      batch.base       = (uint32_t)dst->count;
    }

    /* Grow dst storage (entity_ids + component columns). */
    {
      shift_result_t gr = col_grow(ctx, dst, dst->count + op->count);
      if (gr != shift_ok) {
        flush_cleanup(ctx, i + 1);
        return gr;
      }
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
        ce->destructor(ctx, op->src_col_id, src->entity_ids,
                       src->columns[e->col_idx], op->src_offset, op->count,
                       ce->user_data);
    }

    /*
     * Fire on_leave before updating col_id/offset so the callback can still
     * reach the entity's source data via shift_entity_get_component:
     *
     * Loop A: clear has_pending_move only — is_moving becomes false so
     *         get_component no longer returns shift_error_stale, while
     *         col_id/offset still point into src for a valid lookup.
     * on_leave fires here (entity: not moving, col_id=src, offset=old).
     *
     * Loop B: write entity handles into dst, then update col_id/offset.
     * on_enter fires here; null_collection_on_enter reads m->offset set
     *          in Loop B so the null-pool slot is correctly updated.
     *
     * col_remove_run is the physical (not logical) removal from src and
     * runs last so src->entity_ids[op->src_offset] is still valid above.
     */

    /* Loop A: clear has_pending_move only */
    for (uint32_t r = 0; r < op->count; r++) {
      shift_entity_t e = src->entity_ids[op->src_offset + r];
      ctx->metadata[e.index].has_pending_move = false;
    }

    handler_list_fire(&src->on_leave_handlers, ctx, op->src_col_id,
                      src->entity_ids, op->src_offset, op->count);

    /* Loop B: copy entity handles to dst, update col_id and offset */
    shift_collection_id_t dst_col_id =
        (shift_collection_id_t)(dst - ctx->collections);
    for (uint32_t r = 0; r < op->count; r++) {
      shift_entity_t    e = src->entity_ids[op->src_offset + r];
      shift_metadata_t *m = &ctx->metadata[e.index];
      dst->entity_ids[dest_base + r] = e;
      m->col_id = dst_col_id;
      m->offset = dest_base + r;
    }

    handler_list_fire(&dst->on_enter_handlers, ctx, dst_col_id,
                      dst->entity_ids, dest_base, op->count);

    dst->count += op->count;
    batch.count += op->count;
    col_remove_run(ctx, src, op->src_offset, op->count);
  }

  /* Emit constructors for any trailing batch. */
  flush_batch(ctx, &batch);

  ctx->deferred_queue_count = 0;
  ctx->needs_sort           = false;
  memset(ctx->max_src_offset, 0, sizeof(uint32_t) * ctx->max_collections);
  ctx->null_front = 0;
  return shift_ok;
}
