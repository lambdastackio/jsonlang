/*
Copyright 2015 Google Inc. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <stdlib.h>
#include <stdio.h>

#include <Python.h>

#include "libjsonlang.h"

static char *jsonlang_str(struct JsonlangVm *vm, const char *str)
{
    char *out = jsonlang_realloc(vm, NULL, strlen(str) + 1);
    memcpy(out, str, strlen(str) + 1);
    return out;
}

static const char *exc_to_str(void)
{
    PyObject *ptype, *pvalue, *ptraceback;
    PyErr_Fetch(&ptype, &pvalue, &ptraceback);
    PyObject *exc_str = PyObject_Str(pvalue);
    return PyString_AsString(exc_str);
}

struct NativeCtx {
    struct JsonlangVm *vm;
    PyObject *callback;
    size_t argc;
};

static struct JsonlangJsonValue *python_to_jsonlang_json(struct JsonlangVm *vm, PyObject *v,
                                                       const char **err_msg)
{
    if (PyString_Check(v)) {
        return jsonlang_json_make_string(vm, PyString_AsString(v));
    } else if (PyUnicode_Check(v)) {
        struct JsonlangJsonValue *r;
        PyObject *str = PyUnicode_AsUTF8String(v);
        r = jsonlang_json_make_string(vm, PyString_AsString(str));
        Py_DECREF(str);
        return r;
    } else if (PyBool_Check(v)) {
        return jsonlang_json_make_bool(vm, PyObject_IsTrue(v));
    } else if (PyFloat_Check(v)) {
        return jsonlang_json_make_number(vm, PyFloat_AsDouble(v));
    } else if (PyInt_Check(v)) {
        return jsonlang_json_make_number(vm, (double)(PyInt_AsLong(v)));
    } else if (v == Py_None) {
        return jsonlang_json_make_null(vm);
    } else if (PySequence_Check(v)) {
        Py_ssize_t len, i;
        struct JsonlangJsonValue *arr;
        // Convert it to a O(1) indexable form if necessary.
        PyObject *fast = PySequence_Fast(v, "python_to_jsonlang_json internal error: not sequence");
        len = PySequence_Fast_GET_SIZE(fast);
        arr = jsonlang_json_make_array(vm);
        for (i = 0; i < len; ++i) {
            struct JsonlangJsonValue *json_el;
            PyObject *el = PySequence_Fast_GET_ITEM(fast, i);
            json_el = python_to_jsonlang_json(vm, el, err_msg);
            if (json_el == NULL) {
                Py_DECREF(fast);
                jsonlang_json_destroy(vm, arr);
                return NULL;
            }
            jsonlang_json_array_append(vm, arr, json_el);
        }
        Py_DECREF(fast);
        return arr;
    } else if (PyDict_Check(v)) {
        struct JsonlangJsonValue *obj;
        PyObject *key, *val;
        Py_ssize_t pos = 0;
        obj = jsonlang_json_make_object(vm);
        while (PyDict_Next(v, &pos, &key, &val)) {
            struct JsonlangJsonValue *json_val;
            const char *key_ = PyString_AsString(key);
            if (key_ == NULL) {
                *err_msg = "Non-string key in dict returned from Python Jsonlang native extension.";
                jsonlang_json_destroy(vm, obj);
                return NULL;
            }
            json_val = python_to_jsonlang_json(vm, val, err_msg);
            if (json_val == NULL) {
                jsonlang_json_destroy(vm, obj);
                return NULL;
            }
            jsonlang_json_object_append(vm, obj, key_, json_val);
        }
        return obj;
    } else {
        *err_msg = "Unrecognized type return from Python Jsonlang native extension.";
        return NULL;
    }
}

/* This function is bound for every native callback, but with a different
 * context.
 */
