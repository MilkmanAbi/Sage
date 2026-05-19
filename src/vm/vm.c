#include <math.h>
#include <inttypes.h>
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "module.h"
#include "repl.h"
#include "sage_thread.h"
#include "gc.h"
#include "firefly.h"

extern __thread EnvRootNode* g_gc_root_stack;

// P2: Builtin method dispatch (defined in interpreter.c)
extern int builtin_method_call(Value object, const char* name, int name_len,
                               int argc, Value* args, Value* out);
extern const char* sage_typeof_str(Value val);

#define VM_STACK_MAX 1024

typedef struct ActiveVm {
    BytecodeChunk* chunk;
    Env* current_env;
    Value stack[VM_STACK_MAX];
    int stack_count;
    struct ActiveVm* parent;
} ActiveVm;

static __thread ActiveVm* g_active_vm = NULL;

static ExecResult vm_normal(Value value) {
// ... (omitting intermediate code for brevity in replace tool, but will provide full context in actual call)
    ExecResult result = {0};
    result.value = value;
    return result;
}

static int vm_is_truthy(Value value) {
    if (IS_NIL(value))   return 0;
    if (IS_BOOL(value))  return AS_BOOL(value);
    if (IS_INT(value))   return AS_INT(value) != 0;
    if (IS_NUMERIC(value)) return NUMERIC_AS_DOUBLE(value) != 0.0;
    if (IS_STRING(value)) return AS_STRING(value)[0] != '\0';
    return 1;
}

static void vm_mark_chunk_constants(BytecodeChunk* chunk) {
    if (chunk == NULL) return;

    for (int i = 0; i < chunk->constant_count; i++) {
        gc_mark_value(chunk->constants[i]);
    }
}

static void vm_mark_program_constants(BytecodeProgram* program) {
    if (program == NULL) return;

    for (int i = 0; i < program->function_count; i++) {
        vm_mark_chunk_constants(&program->functions[i].chunk);
    }

    for (int i = 0; i < program->chunk_count; i++) {
        vm_mark_chunk_constants(&program->chunks[i]);
    }
}

void vm_mark_roots(void* active_vm_head) {
    for (ActiveVm* active = (ActiveVm*)active_vm_head; active != NULL; active = active->parent) {
        vm_mark_chunk_constants(active->chunk);
        vm_mark_program_constants(active->chunk != NULL ? active->chunk->program : NULL);
        gc_mark_env(active->current_env);

        for (int i = 0; i < active->stack_count; i++) {
            gc_mark_value(active->stack[i]);
        }
    }
}

static ExecResult vm_error(const char* message) {
    fprintf(stderr, "Runtime Error: %s\n", message);
    ExecResult result = {0};
    result.value = val_nil();
    result.is_throwing = 1;
    result.exception_value = val_exception(message);
    return result;
}

// P11: Firefly-aware VM error with bytecode source location
static ExecResult vm_firefly_error(BytecodeChunk* chunk, int ip, const char* message) {
    FireflyLoc loc = {0};
    if (chunk && ip >= 0 && ip < chunk->code_count) {
        loc.line = chunk->lines[ip];
        loc.column = chunk->columns ? chunk->columns[ip] : 0;
    }
    if (loc.line > 0) {
        firefly_report(FIREFLY_ERROR, loc, "%s", message);
        firefly_end();
    } else {
        fprintf(stderr, "Runtime Error: %s\n", message);
    }
    ExecResult result = {0};
    result.value = val_nil();
    result.is_throwing = 1;
    result.exception_value = val_exception(message);
    return result;
}

static int vm_push(ActiveVm* vm, Value value) {
    if (vm->stack_count >= VM_STACK_MAX) {
        return 0;
    }
    vm->stack[vm->stack_count++] = value;
    return 1;
}

static Value vm_pop(ActiveVm* vm) {
    if (vm->stack_count <= 0) {
        return val_nil();
    }
    return vm->stack[--vm->stack_count];
}

static Value vm_peek(ActiveVm* vm, int distance) {
    if (distance < 0 || vm->stack_count - 1 - distance < 0) {
        return val_nil();
    }
    return vm->stack[vm->stack_count - 1 - distance];
}

#define VM_CHECK_CONST(chunk, idx) \
    do { if ((int)(idx) >= (chunk)->constant_count) { \
        result = vm_error("VM constant pool index out of bounds."); goto done; \
    } } while(0)

#define VM_CHECK_AST(chunk, idx) \
    do { if ((int)(idx) >= (chunk)->ast_stmt_count) { \
        result = vm_error("VM AST statement index out of bounds."); goto done; \
    } } while(0)

static uint16_t read_u16(BytecodeChunk* chunk, int* ip) {
    if (*ip + 2 > chunk->code_count) {
        fprintf(stderr, "VM Error: bytecode read_u16 out of bounds (ip=%d, size=%d)\n", *ip, chunk->code_count);
        *ip = chunk->code_count;  // halt
        return 0;
    }
    uint16_t high = chunk->code[(*ip)++];
    uint16_t low = chunk->code[(*ip)++];
    return (uint16_t)((high << 8) | low);
}

static uint8_t read_u8(BytecodeChunk* chunk, int* ip) {
    if (*ip >= chunk->code_count) {
        fprintf(stderr, "VM Error: bytecode read_u8 out of bounds (ip=%d, size=%d)\n", *ip, chunk->code_count);
        return 0;
    }
    return chunk->code[(*ip)++];
}

