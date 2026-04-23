"""
BUILD file for gRPC - Headers only
For headers and type definitions. 
Note: gRPC cmake build disabled due to complexity of test proto file handling.
When feeder source is ready, can enable cmake rule for .so generation.
"""

cc_library(
    name = "grpc++_public",
    hdrs = glob([
        "include/grpcpp/**/*.h",
        "include/grpc/**/*.h",
        "include/absl/**/*.h",
    ]),
    includes = ["include"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "protobuf",
    hdrs = glob([
        "include/google/protobuf/**/*.h",
        "include/google/protobuf/**/*.inc",
    ]),
    includes = ["include"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "grpc++",
    hdrs = glob(["include/grpcpp/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "grpc_base",
    hdrs = glob(["include/grpc/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "grpc_cc_proto",
    visibility = ["//visibility:public"],
    deps = [":grpc++_public"],
)
