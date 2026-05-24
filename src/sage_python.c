// sage_python.c — Python FFI: embed CPython into SageTree
// Allows calling Python libraries from Sage:
//   import python
//   let np = python.import("numpy")
//   let arr = np.array([1, 2, 3])
//   println(np.mean(arr))

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "value.h"
#include "gc.h"
#include "interpreter.h"
#include "lilybox.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

// ── Initialization ────────────────────────────────────────────────────────────
static int py_initialized = 0;

static void sage_py_ensure_init(void) {
    if (!py_initialized) {
        Py_Initialize();
        py_initialized = 1;
    }
}

// ── Sage → Python value conversion ───────────────────────────────────────────
PyObject* sage_to_pyobject(Value val) {
    switch (val.type) {
        case VAL_INT:    return PyLong_FromLongLong(AS_INT(val));
        case VAL_NUMBER: return PyFloat_FromDouble(AS_NUMBER(val));
        case VAL_BOOL:   return AS_BOOL(val) ? Py_True : Py_False;
        case VAL_NIL:    Py_RETURN_NONE;
        case VAL_STRING: return PyUnicode_FromString(AS_STRING(val));
        case VAL_ARRAY: {
            ArrayValue* arr = val.as.array;
            PyObject* list = PyList_New(arr->count);
            for (int i = 0; i < arr->count; i++) {
                PyList_SetItem(list, i, sage_to_pyobject(arr->elements[i]));
            }
            return list;
        }
        case VAL_DICT: {
            PyObject* dict = PyDict_New();
            DictValue* d = val.as.dict;
            for (int i = 0; i < d->capacity; i++) {
                if (d->entries[i].key != NULL) {
                    PyObject* key = PyUnicode_FromString(d->entries[i].key);
                    PyObject* pval = sage_to_pyobject(d->entries[i].value);
                    PyDict_SetItem(dict, key, pval);
                    Py_DECREF(key);
                    Py_DECREF(pval);
                }
            }
            return dict;
        }
        case VAL_POINTER: {
            // P15: If this is a wrapped PyObject, unwrap it
            if (val.as.pointer && val.as.pointer->type_tag == 99 && val.as.pointer->ptr) {
                PyObject* obj = (PyObject*)val.as.pointer->ptr;
                Py_INCREF(obj);
                return obj;
            }
            Py_RETURN_NONE;
        }
        default: Py_RETURN_NONE;
    }
}

// ── Python → Sage value conversion ───────────────────────────────────────────
Value pyobject_to_sage(PyObject* obj) {
    if (obj == NULL || obj == Py_None) return val_nil();
    
    if (PyBool_Check(obj)) return val_bool(obj == Py_True);
    if (PyLong_Check(obj)) {
        long long v = PyLong_AsLongLong(obj);
        if (v == -1 && PyErr_Occurred()) { PyErr_Clear(); return val_nil(); }
        return val_int((int64_t)v);
    }
    if (PyFloat_Check(obj)) return val_number(PyFloat_AsDouble(obj));
    if (PyUnicode_Check(obj)) {
        const char* s = PyUnicode_AsUTF8(obj);
        return s ? val_string(s) : val_nil();
    }
    if (PyList_Check(obj)) {
        Py_ssize_t len = PyList_Size(obj);
        Value arr = val_array();
        for (Py_ssize_t i = 0; i < len; i++) {
            array_push(&arr, pyobject_to_sage(PyList_GetItem(obj, i)));
        }
        return arr;
    }
    if (PyTuple_Check(obj)) {
        Py_ssize_t len = PyTuple_Size(obj);
        Value arr = val_array();
        for (Py_ssize_t i = 0; i < len; i++) {
            array_push(&arr, pyobject_to_sage(PyTuple_GetItem(obj, i)));
        }
        return arr;
    }
    if (PyDict_Check(obj)) {
        Value dict = val_dict();
        PyObject *key, *pval;
        Py_ssize_t pos = 0;
        while (PyDict_Next(obj, &pos, &key, &pval)) {
            const char* k = PyUnicode_AsUTF8(key);
            if (k) dict_set(&dict, k, pyobject_to_sage(pval));
        }
        return dict;
    }
    
    // P15: Try to convert sequence-like objects (numpy arrays, etc.) to Sage arrays
    if (PySequence_Check(obj) && !PyBytes_Check(obj) && !PyByteArray_Check(obj)) {
        Py_ssize_t len = PySequence_Size(obj);
        if (len >= 0 && len < 1000000) {  // sanity limit
            Value arr = val_array();
            for (Py_ssize_t i = 0; i < len; i++) {
                PyObject* item = PySequence_GetItem(obj, i);
                if (item) {
                    array_push(&arr, pyobject_to_sage(item));
                    Py_DECREF(item);
                }
            }
            return arr;
        }
        PyErr_Clear();
    }
    
    // P15: Try to convert numeric-like objects (numpy.int64, numpy.float64)
    if (PyNumber_Check(obj)) {
        // Try int first
        PyObject* as_int = PyNumber_Long(obj);
        if (as_int) {
            long long v = PyLong_AsLongLong(as_int);
            Py_DECREF(as_int);
            if (!(v == -1 && PyErr_Occurred())) {
                // Check if the original was float-like (has decimal part)
                PyObject* as_float = PyNumber_Float(obj);
                if (as_float) {
                    double fv = PyFloat_AsDouble(as_float);
                    Py_DECREF(as_float);
                    if (fv != (double)v) return val_number(fv);
                }
                return val_int((int64_t)v);
            }
            PyErr_Clear();
        } else {
            PyErr_Clear();
        }
        // Try float
        PyObject* as_float = PyNumber_Float(obj);
        if (as_float) {
            double fv = PyFloat_AsDouble(as_float);
            Py_DECREF(as_float);
            return val_number(fv);
        }
        PyErr_Clear();
    }
    
    // For arbitrary Python objects, wrap as a pointer
    // The pointer type stores the PyObject* and prevents GC
    Py_INCREF(obj);
    Value result;
    result.type = VAL_POINTER;
    PointerValue* pv = SAGE_ALLOC(sizeof(PointerValue));
    pv->ptr = obj;
    pv->size = 0;
    pv->type_tag = 99;  // Magic tag for Python objects
    result.as.pointer = pv;
    return result;
}

