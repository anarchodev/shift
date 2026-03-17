#pragma once

#include <stddef.h>
#include <stdint.h>

/* --------------------------------------------------------------------------
 * Result codes
 * -------------------------------------------------------------------------- */

typedef int shift_result_t;

#define SHIFT_OK               0
#define SHIFT_ERROR_NULL      -1
#define SHIFT_ERROR_OOM       -2
#define SHIFT_ERROR_STALE     -3
#define SHIFT_ERROR_FULL      -4
#define SHIFT_ERROR_NOT_FOUND -5
#define SHIFT_ERROR_INVALID   -6

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
  void  (*free)(void *ptr, void *ctx);
  void  *ctx;
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
  size_t             max_entities;
  size_t             max_components;
  size_t             max_collections;
  size_t             deferred_queue_capacity;
  shift_allocator_t  allocator;
} shift_config_t;

/* --------------------------------------------------------------------------
 * Opaque context type
 * -------------------------------------------------------------------------- */

typedef struct shift_s shift_t;

/* --------------------------------------------------------------------------
 * Context lifecycle  (subject == lib → shift_ prefix)
 * -------------------------------------------------------------------------- */

shift_result_t shift_context_create(const shift_config_t *config, shift_t **out);
void           shift_context_destroy(shift_t *ctx);

/* --------------------------------------------------------------------------
 * Component registration
 * -------------------------------------------------------------------------- */

shift_result_t shift_component_register(shift_t *ctx,
                                         const shift_component_info_t *info,
                                         shift_component_id_t *out_id);

/* --------------------------------------------------------------------------
 * Collection registration and access
 * -------------------------------------------------------------------------- */

shift_result_t shift_collection_register(shift_t *ctx,
                                          shift_collection_id_t col_id,
                                          const shift_component_id_t *comp_ids,
                                          size_t comp_count);

shift_result_t shift_collection_get_component_array(shift_t *ctx,
                                                     shift_collection_id_t col_id,
                                                     shift_component_id_t comp_id,
                                                     void **out_array,
                                                     size_t *out_count);

/* --------------------------------------------------------------------------
 * Entity operations
 * -------------------------------------------------------------------------- */

shift_result_t shift_entity_create(shift_t *ctx,
                                    shift_collection_id_t col_id,
                                    shift_entity_t *out_entity);

shift_result_t shift_entity_destroy(shift_t *ctx, shift_entity_t entity);

shift_result_t shift_entity_move(shift_t *ctx,
                                  shift_entity_t entity,
                                  shift_collection_id_t dest_col_id);

shift_result_t shift_entity_get_component(shift_t *ctx,
                                           shift_entity_t entity,
                                           shift_component_id_t comp_id,
                                           void **out_data);

/* --------------------------------------------------------------------------
 * Flush  (subject == lib → shift_ prefix)
 * -------------------------------------------------------------------------- */

shift_result_t shift_flush(shift_t *ctx);
