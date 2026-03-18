#include "unity.h"
#include "shift.h"

/* --------------------------------------------------------------------------
 * Test fixture helpers
 * -------------------------------------------------------------------------- */

static shift_t *make_ctx(void) {
  shift_config_t cfg = {
      .max_entities            = 64,
      .max_components          = 8,
      .max_collections         = 8,
      .deferred_queue_capacity = 32,
      .allocator               = {0}, /* use default (libc) */
  };
  shift_t       *ctx = NULL;
  shift_result_t r   = shift_context_create(&cfg, &ctx);
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, r);
  TEST_ASSERT_NOT_NULL(ctx);
  return ctx;
}

void setUp(void) { /* nothing */
}
void tearDown(void) { /* nothing */
}

/* --------------------------------------------------------------------------
 * Tests
 * -------------------------------------------------------------------------- */

void test_context_create_destroy(void) {
  shift_t *ctx = make_ctx();
  shift_context_destroy(ctx);
  /* No crash == pass */
}

void test_context_create_null_config(void) {
  shift_t       *ctx = NULL;
  shift_result_t r   = shift_context_create(NULL, &ctx);
  TEST_ASSERT_EQUAL_INT(SHIFT_ERROR_NULL, r);
  TEST_ASSERT_NULL(ctx);
}

void test_component_register(void) {
  shift_t *ctx = make_ctx();

  shift_component_info_t info = {
      .element_size = sizeof(float) * 3, /* vec3 */
      .constructor  = NULL,
      .destructor   = NULL,
  };
  shift_component_id_t id = UINT32_MAX;
  shift_result_t       r  = shift_component_register(ctx, &info, &id);
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, r);
  TEST_ASSERT_EQUAL_UINT32(0, id); /* first registered component gets id 0 */

  shift_context_destroy(ctx);
}

void test_component_register_second(void) {
  shift_t *ctx = make_ctx();

  shift_component_info_t info0 = {.element_size = 4};
  shift_component_info_t info1 = {.element_size = 8};
  shift_component_id_t   id0, id1;

  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_component_register(ctx, &info0, &id0));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_component_register(ctx, &info1, &id1));
  TEST_ASSERT_EQUAL_UINT32(0, id0);
  TEST_ASSERT_EQUAL_UINT32(1, id1);

  shift_context_destroy(ctx);
}

void test_collection_register(void) {
  shift_t *ctx = make_ctx();

  shift_component_info_t info = {.element_size = sizeof(float)};
  shift_component_id_t   comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_component_register(ctx, &info, &comp_id));

  shift_collection_id_t col_id;
  shift_result_t        r = shift_collection_register(
      ctx, &(shift_collection_info_t){.comp_ids = &comp_id, .comp_count = 1},
      &col_id);
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, r);

  shift_context_destroy(ctx);
}

void test_collection_register_sequential_ids(void) {
  /* shift_collection_register auto-assigns IDs. The null collection occupies
   * index 0, so user collections start at 1 and increment. */
  shift_t *ctx = make_ctx();

  shift_component_info_t info = {.element_size = 4};
  shift_component_id_t   comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_component_register(ctx, &info, &comp_id));

  shift_collection_id_t id0, id1, id2;
  TEST_ASSERT_EQUAL_INT(
      SHIFT_OK,
      shift_collection_register(
          ctx,
          &(shift_collection_info_t){.comp_ids = &comp_id, .comp_count = 1},
          &id0));
  TEST_ASSERT_EQUAL_INT(
      SHIFT_OK,
      shift_collection_register(
          ctx,
          &(shift_collection_info_t){.comp_ids = &comp_id, .comp_count = 1},
          &id1));
  TEST_ASSERT_EQUAL_INT(
      SHIFT_OK,
      shift_collection_register(
          ctx,
          &(shift_collection_info_t){.comp_ids = &comp_id, .comp_count = 1},
          &id2));

  /* IDs are sequential starting at 1 (0 is reserved for the null collection) */
  TEST_ASSERT_EQUAL_UINT32(1, id0);
  TEST_ASSERT_EQUAL_UINT32(2, id1);
  TEST_ASSERT_EQUAL_UINT32(3, id2);

  shift_context_destroy(ctx);
}

void test_entity_create(void) {
  shift_t *ctx = make_ctx();

  shift_component_info_t info = {.element_size = sizeof(uint32_t)};
  shift_component_id_t   comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_component_register(ctx, &info, &comp_id));
  shift_collection_id_t col_id;
  TEST_ASSERT_EQUAL_INT(
      SHIFT_OK,
      shift_collection_register(
          ctx,
          &(shift_collection_info_t){.comp_ids = &comp_id, .comp_count = 1},
          &col_id));

  shift_entity_t e;
  shift_result_t r = shift_entity_create_one(ctx, col_id, &e);
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, r);

  shift_context_destroy(ctx);
}