static ExecResult call_function_value(Value callee, int arg_count, Value* args, Env* env) {
    if (callee.type == VAL_NATIVE) {
        return vm_normal(callee.as.native(arg_count, args));
    }

    if (callee.type == VAL_FUNCTION) {
        if (callee.as.function->is_async) {
            // Async call: delegate to thread_spawn_native (AST interpreter runs in a thread)
            Value* spawn_args = SAGE_ALLOC(sizeof(Value) * (arg_count + 1));
            spawn_args[0] = callee;
            for (int i = 0; i < arg_count; i++) spawn_args[i+1] = args[i];
            extern Value thread_spawn_native(int argCount, Value* args);
            Value handle = thread_spawn_native(arg_count + 1, spawn_args);
            free(spawn_args);
            return vm_normal(handle);
        }

        if (callee.as.function->is_vm) {
            BytecodeFunction* function = callee.as.function->vm_function;
            if (function == NULL) {
                return vm_error("Invalid VM function.");
            }
            if (arg_count != function->param_count) {
                return vm_error("Arity mismatch.");
            }

            Env* scope = env_create(callee.as.function->closure);
            for (int i = 0; i < function->param_count; i++) {
                env_define(scope, function->params[i], (int)strlen(function->params[i]), args[i]);
            }

            return vm_execute_chunk(&function->chunk, scope);
        }

        gc_pin();
        ProcStmt* func = (ProcStmt*)AS_FUNCTION(callee);
        // Allow fewer args than params if defaults cover the rest
        int required = func->required_count;  // 0 = all params have defaults, that's valid
        if (arg_count < required || arg_count > func->param_count) {
            gc_unpin();
            return vm_error("Arity mismatch.");
        }

        Env* scope = env_create(callee.as.function->closure);
        for (int i = 0; i < func->param_count; i++) {
            Token param = func->params[i];
            if (i < arg_count) {
                env_define(scope, param.start, param.length, args[i]);
            } else if (func->defaults && func->defaults[i]) {
                // Evaluate default expression in the caller's env
                ExecResult def_res = eval_expr_public(func->defaults[i], env);
                if (def_res.is_throwing) { gc_unpin(); return def_res; }
                env_define(scope, param.start, param.length, def_res.value);
            } else {
                env_define(scope, param.start, param.length, val_nil());
            }
        }

        ExecResult result = interpret(func->body, scope);
        gc_unpin();
        if (result.is_throwing) return result;
        return vm_normal(result.value);
    }

    if (callee.type == VAL_GENERATOR) {
        GeneratorValue* template = callee.as.generator;
        if (arg_count != template->param_count) {
            return vm_error("Arity mismatch.");
        }

        Env* closure = env_create(template->closure);
        if (template->param_count > 0 && template->params != NULL) {
            Token* params = (Token*)template->params;
            for (int i = 0; i < template->param_count; i++) {
                env_define(closure, params[i].start, params[i].length, args[i]);
            }
        }

        return vm_normal(val_generator(template->body, template->params,
                                       template->param_count, closure));
    }

    if (callee.type == VAL_CLASS) {
        gc_pin();
        ClassValue* class_def = callee.as.class_val;
        InstanceValue* instance = instance_create(class_def);
        Value instance_value = val_instance(instance);

        Method* init_method = class_find_method(class_def, "init", 4);
        if (init_method != NULL) {
            Stmt* init_node = (Stmt*)init_method->method_stmt;
            if (init_node == NULL) { gc_unpin(); return vm_error("Invalid init method."); }
            ProcStmt* init_stmt = (init_node->type == STMT_ASYNC_PROC) ? &init_node->as.async_proc : &init_node->as.proc;

            Env* def_env = class_def->defining_env;
            Env* method_env = env_create(def_env ? def_env : env);
            env_define(method_env, "self", 4, instance_value);

            int param_start = (init_stmt->param_count > 0 &&
                              init_stmt->params != NULL &&
                              strncmp(init_stmt->params[0].start, "self", 4) == 0) ? 1 : 0;

            for (int i = param_start; i < init_stmt->param_count; i++) {
                if (i - param_start < arg_count) {
                    env_define(method_env, init_stmt->params[i].start,
                               init_stmt->params[i].length, args[i - param_start]);
                }
            }

            ExecResult init_result = interpret(init_stmt->body, method_env);
            if (init_result.is_throwing) {
                gc_unpin();
                return init_result;
            }
        } else {
            // Auto-init for structs: look for __StructName_fields__ metadata
            char meta_key[256];
            snprintf(meta_key, sizeof(meta_key), "__%.*s_fields__",
                     class_def->name_len, class_def->name);
            Value fields_val;
            if (env_get(env, meta_key, (int)strlen(meta_key), &fields_val) &&
                fields_val.type == VAL_ARRAY) {
                ArrayValue* fields = fields_val.as.array;
                for (int i = 0; i < fields->count && i < arg_count; i++) {
                    if (fields->elements[i].type == VAL_STRING) {
                        char* field_name = AS_STRING(fields->elements[i]);
                        instance_set_field(instance, field_name, (int)strlen(field_name), args[i]);
                    }
                }
            }
        }

        gc_unpin();
        return vm_normal(instance_value);
    }

    // P3: ADT enum variant constructor — dict with __type == "__enum_ctor__"
    if (IS_DICT(callee)) {
        Value ctor_type = dict_get(&callee, "__type");
        if (IS_STRING(ctor_type) && strcmp(AS_STRING(ctor_type), "__enum_ctor__") == 0) {
            Value variant_type = dict_get(&callee, "__variant_type");
            Value tag_val = dict_get(&callee, "__tag");
            Value field_names = dict_get(&callee, "__fields");
            gc_pin();
            Value result = val_dict();
            dict_set(&result, "__type", variant_type);
            dict_set(&result, "__tag", tag_val);
            if (IS_ARRAY(field_names)) {
                ArrayValue* fnames = field_names.as.array;
                for (int i = 0; i < fnames->count && i < arg_count; i++) {
                    dict_set(&result, AS_STRING(fnames->elements[i]), args[i]);
                }
                if (fnames->count == 1 && arg_count >= 1) {
                    dict_set(&result, "__val", args[0]);
                }
            }
            gc_unpin();
            return vm_normal(result);
        }
    }

    return vm_error("Value is not callable.");
}