static struct JsonlangJsonValue *cpython_native_callback(
    void *ctx_, const struct JsonlangJsonValue * const *argv, int *succ)
{
    const struct NativeCtx *ctx = ctx_;
    int i;

    PyObject *arglist;  // Will hold a tuple of strings.
    PyObject *result;  // Will hold a string.

    // Populate python function args.
    arglist = PyTuple_New(ctx->argc);
    for (i = 0; i < ctx->argc; ++i) {
        double d;
        const char *param_str = jsonlang_json_extract_string(ctx->vm, argv[i]);
        int param_null = jsonlang_json_extract_null(ctx->vm, argv[i]);
        int param_bool = jsonlang_json_extract_bool(ctx->vm, argv[i]);
        int param_num = jsonlang_json_extract_number(ctx->vm, argv[i], &d);
        PyObject *pyobj;
        if (param_str != NULL) {
            pyobj = PyString_FromString(param_str);
        } else if (param_null) {
            pyobj = Py_None;
        } else if (param_bool != 2) {
            pyobj = PyBool_FromLong(param_bool);
        } else if (param_num) {
            pyobj = PyFloat_FromDouble(d);
        } else {
            // TODO(dcunnin): Support arrays (to tuples).
            // TODO(dcunnin): Support objects (to dicts).
            Py_DECREF(arglist);
            *succ = 0;
            return jsonlang_json_make_string(ctx->vm, "Non-primitive param.");
        }
        PyTuple_SetItem(arglist, i, pyobj);
    }

    // Call python function.
    result = PyEval_CallObject(ctx->callback, arglist);
    Py_DECREF(arglist);

    if (result == NULL) {
        // Get string from exception.
        struct JsonlangJsonValue *r = jsonlang_json_make_string(ctx->vm, exc_to_str());
        *succ = 0;
        PyErr_Clear();
        return r;
    }

    const char *err_msg;
    struct JsonlangJsonValue *r = python_to_jsonlang_json(ctx->vm, result, &err_msg);
    if (r != NULL) {
        *succ = 1;
    } else {
        *succ = 0;
        r = jsonlang_json_make_string(ctx->vm, err_msg);
    }
    return r;
}


struct ImportCtx {
    struct JsonlangVm *vm;
    PyObject *callback;
};

static char *cpython_import_callback(void *ctx_, const char *base, const char *rel,
                                     char **found_here, int *success)
{
    const struct ImportCtx *ctx = ctx_;
    PyObject *arglist, *result;
    char *out;

    arglist = Py_BuildValue("(s, s)", base, rel);
    result = PyEval_CallObject(ctx->callback, arglist);
    Py_DECREF(arglist);

    if (result == NULL) {
        // Get string from exception
        char *out = jsonlang_str(ctx->vm, exc_to_str());
        *success = 0;
        PyErr_Clear();
        return out;
    }

    if (!PyTuple_Check(result)) {
        out = jsonlang_str(ctx->vm, "import_callback did not return a tuple");
        *success = 0;
    } else if (PyTuple_Size(result) != 2) {
        out = jsonlang_str(ctx->vm, "import_callback did not return a tuple (size 2)");
        *success = 0;
    } else {
        PyObject *file_name = PyTuple_GetItem(result, 0);
        PyObject *file_content = PyTuple_GetItem(result, 1);
        if (!PyString_Check(file_name) || !PyString_Check(file_content)) {
            out = jsonlang_str(ctx->vm, "import_callback did not return a pair of strings");
            *success = 0;
        } else {
            const char *found_here_cstr = PyString_AsString(file_name);
            const char *content_cstr = PyString_AsString(file_content);
            *found_here = jsonlang_str(ctx->vm, found_here_cstr);
            out = jsonlang_str(ctx->vm, content_cstr);
            *success = 1;
        }
    }

    Py_DECREF(result);

    return out;
}

static PyObject *handle_result(struct JsonlangVm *vm, char *out, int error)
{
    if (error) {
        PyErr_SetString(PyExc_RuntimeError, out);
        jsonlang_realloc(vm, out, 0);
        jsonlang_destroy(vm);
        return NULL;
    } else {
        PyObject *ret = PyString_FromString(out);
        jsonlang_realloc(vm, out, 0);
        jsonlang_destroy(vm);
        return ret;
    }
}

int handle_vars(struct JsonlangVm *vm, PyObject *map, int code, int tla)
{
    if (map == NULL) return 1;

    PyObject *key, *val;
    Py_ssize_t pos = 0;

    while (PyDict_Next(map, &pos, &key, &val)) {
        const char *key_ = PyString_AsString(key);
        if (key_ == NULL) {
            jsonlang_destroy(vm);
            return 0;
        }
        const char *val_ = PyString_AsString(val);
        if (val_ == NULL) {
            jsonlang_destroy(vm);
            return 0;
        }
        if (!tla && !code) {
            jsonlang_ext_var(vm, key_, val_);
        } else if (!tla && code) {
            jsonlang_ext_code(vm, key_, val_);
        } else if (tla && !code) {
            jsonlang_tla_var(vm, key_, val_);
        } else {
            jsonlang_tla_code(vm, key_, val_);
        }
    }
    return 1;
}


