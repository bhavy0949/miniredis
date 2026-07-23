#define _POSIX_C_SOURCE 200809L
#include "dict.h"
#include <stdlib.h>
#include <string.h>

/* djb2 string hash — simple, fast, good enough. */
static unsigned long hashKey(const char *s) {
    unsigned long h = 5381;
    int c;
    while ((c = (unsigned char)*s++))
        h = ((h << 5) + h) + c; /* h * 33 + c */
    return h;
}

dict *dictCreate(size_t nbuckets) {
    dict *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->buckets = calloc(nbuckets, sizeof(dictEntry *));
    if (!d->buckets) { free(d); return NULL; }
    d->nbuckets = nbuckets;
    d->size = 0;
    return d;
}

void dictFree(dict *d) {
    if (!d) return;
    for (size_t i = 0; i < d->nbuckets; i++) {
        dictEntry *e = d->buckets[i];
        while (e) {
            dictEntry *next = e->next;
            free(e->key);
            free(e->val);
            free(e);
            e = next;
        }
    }
    free(d->buckets);
    free(d);
}

dictEntry *dictFind(dict *d, const char *key) {
    size_t idx = hashKey(key) % d->nbuckets;
    for (dictEntry *e = d->buckets[idx]; e; e = e->next) {
        if (strcmp(e->key, key) == 0)
            return e;
    }
    return NULL;
}

int dictSet(dict *d, const char *key, const char *val) {
    dictEntry *e = dictFind(d, key);
    if (e) {
        /* overwrite existing value */
        char *copy = strdup(val);
        if (!copy) return -1;
        free(e->val);
        e->val = copy;
        e->expireAtMs = -1; /* SET clears any prior TTL, like real Redis */
        return 0;
    }

    /* new key: prepend to the bucket's chain */
    size_t idx = hashKey(key) % d->nbuckets;
    e = malloc(sizeof(*e));
    if (!e) return -1;
    e->key = strdup(key);
    e->val = strdup(val);
    if (!e->key || !e->val) { free(e->key); free(e->val); free(e); return -1; }
    e->expireAtMs = -1;
    e->next = d->buckets[idx];
    d->buckets[idx] = e;
    d->size++;
    return 0;
}

int dictDel(dict *d, const char *key) {
    size_t idx = hashKey(key) % d->nbuckets;
    dictEntry *e = d->buckets[idx], *prev = NULL;
    while (e) {
        if (strcmp(e->key, key) == 0) {
            if (prev) prev->next = e->next;
            else      d->buckets[idx] = e->next;
            free(e->key);
            free(e->val);
            free(e);
            d->size--;
            return 1;
        }
        prev = e;
        e = e->next;
    }
    return 0;
}
