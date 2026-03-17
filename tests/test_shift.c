#include "unity.h"
#include "shift.h"

/* --------------------------------------------------------------------------
 * Test fixture helpers
 * -------------------------------------------------------------------------- */

static shift_t *make_ctx(void)
{
  shift_config_t cfg = {
    .max_entities            = 64,
    .max_components          = 8,
    .max_collections         = 8,
    .deferred_queue_capacity = 32,
    .allocator               = {0}, /* use default (libc) */
  };
  shift_t *ctx = NULL;
  shift_result_t r = shift_context_create(&cfg, &ctx);
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, r);
  TEST_ASSERT_NOT_NULL(ctx);
  return ctx;
}

void setUp(void)    { /* nothing */ }
void tearDown(void) { /* nothing */ }

/* --------------------------------------------------------------------------
 * Tests
 * -------------------------------------------------------------------------- */

void test_context_create_destroy(void)
{
  shift_t *ctx = make_ctx();
  shift_context_destroy(ctx);
  /* No crash == pass */
}

void test_context_create_null_config(void)
{
  shift_t *ctx = NULL;
  shift_result_t r = shift_context_create(NULL, &ctx);
  TEST_ASSERT_EQUAL_INT(SHIFT_ERROR_NULL, r);
  TEST_ASSERT_NULL(ctx);
}

void test_component_register(void)
{
  shift_t *ctx = make_ctx();

  shift_component_info_t info = {
    .element_size = sizeof(float) * 3, /* vec3 */
    .constructor  = NULL,
    .destructor   = NULL,
  };
  shift_component_id_t id = UINT32_MAX;
  shift_result_t r = shift_component_register(ctx, &info, &id);
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, r);
  TEST_ASSERT_EQUAL_UINT32(0, id); /* first registered component gets id 0 */

  shift_context_destroy(ctx);
}

void test_component_register_second(void)
{
  shift_t *ctx = make_ctx();

  shift_component_info_t info0 = { .element_size = 4 };
  shift_component_info_t info1 = { .element_size = 8 };
  shift_component_id_t id0, id1;

  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_component_register(ctx, &info0, &id0));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_component_register(ctx, &info1, &id1));
  TEST_ASSERT_EQUAL_UINT32(0, id0);
  TEST_ASSERT_EQUAL_UINT32(1, id1);

  shift_context_destroy(ctx);
}

void test_collection_register(void)
{
  shift_t *ctx = make_ctx();

  shift_component_info_t info = { .element_size = sizeof(float) };
  shift_component_id_t comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_component_register(ctx, &info, &comp_id));

  shift_result_t r = shift_collection_register(ctx, /*col_id=*/0, &comp_id, 1);
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, r);

  shift_context_destroy(ctx);
}

void test_collection_register_duplicate_id(void)
{
  shift_t *ctx = make_ctx();

  shift_component_info_t info = { .element_size = 4 };
  shift_component_id_t comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_component_register(ctx, &info, &comp_id));

  TEST_ASSERT_EQUAL_INT(SHIFT_OK,           shift_collection_register(ctx, 42, &comp_id, 1));
  TEST_ASSERT_EQUAL_INT(SHIFT_ERROR_INVALID, shift_collection_register(ctx, 42, &comp_id, 1));

  shift_context_destroy(ctx);
}

void test_entity_create(void)
{
  shift_t *ctx = make_ctx();

  shift_component_info_t info = { .element_size = sizeof(uint32_t) };
  shift_component_id_t comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_component_register(ctx, &info, &comp_id));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_collection_register(ctx, 0, &comp_id, 1));

  shift_entity_t e = {0};
  shift_result_t r = shift_entity_create(ctx, /*col_id=*/0, &e);
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, r);

  shift_context_destroy(ctx);
}

