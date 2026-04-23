"""
BUILD file for gRPC - Builds shared libraries from source using cmake
Generates libgrpc.so, libgrpc++.so, and libprotobuf.so
"""

load("@rules_foreign_cc//foreign_cc:defs.bzl", "cmake")

cmake(
    name = "grpc_shared",
    cache_entries = {
        "CMAKE_BUILD_TYPE": "Release",
        "gRPC_BUILD_TESTS": "OFF",
        "gRPC_BUILD_GRPCPP_PLUGIN": "OFF",
        "gRPC_BUILD_CODEGEN": "OFF",
        "gRPC_INSTALL": "ON",
        "BUILD_SHARED_LIBS": "ON",
        "protobuf_BUILD_TESTS": "OFF",
        "protobuf_BUILD_PROTOC": "OFF",
    },
    generate_args = ["-GNinja"],
    lib_source = "@com_github_grpc_grpc//:all",
    out_include_dir = "include",
    out_shared_libs = [
        "libgrpc.so",
        "libgrpc.so.29",
        "libgrpc++.so",
        "libgrpc++.so.1",
        "libgrpc_unsecure.so",
        "libgrpc_unsecure.so.29",
        "libgrpc++_unsecure.so",
        "libgrpc++_unsecure.so.1",
        "libprotobuf.so",
        "libprotobuf.so.32",
        "libabsl_base.so",
        "libabsl_synchronization.so",
        "libabsl_hash.so",
        "libabsl_log_internal_message.so",
        "libabsl_strings.so",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "grpc_shared_public",
    hdrs = glob([
        "include/grpcpp/**/*.h",
        "include/grpc/**/*.h",
        "include/google/protobuf/**/*.h",
        "include/absl/**/*.h",
    ]),
    includes = ["include"],
    shared_libs = [
        ":grpc_shared",
    ],
    visibility = ["//visibility:public"],
)
