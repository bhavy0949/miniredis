#ifndef DICT_H
#define DICT_H

#include <stddef.h>

/*
 * A tiny hash table (dictionary) with separate chaining.
 * This is your key -> value store. Read it, understand it — you WILL be
 * asked "how does your hash table handle collisions?" in an interview.
 */

typedef struct dictEntry {
    char             *key;
    char             *val;
    long long         expireAtMs; /* -1 = never expires. You'll use this on Day 7 (EXPIRE/TTL). */
    struct dictEntry *next;       /* collision chain */
} dictEntry;

typedef struct dict {
    dictEntry **buckets;
    size_t      nbuckets;
    size_t      size; /* number of stored keys */
} dict;

dict *dictCreate(size_t nbuckets);
void  dictFree(dict *d);

/* Insert or overwrite `key` with a copy of `val`. Returns 0 on success, -1 on OOM. */
int dictSet(dict *d, const char *key, const char *val);

/* Return the entry for `key`, or NULL if absent.
 * NOTE: this does NOT check expiry — deciding whether an expired key counts
 * as "absent" is YOUR job in the command layer (Day 7). */
dictEntry *dictFind(dict *d, const char *key);

/* Delete `key`. Returns 1 if a key was removed, 0 if it wasn't there. */
int dictDel(dict *d, const char *key);

#endif /* DICT_H */
