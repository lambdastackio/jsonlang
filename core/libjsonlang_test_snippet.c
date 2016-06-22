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
#include <string.h>
#include <math.h>

#include <libjsonlang.h>

typedef struct JsonlangJsonValue JJV;

static JJV *native_concat(void *ctx, const JJV * const *argv, int *succ)
{
    struct JsonlangVm *vm = (struct JsonlangVm *)ctx;
    const char *a = jsonlang_json_extract_string(vm, argv[0]);
    const char *b = jsonlang_json_extract_string(vm, argv[1]);
    if (a == NULL || b == NULL) {
        *succ = 0;
        return jsonlang_json_make_string(vm, "Bad params.");
    }
    char *str = malloc(strlen(a) + strlen(b) + 1);
    sprintf(str, "%s%s", a, b);
    JJV *r = jsonlang_json_make_string(vm, str);
    free(str);
    *succ = 1;
    return r;
}

static JJV *native_square(void *ctx, const JJV * const *argv, int *succ)
{
    struct JsonlangVm *vm = (struct JsonlangVm *)ctx;
    double a;
    if (!jsonlang_json_extract_number(vm, argv[0], &a)) {
        *succ = 0;
        return jsonlang_json_make_string(vm, "Bad param 'a'.");
    }
    *succ = 1;
    return jsonlang_json_make_number(vm, a * a);
}

static JJV *native_build(void *ctx, const JJV * const *argv, int *succ)
{
    struct JsonlangVm *vm = (struct JsonlangVm *)ctx;
    (void) argv;
    JJV *obj_top = jsonlang_json_make_object(vm);
    JJV *arr_top = jsonlang_json_make_array(vm);
    JJV *arr1 = jsonlang_json_make_array(vm);
    jsonlang_json_array_append(vm, arr1, jsonlang_json_make_string(vm, "Test 1.1"));
    jsonlang_json_array_append(vm, arr1, jsonlang_json_make_string(vm, "Test 1.2"));
    jsonlang_json_array_append(vm, arr1, jsonlang_json_make_string(vm, "Test 1.3"));
    jsonlang_json_array_append(vm, arr1, jsonlang_json_make_bool(vm, 1));
    jsonlang_json_array_append(vm, arr1, jsonlang_json_make_number(vm, 42));
    jsonlang_json_array_append(vm, arr1, jsonlang_json_make_null(vm));
    jsonlang_json_array_append(vm, arr1, jsonlang_json_make_object(vm));
    jsonlang_json_array_append(vm, arr_top, arr1);
    JJV *arr2 = jsonlang_json_make_array(vm);
    jsonlang_json_array_append(vm, arr2, jsonlang_json_make_string(vm, "Test 2.1"));
    jsonlang_json_array_append(vm, arr2, jsonlang_json_make_string(vm, "Test 2.2"));
    jsonlang_json_array_append(vm, arr2, jsonlang_json_make_string(vm, "Test 2.3"));
    jsonlang_json_array_append(vm, arr2, jsonlang_json_make_bool(vm, 0));
    jsonlang_json_array_append(vm, arr2, jsonlang_json_make_number(vm, -42));
    jsonlang_json_array_append(vm, arr2, jsonlang_json_make_null(vm));
    JJV *little_obj = jsonlang_json_make_object(vm);
    jsonlang_json_object_append(vm, little_obj, "f", jsonlang_json_make_string(vm, "foo"));
    jsonlang_json_object_append(vm, little_obj, "g", jsonlang_json_make_string(vm, "bar"));
    jsonlang_json_array_append(vm, arr2, little_obj);
    jsonlang_json_array_append(vm, arr_top, arr2);
    jsonlang_json_object_append(vm, obj_top, "field", arr_top);
    *succ = 1;
    return obj_top;
}

int main(int argc, const char **argv)
{
    int error;
    char *output;
    struct JsonlangVm *vm;
    const char *params0[] = {NULL};
    const char *params1[] = {"a", NULL};
    const char *params2[] = {"a", "b", NULL};
    if (argc != 2) {
        fprintf(stderr, "libjsonlang_test_snippet <string>\n");
        return EXIT_FAILURE;
    }
    vm = jsonlang_make();
    jsonlang_native_callback(vm, "concat", native_concat, vm, params2);
    jsonlang_native_callback(vm, "square", native_square, vm, params1);
    jsonlang_native_callback(vm, "build", native_build, vm, params0);
    output = jsonlang_evaluate_snippet(vm, "snippet", argv[1], &error);
    if (error) {
        fprintf(stderr, "%s", output);
        jsonlang_realloc(vm, output, 0);
        jsonlang_destroy(vm);
        return EXIT_FAILURE;
    } 
    printf("%s", output);
    jsonlang_realloc(vm, output, 0);
    jsonlang_destroy(vm);
    return EXIT_SUCCESS;
}