// ── Check if a Value wraps a PyObject ────────────────────────────────────────
int is_pyobject(Value val) {
    return val.type == VAL_POINTER && val.as.pointer && val.as.pointer->type_tag == 99;
}

PyObject* get_pyobject(Value val) {
    if (is_pyobject(val)) return (PyObject*)val.as.pointer->ptr;
    return NULL;
}

// ── python.import("module_name") ─────────────────────────────────────────────
static Value py_import_native(int argc, Value* args) {
    if (argc < 1 || !IS_STRING(args[0])) {
        fprintf(stderr, "python.import(): expected string module name\n");
        return val_nil();
    }
    // P8: LilyKnight FFI check
    if (lk_current_sandbox) {
        LKViolation v = lk_check_ffi(lk_current_sandbox, AS_STRING(args[0]));
        if (v != LK_OK) {
            lk_log_violation(lk_current_sandbox, v, AS_STRING(args[0]));
            fprintf(stderr, "SandboxError: Python import '%s' denied by LilyKnight\n", AS_STRING(args[0]));
            return val_nil();
        }
    }
    sage_py_ensure_init();
    
    PyObject* mod = PyImport_ImportModule(AS_STRING(args[0]));
    if (!mod) {
        PyErr_Print();
        fprintf(stderr, "python.import(): failed to import '%s'\n", AS_STRING(args[0]));
        return val_nil();
    }
    return pyobject_to_sage(mod);
}

// ── python.call(obj, method_name, args...) ───────────────────────────────────
static Value py_call_native(int argc, Value* args) {
    if (argc < 2) {
        fprintf(stderr, "python.call(): expected (object, method_name, ...args)\n");
        return val_nil();
    }
    sage_py_ensure_init();
    
    PyObject* obj = is_pyobject(args[0]) ? get_pyobject(args[0]) : sage_to_pyobject(args[0]);
    if (!obj) return val_nil();
    
    const char* method = IS_STRING(args[1]) ? AS_STRING(args[1]) : NULL;
    if (!method) return val_nil();
    
    // Build args tuple
    int nargs = argc - 2;
    PyObject* py_args = PyTuple_New(nargs);
    for (int i = 0; i < nargs; i++) {
        PyTuple_SetItem(py_args, i, sage_to_pyobject(args[i + 2]));
    }
    
    PyObject* method_obj = PyObject_GetAttrString(obj, method);
    if (!method_obj) {
        PyErr_Print();
        Py_DECREF(py_args);
        if (!is_pyobject(args[0])) Py_DECREF(obj);
        return val_nil();
    }
    
    PyObject* result = PyObject_CallObject(method_obj, py_args);
    Py_DECREF(method_obj);
    Py_DECREF(py_args);
    if (!is_pyobject(args[0])) Py_DECREF(obj);
    
    if (!result) {
        PyErr_Print();
        return val_nil();
    }
    
    Value sage_result = pyobject_to_sage(result);
    Py_DECREF(result);
    return sage_result;
}

// ── python.eval("expression") ────────────────────────────────────────────────
static Value py_eval_native(int argc, Value* args) {
    if (argc < 1 || !IS_STRING(args[0])) return val_nil();
    sage_py_ensure_init();
    
    PyObject* main_mod = PyImport_AddModule("__main__");
    PyObject* globals = PyModule_GetDict(main_mod);
    PyObject* result = PyRun_String(AS_STRING(args[0]), Py_eval_input, globals, globals);
    if (!result) {
        PyErr_Print();
        return val_nil();
    }
    Value sage_result = pyobject_to_sage(result);
    Py_DECREF(result);
    return sage_result;
}