void test_entity_stale_detection(void) {
  shift_t *ctx = make_ctx();

  /* Fabricate a handle that looks valid but has a mismatched generation */
  shift_entity_t stale = {.index = 0, .generation = 99};
  /* metadata[0].generation starts at 0, is_alive = false → generation mismatch
   */

  shift_result_t r = shift_entity_destroy_one(ctx, stale);
  TEST_ASSERT_EQUAL_INT(SHIFT_ERROR_STALE, r);

  shift_context_destroy(ctx);
}

void test_flush_empty(void) {
  shift_t *ctx = make_ctx();
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));
  shift_context_destroy(ctx);
}

void test_create_flush_get_component(void) {
  shift_t *ctx = make_ctx();

  shift_component_info_t info = {.element_size = sizeof(uint32_t)};
  shift_component_id_t   comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_component_register(ctx, &info, &comp_id));
  shift_collection_id_t col_id;
  TEST_ASSERT_EQUAL_INT(
      SHIFT_OK,
      shift_collection_register(
          ctx,
          &(shift_collection_info_t){.comp_ids = &comp_id, .comp_count = 1},
          &col_id));

  shift_entity_t e;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create_one(ctx, col_id, &e));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  void *ptr = NULL;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_entity_get_component(ctx, e, comp_id, &ptr));
  TEST_ASSERT_NOT_NULL(ptr);

  *(uint32_t *)ptr = 42;

  /* verify via the array accessor */
  void  *arr   = NULL;
  size_t count = 0;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_collection_get_component_array(
                                      ctx, col_id, comp_id, &arr, &count));
  TEST_ASSERT_EQUAL_size_t(1, count);
  TEST_ASSERT_EQUAL_UINT32(42, ((uint32_t *)arr)[0]);

  shift_context_destroy(ctx);
}

void test_get_component_before_flush(void) {
  shift_t *ctx = make_ctx();

  shift_component_info_t info = {.element_size = sizeof(uint32_t)};
  shift_component_id_t   comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_component_register(ctx, &info, &comp_id));
  shift_collection_id_t col_id;
  TEST_ASSERT_EQUAL_INT(
      SHIFT_OK,
      shift_collection_register(
          ctx,
          &(shift_collection_info_t){.comp_ids = &comp_id, .comp_count = 1},
          &col_id));

  shift_entity_t e;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create_one(ctx, col_id, &e));

  void *ptr = NULL;
  TEST_ASSERT_EQUAL_INT(SHIFT_ERROR_STALE,
                        shift_entity_get_component(ctx, e, comp_id, &ptr));

  shift_context_destroy(ctx);
}

void test_destroy_flush_stale(void) {
  shift_t *ctx = make_ctx();

  shift_component_info_t info = {.element_size = sizeof(uint32_t)};
  shift_component_id_t   comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_component_register(ctx, &info, &comp_id));
  shift_collection_id_t col_id;
  TEST_ASSERT_EQUAL_INT(
      SHIFT_OK,
      shift_collection_register(
          ctx,
          &(shift_collection_info_t){.comp_ids = &comp_id, .comp_count = 1},
          &col_id));

  shift_entity_t e;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create_one(ctx, col_id, &e));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_destroy_one(ctx, e));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  /* handle is now stale */
  TEST_ASSERT_EQUAL_INT(SHIFT_ERROR_STALE, shift_entity_destroy_one(ctx, e));

  /* collection is empty */
  void  *arr   = NULL;
  size_t count = 0;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_collection_get_component_array(
                                      ctx, col_id, comp_id, &arr, &count));
  TEST_ASSERT_EQUAL_size_t(0, count);

  shift_context_destroy(ctx);
}

void test_multi_destroy_swap_remove(void) {
  shift_t *ctx = make_ctx();

  shift_component_info_t info = {.element_size = sizeof(uint32_t)};
  shift_component_id_t   comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_component_register(ctx, &info, &comp_id));
  shift_collection_id_t col_id;
  TEST_ASSERT_EQUAL_INT(
      SHIFT_OK,
      shift_collection_register(
          ctx,
          &(shift_collection_info_t){.comp_ids = &comp_id, .comp_count = 1},
          &col_id));

  shift_entity_t *ep;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create(ctx, 3, col_id, &ep));
  shift_entity_t e0 = ep[0], e1 = ep[1], e2 = ep[2];
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  /* write distinct values */
  void *p0, *p1, *p2;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_entity_get_component(ctx, e0, comp_id, &p0));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_entity_get_component(ctx, e1, comp_id, &p1));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_entity_get_component(ctx, e2, comp_id, &p2));
  *(uint32_t *)p0 = 10;
  *(uint32_t *)p1 = 20;
  *(uint32_t *)p2 = 30;

  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_destroy_one(ctx, e0));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_destroy_one(ctx, e1));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  /* only e2 remains */
  void  *arr   = NULL;
  size_t count = 0;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_collection_get_component_array(
                                      ctx, col_id, comp_id, &arr, &count));
  TEST_ASSERT_EQUAL_size_t(1, count);
  TEST_ASSERT_EQUAL_UINT32(30, ((uint32_t *)arr)[0]);

  void *pe2 = NULL;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_entity_get_component(ctx, e2, comp_id, &pe2));
  TEST_ASSERT_EQUAL_UINT32(30, *(uint32_t *)pe2);

  shift_context_destroy(ctx);
}

