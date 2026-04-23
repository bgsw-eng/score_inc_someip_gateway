load("@rules_cc//cc:cc_library.bzl", "cc_library")

cc_library(
    name = "absl_headers",
    hdrs = glob([
        "absl/**/*.h",
    ]),
    includes = ["."],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "protobuf_headers",
    hdrs = glob([
        "google/protobuf/**/*.h",
    ]),
    includes = ["."],
    visibility = ["//visibility:public"],
)