void test_entity_stale_detection(void)
{
  shift_t *ctx = make_ctx();

  /* Fabricate a handle that looks valid but has a mismatched generation */
  shift_entity_t stale = { .index = 0, .generation = 99 };
  /* metadata[0].generation starts at 0, is_alive = false → generation mismatch */

  shift_result_t r = shift_entity_destroy(ctx, stale);
  TEST_ASSERT_EQUAL_INT(SHIFT_ERROR_STALE, r);

  shift_context_destroy(ctx);
}

void test_flush_empty(void)
{
  shift_t *ctx = make_ctx();
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));
  shift_context_destroy(ctx);
}

void test_create_flush_get_component(void)
{
  shift_t *ctx = make_ctx();

  shift_component_info_t info = { .element_size = sizeof(uint32_t) };
  shift_component_id_t comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_component_register(ctx, &info, &comp_id));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_collection_register(ctx, 0, &comp_id, 1));

  shift_entity_t e = {0};
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create(ctx, 0, &e));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  void *ptr = NULL;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_get_component(ctx, e, comp_id, &ptr));
  TEST_ASSERT_NOT_NULL(ptr);

  *(uint32_t *)ptr = 42;

  /* verify via the array accessor */
  void *arr = NULL; size_t count = 0;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
    shift_collection_get_component_array(ctx, 0, comp_id, &arr, &count));
  TEST_ASSERT_EQUAL_size_t(1, count);
  TEST_ASSERT_EQUAL_UINT32(42, ((uint32_t *)arr)[0]);

  shift_context_destroy(ctx);
}

void test_get_component_before_flush(void)
{
  shift_t *ctx = make_ctx();

  shift_component_info_t info = { .element_size = sizeof(uint32_t) };
  shift_component_id_t comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_component_register(ctx, &info, &comp_id));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_collection_register(ctx, 0, &comp_id, 1));

  shift_entity_t e = {0};
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create(ctx, 0, &e));

  void *ptr = NULL;
  TEST_ASSERT_EQUAL_INT(SHIFT_ERROR_INVALID,
    shift_entity_get_component(ctx, e, comp_id, &ptr));

  shift_context_destroy(ctx);
}

void test_destroy_flush_stale(void)
{
  shift_t *ctx = make_ctx();

  shift_component_info_t info = { .element_size = sizeof(uint32_t) };
  shift_component_id_t comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_component_register(ctx, &info, &comp_id));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_collection_register(ctx, 0, &comp_id, 1));

  shift_entity_t e = {0};
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create(ctx, 0, &e));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_destroy(ctx, e));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  /* handle is now stale */
  TEST_ASSERT_EQUAL_INT(SHIFT_ERROR_STALE, shift_entity_destroy(ctx, e));

  /* collection is empty */
  void *arr = NULL; size_t count = 0;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
    shift_collection_get_component_array(ctx, 0, comp_id, &arr, &count));
  TEST_ASSERT_EQUAL_size_t(0, count);

  shift_context_destroy(ctx);
}

void test_create_destroy_same_flush(void)
{
  shift_t *ctx = make_ctx();

  shift_component_info_t info = { .element_size = sizeof(uint32_t) };
  shift_component_id_t comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_component_register(ctx, &info, &comp_id));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_collection_register(ctx, 0, &comp_id, 1));

  shift_entity_t e = {0};
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create(ctx, 0, &e));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_destroy(ctx, e));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  /* slot recycled — handle is stale */
  TEST_ASSERT_EQUAL_INT(SHIFT_ERROR_STALE, shift_entity_destroy(ctx, e));

  /* entity never entered the collection */
  void *arr = NULL; size_t count = 0;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
    shift_collection_get_component_array(ctx, 0, comp_id, &arr, &count));
  TEST_ASSERT_EQUAL_size_t(0, count);

  shift_context_destroy(ctx);
}