void test_move_flush(void) {
  shift_t *ctx = make_ctx();

  /* comp_a: shared, comp_b: only in col1 */
  shift_component_id_t   comp_a, comp_b;
  shift_component_info_t ia = {.element_size = sizeof(uint32_t)};
  shift_component_info_t ib = {.element_size = sizeof(float)};
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_component_register(ctx, &ia, &comp_a));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_component_register(ctx, &ib, &comp_b));

  shift_component_id_t  col0_comps[] = {comp_a};
  shift_component_id_t  col1_comps[] = {comp_a, comp_b};
  shift_collection_id_t col0_id, col1_id;
  TEST_ASSERT_EQUAL_INT(
      SHIFT_OK,
      shift_collection_register(
          ctx,
          &(shift_collection_info_t){.comp_ids = col0_comps, .comp_count = 1},
          &col0_id));
  TEST_ASSERT_EQUAL_INT(
      SHIFT_OK,
      shift_collection_register(
          ctx,
          &(shift_collection_info_t){.comp_ids = col1_comps, .comp_count = 2},
          &col1_id));

  shift_entity_t e;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create_one(ctx, col0_id, &e));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  /* write a value into comp_a */
  void *pa = NULL;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_entity_get_component(ctx, e, comp_a, &pa));
  *(uint32_t *)pa = 77;

  /* move to col1 */
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_move_one(ctx, e, col1_id));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  /* col0 empty, col1 has one entity */
  void  *arr0 = NULL;
  size_t cnt0 = 0;
  void  *arr1 = NULL;
  size_t cnt1 = 0;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_collection_get_component_array(
                                      ctx, col0_id, comp_a, &arr0, &cnt0));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_collection_get_component_array(
                                      ctx, col1_id, comp_a, &arr1, &cnt1));
  TEST_ASSERT_EQUAL_size_t(0, cnt0);
  TEST_ASSERT_EQUAL_size_t(1, cnt1);

  /* comp_a preserved, comp_b zero-initialised */
  void *pa2 = NULL, *pb2 = NULL;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_entity_get_component(ctx, e, comp_a, &pa2));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_entity_get_component(ctx, e, comp_b, &pb2));
  TEST_ASSERT_EQUAL_UINT32(77, *(uint32_t *)pa2);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, *(float *)pb2);

  shift_context_destroy(ctx);
}

static int  g_ctor_count      = 0;
static int  g_ctor_call_count = 0;
static void counting_ctor(void *data, uint32_t count) {
  (void)data;
  g_ctor_count += (int)count;
  g_ctor_call_count++;
}

static int  g_dtor_count      = 0;
static int  g_dtor_call_count = 0;
static void counting_dtor(void *data, uint32_t count) {
  (void)data;
  g_dtor_count += (int)count;
  g_dtor_call_count++;
}

void test_destructor_called(void) {
  shift_t *ctx      = make_ctx();
  g_dtor_count      = 0;
  g_dtor_call_count = 0;

  shift_component_info_t info = {
      .element_size = sizeof(uint32_t),
      .destructor   = counting_dtor,
  };
  shift_component_id_t comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_component_register(ctx, &info, &comp_id));
  shift_collection_id_t col_id;
  TEST_ASSERT_EQUAL_INT(
      SHIFT_OK,
      shift_collection_register(
          ctx,
          &(shift_collection_info_t){.comp_ids = &comp_id, .comp_count = 1},
          &col_id));

  shift_entity_t *ep;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create(ctx, 2, col_id, &ep));
  shift_entity_t e0 = ep[0], e1 = ep[1];
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));
  TEST_ASSERT_EQUAL_INT(0, g_dtor_count);

  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_destroy_one(ctx, e0));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_destroy_one(ctx, e1));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  TEST_ASSERT_EQUAL_INT(2, g_dtor_count);

  shift_context_destroy(ctx);
}

