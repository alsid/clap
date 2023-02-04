// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "util.h"

void cleanup__fd(int *fd)
{
    close(*fd);
}

void cleanup__FILEp(FILE **f)
{
    if (*f)
        fclose(*f);
}

void cleanup__malloc(void **x)
{
    free(*x);
}

void cleanup__charp(char **s)
{
    free(*s);
}

void cleanup__ucharp(uchar **s)
{
    free(*s);
}

void *memdup(const void *x, size_t size)
{
    void *r = malloc(size);
    if (r) {
        memcpy(r, x, size);
    }
    return r;
}

void *darray_resize(struct darray *da, unsigned int nr_el)
{
    void *new;

    /*
     * XXX: be smarter. For now, just avoid realloc() on deletion:
     *  - the array is likely to get repopulated (game.c)
     *  - or, filled in once and then cleared out (gltf.c)
     */
    if (nr_el < da->nr_el)
        goto out;

    new = realloc(da->array, nr_el * da->elsz);

    if (!new)
        return NULL;

    da->array = new;
    if (nr_el > da->nr_el)
        memset(new + da->nr_el * da->elsz, 0, (nr_el - da->nr_el) * da->elsz);

out:
    da->nr_el = nr_el;

    return da->array;
}

void *darray_add(struct darray *da)
{
    void *new = darray_resize(da, da->nr_el + 1);

    if (!new)
        return NULL;

    new = darray_get(da, da->nr_el - 1);
    memset(new, 0, da->elsz);

    return new;
}

void *darray_insert(struct darray *da, int idx)
{
    void *new = darray_resize(da, da->nr_el + 1);

    if (!new)
        return NULL;

    memmove(new + (idx + 1) * da->elsz, new + idx * da->elsz,
            (da->nr_el - idx - 1) * da->elsz);
    new = darray_get(da, idx);
    memset(new, 0, da->elsz);

    return new;
}

void darray_delete(struct darray *da, int idx)
{
    memmove(da->array + idx * da->elsz, da->array + (idx + 1) * da->elsz,
            (da->nr_el - idx - 1) * da->elsz);

    (void)darray_resize(da, da->nr_el - 1);
}

void darray_clearout(struct darray *da)
{
    free(da->array);
    da->array = NULL;
    da->nr_el = 0;
}

static unsigned long hash_simple(struct hashmap *hm, unsigned int key)
{
    return key & (hm->nr_buckets - 1);
}

int hashmap_init(struct hashmap *hm, size_t nr_buckets)
{
    int i;

    if (nr_buckets & (nr_buckets - 1))
        return -1;

    hm->buckets = calloc(nr_buckets, sizeof(struct list));
    hm->nr_buckets = nr_buckets;
    hm->hash = hash_simple;
    list_init(&hm->list);

    for (i = 0; i < nr_buckets; i++)
        list_init(&hm->buckets[i]);

    return 0;
}

void hashmap_done(struct hashmap *hm)
{
    struct hashmap_entry *e, *it;

    list_for_each_entry_iter(e, it, &hm->list, list_entry) {
        list_del(&e->list_entry);
        free(e);
    }
    free(hm->buckets);
    hm->nr_buckets = 0;
}

static struct hashmap_entry *
_hashmap_find(struct hashmap *hm, unsigned int key, unsigned long *phash)
{
    struct hashmap_entry *e;

    *phash = hm->hash(hm, key);
    list_for_each_entry(e, &hm->buckets[*phash], entry) {
        if (e->key == key)
            return e;
    }

    return NULL;
}

void *hashmap_find(struct hashmap *hm, unsigned int key)
{
    unsigned long hash;
    struct hashmap_entry *e = _hashmap_find(hm, key, &hash);

    return e ? e->value : NULL;
}

void hashmap_delete(struct hashmap *hm, unsigned int key)
{
    unsigned long hash;
    struct hashmap_entry *e = _hashmap_find(hm, key, &hash);

    if (!e)
        return;

    list_del(&e->entry);
    list_del(&e->list_entry);
    free(e);
}

int hashmap_insert(struct hashmap *hm, unsigned int key, void *value)
{
    unsigned long hash;
    struct hashmap_entry *e;
    void *v;

    if (_hashmap_find(hm, key, &hash))
        return -EBUSY;

    e = calloc(1, sizeof(*e));
    if (!e)
        return -ENOMEM;

    e->value = value;
    e->key = key;
    list_append(&hm->buckets[hash], &e->entry);
    list_append(&hm->list, &e->list_entry);

    return 0;
}

void hashmap_for_each(struct hashmap *hm, void (*cb)(void *value, void *data), void *data)
{
    struct hashmap_entry *e;

    list_for_each_entry(e, &hm->list, list_entry) {
        cb(e->value, data);
    }
}

struct exit_handler {
    exit_handler_fn     fn;
    struct exit_handler *next;
};

static struct exit_handler *ehs;

notrace int exit_cleanup(exit_handler_fn fn)
{
    struct exit_handler *eh, **lastp;

    eh = malloc(sizeof(*eh));
    if (!eh)
        return -ENOMEM;

    memset(eh, 0, sizeof(*eh));
    eh->fn = fn;

    for (lastp = &ehs; *lastp; lastp = &((*lastp)->next))
        ;

    *lastp = eh;

    return 0;
}

void exit_cleanup_run(int status)
{
    struct exit_handler *eh;

    /* XXX: free all the ehs too */
    for (eh = ehs; eh; eh = eh->next) {
        eh->fn(status);
    }
    fflush(stdout);
}

static void __attribute__((destructor)) do_exit(void)
{
    exit_cleanup_run(0);
}
