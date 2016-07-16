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

#include "libjsonlang++.h"

#include <string>
#include <fstream>
#include <streambuf>

#include "gtest/gtest.h"

namespace jsonlang {

std::string readFile(const std::string& filename)
{
    std::ifstream in(filename);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

TEST(JsonlangTest, TestEvaluateSnippet)
{
    const std::string input = readFile("cpp/testdata/example.jsonlang");
    const std::string expected = readFile("cpp/testdata/example_golden.json");

    Jsonlang jsonlang;
    ASSERT_TRUE(jsonlang.init());
    std::string output;
    EXPECT_TRUE(jsonlang.evaluateSnippet("snippet", input, &output));
    EXPECT_EQ(expected, output);
    EXPECT_EQ("", jsonlang.lastError());
}

TEST(JsonlangTest, TestEvaluateInvalidSnippet)
{
    const std::string input = readFile("cpp/testdata/invalid.jsonlang");
    const std::string error = readFile("cpp/testdata/invalid.out");

    Jsonlang jsonlang;
    ASSERT_TRUE(jsonlang.init());
    std::string output;
    EXPECT_FALSE(jsonlang.evaluateSnippet("cpp/testdata/invalid.jsonlang",
                                         input, &output));
    EXPECT_EQ("", output);
    EXPECT_EQ(error, jsonlang.lastError());
}

TEST(JsonlangTest, TestEvaluateFile)
{
    const std::string expected = readFile("cpp/testdata/example_golden.json");

    Jsonlang jsonlang;
    ASSERT_TRUE(jsonlang.init());
    std::string output;
    EXPECT_TRUE(jsonlang.evaluateFile("cpp/testdata/example.jsonlang", &output));
    EXPECT_EQ(expected, output);
    EXPECT_EQ("", jsonlang.lastError());
}

TEST(JsonlangTest, TestEvaluateInvalidFile)
{
    const std::string expected = readFile("cpp/testdata/invalid.out");

    Jsonlang jsonlang;
    ASSERT_TRUE(jsonlang.init());
    std::string output;
    EXPECT_FALSE(jsonlang.evaluateFile("cpp/testdata/invalid.jsonlang", &output));
    EXPECT_EQ("", output);
    EXPECT_EQ(expected, jsonlang.lastError());
}

TEST(JsonlangTest, TestAddImportPath)
{
    const std::string expected = readFile("cpp/testdata/importing_golden.json");

    Jsonlang jsonlang;
    ASSERT_TRUE(jsonlang.init());
    jsonlang.addImportPath("cpp/testdata");
    std::string output;
    EXPECT_TRUE(jsonlang.evaluateFile("cpp/testdata/importing.jsonlang", &output));
    EXPECT_EQ(expected, output);
    EXPECT_EQ("", jsonlang.lastError());
}

}  // namespace jsonlang