void test_destroy_run_batches_destructor(void) {
  /* Create 3 entities in order, destroy in order — peek should merge into one
   * DESTROY run so the destructor is called once with count=3. */
  shift_t *ctx      = make_ctx();
  g_dtor_count      = 0;
  g_dtor_call_count = 0;

  shift_component_info_t info = {
      .element_size = sizeof(uint32_t),
      .destructor   = counting_dtor,
  };
  shift_component_id_t comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_component_register(ctx, &info, &comp_id));
  shift_collection_id_t col_id;
  TEST_ASSERT_EQUAL_INT(
      SHIFT_OK,
      shift_collection_register(
          ctx,
          &(shift_collection_info_t){.comp_ids = &comp_id, .comp_count = 1},
          &col_id));

  shift_entity_t *ep;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create(ctx, 3, col_id, &ep));
  shift_entity_t e0 = ep[0], e1 = ep[1], e2 = ep[2];
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  /* Destroy in offset order (0, 1, 2) — peek merges into one run. */
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_destroy_one(ctx, e0));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_destroy_one(ctx, e1));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_destroy_one(ctx, e2));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  TEST_ASSERT_EQUAL_INT(3, g_dtor_count);
  TEST_ASSERT_EQUAL_INT(1, g_dtor_call_count); /* one batch call, not three */

  void  *arr   = NULL;
  size_t count = 0;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_collection_get_component_array(
                                      ctx, col_id, comp_id, &arr, &count));
  TEST_ASSERT_EQUAL_size_t(0, count);

  shift_context_destroy(ctx);
}

void test_create_run_batches_constructor(void) {
  /* Create 3 entities consecutively — flush should group them into one
   * constructor call with count=3. */
  shift_t *ctx      = make_ctx();
  g_ctor_count      = 0;
  g_ctor_call_count = 0;

  shift_component_info_t info = {
      .element_size = sizeof(uint32_t),
      .constructor  = counting_ctor,
  };
  shift_component_id_t comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_component_register(ctx, &info, &comp_id));
  shift_collection_id_t col_id;
  TEST_ASSERT_EQUAL_INT(
      SHIFT_OK,
      shift_collection_register(
          ctx,
          &(shift_collection_info_t){.comp_ids = &comp_id, .comp_count = 1},
          &col_id));

  shift_entity_t *ep;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create(ctx, 3, col_id, &ep));
  shift_entity_t e0 = ep[0], e1 = ep[1], e2 = ep[2];
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  TEST_ASSERT_EQUAL_INT(3, g_ctor_count);
  TEST_ASSERT_EQUAL_INT(1, g_ctor_call_count); /* one batch call, not three */

  void  *arr   = NULL;
  size_t count = 0;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_collection_get_component_array(
                                      ctx, col_id, comp_id, &arr, &count));
  TEST_ASSERT_EQUAL_size_t(3, count);

  shift_context_destroy(ctx);
}

void test_entity_alloc_move_to_null(void) {
  shift_t *ctx = make_ctx();

  shift_component_info_t info = {.element_size = sizeof(uint32_t)};
  shift_component_id_t   comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_component_register(ctx, &info, &comp_id));
  shift_collection_id_t col_id;
  TEST_ASSERT_EQUAL_INT(
      SHIFT_OK,
      shift_collection_register(
          ctx,
          &(shift_collection_info_t){.comp_ids = &comp_id, .comp_count = 1},
          &col_id));

  shift_entity_t e;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create_one(ctx, col_id, &e));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  /* Destroy entity */
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_destroy_one(ctx, e));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  /* handle is now stale */
  TEST_ASSERT_EQUAL_INT(SHIFT_ERROR_STALE, shift_entity_destroy_one(ctx, e));

  /* collection is empty */
  void  *arr   = NULL;
  size_t count = 0;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_collection_get_component_array(
                                      ctx, col_id, comp_id, &arr, &count));
  TEST_ASSERT_EQUAL_size_t(0, count);

  shift_context_destroy(ctx);
}

void test_max_collections_unaffected(void) {
  /* The null collection occupies index 0 internally; user-visible semantics
     are unchanged: max_collections=8 means 8 user collections. */
  shift_config_t cfg = {
      .max_entities            = 64,
      .max_components          = 8,
      .max_collections         = 8,
      .deferred_queue_capacity = 32,
      .allocator               = {0},
  };
  shift_t *ctx = NULL;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_context_create(&cfg, &ctx));

  shift_component_info_t info = {.element_size = sizeof(uint32_t)};
  shift_component_id_t   comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_component_register(ctx, &info, &comp_id));

  /* All 8 user collections should register successfully */
  for (uint32_t i = 0; i < 8; i++) {
    shift_collection_id_t col_id;
    TEST_ASSERT_EQUAL_INT(
        SHIFT_OK,
        shift_collection_register(
            ctx,
            &(shift_collection_info_t){.comp_ids = &comp_id, .comp_count = 1},
            &col_id));
  }

  /* 9th should fail */
  shift_collection_id_t extra_id;
  TEST_ASSERT_EQUAL_INT(
      SHIFT_ERROR_FULL,
      shift_collection_register(
          ctx,
          &(shift_collection_info_t){.comp_ids = &comp_id, .comp_count = 1},
          &extra_id));

  shift_context_destroy(ctx);
}

