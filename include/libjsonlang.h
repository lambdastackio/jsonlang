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

#ifndef LIB_JSONLANG_H
#define LIB_JSONLANG_H

#include <stddef.h>

/** \file This file is a library interface for evaluating Jsonlang.  It is written in C++ but exposes
 * a C interface for easier wrapping by other languages.  See \see jsonlang_lib_test.c for an example
 * of using the library.
 */


#define LIB_JSONLANG_VERSION "v0.8.7"


/** Return the version string of the Jsonlang interpreter.  Conforms to semantic versioning
 * http://semver.org/ If this does not match LIB_JSONLANG_VERSION then there is a mismatch between
 * header and compiled library.
 */
const char *jsonlang_version(void);

/** Jsonlang virtual machine context. */
struct JsonlangVm;

/** Create a new Jsonlang virtual machine. */
struct JsonlangVm *jsonlang_make(void);

/** Set the maximum stack depth. */
void jsonlang_max_stack(struct JsonlangVm *vm, unsigned v);

/** Set the number of objects required before a garbage collection cycle is allowed. */
void jsonlang_gc_min_objects(struct JsonlangVm *vm, unsigned v);

/** Run the garbage collector after this amount of growth in the number of objects. */
void jsonlang_gc_growth_trigger(struct JsonlangVm *vm, double v);

/** Expect a string as output and don't JSON encode it. */
void jsonlang_string_output(struct JsonlangVm *vm, int v);

/** Callback used to load imports.
 *
 * The returned char* should be allocated with jsonlang_realloc.  It will be cleaned up by
 * libjsonlang when no-longer needed.
 *
 * \param ctx User pointer, given in jsonlang_import_callback.
 * \param base The directory containing the code that did the import.
 * \param rel The path imported by the code.
 * \param found_here Set this byref param to path to the file, absolute or relative to the
 *     process's CWD.  This is necessary so that imports from the content of the imported file can
 *     be resolved correctly.  Allocate memory with jsonlang_realloc.  Only use when *success = 0.
 * \param success Set this byref param to 1 to indicate success and 0 for failure.
 * \returns The content of the imported file, or an error message.
 */
typedef char *JsonlangImportCallback(void *ctx, const char *base, const char *rel, char **found_here, int *success);

/** An opaque type which can only be utilized via the jsonlang_json_* family of functions.
 */
struct JsonlangJsonValue;

/** If the value is a string, return it as UTF8 otherwise return NULL.
 */
const char *jsonlang_json_extract_string(struct JsonlangVm *vm, const struct JsonlangJsonValue *v);

/** If the value is a number, return 1 and store the number in out, otherwise return 0.
 */
int jsonlang_json_extract_number(struct JsonlangVm *vm, const struct JsonlangJsonValue *v, double *out);

/** Return 0 if the value is false, 1 if it is true, and 2 if it is not a bool.
 */
int jsonlang_json_extract_bool(struct JsonlangVm *vm, const struct JsonlangJsonValue *v);

/** Return 1 if the value is null, else 0.
 */
int jsonlang_json_extract_null(struct JsonlangVm *vm, const struct JsonlangJsonValue *v);

/** Convert the given UTF8 string to a JsonlangJsonValue.
 */
struct JsonlangJsonValue *jsonlang_json_make_string(struct JsonlangVm *vm, const char *v);

/** Convert the given double to a JsonlangJsonValue.
 */
struct JsonlangJsonValue *jsonlang_json_make_number(struct JsonlangVm *vm, double v);

/** Convert the given bool (1 or 0) to a JsonlangJsonValue.
 */
struct JsonlangJsonValue *jsonlang_json_make_bool(struct JsonlangVm *vm, int v);

/** Make a JsonlangJsonValue representing null.
 */
struct JsonlangJsonValue *jsonlang_json_make_null(struct JsonlangVm *vm);

/** Make a JsonlangJsonValue representing an array.
 *
 * Assign elements with jsonlang_json_array_append.
 */
struct JsonlangJsonValue *jsonlang_json_make_array(struct JsonlangVm *vm);

/** Add v to the end of the array.
 */
void jsonlang_json_array_append(struct JsonlangVm *vm,
                               struct JsonlangJsonValue *arr,
                               struct JsonlangJsonValue *v);

/** Make a JsonlangJsonValue representing an object with the given number of fields.
 *
 * Every index of the array must have a unique value assigned with jsonlang_json_array_element.
 */
struct JsonlangJsonValue *jsonlang_json_make_object(struct JsonlangVm *vm);

/** Add the field f to the object, bound to v.
 *
 * This replaces any previous binding of the field.
 */
