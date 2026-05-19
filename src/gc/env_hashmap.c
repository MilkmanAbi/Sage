// env_hashmap.c — Robin Hood open-addressing hashmap implementation
#include "env_hashmap.h"
#include <stdlib.h>
#include <string.h>
#include "value.h"

static int next_pow2(int n) {
    int p = 8;
    while (p < n) p <<= 1;
    return p;
}

void envmap_init(EnvMap* m, int initial_cap) {
    m->cap     = next_pow2(initial_cap < 8 ? 8 : initial_cap);
    m->mask    = m->cap - 1;
    m->count   = 0;
    m->entries = (EnvEntry*)calloc((size_t)m->cap, sizeof(EnvEntry));
    for (int i = 0; i < m->cap; i++) m->entries[i].dist = -1;
}

void envmap_free(EnvMap* m) {
    free(m->entries);
    m->entries = NULL;
    m->cap = m->count = m->mask = 0;
}

static void envmap_grow(EnvMap* m) {
    int old_cap    = m->cap;
    EnvEntry* old  = m->entries;
    m->cap         = old_cap * 2;
    m->mask        = m->cap - 1;
    m->count       = 0;
    m->entries     = (EnvEntry*)calloc((size_t)m->cap, sizeof(EnvEntry));
    for (int i = 0; i < m->cap; i++) m->entries[i].dist = -1;
    for (int i = 0; i < old_cap; i++) {
        if (old[i].dist >= 0)
            envmap_set(m, old[i].key, old[i].key_len, old[i].value);
    }
    free(old);
}

int envmap_set(EnvMap* m, const char* key, int len, Value val) {
    if (m->count * 4 >= m->cap * 3) envmap_grow(m);
    uint32_t slot  = _envmap_hash(key, len) & (uint32_t)m->mask;
    EnvEntry ins   = { key, len, 0, val };
    int inserted   = 1;
    for (;;) {
        EnvEntry* e = &m->entries[slot];
        if (e->dist < 0) {
            *e = ins;
            m->count++;
            return inserted;
        }
        if (e->key_len == len && memcmp(e->key, key, (size_t)len) == 0) {
            e->value = val;
            return 0;
        }
        if (e->dist < ins.dist) {
            EnvEntry tmp = *e;
            *e   = ins;
            ins  = tmp;
            inserted = 0;
        }
        ins.dist++;
        slot = (slot + 1) & (uint32_t)m->mask;
    }
}

int envmap_get(const EnvMap* m, const char* key, int len, Value* out) {
    if (m->count == 0) return 0;
    uint32_t slot = _envmap_hash(key, len) & (uint32_t)m->mask;
    int probe     = 0;
    for (;;) {
        EnvEntry* e = &m->entries[(slot + probe) & (uint32_t)m->mask];
        if (e->dist < 0)       return 0;
        if (e->dist < probe)   return 0;
        if (e->key_len == len && memcmp(e->key, key, (size_t)len) == 0) {
            if (out) {
                *out = e->value;
            }
            return 1;
        }
        probe++;
    }
}

void envmap_clear(EnvMap* m) {
    for (int i = 0; i < m->cap; i++) m->entries[i].dist = -1;
    m->count = 0;
}
