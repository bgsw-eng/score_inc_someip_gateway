# Minimal wrapper to re-export gRPC's native Bazel targets
# gRPC's WORKSPACE file provides its own dependency management
# This simply aliases the key targets needed by Score_App

alias(
    name = "grpc++_public",
    actual = "@com_github_grpc_grpc//src/cpp:grpc++",
    visibility = ["//visibility:public"],
)

alias(
    name = "grpc_cc_proto",
    actual = "@com_github_grpc_grpc//bazel:grpc_cc_proto",
    visibility = ["//visibility:public"],
)

alias(
    name = "grpc_cpp_plugin",
    actual = "@com_github_grpc_grpc//src/compiler:grpc_cpp_plugin",
    visibility = ["//visibility:public"],
)