void test_multi_destroy_swap_remove(void)
{
  shift_t *ctx = make_ctx();

  shift_component_info_t info = { .element_size = sizeof(uint32_t) };
  shift_component_id_t comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_component_register(ctx, &info, &comp_id));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_collection_register(ctx, 0, &comp_id, 1));

  shift_entity_t e0, e1, e2;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create(ctx, 0, &e0));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create(ctx, 0, &e1));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create(ctx, 0, &e2));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  /* write distinct values */
  void *p0, *p1, *p2;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_get_component(ctx, e0, comp_id, &p0));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_get_component(ctx, e1, comp_id, &p1));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_get_component(ctx, e2, comp_id, &p2));
  *(uint32_t *)p0 = 10;
  *(uint32_t *)p1 = 20;
  *(uint32_t *)p2 = 30;

  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_destroy(ctx, e0));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_destroy(ctx, e1));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  /* only e2 remains */
  void *arr = NULL; size_t count = 0;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
    shift_collection_get_component_array(ctx, 0, comp_id, &arr, &count));
  TEST_ASSERT_EQUAL_size_t(1, count);
  TEST_ASSERT_EQUAL_UINT32(30, ((uint32_t *)arr)[0]);

  void *pe2 = NULL;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_get_component(ctx, e2, comp_id, &pe2));
  TEST_ASSERT_EQUAL_UINT32(30, *(uint32_t *)pe2);

  shift_context_destroy(ctx);
}

void test_move_flush(void)
{
  shift_t *ctx = make_ctx();

  /* comp_a: shared, comp_b: only in col1 */
  shift_component_id_t comp_a, comp_b;
  shift_component_info_t ia = { .element_size = sizeof(uint32_t) };
  shift_component_info_t ib = { .element_size = sizeof(float) };
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_component_register(ctx, &ia, &comp_a));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_component_register(ctx, &ib, &comp_b));

  shift_component_id_t col0_comps[] = { comp_a };
  shift_component_id_t col1_comps[] = { comp_a, comp_b };
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_collection_register(ctx, 0, col0_comps, 1));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_collection_register(ctx, 1, col1_comps, 2));

  shift_entity_t e;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create(ctx, 0, &e));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  /* write a value into comp_a */
  void *pa = NULL;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_get_component(ctx, e, comp_a, &pa));
  *(uint32_t *)pa = 77;

  /* move to col1 */
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_move(ctx, e, 1));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  /* col0 empty, col1 has one entity */
  void *arr0 = NULL; size_t cnt0 = 0;
  void *arr1 = NULL; size_t cnt1 = 0;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
    shift_collection_get_component_array(ctx, 0, comp_a, &arr0, &cnt0));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
    shift_collection_get_component_array(ctx, 1, comp_a, &arr1, &cnt1));
  TEST_ASSERT_EQUAL_size_t(0, cnt0);
  TEST_ASSERT_EQUAL_size_t(1, cnt1);

  /* comp_a preserved, comp_b zero-initialised */
  void *pa2 = NULL, *pb2 = NULL;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_get_component(ctx, e, comp_a, &pa2));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_get_component(ctx, e, comp_b, &pb2));
  TEST_ASSERT_EQUAL_UINT32(77,  *(uint32_t *)pa2);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, *(float *)pb2);

  shift_context_destroy(ctx);
}

static int g_ctor_count      = 0;
static int g_ctor_call_count = 0;
static void counting_ctor(void *data, uint32_t count)
{
  (void)data;
  g_ctor_count += (int)count;
  g_ctor_call_count++;
}

static int g_dtor_count      = 0;
static int g_dtor_call_count = 0;
static void counting_dtor(void *data, uint32_t count)
{
  (void)data;
  g_dtor_count += (int)count;
  g_dtor_call_count++;
}

void test_destructor_called(void)
{
  shift_t *ctx = make_ctx();
  g_dtor_count = 0; g_dtor_call_count = 0;

  shift_component_info_t info = {
    .element_size = sizeof(uint32_t),
    .destructor   = counting_dtor,
  };
  shift_component_id_t comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_component_register(ctx, &info, &comp_id));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_collection_register(ctx, 0, &comp_id, 1));

  shift_entity_t e0, e1;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create(ctx, 0, &e0));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create(ctx, 0, &e1));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));
  TEST_ASSERT_EQUAL_INT(0, g_dtor_count);

  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_destroy(ctx, e0));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_destroy(ctx, e1));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  TEST_ASSERT_EQUAL_INT(2, g_dtor_count);

  shift_context_destroy(ctx);
}

