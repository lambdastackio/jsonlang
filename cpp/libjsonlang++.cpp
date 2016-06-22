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

#include "libjsonlang++.h"

namespace jsonlang {

Jsonlang::Jsonlang()
{}

Jsonlang::~Jsonlang()
{
    if (vm_ != nullptr) {
        ::jsonlang_destroy(vm_);
    }
}

/* static */
std::string Jsonlang::version()
{
    return ::jsonlang_version();
}

bool Jsonlang::init() {
    vm_ = static_cast<struct JsonlangVm*>(::jsonlang_make());
    return vm_ != nullptr;
}

void Jsonlang::setMaxStack(uint32_t depth)
{
    ::jsonlang_max_stack(vm_, static_cast<unsigned>(depth));
}

void Jsonlang::setGcMinObjects(uint32_t objects)
{
    ::jsonlang_gc_min_objects(vm_, static_cast<unsigned>(objects));
}

void Jsonlang::setGcGrowthTrigger(double growth)
{
    ::jsonlang_gc_growth_trigger(vm_, growth);
}

void Jsonlang::setStringOutput(bool string_output)
{
    ::jsonlang_string_output(vm_, string_output);
}

void Jsonlang::addImportPath(const std::string& path)
{
    ::jsonlang_jpath_add(vm_, path.c_str());
}

void Jsonlang::setMaxTrace(uint32_t lines)
{
    ::jsonlang_max_trace(vm_, static_cast<unsigned>(lines));
}

void Jsonlang::bindExtVar(const std::string& key, const std::string& value)
{
    ::jsonlang_ext_var(vm_, key.c_str(), value.c_str());
}

void Jsonlang::bindExtCodeVar(const std::string& key, const std::string& value)
{
    ::jsonlang_ext_code(vm_, key.c_str(), value.c_str());
}

bool Jsonlang::evaluateFile(const std::string& filename, std::string* output)
{
    if (output == nullptr) {
        return false;
    }
    int error = 0;
    const char* jsonlang_output = ::jsonlang_evaluate_file(vm_, filename.c_str(), &error);
    if (error != 0) {
        last_error_.assign(jsonlang_output);
        return false;
    }
    output->assign(jsonlang_output);
    return true;
}

bool Jsonlang::evaluateSnippet(const std::string& filename,
                              const std::string& snippet,
                              std::string* output)
{
    if (output == nullptr) {
        return false;
    }
    int error = 0;
    const char* jsonlang_output = ::jsonlang_evaluate_snippet(
        vm_, filename.c_str(), snippet.c_str(), &error);
    if (error != 0) {
        last_error_.assign(jsonlang_output);
        return false;
    }
    output->assign(jsonlang_output);
    return true;
}

namespace {
void parseMultiOutput(const char* jsonlang_output,
                      std::map<std::string, std::string>* outputs)
{
    for (const char* c = jsonlang_output; *c != '\0'; ) {
        const char *filename = c;
        const char *c2 = c;
        while (*c2 != '\0') ++c2;
        ++c2;
        const char *json = c2;
        while (*c2 != '\0') ++c2;
        ++c2;
        c = c2;
        outputs->insert(std::make_pair(filename, json));
    }
}
}  // namespace

bool Jsonlang::evaluateFileMulti(const std::string& filename,
                                std::map<std::string, std::string>* outputs)
{
    if (outputs == nullptr) {
        return false;
    }
    int error = 0;
    const char* jsonlang_output =
        ::jsonlang_evaluate_file_multi(vm_, filename.c_str(), &error);
    if (error != 0) {
        last_error_.assign(jsonlang_output);
        return false;
    }
    parseMultiOutput(jsonlang_output, outputs);
    return true;
}

bool Jsonlang::evaluateSnippetMulti(const std::string& filename,
                                   const std::string& snippet,
                                   std::map<std::string, std::string>* outputs)
{
    if (outputs == nullptr) {
        return false;
    }
    int error = 0;
    const char* jsonlang_output = ::jsonlang_evaluate_snippet_multi(
        vm_, filename.c_str(), snippet.c_str(), &error);
    if (error != 0) {
        last_error_.assign(jsonlang_output);
        return false;
    }
    parseMultiOutput(jsonlang_output, outputs);
    return true;
}

std::string Jsonlang::lastError() const
{
    return last_error_;
}

}  // namespace jsonlang
