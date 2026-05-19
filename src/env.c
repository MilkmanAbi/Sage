#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "env.h"
#include "gc.h"
#include "sage_thread.h"

static Env* allocated_envs = NULL;
static unsigned long long next_env_id = 1;
static sage_mutex_t env_mutex = SAGE_MUTEX_INITIALIZER;

// Helper function to duplicate a string with a max length (similar to strndup)
static char* my_strndup(const char* s, size_t n) {
    char* result;
    size_t len = 0;

    // Count length up to n or null terminator
    while (len < n && s[len] != '\0') {
        len++;
    }

    result = (char*)malloc(len + 1);
    if (!result) return NULL;

    memcpy(result, s, len);
    result[len] = '\0'; // Explicit null terminator
    return result;
}

// P13: Env pool for fast function call allocation
#define ENV_POOL_SIZE 256
static Env env_pool[ENV_POOL_SIZE];
static int env_pool_top = 0;  // Stack pointer into pool

Env* env_create(Env* parent) {
    Env* env;
    if (env_pool_top > 0) {
        // Fast path: reuse pooled env
        env = &env_pool[--env_pool_top];
    } else {
        env = SAGE_ALLOC(sizeof(Env));
    }
    env->head   = NULL;
    env->parent = parent;
    env->marked = 0;
    env->count  = 0;
    env->map    = NULL;
    sage_mutex_lock(&env_mutex);
    env->id = next_env_id++;
    env->alloc_next = allocated_envs;
    allocated_envs = env;
    sage_mutex_unlock(&env_mutex);
    return env;
}


void env_define(Env* env, const char* name, int length, Value value) {
    // Always use linked list as authoritative store; map is a lookup index only.
    // Search linked list for update
    EnvNode* current = env->head;
    while (current != NULL) {
        if (current->name_length == length &&
            memcmp(current->name, name, (size_t)length) == 0) {
            if (gc.mode == GC_MODE_ARC || gc.mode == GC_MODE_ORC) {
                arc_assign_value(&current->value, value);
            } else {
                GC_WRITE_BARRIER(current->value);
                current->value = value;
            }
            // Keep map in sync
            if (env->map) {
                envmap_set(env->map, name, length, value);
            }
            return;
        }
        current = current->next;
    }

    // Create new in current scope
    EnvNode* node = SAGE_ALLOC(sizeof(EnvNode));
    node->name = my_strndup(name, length);
    node->name_length = length;
    node->owns_name = 1;
    node->is_const = 0;  // P4: mutable
    node->value = value;
    node->next = env->head;
    env->head = node;
    env->count++;
    // Also insert into map index if present
    if (env->map) {
        envmap_set(env->map, node->name, length, value);
    }
}

// P13: EnvNode pool for fast parameter binding
#define ENVNODE_POOL_SIZE 512
static EnvNode envnode_pool[ENVNODE_POOL_SIZE];
static int envnode_pool_top = 0;

static EnvNode* envnode_alloc(void) {
    if (envnode_pool_top > 0) return &envnode_pool[--envnode_pool_top];
    return SAGE_ALLOC(sizeof(EnvNode));
}

void env_define_const(Env* env, const char* name, int length, Value value) {
    // Always keep linked list as the authoritative store (required for env_get_node / inline cache).
    // Map is an INDEX that accelerates env_get() — not a replacement.
    // Search linked list (small scopes)
    EnvNode* current = env->head;
    while (current != NULL) {
        if (current->name_length == length &&
            memcmp(current->name, name, (size_t)length) == 0) {
            if (gc.mode == GC_MODE_ARC || gc.mode == GC_MODE_ORC) {
                arc_assign_value(&current->value, value);
            } else {
                GC_WRITE_BARRIER(current->value);
                current->value = value;
            }
            // Also update map index if present
            if (env->map) {
                envmap_set(env->map, name, length, value);
            }
            return;
        }
        current = current->next;
    }

    // Create new node in current scope (linked list is always authoritative)
    EnvNode* node = envnode_alloc();
    node->name        = (char*)name;
    node->name_length = length;
    node->owns_name   = 0;
    node->is_const    = 1;  // P4: immutable
    node->value       = value;
    node->next        = env->head;
    env->head         = node;
    env->count++;
    // Also insert into the map (index) if it exists
    if (env->map) {
        envmap_set(env->map, name, length, value);
    }

    // Promote to hashmap when exceeding threshold (large scopes: modules, class bodies)
    if (env->count == ENV_MAP_THRESHOLD + 1) {
        env->map = (EnvMap*)SAGE_ALLOC(sizeof(EnvMap));
        envmap_init(env->map, env->count * 2);
        // Migrate all existing linked list nodes into the hashmap
        EnvNode* n = env->head;
        while (n) {
            envmap_set(env->map, n->name, n->name_length, n->value);
            n = n->next;
        }
        // Keep the linked list as a fallback for GC traversal (for now)
    }
}


