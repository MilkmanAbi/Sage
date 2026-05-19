#ifndef SAGE_ENV_H
#define SAGE_ENV_H

#include "value.h"
#include "env_hashmap.h"

typedef struct EnvNode {
    char* name;
    int name_length;        // Cached name length — avoids strlen in hot lookup path
    int owns_name;          // Whether this node owns (and must free) its name string
    int is_const;           // P4: 1 = immutable (let/const), 0 = mutable (var)
    Value value;
    struct EnvNode* next;
} EnvNode;

// Threshold: scopes with > ENV_MAP_THRESHOLD vars use a hashmap
#define ENV_MAP_THRESHOLD 8

typedef struct Env {
    EnvNode*           head;       // Variables (linked list, used when count <= threshold)
    struct Env*        parent;     // Enclosing scope
    struct Env*        alloc_next; // Internal registry for shutdown cleanup
    unsigned long long id;         // Unique ID for inline caching
    int                marked;     // GC mark flag
    int                count;      // Number of variables in THIS scope
    // Hashmap (allocated lazily when count > ENV_MAP_THRESHOLD)
    struct EnvMap*     map;        // NULL until promoted
} Env;

typedef struct EnvRootNode {
    Env* env;
    struct EnvRootNode* next;
} EnvRootNode;

extern __thread EnvRootNode* g_gc_root_stack;

Env* env_create(Env* parent);
void env_define(Env* env, const char* name, int length, Value value);
void env_define_const(Env* env, const char* name, int length, Value value);
int env_get(Env* env, const char* name, int length, Value* value);
int env_get_node(Env* env, const char* name, int length, Env** out_env, EnvNode** out_node);
int env_assign(Env* env, const char* name, int length, Value value);
void env_cleanup_all(void);
void env_sweep_unmarked(void);
void env_clear_marks(void);

#endif