void jsonlang_json_object_append(struct JsonlangVm *vm,
                                struct JsonlangJsonValue *obj,
                                const char *f,
                                struct JsonlangJsonValue *v);

/** Clean up a JSON subtree.
 *
 * This is useful if you want to abort with an error mid-way through building a complex value.
 */
void jsonlang_json_destroy(struct JsonlangVm *vm, struct JsonlangJsonValue *v);

/** Callback to provide native extensions to Jsonlang.
 *
 * The returned JsonlangJsonValue* should be allocated with jsonlang_realloc.  It will be cleaned up
 * along with the objects rooted at argv by libjsonlang when no-longer needed.  Return a string upon
 * failure, which will appear in Jsonlang as an error.
 *
 * \param ctx User pointer, given in jsonlang_native_callback.
 * \param argc The number of arguments from Jsonlang code.
 * \param argv Array of arguments from Jsonlang code.
 * \param success Set this byref param to 1 to indicate success and 0 for failure.
 * \returns The content of the imported file, or an error message.
 */
typedef struct JsonlangJsonValue *JsonlangNativeCallback(void *ctx,
                                                       const struct JsonlangJsonValue * const *argv,
                                                       int *success);

/** Allocate, resize, or free a buffer.  This will abort if the memory cannot be allocated.  It will
 * only return NULL if sz was zero.
 *
 * \param buf If NULL, allocate a new buffer.  If an previously allocated buffer, resize it.
 * \param sz The size of the buffer to return.  If zero, frees the buffer.
 * \returns The new buffer.
 */
char *jsonlang_realloc(struct JsonlangVm *vm, char *buf, size_t sz);

/** Override the callback used to locate imports.
 */
void jsonlang_import_callback(struct JsonlangVm *vm, JsonlangImportCallback *cb, void *ctx);

/** Register a native extension.
 *
 * This will appear in Jsonlang as a function type and can be accessed from std.nativeExt("foo").
 *
 * DO NOT register native callbacks with side-effects!  Jsonlang is a lazy functional language and
 * will call your function when you least expect it, more times than you expect, or not at all.
 *
 * \param vm The vm.
 * \param name The name of the function as visible to Jsonlang code, e.g. "foo".
 * \param cb The PURE function that implements the behavior you want.
 * \param ctx User pointer, stash non-global state you need here.
 * \param params The names of the params.  Must be valid Jsonlang identifiers.
 */
void jsonlang_native_callback(struct JsonlangVm *vm, const char *name, JsonlangNativeCallback *cb,
                             void *ctx, const char * const *params);

/** Bind a Jsonlang external var to the given string.
 *
 * Argument values are copied so memory should be managed by caller.
 */
void jsonlang_ext_var(struct JsonlangVm *vm, const char *key, const char *val);

/** Bind a Jsonlang external var to the given code.
 *
 * Argument values are copied so memory should be managed by caller.
 */
void jsonlang_ext_code(struct JsonlangVm *vm, const char *key, const char *val);

/** Bind a string top-level argument for a top-level parameter.
 *
 * Argument values are copied so memory should be managed by caller.
 */
void jsonlang_tla_var(struct JsonlangVm *vm, const char *key, const char *val);

/** Bind a code top-level argument for a top-level parameter.
 *
 * Argument values are copied so memory should be managed by caller.
 */
void jsonlang_tla_code(struct JsonlangVm *vm, const char *key, const char *val);

/** Indentation level when reformatting (number of spaeces).
 *
 * \param n Number of spaces, must be > 0.
 */
void jsonlang_fmt_indent(struct JsonlangVm *vm, int n);

/** Indentation level when reformatting (number of spaeces).
 *
 * \param n Number of spaces, must be > 0.
 */
void jsonlang_fmt_max_blank_lines(struct JsonlangVm *vm, int n);

/** Preferred style for string literals ("" or '').
 *
 * \param c String style as a char ('d', 's', or 'l' (leave)).
 */
void jsonlang_fmt_string(struct JsonlangVm *vm, int c);

/** Preferred style for line comments (# or //).
 *
 * \param c Comment style as a char ('h', 's', or 'l' (leave)).
 */
void jsonlang_fmt_comment(struct JsonlangVm *vm, int c);

/** Whether to add an extra space on the inside of arrays.
 */
void jsonlang_fmt_pad_arrays(struct JsonlangVm *vm, int v);

/** Whether to add an extra space on the inside of objects.
 */
void jsonlang_fmt_pad_objects(struct JsonlangVm *vm, int v);

/** Use syntax sugar where possible with field names.
 */
void jsonlang_fmt_pretty_field_names(struct JsonlangVm *vm, int v);