int env_get(Env* env, const char* name, int length, Value* out_value) {
    if (env == NULL || name == NULL) return 0;
    Env* current_env = env;

    while (current_env != NULL) {
        // Hashmap fast path (O(1)) — check first
        if (current_env->map) {
            if (envmap_get(current_env->map, name, length, out_value)) return 1;
            // Map miss: variable was defined via env_define (not env_define_const).
            // Fall through to linked list scan as authoritative fallback.
        }
        // Linked list scan (always valid, map is only an index)
        {
            EnvNode* cur = current_env->head;
            while (cur != NULL) {
                if (cur->name_length == length &&
                    memcmp(cur->name, name, (size_t)length) == 0) {
                    *out_value = cur->value;
                    return 1;
                }
                cur = cur->next;
            }
        }
        current_env = current_env->parent;
    }
    return 0;
}

int env_get_node(Env* env, const char* name, int length, Env** out_env, EnvNode** out_node) {
    if (env == NULL || name == NULL) return 0;
    Env* current_env = env;

    while (current_env != NULL) {
        EnvNode* current = current_env->head;
        while (current != NULL) {
            if (current->name_length == length &&
                memcmp(current->name, name, (size_t)length) == 0) {
                if (out_env) *out_env = current_env;
                if (out_node) *out_node = current;
                return 1;
            }
            current = current->next;
        }
        current_env = current_env->parent;
    }
    return 0;
}

// Assign to an existing variable (searches up the scope chain)
int env_assign(Env* env, const char* name, int length, Value value) {
    Env* current_env = env;

    while (current_env != NULL) {
        EnvNode* current = current_env->head;
        while (current != NULL) {
            if (current->name_length == length &&
                memcmp(current->name, name, (size_t)length) == 0) {
                // P4: Immutability enforcement
                if (current->is_const) {
                    fprintf(stderr, "TypeError: cannot reassign immutable variable '%.*s'\n",
                            length, name);
                    return 0;
                }
                if (gc.mode == GC_MODE_ARC || gc.mode == GC_MODE_ORC) {
                    arc_assign_value(&current->value, value);
                } else {
                    GC_WRITE_BARRIER(current->value);
                    current->value = value;
                }
                // Keep map in sync (critical: map is read first by env_get)
                if (current_env->map) {
                    int was_found = envmap_set(current_env->map, name, length, value);
                    (void)was_found;
                }
                return 1;
            }
            current = current->next;
        }
        current_env = current_env->parent;
    }
    return 0; // Not found
}

void env_cleanup_all(void) {
    sage_mutex_lock(&env_mutex);
    while (allocated_envs != NULL) {
        Env* env = allocated_envs;
        allocated_envs = allocated_envs->alloc_next;

        EnvNode* current = env->head;
        while (current != NULL) {
            EnvNode* next = current->next;
            if (current->owns_name) free(current->name);
            free(current);
            current = next;
        }

        free(env);
    }
    sage_mutex_unlock(&env_mutex);
}

// Free environments not marked as reachable during GC
void env_sweep_unmarked(void) {
    sage_mutex_lock(&env_mutex);
    Env** ptr = &allocated_envs;
    while (*ptr != NULL) {
        Env* env = *ptr;
        if (!env->marked) {
            // Remove from list and free
            *ptr = env->alloc_next;

            EnvNode* node = env->head;
            while (node != NULL) {
                EnvNode* next = node->next;
                if (node->owns_name) free(node->name);
                free(node);
                node = next;
            }
            free(env);
        } else {
            // Reachable — clear mark for next cycle and advance
            env->marked = 0;
            ptr = &env->alloc_next;
        }
    }
    sage_mutex_unlock(&env_mutex);
}

// Clear all env marks (used if sweep is skipped)
void env_clear_marks(void) {
    sage_mutex_lock(&env_mutex);
    Env* env = allocated_envs;
    while (env != NULL) {
        env->marked = 0;
        env = env->alloc_next;
    }
    sage_mutex_unlock(&env_mutex);
}