// ── python.exec("statement") ─────────────────────────────────────────────────
static Value py_exec_native(int argc, Value* args) {
    if (argc < 1 || !IS_STRING(args[0])) return val_nil();
    sage_py_ensure_init();
    
    PyObject* main_mod = PyImport_AddModule("__main__");
    PyObject* globals = PyModule_GetDict(main_mod);
    int ok = PyRun_SimpleString(AS_STRING(args[0]));
    (void)globals;
    return val_bool(ok == 0);
}

// ── python.getattr(obj, name) ────────────────────────────────────────────────
static Value py_getattr_native(int argc, Value* args) {
    if (argc < 2 || !IS_STRING(args[1])) return val_nil();
    sage_py_ensure_init();
    
    PyObject* obj = is_pyobject(args[0]) ? get_pyobject(args[0]) : sage_to_pyobject(args[0]);
    if (!obj) return val_nil();
    
    PyObject* attr = PyObject_GetAttrString(obj, AS_STRING(args[1]));
    if (!attr) {
        PyErr_Clear();
        if (!is_pyobject(args[0])) Py_DECREF(obj);
        return val_nil();
    }
    
    // If it's callable (a method/function), return it as a PyObject wrapper
    // If it's a value, convert to Sage
    Value result;
    if (PyCallable_Check(attr)) {
        result = pyobject_to_sage(attr);  // Wraps as VAL_POINTER
    } else {
        result = pyobject_to_sage(attr);
        Py_DECREF(attr);
    }
    if (!is_pyobject(args[0])) Py_DECREF(obj);
    return result;
}

// ── python.invoke(callable, ...args) ─────────────────────────────────────────
static Value py_invoke_native(int argc, Value* args) {
    if (argc < 1) return val_nil();
    sage_py_ensure_init();
    
    PyObject* callable = is_pyobject(args[0]) ? get_pyobject(args[0]) : sage_to_pyobject(args[0]);
    if (!callable || !PyCallable_Check(callable)) {
        if (!is_pyobject(args[0]) && callable) Py_DECREF(callable);
        return val_nil();
    }
    
    int nargs = argc - 1;
    PyObject* py_args = PyTuple_New(nargs);
    for (int i = 0; i < nargs; i++) {
        PyTuple_SetItem(py_args, i, sage_to_pyobject(args[i + 1]));
    }
    
    PyObject* result = PyObject_CallObject(callable, py_args);
    Py_DECREF(py_args);
    if (!is_pyobject(args[0])) Py_DECREF(callable);
    
    if (!result) {
        PyErr_Print();
        return val_nil();
    }
    Value sage_result = pyobject_to_sage(result);
    Py_DECREF(result);
    return sage_result;
}

// ── Module registration ──────────────────────────────────────────────────────
#include "module.h"

Module* create_python_module(ModuleCache* cache) {
    Module* m = create_native_module(cache, "python");
    Environment* e = m->env;
    
    env_define_const(e, "import",  6, val_native(py_import_native));
    env_define_const(e, "call",    4, val_native(py_call_native));
    env_define_const(e, "eval",    4, val_native(py_eval_native));
    env_define_const(e, "exec",    4, val_native(py_exec_native));
    env_define_const(e, "getattr", 7, val_native(py_getattr_native));
    env_define_const(e, "invoke",  6, val_native(py_invoke_native));
    
    return m;
}

// ── Cleanup ──────────────────────────────────────────────────────────────────
void sage_python_shutdown(void) {
    if (py_initialized) {
        Py_Finalize();
        py_initialized = 0;
    }
}

// ── P13: Direct dispatch helpers (called from interpreter.c) ─────────────────

Value sage_py_getattr_direct(Value obj, const char* name, int name_len) {
    if (!is_pyobject(obj)) return val_nil();
    sage_py_ensure_init();
    
    char attr_name[256];
    int alen = name_len < 255 ? name_len : 255;
    memcpy(attr_name, name, alen);
    attr_name[alen] = '\0';
    
    PyObject* pyobj = get_pyobject(obj);
    if (!pyobj) return val_nil();
    
    PyObject* attr = PyObject_GetAttrString(pyobj, attr_name);
    if (!attr) { PyErr_Clear(); return val_nil(); }
    
    Value result = pyobject_to_sage(attr);
    // Don't DECREF callable attrs — they're wrapped as PyObject pointers
    if (!PyCallable_Check(attr)) Py_DECREF(attr);
    return result;
}

Value sage_py_call_direct(Value callable, int argc, Value* args) {
    if (!is_pyobject(callable)) return val_nil();
    sage_py_ensure_init();
    
    PyObject* py_callable = get_pyobject(callable);
    if (!py_callable || !PyCallable_Check(py_callable)) return val_nil();
    
    PyObject* py_args = PyTuple_New(argc);
    for (int i = 0; i < argc; i++) {
        PyTuple_SetItem(py_args, i, sage_to_pyobject(args[i]));
    }
    
    PyObject* result = PyObject_CallObject(py_callable, py_args);
    Py_DECREF(py_args);
    
    if (!result) {
        PyErr_Print();
        return val_nil();
    }
    Value sage_result = pyobject_to_sage(result);
    Py_DECREF(result);
    return sage_result;
}