int handle_import_callback(struct ImportCtx *ctx, PyObject *import_callback)
{
    if (import_callback == NULL) return 1;

    if (!PyCallable_Check(import_callback)) {
        jsonlang_destroy(ctx->vm);
        PyErr_SetString(PyExc_TypeError, "import_callback must be callable");
        return 0;
    }

    jsonlang_import_callback(ctx->vm, cpython_import_callback, ctx);

    return 1;
}


/** Register native callbacks with Jsonlang VM.
 *
 * Example native_callbacks = { 'name': (('p1', 'p2', 'p3'), func) }
 *
 * May set *ctxs, in which case it should be free()'d by caller.
 *
 * \returns 1 on success, 0 with exception set upon failure.
 */
static int handle_native_callbacks(struct JsonlangVm *vm, PyObject *native_callbacks,
                                   struct NativeCtx **ctxs)
{
    size_t num_natives = 0;
    PyObject *key, *val;
    Py_ssize_t pos = 0;

    if (native_callbacks == NULL) return 1;

    /* Verify the input before we allocate memory, throw all errors at this point.
     * Also, count the callbacks to see how much memory we need.
     */
    while (PyDict_Next(native_callbacks, &pos, &key, &val)) {
        Py_ssize_t i;
        Py_ssize_t num_params;
        PyObject *params;
        const char *key_ = PyString_AsString(key);
        if (key_ == NULL) {
            PyErr_SetString(PyExc_TypeError, "native callback dict keys must be string");
            goto bad;
        }
        if (!PyTuple_Check(val)) {
            PyErr_SetString(PyExc_TypeError, "native callback dict values must be tuples");
            goto bad;
        } else if (PyTuple_Size(val) != 2) {
            PyErr_SetString(PyExc_TypeError, "native callback tuples must have size 2");
            goto bad;
        }
        params = PyTuple_GetItem(val, 0);
        if (!PyTuple_Check(params)) {
            PyErr_SetString(PyExc_TypeError, "native callback params must be a tuple");
            goto bad;
        }
        /* Check the params are all strings */
        num_params = PyTuple_Size(params);
        for (i = 0; i < num_params ; ++i) {
            PyObject *param = PyTuple_GetItem(params, 0);
            if (!PyString_Check(param)) {
                PyErr_SetString(PyExc_TypeError, "native callback param must be string");
                goto bad;
            }
        }
        if (!PyCallable_Check(PyTuple_GetItem(val, 1))) {
            PyErr_SetString(PyExc_TypeError, "native callback must be callable");
            goto bad;
        }

        num_natives++;
        continue;

        bad:
        jsonlang_destroy(vm);
        return 0;
    }

    if (num_natives == 0) {
        return 1;
    }

    *ctxs = malloc(sizeof(struct NativeCtx) * num_natives);

    /* Re-use num_natives but just as a counter this time. */
    num_natives = 0;
    pos = 0;
    while (PyDict_Next(native_callbacks, &pos, &key, &val)) {
        Py_ssize_t i;
        Py_ssize_t num_params;
        PyObject *params;
        const char *key_ = PyString_AsString(key);
        params = PyTuple_GetItem(val, 0);
        num_params = PyTuple_Size(params);
        /* Include space for terminating NULL. */
        const char **params_c = malloc(sizeof(const char*) * (num_params + 1));
        for (i = 0; i < num_params ; ++i) {
            params_c[i] = PyString_AsString(PyTuple_GetItem(params, i));
        }
        params_c[num_params] = NULL;
        (*ctxs)[num_natives].vm = vm;
        (*ctxs)[num_natives].callback = PyTuple_GetItem(val, 1);
        (*ctxs)[num_natives].argc = num_params;
        jsonlang_native_callback(vm, key_, cpython_native_callback, &(*ctxs)[num_natives],
                                params_c);
        free(params_c);
        num_natives++;
    }

    return 1;
}


