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

#include <libjsonlang.h>

int main(int argc, const char **argv)
{
    int error;
    char *output;
    struct JsonlangVm *vm;
    if (argc != 2) {
        fprintf(stderr, "libjsonlang_test_file <file>\n");
        return EXIT_FAILURE;
    }
    vm = jsonlang_make();
    output = jsonlang_evaluate_file(vm, argv[1], &error);
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
