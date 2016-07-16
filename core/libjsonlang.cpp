/*
Copyright 2016 LambdaStack All rights reserved.

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

#include <cstdlib>
#include <cstring>
#include <cerrno>

#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

extern "C" {
#include "libjsonlang.h"
}

#include "desugarer.h"
#include "formatter.h"
#include "json.h"
#include "parser.h"
#include "static_analysis.h"
#include "vm.h"

static void memory_panic(void)
{
    fputs("FATAL ERROR: A memory allocation error occurred.\n", stderr);
    abort();
}

static char *from_string(JsonlangVm* vm, const std::string &v)
{
    char *r = jsonlang_realloc(vm, nullptr, v.length() + 1);
    std::strcpy(r, v.c_str());
    return r;
}

static char *default_import_callback(void *ctx, const char *dir, const char *file,
                                     char **found_here_cptr, int *success);

const char *jsonlang_json_extract_string(JsonlangVm *vm, const struct JsonlangJsonValue *v)
{
    (void) vm;
    if (v->kind != JsonlangJsonValue::STRING)
        return nullptr;
    return v->string.c_str();
}

int jsonlang_json_extract_number(struct JsonlangVm *vm, const struct JsonlangJsonValue *v, double *out)
{
    (void) vm;
    if (v->kind != JsonlangJsonValue::NUMBER)
        return 0;
    *out = v->number;
    return 1;
}

int jsonlang_json_extract_bool(struct JsonlangVm *vm, const struct JsonlangJsonValue *v)
{
    (void) vm;
    if (v->kind != JsonlangJsonValue::BOOL) return 2;
    return v->number != 0;
}

int jsonlang_json_extract_null(struct JsonlangVm *vm, const struct JsonlangJsonValue *v)
{
    (void) vm;
    return v->kind == JsonlangJsonValue::NULL_KIND;
}

JsonlangJsonValue *jsonlang_json_make_string(JsonlangVm *vm, const char *v)
{
    (void) vm;
    JsonlangJsonValue *r = new JsonlangJsonValue();
    r->kind = JsonlangJsonValue::STRING;
    r->string = v;
    return r;
}

JsonlangJsonValue *jsonlang_json_make_number(struct JsonlangVm *vm, double v)
{
    (void) vm;
    JsonlangJsonValue *r = new JsonlangJsonValue();
    r->kind = JsonlangJsonValue::NUMBER;
    r->number = v;
    return r;
}

JsonlangJsonValue *jsonlang_json_make_bool(struct JsonlangVm *vm, int v)
{
    (void) vm;
    JsonlangJsonValue *r = new JsonlangJsonValue();
    r->kind = JsonlangJsonValue::BOOL;
    r->number = v != 0;
    return r;
}

JsonlangJsonValue *jsonlang_json_make_null(struct JsonlangVm *vm)
{
    (void) vm;
    JsonlangJsonValue *r = new JsonlangJsonValue();
    r->kind = JsonlangJsonValue::NULL_KIND;
    return r;
}

JsonlangJsonValue *jsonlang_json_make_array(JsonlangVm *vm)
{
    (void) vm;
    JsonlangJsonValue *r = new JsonlangJsonValue();
    r->kind = JsonlangJsonValue::ARRAY;
    return r;
}

void jsonlang_json_array_append(JsonlangVm *vm, JsonlangJsonValue *arr, JsonlangJsonValue *v)
{
    (void) vm;
    assert(arr->kind == JsonlangJsonValue::ARRAY);
    arr->elements.emplace_back(v);
}

JsonlangJsonValue *jsonlang_json_make_object(JsonlangVm *vm)
{
    (void) vm;
    JsonlangJsonValue *r = new JsonlangJsonValue();
    r->kind = JsonlangJsonValue::OBJECT;
    return r;
}

void jsonlang_json_object_append(JsonlangVm *vm, JsonlangJsonValue *obj,
                                const char *f, JsonlangJsonValue *v)
{
    (void) vm;
    assert(obj->kind == JsonlangJsonValue::OBJECT);
    obj->fields[std::string(f)] = std::unique_ptr<JsonlangJsonValue>(v);
}

void jsonlang_json_destroy(JsonlangVm *vm, JsonlangJsonValue *v)
{
    (void) vm;
    delete v;
}

struct JsonlangVm {
    double gcGrowthTrigger;
    unsigned maxStack;
    unsigned gcMinObjects;
    unsigned maxTrace;
    std::map<std::string, VmExt> ext;
    std::map<std::string, VmExt> tla;
    JsonlangImportCallback *importCallback;
    VmNativeCallbackMap nativeCallbacks;
    void *importCallbackContext;
    bool stringOutput;
    std::vector<std::string> jpaths;

    FmtOpts fmtOpts;
    bool fmtDebugDesugaring;

    JsonlangVm(void)
      : gcGrowthTrigger(2.0), maxStack(500), gcMinObjects(1000), maxTrace(20),
        importCallback(default_import_callback), importCallbackContext(this), stringOutput(false),
        fmtDebugDesugaring(false)
    {
        jpaths.emplace_back("/usr/share/" + std::string(jsonlang_version()) + "/");
        jpaths.emplace_back("/usr/local/share/" + std::string(jsonlang_version()) + "/");
    }
};

enum ImportStatus {
    IMPORT_STATUS_OK,
    IMPORT_STATUS_FILE_NOT_FOUND,
    IMPORT_STATUS_IO_ERROR
};

static enum ImportStatus try_path(const std::string &dir, const std::string &rel,
                                  std::string &content, std::string &found_here,
                                  std::string &err_msg)
{
    std::string abs_path;
    if (rel.length() == 0) {
        err_msg = "The empty string is not a valid filename";
        return IMPORT_STATUS_IO_ERROR;
    }
    // It is possible that rel is actually absolute.
    if (rel[0] == '/') {
        abs_path = rel;
    } else {
        abs_path = dir + rel;
    }

    if (abs_path[abs_path.length() - 1] == '/') {
        err_msg = "Attempted to import a directory";
        return IMPORT_STATUS_IO_ERROR;
    }

    std::ifstream f;
    f.open(abs_path.c_str());
    if (!f.good()) return IMPORT_STATUS_FILE_NOT_FOUND;
    try {
        content.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    } catch (const std::ios_base::failure &io_err) {
        err_msg = io_err.what();
        return IMPORT_STATUS_IO_ERROR;
    }
    if (!f.good()) {
        err_msg = strerror(errno);
        return IMPORT_STATUS_IO_ERROR;
    }

    found_here = abs_path;

    return IMPORT_STATUS_OK;
}

static char *default_import_callback(void *ctx, const char *dir, const char *file,
                                     char **found_here_cptr, int *success)
{
    auto *vm = static_cast<JsonlangVm*>(ctx);

    std::string input, found_here, err_msg;

    ImportStatus status = try_path(dir, file, input, found_here, err_msg);

    std::vector<std::string> jpaths(vm->jpaths);

    // If not found, try library search path.
    while (status == IMPORT_STATUS_FILE_NOT_FOUND) {
        if (jpaths.size() == 0) {
            *success = 0;
            const char *err = "No match locally or in the Jsonlang library paths.";
            char *r = jsonlang_realloc(vm, nullptr, std::strlen(err) + 1);
            std::strcpy(r, err);
            return r;
        }
        status = try_path(jpaths.back(), file, input, found_here, err_msg);
        jpaths.pop_back();
    }

    if (status == IMPORT_STATUS_IO_ERROR) {
        *success = 0;
        return from_string(vm, err_msg);
    } else {
        assert(status == IMPORT_STATUS_OK);
        *success = 1;
        *found_here_cptr = from_string(vm, found_here);
        return from_string(vm, input);
    }
}

#define TRY try {
#define CATCH(func) \
    } catch (const std::bad_alloc &) {\
        memory_panic(); \
    } catch (const std::exception &e) {\
        std::cerr << "Something went wrong during " func ", please report this: " \
                  << e.what() << std::endl; \
        abort(); \
    }

const char *jsonlang_version(void)
{
    return LIB_JSONLANG_VERSION;
}

JsonlangVm *jsonlang_make(void)
{
    TRY
    return new JsonlangVm();
    CATCH("jsonlang_make")
    return nullptr;
}

void jsonlang_destroy(JsonlangVm *vm)
{
    TRY
    delete vm;
    CATCH("jsonlang_destroy")
}

void jsonlang_max_stack(JsonlangVm *vm, unsigned v)
{
    vm->maxStack = v;
}

void jsonlang_gc_min_objects(JsonlangVm *vm, unsigned v)
{
    vm->gcMinObjects = v;
}

void jsonlang_gc_growth_trigger(JsonlangVm *vm, double v)
{
    vm->gcGrowthTrigger = v;
}

void jsonlang_string_output(struct JsonlangVm *vm, int v)
{
    vm->stringOutput = bool(v);
}

void jsonlang_import_callback(struct JsonlangVm *vm, JsonlangImportCallback *cb, void *ctx)
{
    vm->importCallback = cb;
    vm->importCallbackContext = ctx;
}

void jsonlang_native_callback(struct JsonlangVm *vm, const char *name, JsonlangNativeCallback *cb,
                             void *ctx, const char * const *params)
{
    std::vector<std::string> params2;
    for (; *params != nullptr; params++)
        params2.push_back(*params);
    vm->nativeCallbacks[name] = VmNativeCallback {cb, ctx, params2};
}


void jsonlang_ext_var(JsonlangVm *vm, const char *key, const char *val)
{
    vm->ext[key] = VmExt(val, false);
}

void jsonlang_ext_code(JsonlangVm *vm, const char *key, const char *val)
{
    vm->ext[key] = VmExt(val, true);
}

void jsonlang_tla_var(JsonlangVm *vm, const char *key, const char *val)
{
    vm->tla[key] = VmExt(val, false);
}

void jsonlang_tla_code(JsonlangVm *vm, const char *key, const char *val)
{
    vm->tla[key] = VmExt(val, true);
}

void jsonlang_fmt_debug_desugaring(JsonlangVm *vm, int v)
{
    vm->fmtDebugDesugaring = v;
}

void jsonlang_fmt_indent(JsonlangVm *vm, int v)
{
    vm->fmtOpts.indent = v;
}

void jsonlang_fmt_max_blank_lines(JsonlangVm *vm, int v)
{
    vm->fmtOpts.maxBlankLines = v;
}

void jsonlang_fmt_string(JsonlangVm *vm, int v)
{
    if (v != 'd' && v != 's' && v != 'l')
        v = 'l';
    vm->fmtOpts.stringStyle = v;
}

void jsonlang_fmt_comment(JsonlangVm *vm, int v)
{
    if (v != 'h' && v != 's' && v != 'l')
        v = 'l';
    vm->fmtOpts.commentStyle = v;
}

void jsonlang_fmt_pad_arrays(JsonlangVm *vm, int v)
{
    vm->fmtOpts.padArrays = v;
}

void jsonlang_fmt_pad_objects(JsonlangVm *vm, int v)
{
    vm->fmtOpts.padObjects = v;
}

void jsonlang_fmt_pretty_field_names(JsonlangVm *vm, int v)
{
    vm->fmtOpts.prettyFieldNames = v;
}

void jsonlang_max_trace(JsonlangVm *vm, unsigned v)
{
    vm->maxTrace = v;
}

void jsonlang_jpath_add(JsonlangVm *vm, const char *path_)
{
    if (std::strlen(path_) == 0) return;
    std::string path = path_;
    if (path[path.length() - 1] != '/') path += '/';
    vm->jpaths.emplace_back(path);
}

static char *jsonlang_fmt_snippet_aux(JsonlangVm *vm, const char *filename, const char *snippet,
                                     int *error)
{
    try {
        Allocator alloc;
        std::string json_str;
        AST *expr;
        std::map<std::string, std::string> files;
        Tokens tokens = jsonlang_lex(filename, snippet);

        // std::cout << jsonlang_unlex(tokens);

        expr = jsonlang_parse(&alloc, tokens);
        Fodder final_fodder = tokens.front().fodder;

        if (vm->fmtDebugDesugaring)
            jsonlang_desugar(&alloc, expr, &vm->tla);

        json_str = jsonlang_fmt(expr, final_fodder, vm->fmtOpts);

        json_str += "\n";

        *error = false;
        return from_string(vm, json_str);

    } catch (StaticError &e) {
        std::stringstream ss;
        ss << "STATIC ERROR: " << e << std::endl;
        *error = true;
        return from_string(vm, ss.str());
    }

}

char *jsonlang_fmt_file(JsonlangVm *vm, const char *filename, int *error)
{
    TRY
        std::ifstream f;
        f.open(filename);
        if (!f.good()) {
            std::stringstream ss;
            ss << "Opening input file: " << filename << ": " << strerror(errno);
            *error = true;
            return from_string(vm, ss.str());
        }
        std::string input;
        input.assign(std::istreambuf_iterator<char>(f),
                     std::istreambuf_iterator<char>());

        return jsonlang_fmt_snippet_aux(vm, filename, input.c_str(), error);
    CATCH("jsonlang_fmt_file")
    return nullptr;  // Never happens.
}

char *jsonlang_fmt_snippet(JsonlangVm *vm, const char *filename, const char *snippet, int *error)
{
    TRY
        return jsonlang_fmt_snippet_aux(vm, filename, snippet, error);
    CATCH("jsonlang_fmt_snippet")
    return nullptr;  // Never happens.
}


namespace {
enum EvalKind { REGULAR, MULTI, STREAM };
}  // namespace

static char *jsonlang_evaluate_snippet_aux(JsonlangVm *vm, const char *filename,
                                          const char *snippet, int *error, EvalKind kind)
{
    try {
        Allocator alloc;
        AST *expr;
        Tokens tokens = jsonlang_lex(filename, snippet);

        expr = jsonlang_parse(&alloc, tokens);

        jsonlang_desugar(&alloc, expr, &vm->tla);

        jsonlang_static_analysis(expr);
        switch (kind) {
            case REGULAR: {
                std::string json_str = jsonlang_vm_execute(
                    &alloc, expr, vm->ext, vm->maxStack, vm->gcMinObjects,
                    vm->gcGrowthTrigger, vm->nativeCallbacks, vm->importCallback,
                    vm->importCallbackContext, vm->stringOutput);
                json_str += "\n";
                *error = false;
                return from_string(vm, json_str);
            }
            break;

            case MULTI: {
                std::map<std::string, std::string> files = jsonlang_vm_execute_multi(
                    &alloc, expr, vm->ext, vm->maxStack, vm->gcMinObjects,
                    vm->gcGrowthTrigger, vm->nativeCallbacks, vm->importCallback,
                    vm->importCallbackContext, vm->stringOutput);
                size_t sz = 1; // final sentinel
                for (const auto &pair : files) {
                    sz += pair.first.length() + 1; // include sentinel
                    sz += pair.second.length() + 2; // Add a '\n' as well as sentinel
                }
                char *buf = (char*)::malloc(sz);
                if (buf == nullptr) memory_panic();
                std::ptrdiff_t i = 0;
                for (const auto &pair : files) {
                    memcpy(&buf[i], pair.first.c_str(), pair.first.length() + 1);
                    i += pair.first.length() + 1;
                    memcpy(&buf[i], pair.second.c_str(), pair.second.length());
                    i += pair.second.length();
                    buf[i] = '\n';
                    i++;
                    buf[i] = '\0';
                    i++;
                }
                buf[i] = '\0'; // final sentinel
                *error = false;
                return buf;
            }
            break;

            case STREAM: {
                std::vector<std::string> documents = jsonlang_vm_execute_stream(
                    &alloc, expr, vm->ext, vm->maxStack, vm->gcMinObjects,
                    vm->gcGrowthTrigger, vm->nativeCallbacks, vm->importCallback,
                    vm->importCallbackContext);
                size_t sz = 1; // final sentinel
                for (const auto &doc : documents) {
                    sz += doc.length() + 2; // Add a '\n' as well as sentinel
                }
                char *buf = (char*)::malloc(sz);
                if (buf == nullptr) memory_panic();
                std::ptrdiff_t i = 0;
                for (const auto &doc : documents) {
                    memcpy(&buf[i], doc.c_str(), doc.length());
                    i += doc.length();
                    buf[i] = '\n';
                    i++;
                    buf[i] = '\0';
                    i++;
                }
                buf[i] = '\0'; // final sentinel
                *error = false;
                return buf;
            }
            break;

            default:
            fputs("INTERNAL ERROR: bad value of 'kind', probably memory corruption.\n", stderr);
            abort();
        }

    } catch (StaticError &e) {
        std::stringstream ss;
        ss << "STATIC ERROR: " << e << std::endl;
        *error = true;
        return from_string(vm, ss.str());

    } catch (RuntimeError &e) {
        std::stringstream ss;
        ss << "RUNTIME ERROR: " << e.msg << std::endl;
        const long max_above = vm->maxTrace / 2;
        const long max_below = vm->maxTrace - max_above;
        const long sz = e.stackTrace.size();
        for (long i = 0 ; i < sz ; ++i) {
            const auto &f = e.stackTrace[i];
            if (vm->maxTrace > 0 && i >= max_above && i < sz - max_below) {
                if (i == max_above)
                    ss << "\t..." << std::endl;
            } else {
                ss << "\t" << f.location << "\t" << f.name << std::endl;
            }
        }
        *error = true;
        return from_string(vm, ss.str());
    }

    return nullptr;  // Quiet, compiler.
}

static char *jsonlang_evaluate_file_aux(JsonlangVm *vm, const char *filename, int *error, EvalKind kind)
{
    std::ifstream f;
    f.open(filename);
    if (!f.good()) {
        std::stringstream ss;
        ss << "Opening input file: " << filename << ": " << strerror(errno);
        *error = true;
        return from_string(vm, ss.str());
    }
    std::string input;
    input.assign(std::istreambuf_iterator<char>(f),
                 std::istreambuf_iterator<char>());

    return jsonlang_evaluate_snippet_aux(vm, filename, input.c_str(), error, kind);
}

char *jsonlang_evaluate_file(JsonlangVm *vm, const char *filename, int *error)
{
    TRY
    return jsonlang_evaluate_file_aux(vm, filename, error, REGULAR);
    CATCH("jsonlang_evaluate_file")
    return nullptr;  // Never happens.
}

char *jsonlang_evaluate_file_multi(JsonlangVm *vm, const char *filename, int *error)
{
    TRY
    return jsonlang_evaluate_file_aux(vm, filename, error, MULTI);
    CATCH("jsonlang_evaluate_file_multi")
    return nullptr;  // Never happens.
}

char *jsonlang_evaluate_file_stream(JsonlangVm *vm, const char *filename, int *error)
{
    TRY
    return jsonlang_evaluate_file_aux(vm, filename, error, STREAM);
    CATCH("jsonlang_evaluate_file_stream")
    return nullptr;  // Never happens.
}

char *jsonlang_evaluate_snippet(JsonlangVm *vm, const char *filename, const char *snippet, int *error)
{
    TRY
    return jsonlang_evaluate_snippet_aux(vm, filename, snippet, error, REGULAR);
    CATCH("jsonlang_evaluate_snippet")
    return nullptr;  // Never happens.
}

char *jsonlang_evaluate_snippet_multi(JsonlangVm *vm, const char *filename,
                                     const char *snippet, int *error)
{
    TRY
    return jsonlang_evaluate_snippet_aux(vm, filename, snippet, error, MULTI);
    CATCH("jsonlang_evaluate_snippet_multi")
    return nullptr;  // Never happens.
}

char *jsonlang_evaluate_snippet_stream(JsonlangVm *vm, const char *filename,
                                     const char *snippet, int *error)
{
    TRY
    return jsonlang_evaluate_snippet_aux(vm, filename, snippet, error, STREAM);
    CATCH("jsonlang_evaluate_snippet_stream")
    return nullptr;  // Never happens.
}

char *jsonlang_realloc(JsonlangVm *vm, char *str, size_t sz)
{
    (void) vm;
    if (str == nullptr) {
        if (sz == 0) return nullptr;
        auto *r = static_cast<char*>(::malloc(sz));
        if (r == nullptr) memory_panic();
        return r;
    } else {
        if (sz == 0) {
            ::free(str);
            return nullptr;
        } else {
            auto *r = static_cast<char*>(::realloc(str, sz));
            if (r == nullptr) memory_panic();
            return r;
        }
    }
}