void test_destroy_run_batches_destructor(void)
{
  /* Create 3 entities in order, destroy in order — peek should merge into one
   * DESTROY run so the destructor is called once with count=3. */
  shift_t *ctx = make_ctx();
  g_dtor_count = 0; g_dtor_call_count = 0;

  shift_component_info_t info = {
    .element_size = sizeof(uint32_t),
    .destructor   = counting_dtor,
  };
  shift_component_id_t comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_component_register(ctx, &info, &comp_id));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_collection_register(ctx, 0, &comp_id, 1));

  shift_entity_t e0, e1, e2;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create(ctx, 0, &e0));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create(ctx, 0, &e1));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create(ctx, 0, &e2));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  /* Destroy in offset order (0, 1, 2) — peek merges into one run. */
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_destroy(ctx, e0));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_destroy(ctx, e1));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_destroy(ctx, e2));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  TEST_ASSERT_EQUAL_INT(3, g_dtor_count);
  TEST_ASSERT_EQUAL_INT(1, g_dtor_call_count); /* one batch call, not three */

  void *arr = NULL; size_t count = 0;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
    shift_collection_get_component_array(ctx, 0, comp_id, &arr, &count));
  TEST_ASSERT_EQUAL_size_t(0, count);

  shift_context_destroy(ctx);
}

void test_create_run_batches_constructor(void)
{
  /* Create 3 entities consecutively — flush should group them into one
   * constructor call with count=3. */
  shift_t *ctx = make_ctx();
  g_ctor_count = 0; g_ctor_call_count = 0;

  shift_component_info_t info = {
    .element_size = sizeof(uint32_t),
    .constructor  = counting_ctor,
  };
  shift_component_id_t comp_id;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_component_register(ctx, &info, &comp_id));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_collection_register(ctx, 0, &comp_id, 1));

  shift_entity_t e0, e1, e2;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create(ctx, 0, &e0));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create(ctx, 0, &e1));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_entity_create(ctx, 0, &e2));
  TEST_ASSERT_EQUAL_INT(SHIFT_OK, shift_flush(ctx));

  TEST_ASSERT_EQUAL_INT(3, g_ctor_count);
  TEST_ASSERT_EQUAL_INT(1, g_ctor_call_count); /* one batch call, not three */

  void *arr = NULL; size_t count = 0;
  TEST_ASSERT_EQUAL_INT(SHIFT_OK,
    shift_collection_get_component_array(ctx, 0, comp_id, &arr, &count));
  TEST_ASSERT_EQUAL_size_t(3, count);

  shift_context_destroy(ctx);
}

/* --------------------------------------------------------------------------
 * Runner
 * -------------------------------------------------------------------------- */

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_context_create_destroy);
  RUN_TEST(test_context_create_null_config);
  RUN_TEST(test_component_register);
  RUN_TEST(test_component_register_second);
  RUN_TEST(test_collection_register);
  RUN_TEST(test_collection_register_duplicate_id);
  RUN_TEST(test_entity_create);
  RUN_TEST(test_entity_stale_detection);
  RUN_TEST(test_flush_empty);
  RUN_TEST(test_create_flush_get_component);
  RUN_TEST(test_get_component_before_flush);
  RUN_TEST(test_destroy_flush_stale);
  RUN_TEST(test_create_destroy_same_flush);
  RUN_TEST(test_multi_destroy_swap_remove);
  RUN_TEST(test_move_flush);
  RUN_TEST(test_destructor_called);
  RUN_TEST(test_destroy_run_batches_destructor);
  RUN_TEST(test_create_run_batches_constructor);
  return UNITY_END();
}
