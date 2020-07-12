#include <stdlib.h>
#include <stdbool.h>
#include "object.h"
#include "common.h"
#include "util.h"

#define TEST_MAGIC0 0xdeadbeef

struct x0 {
    struct ref	ref;
    unsigned long magic;
};

static unsigned int failcount, dropcount;

static void reset_counters(void)
{
    failcount = 0;
    dropcount = 0;
}

static bool ok_counters(void)
{
    return !failcount && dropcount == 1;
}

static void test_drop(struct ref *ref)
{
    struct x0 *x0 = container_of(ref, struct x0, ref);

    if (x0->magic != TEST_MAGIC0) {
        failcount++;
        return;
    }

    dropcount++;
    free(x0);
}

static int refcount_test0(void)
{
    struct x0 *x0 = ref_new(struct x0, ref, test_drop);

    reset_counters();
    x0->magic = TEST_MAGIC0;
    ref_put(&x0->ref);
    if (!ok_counters())
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}

static int refcount_test1(void)
{
    struct x0 *x0 = ref_new(struct x0, ref, test_drop);

    reset_counters();
    x0->magic = TEST_MAGIC0;
    x0 = ref_get(x0);
    if (x0->magic != TEST_MAGIC0)
        return EXIT_FAILURE;

    ref_put(&x0->ref);
    ref_put(&x0->ref);
    if (!ok_counters())
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}

static int refcount_test2(void)
{
    static struct x0 xS = { .ref = REF_STATIC, .magic = TEST_MAGIC0 };

    reset_counters();
    ref_put(&xS.ref);
    if (dropcount || failcount)
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}

static int __refcount_test3(void)
{
    struct x0 *x0 = ref_new(struct x0, ref, test_drop);
    CU(ref) unused struct ref *ref = &x0->ref;

    x0->magic = TEST_MAGIC0;

    return EXIT_SUCCESS;
}

static int refcount_test3(void)
{
    int ret;

    reset_counters();
    ret = __refcount_test3();
    if (ret)
        return ret;
    if (!ok_counters())
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}

struct list_entry {
    struct list entry;
    unsigned int i;
};

#define LIST_MAX 10
static int list_test0(void)
{
    struct list_entry entries[LIST_MAX], *e;
    struct list       list;
    int               i;

    list_init(&list);
    for (i = 0; i < 10; i++) {
        entries[i].i = i;
        list_append(&list, &entries[i].entry);
    }

    i = 0;
    list_for_each_entry(e, &list, entry) {
        if (i != e->i)
            return EXIT_FAILURE;
        i++;
    }
    if (i != 10)
        return EXIT_FAILURE;

    e = list_first_entry(&list, struct list_entry, entry);
    if (e->i != 0)
        return EXIT_FAILURE;

    e = list_last_entry(&list, struct list_entry, entry);
    if (e->i != LIST_MAX - 1)
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}

static struct test {
    const char	*name;
    int			(*test)(void);
} tests[] = {
    { .name = "refcount basic", .test = refcount_test0 },
    { .name = "refcount get/put", .test = refcount_test1 },
    { .name = "refcount static", .test = refcount_test2 },
    { .name = "refcount cleanup", .test = refcount_test3 },
    { .name = "list_for_each", .test = list_test0 },
};

int main()
{
    int ret, i;

    for (i = 0; i < array_size(tests); i++) {
        failcount = 0;
        ret = tests[i].test();
        msg("test %-40s: %s\n", tests[i].name, ret ? "FAILED" : "PASSED");
        if (ret)
            break;
    }

#ifdef CONFIG_BROWSER
    exit_cleanup_run(ret);
#endif
    return ret;
}

