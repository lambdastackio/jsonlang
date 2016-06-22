package(default_visibility = ["//visibility:public"])

load(
    "@bazel_tools//tools/build_defs/jsonlang:jsonlang.bzl",
    "jsonlang_to_json_test",
)

jsonlang_to_json_test(
    name = "example_test",
    src = "example.jsonlang",
    golden = "example_golden.json",
    jsonlang = "//cmd:jsonlang",
    std = "//stdlib",
)

jsonlang_to_json_test(
    name = "importing_test",
    src = "importing.jsonlang",
    golden = "importing_golden.json",
    imports = ["."],
    jsonlang = "//cmd:jsonlang",
    std = "//stdlib",
)

jsonlang_to_json_test(
    name = "invalid_test",
    src = "invalid.jsonlang",
    golden = "invalid.out",
    error = 1,
    jsonlang = "//cmd:jsonlang",
    std = "//stdlib",
)

filegroup(
    name = "testdata",
    srcs = [
        "example.jsonlang",
        "example_golden.json",
        "importing.jsonlang",
        "importing_golden.json",
        "invalid.jsonlang",
        "invalid.out",
    ],
)