static PyObject* evaluate_file(PyObject* self, PyObject* args, PyObject *keywds)
{
    const char *filename;
    char *out;
    unsigned max_stack = 500, gc_min_objects = 1000, max_trace = 20;
    double gc_growth_trigger = 2;
    int error;
    PyObject *ext_vars = NULL, *ext_codes = NULL;
    PyObject *tla_vars = NULL, *tla_codes = NULL;
    PyObject *import_callback = NULL;
    PyObject *native_callbacks = NULL;
    struct JsonlangVm *vm;
    static char *kwlist[] = {
        "filename",
        "max_stack", "gc_min_objects", "gc_growth_trigger", "ext_vars",
        "ext_codes", "tla_vars", "tla_codes", "max_trace", "import_callback",
        "native_callbacks",
        NULL
    };

    (void) self;

    if (!PyArg_ParseTupleAndKeywords(
        args, keywds, "s|IIdOOOOIOO", kwlist,
        &filename,
        &max_stack, &gc_min_objects, &gc_growth_trigger, &ext_vars,
        &ext_codes, &tla_vars, &tla_codes, &max_trace, &import_callback,
        &native_callbacks)) {
        return NULL;
    }

    vm = jsonlang_make();
    jsonlang_max_stack(vm, max_stack);
    jsonlang_gc_min_objects(vm, gc_min_objects);
    jsonlang_max_trace(vm, max_trace);
    jsonlang_gc_growth_trigger(vm, gc_growth_trigger);
    if (!handle_vars(vm, ext_vars, 0, 0)) return NULL;
    if (!handle_vars(vm, ext_codes, 1, 0)) return NULL;
    if (!handle_vars(vm, tla_vars, 0, 1)) return NULL;
    if (!handle_vars(vm, tla_codes, 1, 1)) return NULL;
    struct ImportCtx ctx = { vm, import_callback };
    if (!handle_import_callback(&ctx, import_callback)) {
        return NULL;
    }
    struct NativeCtx *ctxs = NULL;
    if (!handle_native_callbacks(vm, native_callbacks, &ctxs)) {
        free(ctxs);
        return NULL;
    }
    out = jsonlang_evaluate_file(vm, filename, &error);
    free(ctxs);
    return handle_result(vm, out, error);
}

static PyObject* evaluate_snippet(PyObject* self, PyObject* args, PyObject *keywds)
{
    const char *filename, *src;
    char *out;
    unsigned max_stack = 500, gc_min_objects = 1000, max_trace = 20;
    double gc_growth_trigger = 2;
    int error;
    PyObject *ext_vars = NULL, *ext_codes = NULL;
    PyObject *tla_vars = NULL, *tla_codes = NULL;
    PyObject *import_callback = NULL;
    PyObject *native_callbacks = NULL;
    struct JsonlangVm *vm;
    static char *kwlist[] = {
        "filename", "src",
        "max_stack", "gc_min_objects", "gc_growth_trigger", "ext_vars",
        "ext_codes", "tla_vars", "tla_codes", "max_trace", "import_callback",
        "native_callbacks",
        NULL
    };

    (void) self;

    if (!PyArg_ParseTupleAndKeywords(
        args, keywds, "ss|IIdOOOOIOO", kwlist,
        &filename, &src,
        &max_stack, &gc_min_objects, &gc_growth_trigger, &ext_vars,
        &ext_codes, &tla_vars, &tla_codes, &max_trace, &import_callback,
        &native_callbacks)) {
        return NULL;
    }

    vm = jsonlang_make();
    jsonlang_max_stack(vm, max_stack);
    jsonlang_gc_min_objects(vm, gc_min_objects);
    jsonlang_max_trace(vm, max_trace);
    jsonlang_gc_growth_trigger(vm, gc_growth_trigger);
    if (!handle_vars(vm, ext_vars, 0, 0)) return NULL;
    if (!handle_vars(vm, ext_codes, 1, 0)) return NULL;
    if (!handle_vars(vm, tla_vars, 0, 1)) return NULL;
    if (!handle_vars(vm, tla_codes, 1, 1)) return NULL;
    struct ImportCtx ctx = { vm, import_callback };
    if (!handle_import_callback(&ctx, import_callback)) {
        return NULL;
    }
    struct NativeCtx *ctxs = NULL;
    if (!handle_native_callbacks(vm, native_callbacks, &ctxs)) {
        free(ctxs);
        return NULL;
    }
    out = jsonlang_evaluate_snippet(vm, filename, src, &error);
    free(ctxs);
    return handle_result(vm, out, error);
}

static PyMethodDef module_methods[] = {
    {"evaluate_file", (PyCFunction)evaluate_file, METH_VARARGS | METH_KEYWORDS,
     "Interpret the given Jsonlang file."},
    {"evaluate_snippet", (PyCFunction)evaluate_snippet, METH_VARARGS | METH_KEYWORDS,
     "Interpret the given Jsonlang code."},
    {NULL, NULL, 0, NULL}
};

PyMODINIT_FUNC init_jsonlang(void)
{
    Py_InitModule3("_jsonlang", module_methods, "A Python interface to Jsonlang.");
}