/* --------------------------------------------------------------------------
 * on_enter / on_leave callback test
 * -------------------------------------------------------------------------- */

static int g_enter_count = 0;
static int g_leave_count = 0;

static void test_on_enter_cb(shift_t *ctx, const shift_entity_t *entities,
                             uint32_t count) {
  (void)ctx;
  (void)entities;
  g_enter_count += (int)count;
}

static void test_on_leave_cb(shift_t *ctx, const shift_entity_t *entities,
                             uint32_t count) {
  (void)ctx;
  (void)entities;
  g_leave_count += (int)count;
}

void test_collection_on_enter_on_leave(void) {
  shift_t *ctx  = make_ctx();
  g_enter_count = 0;
  g_leave_count = 0;

  shift_component_info_t info = {.element_size = sizeof(uint32_t)};
  shift_component_id_t   comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_component_register(ctx, &info, &comp_id));

  shift_collection_info_t col_info = {
      .comp_ids   = &comp_id,
      .comp_count = 1,
      .on_enter   = test_on_enter_cb,
      .on_leave   = test_on_leave_cb,
  };
  shift_collection_id_t col_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_collection_register(ctx, &col_info, &col_id));

  shift_entity_t e;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create_one(ctx, col_id, &e));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  TEST_ASSERT_EQUAL_INT(1, g_enter_count); /* on_enter fired once */
  TEST_ASSERT_EQUAL_INT(0, g_leave_count);

  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_destroy_one(ctx, e));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  TEST_ASSERT_EQUAL_INT(1, g_enter_count);
  TEST_ASSERT_EQUAL_INT(1, g_leave_count); /* on_leave fired once */

  shift_context_destroy(ctx);
}

void test_entity_create_one_consecutive(void) {
  shift_t *ctx = make_ctx();

  shift_component_info_t info = {.element_size = sizeof(uint32_t)};
  shift_component_id_t   comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_component_register(ctx, &info, &comp_id));
  shift_collection_id_t col_id;
  TEST_ASSERT_EQUAL_INT(
      SHIFT_OK,
      shift_collection_register(
          ctx,
          &(shift_collection_info_t){.comp_ids = &comp_id, .comp_count = 1},
          &col_id));

  /* First two creates yield consecutive front-to-back indices. */
  shift_entity_t e0, e1;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create_one(ctx, col_id, &e0));
  TEST_ASSERT_EQUAL_UINT32(0, e0.index);
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create_one(ctx, col_id, &e1));
  TEST_ASSERT_EQUAL_UINT32(1, e1.index);

  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  /* After flush: col_remove_run backfills the gap from the tail.
   * max_entities=64, run=[0,2), tail entities at positions 62 and 63 fill
   * positions 0 and 1.  null_front is reset to 0, so the next create
   * returns the entity that was at position 62 (index 62). */
  shift_entity_t e2;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create_one(ctx, col_id, &e2));
  TEST_ASSERT_EQUAL_UINT32(62, e2.index);

  shift_context_destroy(ctx);

  /* A fresh context always hands out index 0 first. */
  shift_t               *ctx2  = make_ctx();
  shift_component_info_t info2 = {.element_size = sizeof(uint32_t)};
  shift_component_id_t   comp_id2;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_component_register(ctx2, &info2, &comp_id2));
  shift_collection_id_t col_id2;
  TEST_ASSERT_EQUAL_INT(
      SHIFT_OK,
      shift_collection_register(
          ctx2,
          &(shift_collection_info_t){.comp_ids = &comp_id2, .comp_count = 1},
          &col_id2));
  shift_entity_t e3;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create_one(ctx2, col_id2, &e3));
  TEST_ASSERT_EQUAL_UINT32(0, e3.index);
  shift_context_destroy(ctx2);
}

void test_entity_create_batch(void) {
  shift_t *ctx = make_ctx();

  shift_component_info_t info = {.element_size = sizeof(uint32_t)};
  shift_component_id_t   comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_component_register(ctx, &info, &comp_id));
  shift_collection_id_t col_id;
  TEST_ASSERT_EQUAL_INT(
      SHIFT_OK,
      shift_collection_register(
          ctx,
          &(shift_collection_info_t){.comp_ids = &comp_id, .comp_count = 1},
          &col_id));

  /* count=0 is invalid */
  shift_entity_t *ep;
  TEST_ASSERT_EQUAL_INT(SHIFT_ERROR_INVALID,
                        shift_entity_create(ctx, 0, col_id, &ep));

  /* create 3 at once */
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create(ctx, 3, col_id, &ep));
  TEST_ASSERT_EQUAL_UINT32(0, ep[0].index);
  TEST_ASSERT_EQUAL_UINT32(1, ep[1].index);
  TEST_ASSERT_EQUAL_UINT32(2, ep[2].index);

  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  void  *arr   = NULL;
  size_t count = 0;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_collection_get_component_array(
                                      ctx, col_id, comp_id, &arr, &count));
  TEST_ASSERT_EQUAL_size_t(3, count);

  /* creating more than available returns SHIFT_ERROR_FULL */
  /* max_entities=64, 3 already used, 61 remain */
  TEST_ASSERT_EQUAL_INT(SHIFT_ERROR_FULL,
                        shift_entity_create(ctx, 62, col_id, &ep));

  shift_context_destroy(ctx);
}