static ExecResult call_method_value(Value object, const char* method_name, int arg_count, Value* args, Env* env) {
    if (IS_INSTANCE(object)) {
        gc_pin();
        Method* method = class_find_method(object.as.instance->class_def, method_name, (int)strlen(method_name));
        if (method == NULL) {
            gc_unpin();
            return vm_error("Undefined method.");
        }

        Stmt* method_node = (Stmt*)method->method_stmt;
        ProcStmt* method_stmt = (method_node->type == STMT_ASYNC_PROC) ? &method_node->as.async_proc : &method_node->as.proc;
        Env* def_env = object.as.instance->class_def->defining_env;
        Env* method_env = env_create(def_env ? def_env : env);
        env_define(method_env, "self", 4, object);
        

        // Track class owning method for super resolution
        ClassValue* owner = class_find_method_owner(object.as.instance->class_def, method_name, (int)strlen(method_name));
        if (owner) env_define_const(method_env, "__class__", 9, val_class(owner));

        int param_start = (method_stmt->param_count > 0 &&
                          strncmp(method_stmt->params[0].start, "self", 4) == 0) ? 1 : 0;
        for (int i = param_start; i < method_stmt->param_count; i++) {
            if (i - param_start < arg_count) {
                env_define(method_env, method_stmt->params[i].start,
                           method_stmt->params[i].length, args[i - param_start]);
            }
        }

        ExecResult result = interpret(method_stmt->body, method_env);
        gc_unpin();
        if (result.is_throwing) return result;
        return vm_normal(result.value);
    }

    if (IS_MODULE(object)) {
        int found = 0;
        Value attr = module_get_attr(AS_MODULE(object), method_name, (int)strlen(method_name), &found);
        if (!found) {
            return vm_error("Module attribute is not defined.");
        }
        return call_function_value(attr, arg_count, args, env);
    }

    // P2: Builtin method dispatch for string, array, dict
    {
        Value method_result;
        if (builtin_method_call(object, method_name, (int)strlen(method_name),
                               arg_count, args, &method_result)) {
            return vm_normal(method_result);
        }
    }

    // P3: ADT enum constructor via method call — Shape.Circle(5.0)
    // When object is a dict (enum) and method_name is a variant with ctor info
    if (IS_DICT(object)) {
        Value variant_val = dict_get(&object, method_name);
        if (IS_DICT(variant_val)) {
            Value ctor_type = dict_get(&variant_val, "__type");
            if (IS_STRING(ctor_type) && strcmp(AS_STRING(ctor_type), "__enum_ctor__") == 0) {
                Value variant_type = dict_get(&variant_val, "__variant_type");
                Value tag_val = dict_get(&variant_val, "__tag");
                Value field_names = dict_get(&variant_val, "__fields");
                gc_pin();
                Value result = val_dict();
                dict_set(&result, "__type", variant_type);
                dict_set(&result, "__tag", tag_val);
                if (IS_ARRAY(field_names)) {
                    ArrayValue* fnames = field_names.as.array;
                    for (int i = 0; i < fnames->count && i < arg_count; i++) {
                        dict_set(&result, AS_STRING(fnames->elements[i]), args[i]);
                    }
                    if (fnames->count == 1 && arg_count >= 1) {
                        dict_set(&result, "__val", args[0]);
                    }
                }
                gc_unpin();
                return vm_normal(result);
            }
        }
    }

    // P13: PyObject method call — math.sqrt(144), json.dumps(data)
#ifndef SAGE_NO_PYTHON
    if (object.type == VAL_POINTER && object.as.pointer && object.as.pointer->type_tag == 99) {
        extern Value sage_py_getattr_direct(Value obj, const char* name, int name_len);
        extern Value sage_py_call_direct(Value callable, int argc, Value* args);
        Value py_method = sage_py_getattr_direct(object, method_name, (int)strlen(method_name));
        if (py_method.type == VAL_POINTER && py_method.as.pointer &&
            py_method.as.pointer->type_tag == 99) {
            Value result = sage_py_call_direct(py_method, arg_count, args);
            return vm_normal(result);
        }
    }
#endif

    { char errbuf[128]; snprintf(errbuf, sizeof(errbuf), "TypeError: %s has no methods", sage_typeof_str(object)); return vm_error(errbuf); }
}

