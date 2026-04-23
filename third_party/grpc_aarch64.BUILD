"""
BUILD file for gRPC cross-compiled for aarch64
Builds libgrpc.so and libgrpc++.so from source using CMake
"""

load("@rules_foreign_cc//foreign_cc:defs.bzl", "cmake")

cmake(
    name = "grpc_aarch64",
    build_args = [
        "--parallel",
        "$(nproc)",
    ],
    build_data = [
        "@com_github_grpc_grpc//:all",
    ],
    cache_entries = {
        "CMAKE_BUILD_TYPE": "Release",
        "gRPC_INSTALL": "ON",
        "gRPC_BUILD_TESTS": "OFF",
        "gRPC_BUILD_GRPCPP_PLUGIN": "ON",
        "ABSL_ENABLE_INSTALL": "ON",
    },
    env = {
        "CC": "aarch64-unknown-linux-gnu-gcc",
        "CXX": "aarch64-unknown-linux-gnu-g++",
        "AR": "aarch64-unknown-linux-gnu-ar",
        "RANLIB": "aarch64-unknown-linux-gnu-ranlib",
    },
    lib_source = "@com_github_grpc_grpc//:all",
    out_lib_dir = "lib",
    out_shared_libs = [
        "libgrpc.so.34",
        "libgrpc++.so.1",
        "libabsl_*.so",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "grpc++_public_aarch64",
    hdrs = glob([
        "include/grpcpp/**/*.h",
        "include/grpc/**/*.h",
        "include/absl/**/*.h",
    ]),
    includes = ["include"],
    shared_libs = [":grpc_aarch64"],
    visibility = ["//visibility:public"],
)