/* --------------------------------------------------------------------------
 * shift_entity_move (batch) tests
 * -------------------------------------------------------------------------- */

void test_move_batch_null_args(void) {
  shift_t *ctx = make_ctx();

  shift_component_info_t info = {.element_size = sizeof(uint32_t)};
  shift_component_id_t   comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_component_register(ctx, &info, &comp_id));
  shift_collection_id_t col_id;
  TEST_ASSERT_EQUAL_INT(
      SHIFT_OK,
      shift_collection_register(
          ctx,
          &(shift_collection_info_t){.comp_ids = &comp_id, .comp_count = 1},
          &col_id));

  shift_entity_t e;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create_one(ctx, col_id, &e));

  TEST_ASSERT_EQUAL_INT(SHIFT_ERROR_NULL,
                        shift_entity_move(NULL, &e, 1, col_id));
  TEST_ASSERT_EQUAL_INT(SHIFT_ERROR_NULL,
                        shift_entity_move(ctx, NULL, 1, col_id));

  shift_context_destroy(ctx);
}

void test_move_batch_zero_count(void) {
  shift_t *ctx = make_ctx();

  shift_component_info_t info = {.element_size = sizeof(uint32_t)};
  shift_component_id_t   comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_component_register(ctx, &info, &comp_id));
  shift_collection_id_t col_id;
  TEST_ASSERT_EQUAL_INT(
      SHIFT_OK,
      shift_collection_register(
          ctx,
          &(shift_collection_info_t){.comp_ids = &comp_id, .comp_count = 1},
          &col_id));

  shift_entity_t e;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create_one(ctx, col_id, &e));

  TEST_ASSERT_EQUAL_INT(SHIFT_ERROR_INVALID,
                        shift_entity_move(ctx, &e, 0, col_id));

  shift_context_destroy(ctx);
}

void test_move_batch_basic(void) {
  /* Batch-move 4 entities from a source collection into a dest collection and
   * verify they all land correctly after flush. */
  shift_t *ctx = make_ctx();

  shift_component_info_t info = {.element_size = sizeof(uint32_t)};
  shift_component_id_t   comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_component_register(ctx, &info, &comp_id));
  shift_collection_id_t col_src_id, col_id;
  TEST_ASSERT_EQUAL_INT(
      SHIFT_OK,
      shift_collection_register(
          ctx,
          &(shift_collection_info_t){.comp_ids = &comp_id, .comp_count = 1},
          &col_src_id));
  TEST_ASSERT_EQUAL_INT(
      SHIFT_OK,
      shift_collection_register(
          ctx,
          &(shift_collection_info_t){.comp_ids = &comp_id, .comp_count = 1},
          &col_id));

  shift_entity_t *ep;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create(ctx, 4, col_src_id, &ep));
  shift_entity_t entities[4] = {ep[0], ep[1], ep[2], ep[3]};
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_move(ctx, entities, 4, col_id));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  void  *arr   = NULL;
  size_t count = 0;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_collection_get_component_array(
                                      ctx, col_id, comp_id, &arr, &count));
  TEST_ASSERT_EQUAL_size_t(4, count);

  shift_context_destroy(ctx);
}

void test_move_batch_peek_fires(void) {
  /* Contiguous entities batch-moved from a source collection to the same dest:
   * the peek optimization should merge them into one deferred op → constructor
   * is called once with count=N, not N times with count=1. */
  shift_t *ctx      = make_ctx();
  g_ctor_count      = 0;
  g_ctor_call_count = 0;

  shift_component_info_t info = {
      .element_size = sizeof(uint32_t),
      .constructor  = counting_ctor,
  };
  shift_component_id_t comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_component_register(ctx, &info, &comp_id));
  shift_collection_id_t col_id;
  TEST_ASSERT_EQUAL_INT(
      SHIFT_OK,
      shift_collection_register(
          ctx,
          &(shift_collection_info_t){.comp_ids = &comp_id, .comp_count = 1},
          &col_id));

  /* Source collection uses a separate component (no constructor) so that
   * placing entities there does not fire counting_ctor. */
  shift_component_info_t src_info = {.element_size = sizeof(uint32_t)};
  shift_component_id_t   comp_src;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_component_register(ctx, &src_info, &comp_src));
  shift_collection_id_t col_src_id;
  TEST_ASSERT_EQUAL_INT(
      SHIFT_OK,
      shift_collection_register(
          ctx,
          &(shift_collection_info_t){.comp_ids = &comp_src, .comp_count = 1},
          &col_src_id));

  shift_entity_t *ep;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create(ctx, 5, col_src_id, &ep));
  shift_entity_t entities[5] = {ep[0], ep[1], ep[2], ep[3], ep[4]};
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));
  TEST_ASSERT_EQUAL_INT(0, g_ctor_count); /* no ctor yet */

  /* Batch-move the 5 contiguous entities to col_id: peek merges into one op. */
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_move(ctx, entities, 5, col_id));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  TEST_ASSERT_EQUAL_INT(5, g_ctor_count);
  TEST_ASSERT_EQUAL_INT(1, g_ctor_call_count); /* single batch call */

  shift_context_destroy(ctx);
}