ExecResult vm_execute_chunk(BytecodeChunk* chunk, Env* env) {
    ActiveVm vm;
    ExecResult result = vm_normal(val_nil());
    int ip = 0;
    EnvRootNode root_node;
    root_node.env = env;
    
    ThreadState* ts = gc_get_thread_state();
    if (ts) {
        root_node.next = ts->gc_root_stack;
        ts->gc_root_stack = &root_node;
    } else {
        // Fallback for unregistered threads (less safe)
        root_node.next = g_gc_root_stack;
        g_gc_root_stack = &root_node;
    }

    ActiveVm* previous_vm = g_active_vm;
    
    memset(&vm, 0, sizeof(vm));
    vm.chunk = chunk;
    vm.current_env = env;
    vm.parent = previous_vm;

    g_active_vm = &vm;
    if (ts) ts->active_vm = g_active_vm;

    while (ip < chunk->code_count) {
        BytecodeOp op = (BytecodeOp)chunk->code[ip++];

        switch (op) {
            case BC_OP_CONSTANT: {
                uint16_t index = read_u16(chunk, &ip);
                VM_CHECK_CONST(chunk, index);
                if (!vm_push(&vm, chunk->constants[index])) {
                    result = vm_error("VM stack overflow.");
                    goto done;
                }
                break;
            }
            case BC_OP_NIL:
                if (!vm_push(&vm, val_nil())) {
                    result = vm_error("VM stack overflow.");
                    goto done;
                }
                break;
            case BC_OP_TRUE:
                if (!vm_push(&vm, val_bool(1))) {
                    result = vm_error("VM stack overflow.");
                    goto done;
                }
                break;
            case BC_OP_FALSE:
                if (!vm_push(&vm, val_bool(0))) {
                    result = vm_error("VM stack overflow.");
                    goto done;
                }
                break;
            case BC_OP_POP:
                (void)vm_pop(&vm);
                break;
            case BC_OP_GET_GLOBAL: {
                int op_ip = ip;  // save ip before read_u16 advances it
                uint16_t name_index = read_u16(chunk, &ip);
                VM_CHECK_CONST(chunk, name_index);
                Value name = chunk->constants[name_index];
                Value resolved = val_nil();
                if (!env_get(vm.current_env, AS_STRING(name), (int)strlen(AS_STRING(name)), &resolved)) {
                    char _errbuf[256];
                    snprintf(_errbuf, sizeof(_errbuf), "Undefined variable '%s'", AS_STRING(name));
                    result = vm_firefly_error(chunk, op_ip, _errbuf);
                    goto done;
                }
                if (!vm_push(&vm, resolved)) {
                    result = vm_error("VM stack overflow.");
                    goto done;
                }
                break;
            }
            case BC_OP_DEFINE_GLOBAL: {
                uint16_t name_index = read_u16(chunk, &ip);
                VM_CHECK_CONST(chunk, name_index);
                Value name = chunk->constants[name_index];
                Value value = vm_pop(&vm);
                // P2: Struct value semantics — copy on define
                if (IS_INSTANCE(value) && value.as.instance->class_def->is_struct) {
                    value = instance_copy(value);
                }
                env_define(vm.current_env, AS_STRING(name), (int)strlen(AS_STRING(name)), value);
                break;
            }
            case BC_OP_SET_GLOBAL: {
                int op_ip = ip;
                uint16_t name_index = read_u16(chunk, &ip);
                VM_CHECK_CONST(chunk, name_index);
                Value name = chunk->constants[name_index];
                Value value = vm_peek(&vm, 0);
                if (!env_assign(vm.current_env, AS_STRING(name), (int)strlen(AS_STRING(name)), value)) {
                    char _errbuf[256];
                    snprintf(_errbuf, sizeof(_errbuf), "Undefined variable '%s'", AS_STRING(name));
                    result = vm_firefly_error(chunk, op_ip, _errbuf);
                    goto done;
                }
                break;
            }
            case BC_OP_DEFINE_FUNCTION: {
                uint16_t name_index = read_u16(chunk, &ip);
                uint16_t function_index = read_u16(chunk, &ip);
                VM_CHECK_CONST(chunk, name_index);

                if (chunk->program == NULL || function_index >= chunk->program->function_count) {
                    result = vm_error("Invalid compiled VM function reference.");
                    goto done;
                }

                Value name = chunk->constants[name_index];
                Value function = val_bytecode_function(&chunk->program->functions[function_index], vm.current_env);
                env_define(vm.current_env, AS_STRING(name), (int)strlen(AS_STRING(name)), function);
                break;
            }
            case BC_OP_GET_PROPERTY: {
                uint16_t name_index = read_u16(chunk, &ip);
                VM_CHECK_CONST(chunk, name_index);
                Value object = vm_pop(&vm);
                const char* property = AS_STRING(chunk->constants[name_index]);

                if (IS_INSTANCE(object)) {
                    Value field = instance_get_field(object.as.instance, property, (int)strlen(property));
                    if (!vm_push(&vm, field)) {
                        result = vm_error("VM stack overflow.");
                        goto done;
                    }
                } else if (IS_MODULE(object)) {
                    int found = 0;
                    Value attr = module_get_attr(AS_MODULE(object), property, (int)strlen(property), &found);
                    if (!found) {
                        result = vm_error("Module attribute is not defined.");
                        goto done;
                    }
                    if (!vm_push(&vm, attr)) {
                        result = vm_error("VM stack overflow.");
                        goto done;
                    }
                // P2: Dict dot-access (enums and regular dicts)
                } else if (IS_DICT(object)) {
                    Value val = dict_get(&object, property);
                    if (!vm_push(&vm, val)) {
                        result = vm_error("VM stack overflow.");
                        goto done;
                    }
                // P2: String/Array .length
                } else if (IS_STRING(object) && strcmp(property, "length") == 0) {
                    if (!vm_push(&vm, val_int((int64_t)strlen(AS_STRING(object))))) {
                        result = vm_error("VM stack overflow.");
                        goto done;
                    }
                } else if (IS_ARRAY(object) && strcmp(property, "length") == 0) {
                    if (!vm_push(&vm, val_int(object.as.array->count))) {
                        result = vm_error("VM stack overflow.");
                        goto done;
                    }
                // P13: PyObject attribute access
#ifndef SAGE_NO_PYTHON
                } else if (object.type == VAL_POINTER && object.as.pointer && object.as.pointer->type_tag == 99) {
                    extern Value sage_py_getattr_direct(Value obj, const char* name, int name_len);
                    Value attr = sage_py_getattr_direct(object, property, (int)strlen(property));
                    if (!vm_push(&vm, attr)) {
                        result = vm_error("VM stack overflow.");
                        goto done;
                    }
#endif
                } else {
                    result = vm_error("Only instances and modules have properties.");
                    goto done;
                }
                break;
            }
            case BC_OP_SET_PROPERTY: {
                uint16_t name_index = read_u16(chunk, &ip);
                VM_CHECK_CONST(chunk, name_index);
                Value value = vm_pop(&vm);
                Value object = vm_pop(&vm);
                const char* property = AS_STRING(chunk->constants[name_index]);

                if (!IS_INSTANCE(object)) {
                    result = vm_error("Only instances have properties.");
                    goto done;
                }

                instance_set_field(object.as.instance, property, (int)strlen(property), value);
                if (!vm_push(&vm, value)) {
                    result = vm_error("VM stack overflow.");
                    goto done;
                }
                break;
            }
            case BC_OP_GET_INDEX: {
                Value index = vm_pop(&vm);
                Value object = vm_pop(&vm);

                if (object.type == VAL_ARRAY && IS_NUMERIC(index)) {
                    int idx = (int)NUMERIC_AS_INT(index);
                    if (idx < 0) idx += object.as.array->count;  // P6: negative indexing
                    if (!vm_push(&vm, array_get(&object, idx))) {
                        result = vm_error("VM stack overflow.");
                        goto done;
                    }
                } else if (object.type == VAL_TUPLE && IS_NUMERIC(index)) {
                    int idx = (int)NUMERIC_AS_INT(index);
                    if (idx < 0) idx += object.as.tuple->count;  // P6: negative indexing
                    if (!vm_push(&vm, tuple_get(&object, (int)NUMERIC_AS_INT(index)))) {
                        result = vm_error("VM stack overflow.");
                        goto done;
                    }
                } else if (object.type == VAL_STRING && IS_NUMERIC(index)) {
                    int string_index = (int)NUMERIC_AS_INT(index);
                    char* string = AS_STRING(object);
                    int string_length = (int)strlen(string);
                    if (string_index < 0) string_index += string_length;
                    if (string_index < 0 || string_index >= string_length) {
                        result = vm_error("String index out of bounds.");
                        goto done;
                    }
                    char* character = SAGE_ALLOC(2);
                    character[0] = string[string_index];
                    character[1] = '\0';
                    if (!vm_push(&vm, val_string_take(character))) {
                        result = vm_error("VM stack overflow.");
                        goto done;
                    }
                } else if (object.type == VAL_DICT && IS_STRING(index)) {
                    if (!vm_push(&vm, dict_get(&object, AS_STRING(index)))) {
                        result = vm_error("VM stack overflow.");
                        goto done;
                    }
                } else {
                    result = vm_error("Invalid indexing operation.");
                    goto done;
                }
                break;
            }
            case BC_OP_SET_INDEX: {
                Value value = vm_pop(&vm);
                Value index = vm_pop(&vm);
                Value object = vm_pop(&vm);

                if (object.type == VAL_ARRAY && IS_NUMERIC(index)) {
                    array_set(&object, (int)NUMERIC_AS_INT(index), value);
                } else if (object.type == VAL_DICT && IS_STRING(index)) {
                    dict_set(&object, AS_STRING(index), value);
                } else {
                    result = vm_error("Invalid index assignment.");
                    goto done;
                }

                if (!vm_push(&vm, value)) {
                    result = vm_error("VM stack overflow.");
                    goto done;
                }
                break;
            }
            case BC_OP_SLICE: {
                Value end = vm_pop(&vm);
                Value start = vm_pop(&vm);
                Value object = vm_pop(&vm);

                int start_index = 0;
                int end_index = 0;

                if (IS_ARRAY(object)) {
                    end_index = object.as.array->count;
                } else if (IS_STRING(object)) {
                    end_index = (int)strlen(AS_STRING(object));
                } else {
                    result = vm_error("Can only slice arrays or strings.");
                    goto done;
                }

                if (!IS_NIL(start)) {
                    if (!IS_NUMERIC(start)) {
                        result = vm_error("Slice start must be a number.");
                        goto done;
                    }
                    start_index = (int)NUMERIC_AS_INT(start);
                }
                if (!IS_NIL(end)) {
                    if (!IS_NUMERIC(end)) {
                        result = vm_error("Slice end must be a number.");
                        goto done;
                    }
                    end_index = (int)NUMERIC_AS_INT(end);
                }

                if (IS_ARRAY(object)) {
                    if (!vm_push(&vm, array_slice(&object, start_index, end_index))) {
                        result = vm_error("VM stack overflow.");
                        goto done;
                    }
                } else {
                    char* string = AS_STRING(object);
                    int string_length = (int)strlen(string);
                    if (start_index < 0) start_index += string_length;
                    if (end_index < 0) end_index += string_length;
                    if (start_index < 0) start_index = 0;
                    if (end_index > string_length) end_index = string_length;
                    if (start_index >= end_index) {
                        if (!vm_push(&vm, val_string(""))) {
                            result = vm_error("VM stack overflow.");
                            goto done;
                        }
                    } else {
                        int length = end_index - start_index;
                        char* slice = SAGE_ALLOC((size_t)length + 1);
                        memcpy(slice, string + start_index, (size_t)length);
                        slice[length] = '\0';
                        if (!vm_push(&vm, val_string_take(slice))) {
                            result = vm_error("VM stack overflow.");
                            goto done;
                        }
                    }
                }
                break;
            }
            case BC_OP_ADD:
            case BC_OP_SUB:
            case BC_OP_MUL:
            case BC_OP_DIV:
            case BC_OP_MOD:
            case BC_OP_EQUAL:
            case BC_OP_NOT_EQUAL:
            case BC_OP_GREATER:
            case BC_OP_GREATER_EQUAL:
            case BC_OP_LESS:
            case BC_OP_LESS_EQUAL:
            case BC_OP_BIT_AND:
            case BC_OP_BIT_OR:
            case BC_OP_BIT_XOR:
            case BC_OP_SHIFT_LEFT:
            case BC_OP_SHIFT_RIGHT: {
                Value right = vm_pop(&vm);
                Value left = vm_pop(&vm);
                Value out = val_nil();

                if (op == BC_OP_EQUAL || op == BC_OP_NOT_EQUAL) {
                    int equal = values_equal(left, right);
                    out = val_bool(op == BC_OP_EQUAL ? equal : !equal);
                } else if (op == BC_OP_GREATER || op == BC_OP_GREATER_EQUAL ||
                           op == BC_OP_LESS || op == BC_OP_LESS_EQUAL) {
                    if (IS_NUMERIC(left) && IS_NUMERIC(right)) {
                        if (IS_INT(left) && IS_INT(right)) {
                            int64_t l = AS_INT(left), r = AS_INT(right);
                            if (op == BC_OP_GREATER) out = val_bool(l > r);
                            else if (op == BC_OP_GREATER_EQUAL) out = val_bool(l >= r);
                            else if (op == BC_OP_LESS) out = val_bool(l < r);
                            else out = val_bool(l <= r);
                        } else {
                            double l = NUMERIC_AS_DOUBLE(left), r = NUMERIC_AS_DOUBLE(right);
                            if (op == BC_OP_GREATER) out = val_bool(l > r);
                            else if (op == BC_OP_GREATER_EQUAL) out = val_bool(l >= r);
                            else if (op == BC_OP_LESS) out = val_bool(l < r);
                            else out = val_bool(l <= r);
                        }
                    } else if (IS_STRING(left) && IS_STRING(right)) {
                        int cmp = strcmp(AS_STRING(left), AS_STRING(right));
                        if (op == BC_OP_GREATER) out = val_bool(cmp > 0);
                        else if (op == BC_OP_GREATER_EQUAL) out = val_bool(cmp >= 0);
                        else if (op == BC_OP_LESS) out = val_bool(cmp < 0);
                        else out = val_bool(cmp <= 0);
                    } else {
                        result = vm_error("Operands must be numbers or strings.");
                        goto done;
                    }
                } else if (op == BC_OP_ADD && IS_STRING(left) && IS_STRING(right)) {
                    size_t len1 = strlen(AS_STRING(left));
                    size_t len2 = strlen(AS_STRING(right));
                    char* joined = SAGE_ALLOC(len1 + len2 + 1);
                    memcpy(joined, AS_STRING(left), len1);
                    memcpy(joined + len1, AS_STRING(right), len2 + 1);
                    out = val_string_take(joined);
                } else if (left.type == VAL_INSTANCE || right.type == VAL_INSTANCE) {
                    // Operator overloading via dunder methods in MossVM
                    const char* dunder = NULL;
                    int dlen = 0;
                    if      (op == BC_OP_ADD) { dunder = "__add__"; dlen = 7; }
                    else if (op == BC_OP_SUB) { dunder = "__sub__"; dlen = 7; }
                    else if (op == BC_OP_MUL) { dunder = "__mul__"; dlen = 7; }
                    else if (op == BC_OP_DIV) { dunder = "__div__"; dlen = 7; }
                    else if (op == BC_OP_MOD) { dunder = "__mod__"; dlen = 7; }
                    if (dunder && left.type == VAL_INSTANCE && left.as.instance->class_def) {
                        Method* m = class_find_method(left.as.instance->class_def, dunder, dlen);
                        if (m) {
                            Stmt* mnode = (Stmt*)m->method_stmt;
                            ProcStmt* mproc = (mnode->type == STMT_ASYNC_PROC) ?
                                &mnode->as.async_proc : &mnode->as.proc;
                            Env* menv = env_create(
                                left.as.instance->class_def->defining_env
                                ? left.as.instance->class_def->defining_env : vm.current_env);
                            env_define(menv, "self", 4, left);
                            int ps = (mproc->param_count > 0 &&
                                      strncmp(mproc->params[0].start,"self",4)==0) ? 1 : 0;
                            if (ps < mproc->param_count)
                                env_define(menv, mproc->params[ps].start,
                                           mproc->params[ps].length, right);
                            ExecResult dr = interpret(mproc->body, menv);
                            if (dr.is_throwing) { result = vm_error("Operator error."); goto done; }
                            out = dr.value;
                        } else {
                            result = vm_error("Operands must be numbers or strings.");
                            goto done;
                        }
                    } else {
                        result = vm_error("Operands must be numbers or strings.");
                        goto done;
                    }
                } else if (IS_NUMERIC(left) && IS_NUMERIC(right)) {
                    switch (op) {
                        case BC_OP_ADD: out = sage_add(left, right); break;
                        case BC_OP_SUB: out = sage_sub(left, right); break;
                        case BC_OP_MUL: out = sage_mul(left, right); break;
                        case BC_OP_DIV: out = sage_div(left, right); break;
                        case BC_OP_MOD: out = sage_mod(left, right); break;
                        case BC_OP_BIT_AND: out = val_int(NUMERIC_AS_INT(left) & NUMERIC_AS_INT(right)); break;
                        case BC_OP_BIT_OR: out = val_int(NUMERIC_AS_INT(left) | NUMERIC_AS_INT(right)); break;
                        case BC_OP_BIT_XOR: out = val_int(NUMERIC_AS_INT(left) ^ NUMERIC_AS_INT(right)); break;
                        case BC_OP_SHIFT_LEFT: out = val_int(NUMERIC_AS_INT(left) << NUMERIC_AS_INT(right)); break;
                        case BC_OP_SHIFT_RIGHT: out = val_int(NUMERIC_AS_INT(left) >> NUMERIC_AS_INT(right)); break;
                        default: break;
                    }
                // P6: String repetition in VM: "ha" * 3 or 3 * "ha"
                } else if (op == BC_OP_MUL &&
                           ((IS_STRING(left) && IS_NUMERIC(right)) ||
                            (IS_NUMERIC(left) && IS_STRING(right)))) {
                    const char* s = IS_STRING(left) ? AS_STRING(left) : AS_STRING(right);
                    int rep = IS_NUMERIC(left) ? (int)NUMERIC_AS_INT(left) : (int)NUMERIC_AS_INT(right);
                    if (rep <= 0) { out = val_string(""); }
                    else {
                        size_t slen = strlen(s);
                        char* buf = SAGE_ALLOC(slen * (size_t)rep + 1);
                        char* p = buf;
                        for (int ri = 0; ri < rep; ri++) { memcpy(p, s, slen); p += slen; }
                        *p = '\0';
                        out = val_string_take(buf);
                    }
                } else {
                    result = vm_error("Operands must be numbers or strings.");
                    goto done;
                }

                if (!vm_push(&vm, out)) {
                    result = vm_error("VM stack overflow.");
                    goto done;
                }
                break;
            }
            case BC_OP_NEGATE: {
                Value value = vm_pop(&vm);
                if (!IS_NUMERIC(value)) {
                    result = vm_error("Unary '-' requires a number.");
                    goto done;
                }
                if (!vm_push(&vm, sage_negate(value))) {
                    result = vm_error("VM stack overflow.");
                    goto done;
                }
                break;
            }
            case BC_OP_BIT_NOT: {
                Value value = vm_pop(&vm);
                if (!IS_NUMERIC(value)) {
                    result = vm_error("Bitwise NOT operand must be a number.");
                    goto done;
                }
                if (!vm_push(&vm, val_int(~NUMERIC_AS_INT(value)))) {
                    result = vm_error("VM stack overflow.");
                    goto done;
                }
                break;
            }
            case BC_OP_NOT: {
                Value value = vm_pop(&vm);
                if (!vm_push(&vm, val_bool(!vm_is_truthy(value)))) {
                    result = vm_error("VM stack overflow.");
                    goto done;
                }
                break;
            }
            case BC_OP_TRUTHY: {
                Value value = vm_pop(&vm);
                if (!vm_push(&vm, val_bool(vm_is_truthy(value)))) {
                    result = vm_error("VM stack overflow.");
                    goto done;
                }
                break;
            }
            case BC_OP_JUMP:
                ip = (int)read_u16(chunk, &ip);
                break;
            case BC_OP_JUMP_IF_FALSE: {
                uint16_t target = read_u16(chunk, &ip);
                if (!vm_is_truthy(vm_peek(&vm, 0))) {
                    ip = (int)target;
                }
                break;
            }
            case BC_OP_CALL: {
                int arg_count = (int)read_u8(chunk, &ip);
                if (vm.stack_count < arg_count + 1) {
                    result = vm_error("VM stack underflow on call.");
                    goto done;
                }
                
                // Keep values on stack during call so they are marked by GC
                Value callee = vm.stack[vm.stack_count - 1 - arg_count];
                Value* args = &vm.stack[vm.stack_count - arg_count];

                ExecResult call_result = call_function_value(callee, arg_count, args, vm.current_env);
                
                // Pop callee and args
                vm.stack_count -= (arg_count + 1);

                if (call_result.is_throwing) {
                    result = call_result;
                    goto done;
                }
                if (!vm_push(&vm, call_result.value)) {
                    result = vm_error("VM stack overflow.");
                    goto done;
                }
                break;
            }
            case BC_OP_CALL_METHOD: {
                uint16_t name_index = read_u16(chunk, &ip);
                VM_CHECK_CONST(chunk, name_index);
                int arg_count = (int)read_u8(chunk, &ip);
                if (vm.stack_count < arg_count + 1) {
                    result = vm_error("VM stack underflow on method call.");
                    goto done;
                }

                // Keep values on stack during call so they are marked by GC
                Value object = vm.stack[vm.stack_count - 1 - arg_count];
                Value* args = &vm.stack[vm.stack_count - arg_count];

                ExecResult call_result = call_method_value(object, AS_STRING(chunk->constants[name_index]), arg_count, args, vm.current_env);
                
                // Pop object and args
                vm.stack_count -= (arg_count + 1);

                if (call_result.is_throwing) {
                    result = call_result;
                    goto done;
                }
                if (!vm_push(&vm, call_result.value)) {
                    result = vm_error("VM stack overflow.");
                    goto done;
                }
                break;
            }
            case BC_OP_ARRAY: {
                uint16_t count = read_u16(chunk, &ip);
                Value array = val_array();
                
                // Add values to array without popping them first
                // to ensure they are marked by GC if array_push triggers it.
                for (int i = 0; i < (int)count; i++) {
                    array_push(&array, vm.stack[vm.stack_count - (int)count + i]);
                }
                
                // Now pop them
                vm.stack_count -= (int)count;

                if (!vm_push(&vm, array)) {
                    result = vm_error("VM stack overflow.");
                    goto done;
                }
                break;
            }
            case BC_OP_TUPLE: {
                uint16_t count = read_u16(chunk, &ip);
                // Use values directly from stack
                Value tuple = val_tuple(&vm.stack[vm.stack_count - (int)count], (int)count);
                
                // Now pop them
                vm.stack_count -= (int)count;

                if (!vm_push(&vm, tuple)) {
                    result = vm_error("VM stack overflow.");
                    goto done;
                }
                break;
            }
            case BC_OP_DICT: {
                uint16_t count = read_u16(chunk, &ip);
                Value dictionary = val_dict();
                Value* values = SAGE_ALLOC(sizeof(Value) * (size_t)count * 2);
                for (int i = ((int)count * 2) - 1; i >= 0; i--) {
                    values[i] = vm_pop(&vm);
                }
                for (int i = 0; i < (int)count; i++) {
                    Value key = values[i * 2];
                    Value value = values[i * 2 + 1];
                    if (!IS_STRING(key)) {
                        result = vm_error("Dictionary keys must be strings in bytecode mode.");
                        free(values);
                        goto done;
                    }
                    dict_set(&dictionary, AS_STRING(key), value);
                }
                free(values);
                if (!vm_push(&vm, dictionary)) {
                    result = vm_error("VM stack overflow.");
                    goto done;
                }
                break;
            }
            case BC_OP_PRINT: {
                Value value = vm_pop(&vm);
                // Call __str__ if the instance defines it
                if (value.type == VAL_INSTANCE && value.as.instance &&
                        value.as.instance->class_def) {
                    Method* str_m = class_find_method(value.as.instance->class_def, "__str__", 7);
                    if (str_m) {
                        Stmt* mnode = (Stmt*)str_m->method_stmt;
                        ProcStmt* mproc = (mnode->type == STMT_ASYNC_PROC)
                            ? &mnode->as.async_proc : &mnode->as.proc;
                        Env* def_env = value.as.instance->class_def->defining_env;
                        Env* str_env = env_create(def_env ? def_env : vm.current_env);
                        env_define(str_env, "self", 4, value);
                        ExecResult sr = interpret(mproc->body, str_env);
                        if (!sr.is_throwing && IS_STRING(sr.value)) {
                            printf("%s\n", AS_STRING(sr.value));
                        } else {
                            print_value(value); printf("\n");
                        }
                        break;
                    }
                }
                print_value(value);
                printf("\n");
                break;
            }
            case BC_OP_PUSH_ENV:
                vm.current_env = env_create(vm.current_env);
                break;
            case BC_OP_POP_ENV:
                if (vm.current_env == NULL || vm.current_env->parent == NULL) {
                    result = vm_error("Cannot pop the root VM scope.");
                    goto done;
                }
                vm.current_env = vm.current_env->parent;
                break;
            case BC_OP_DUP: {
                uint8_t distance = read_u8(chunk, &ip);
                if ((int)distance >= vm.stack_count) {
                    result = vm_error("Invalid VM stack duplicate.");
                    goto done;
                }
                if (!vm_push(&vm, vm_peek(&vm, (int)distance))) {
                    result = vm_error("VM stack overflow.");
                    goto done;
                }
                break;
            }
            case BC_OP_ARRAY_LEN: {
                Value value = vm_pop(&vm);
                if (!IS_ARRAY(value)) {
                    result = vm_error("for loop iterable must be an array.");
                    goto done;
                }
                if (!vm_push(&vm, val_number((double)value.as.array->count))) {
                    result = vm_error("VM stack overflow.");
                    goto done;
                }
                break;
            }
            case BC_OP_EXEC_AST_STMT: {
                uint16_t stmt_index = read_u16(chunk, &ip);
                VM_CHECK_AST(chunk, stmt_index);
                ExecResult ast_result = interpret(chunk->ast_stmts[stmt_index], vm.current_env);
                if (ast_result.is_throwing) {
                    result = ast_result;
                    goto done;
                }
                if (!vm_push(&vm, ast_result.value)) {
                    result = vm_error("VM stack overflow.");
                    goto done;
                }
                break;
            }
            // ================================================================
            case BC_OP_AWAIT: {
                Value v = vm_pop(&vm);
                if (v.type == VAL_THREAD) {
                    ThreadValue* tv = v.as.thread;
                    if (!tv->joined) {
                        sage_thread_t* handle = (sage_thread_t*)tv->handle;
                        sage_thread_join(*handle, NULL);
                        tv->joined = 1;
                    }
                    typedef struct { FunctionValue* func; int arg_count; Value* args; Value result; } SageThreadData;
                    SageThreadData* td = (SageThreadData*)tv->data;
                    if (!vm_push(&vm, td->result)) { result = vm_error("Stack overflow."); goto done; }
                } else {
                    // Not a thread — just push the value back (already resolved)
                    if (!vm_push(&vm, v)) { result = vm_error("Stack overflow."); goto done; }
                }
                break;
            }

            case BC_OP_BREAK:
            case BC_OP_CONTINUE:
            case BC_OP_LOOP_BACK:
                result = vm_error("Unexpected break/continue/loop opcode at runtime.");
                goto done;

            // Import, class, try/raise — these are handled via AST fallback
            // in hybrid mode. If they appear as raw opcodes, it's an error.
            case BC_OP_IMPORT:
            case BC_OP_CLASS:
            case BC_OP_METHOD:
            case BC_OP_INHERIT:
            case BC_OP_SETUP_TRY:
            case BC_OP_END_TRY:
            case BC_OP_RAISE:
                result = vm_error("Unimplemented bytecode opcode.");
                goto done;

            case BC_OP_RETURN:
                result = vm_normal(vm.stack_count > 0 ? vm_pop(&vm) : val_nil());
                goto done;
        }
    }

done:
    g_active_vm = previous_vm;
    if (ts) {
        ts->active_vm = g_active_vm;
        ts->gc_root_stack = root_node.next;
    } else {
        g_gc_root_stack = root_node.next;
    }
    return result;
}

ExecResult vm_execute_program(BytecodeProgram* program, Env* env) {
    ExecResult result = vm_normal(val_nil());
    if (program == NULL) {
        return result;
    }

    for (int i = 0; i < program->chunk_count; i++) {
        result = vm_execute_chunk(&program->chunks[i], env);
        if (result.is_throwing) {
            return result;
        }
    }

    return result;
}