/** If set to 1, will reformat the Jsonlang input after desugaring. */
void jsonlang_fmt_debug_desugaring(struct JsonlangVm *vm, int v);

/** Reformat a file containing Jsonlang code, return a Jsonlang string.
 *
 * The returned string should be cleaned up with jsonlang_realloc.
 *
 * \param filename Path to a file containing Jsonlang code.
 * \param error Return by reference whether or not there was an error.
 * \returns Either Jsonlang code or the error message.
 */
char *jsonlang_fmt_file(struct JsonlangVm *vm,
                       const char *filename,
                       int *error);

/** Evaluate a string containing Jsonlang code, return a Jsonlang string.
 *
 * The returned string should be cleaned up with jsonlang_realloc.
 *
 * \param filename Path to a file (used in error messages).
 * \param snippet Jsonlang code to execute.
 * \param error Return by reference whether or not there was an error.
 * \returns Either JSON or the error message.
 */
char *jsonlang_fmt_snippet(struct JsonlangVm *vm,
                          const char *filename,
                          const char *snippet,
                          int *error);

/** Set the number of lines of stack trace to display (0 for all of them). */
void jsonlang_max_trace(struct JsonlangVm *vm, unsigned v);

/** Add to the default import callback's library search path. */
void jsonlang_jpath_add(struct JsonlangVm *vm, const char *v);

/** Evaluate a file containing Jsonlang code, return a JSON string.
 *
 * The returned string should be cleaned up with jsonlang_realloc.
 *
 * \param filename Path to a file containing Jsonlang code.
 * \param error Return by reference whether or not there was an error.
 * \returns Either JSON or the error message.
 */
char *jsonlang_evaluate_file(struct JsonlangVm *vm,
                            const char *filename,
                            int *error);

/** Evaluate a string containing Jsonlang code, return a JSON string.
 *
 * The returned string should be cleaned up with jsonlang_realloc.
 *
 * \param filename Path to a file (used in error messages).
 * \param snippet Jsonlang code to execute.
 * \param error Return by reference whether or not there was an error.
 * \returns Either JSON or the error message.
 */
char *jsonlang_evaluate_snippet(struct JsonlangVm *vm,
                               const char *filename,
                               const char *snippet,
                               int *error);

/** Evaluate a file containing Jsonlang code, return a number of named JSON files.
 *
 * The returned character buffer contains an even number of strings, the filename and JSON for each
 * JSON file interleaved.  It should be cleaned up with jsonlang_realloc.
 *
 * \param filename Path to a file containing Jsonlang code.
 * \param error Return by reference whether or not there was an error.
 * \returns Either the error, or a sequence of strings separated by \0, terminated with \0\0.
 */
char *jsonlang_evaluate_file_multi(struct JsonlangVm *vm,
                                  const char *filename,
                                  int *error);

/** Evaluate a string containing Jsonlang code, return a number of named JSON files.
 *
 * The returned character buffer contains an even number of strings, the filename and JSON for each
 * JSON file interleaved.  It should be cleaned up with jsonlang_realloc.
 *
 * \param filename Path to a file containing Jsonlang code.
 * \param snippet Jsonlang code to execute.
 * \param error Return by reference whether or not there was an error.
 * \returns Either the error, or a sequence of strings separated by \0, terminated with \0\0.
 */
char *jsonlang_evaluate_snippet_multi(struct JsonlangVm *vm,
                                     const char *filename,
                                     const char *snippet,
                                     int *error);

/** Evaluate a file containing Jsonlang code, return a number of JSON files.
 *
 * The returned character buffer contains several strings.  It should be cleaned up with
 * jsonlang_realloc.
 *
 * \param filename Path to a file containing Jsonlang code.
 * \param error Return by reference whether or not there was an error.
 * \returns Either the error, or a sequence of strings separated by \0, terminated with \0\0.
 */
char *jsonlang_evaluate_file_stream(struct JsonlangVm *vm,
                                   const char *filename,
                                   int *error);

/** Evaluate a string containing Jsonlang code, return a number of JSON files.
 *
 * The returned character buffer contains several strings.  It should be cleaned up with
 * jsonlang_realloc.
 *
 * \param filename Path to a file containing Jsonlang code.
 * \param snippet Jsonlang code to execute.
 * \param error Return by reference whether or not there was an error.
 * \returns Either the error, or a sequence of strings separated by \0, terminated with \0\0.
 */
char *jsonlang_evaluate_snippet_stream(struct JsonlangVm *vm,
                                      const char *filename,
                                      const char *snippet,
                                      int *error);

/** Complement of \see jsonlang_vm_make. */
void jsonlang_destroy(struct JsonlangVm *vm);

#endif  // LIB_JSONLANG_H