void test_move_batch_stale(void) {
  /* A stale handle mid-array causes early return with SHIFT_ERROR_STALE. */
  shift_t *ctx = make_ctx();

  shift_component_info_t info = {.element_size = sizeof(uint32_t)};
  shift_component_id_t   comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_component_register(ctx, &info, &comp_id));
  shift_collection_id_t col_id;
  TEST_ASSERT_EQUAL_INT(
      SHIFT_OK,
      shift_collection_register(
          ctx,
          &(shift_collection_info_t){.comp_ids = &comp_id, .comp_count = 1},
          &col_id));

  shift_entity_t *ep;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create(ctx, 2, col_id, &ep));
  shift_entity_t e0 = ep[0], e1 = ep[1];
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  shift_entity_t stale    = {.index = 0, .generation = 99};
  shift_entity_t batch[3] = {e0, stale, e1};
  TEST_ASSERT_EQUAL_INT(SHIFT_ERROR_STALE,
                        shift_entity_move(ctx, batch, 3, col_id));

  shift_context_destroy(ctx);
}

void test_move_batch_multi_source(void) {
  /* Entities from two different source collections are all moved to a third.
   * Shared component data must be preserved for each. */
  shift_t *ctx = make_ctx();

  shift_component_id_t   comp_a;
  shift_component_info_t ia = {.element_size = sizeof(uint32_t)};
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_component_register(ctx, &ia, &comp_a));

  /* col 0 and col 1 both have comp_a; col 2 is the merge destination */
  shift_collection_id_t col0_id, col1_id, col2_id;
  TEST_ASSERT_EQUAL_INT(
      SHIFT_OK,
      shift_collection_register(
          ctx, &(shift_collection_info_t){.comp_ids = &comp_a, .comp_count = 1},
          &col0_id));
  TEST_ASSERT_EQUAL_INT(
      SHIFT_OK,
      shift_collection_register(
          ctx, &(shift_collection_info_t){.comp_ids = &comp_a, .comp_count = 1},
          &col1_id));
  TEST_ASSERT_EQUAL_INT(
      SHIFT_OK,
      shift_collection_register(
          ctx, &(shift_collection_info_t){.comp_ids = &comp_a, .comp_count = 1},
          &col2_id));

  /* Place two entities in col 0, two in col 1. */
  shift_entity_t *ep0, *ep1;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create(ctx, 2, col0_id, &ep0));
  shift_entity_t ea = ep0[0], eb = ep0[1];
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create(ctx, 2, col1_id, &ep1));
  shift_entity_t ec = ep1[0], ed = ep1[1];
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  /* Write distinct marker values. */
  void *pa, *pb, *pc, *pd;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_entity_get_component(ctx, ea, comp_a, &pa));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_entity_get_component(ctx, eb, comp_a, &pb));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_entity_get_component(ctx, ec, comp_a, &pc));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_entity_get_component(ctx, ed, comp_a, &pd));
  *(uint32_t *)pa = 10;
  *(uint32_t *)pb = 20;
  *(uint32_t *)pc = 30;
  *(uint32_t *)pd = 40;

  /* Batch-move all four (from two different sources) into col 2. */
  shift_entity_t batch[4] = {ea, eb, ec, ed};
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_move(ctx, batch, 4, col2_id));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  /* col 0 and col 1 are empty; col 2 has all four entities. */
  void  *arr;
  size_t cnt;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_collection_get_component_array(
                                      ctx, col0_id, comp_a, &arr, &cnt));
  TEST_ASSERT_EQUAL_size_t(0, cnt);
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_collection_get_component_array(
                                      ctx, col1_id, comp_a, &arr, &cnt));
  TEST_ASSERT_EQUAL_size_t(0, cnt);
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_collection_get_component_array(
                                      ctx, col2_id, comp_a, &arr, &cnt));
  TEST_ASSERT_EQUAL_size_t(4, cnt);

  /* Component data preserved. */
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_entity_get_component(ctx, ea, comp_a, &pa));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_entity_get_component(ctx, eb, comp_a, &pb));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_entity_get_component(ctx, ec, comp_a, &pc));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_entity_get_component(ctx, ed, comp_a, &pd));
  TEST_ASSERT_EQUAL_UINT32(10, *(uint32_t *)pa);
  TEST_ASSERT_EQUAL_UINT32(20, *(uint32_t *)pb);
  TEST_ASSERT_EQUAL_UINT32(30, *(uint32_t *)pc);
  TEST_ASSERT_EQUAL_UINT32(40, *(uint32_t *)pd);

  shift_context_destroy(ctx);
}

void test_flush_noncontiguous_removes(void) {
  /* Destroy entities at non-contiguous offsets (0 and 2) from the same
   * collection in a single flush.  Without the sort fix, processing offset=0
   * first causes col_remove_run to swap entity@2 into slot 0; the pending op
   * for entity@2 (src_offset=2) then fails the bounds check and is silently
   * dropped, leaving entity@2 stuck with has_pending_move=true. */
  shift_t *ctx = make_ctx();

  shift_component_info_t info = {.element_size = sizeof(uint32_t)};
  shift_component_id_t   comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_component_register(ctx, &info, &comp_id));
  shift_collection_id_t col_id;
  TEST_ASSERT_EQUAL_INT(
      SHIFT_OK,
      shift_collection_register(
          ctx,
          &(shift_collection_info_t){.comp_ids = &comp_id, .comp_count = 1},
          &col_id));

  shift_entity_t *ep;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create(ctx, 3, col_id, &ep));
  shift_entity_t e0 = ep[0], e1 = ep[1], e2 = ep[2];
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  void *p0, *p1, *p2;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_entity_get_component(ctx, e0, comp_id, &p0));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_entity_get_component(ctx, e1, comp_id, &p1));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_entity_get_component(ctx, e2, comp_id, &p2));
  *(uint32_t *)p0 = 10;
  *(uint32_t *)p1 = 20;
  *(uint32_t *)p2 = 30;

  /* Destroy e0 (offset=0) and e2 (offset=2): non-contiguous, separate ops. */
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_destroy_one(ctx, e0));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_destroy_one(ctx, e2));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  /* Only e1 should remain. */
  void  *arr   = NULL;
  size_t count = 0;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_collection_get_component_array(
                                      ctx, col_id, comp_id, &arr, &count));
  TEST_ASSERT_EQUAL_size_t(1, count);
  TEST_ASSERT_EQUAL_UINT32(20, ((uint32_t *)arr)[0]);

  TEST_ASSERT_EQUAL_INT(SHIFT_ERROR_STALE, shift_entity_destroy_one(ctx, e0));
  TEST_ASSERT_EQUAL_INT(SHIFT_ERROR_STALE, shift_entity_destroy_one(ctx, e2));

  void *pe1 = NULL;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
                        shift_entity_get_component(ctx, e1, comp_id, &pe1));
  TEST_ASSERT_EQUAL_UINT32(20, *(uint32_t *)pe1);

  shift_context_destroy(ctx);
}

/* --------------------------------------------------------------------------
 * Runner
 * -------------------------------------------------------------------------- */

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_context_create_destroy);
  RUN_TEST(test_context_create_null_config);
  RUN_TEST(test_component_register);
  RUN_TEST(test_component_register_second);
  RUN_TEST(test_collection_register);
  RUN_TEST(test_collection_register_sequential_ids);
  RUN_TEST(test_entity_create);
  RUN_TEST(test_entity_stale_detection);
  RUN_TEST(test_flush_empty);
  RUN_TEST(test_create_flush_get_component);
  RUN_TEST(test_get_component_before_flush);
  RUN_TEST(test_destroy_flush_stale);
  RUN_TEST(test_multi_destroy_swap_remove);
  RUN_TEST(test_move_flush);
  RUN_TEST(test_destructor_called);
  RUN_TEST(test_destroy_run_batches_destructor);
  RUN_TEST(test_create_run_batches_constructor);
  RUN_TEST(test_entity_alloc_move_to_null);
  RUN_TEST(test_max_collections_unaffected);
  RUN_TEST(test_collection_on_enter_on_leave);
  RUN_TEST(test_entity_create_one_consecutive);
  RUN_TEST(test_entity_create_batch);
  RUN_TEST(test_move_batch_null_args);
  RUN_TEST(test_move_batch_zero_count);
  RUN_TEST(test_move_batch_basic);
  RUN_TEST(test_move_batch_peek_fires);
  RUN_TEST(test_move_batch_stale);
  RUN_TEST(test_move_batch_multi_source);
  RUN_TEST(test_flush_noncontiguous_removes);
  return UNITY_END();
}
