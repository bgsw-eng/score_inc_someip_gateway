# BUILD file for local gRPC source tree
# Uses headers from cloned gRPC v1.70.1 source

filegroup(
    name = "all",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)

cc_library(
    name = "absl_headers",
    visibility = ["//visibility:public"],
    deps = [
        "@com_github_absl//absl/algorithm:algorithm",
        "@com_github_absl//absl/algorithm:container",
        "@com_github_absl//absl/base:base",
        "@com_github_absl//absl/base:core_headers",
        "@com_github_absl//absl/cleanup:cleanup",
        "@com_github_absl//absl/container:flat_hash_map",
        "@com_github_absl//absl/container:flat_hash_set",
        "@com_github_absl//absl/functional:any_invocable",
        "@com_github_absl//absl/memory:memory",
        "@com_github_absl//absl/meta:type_traits",
        "@com_github_absl//absl/status:status",
        "@com_github_absl//absl/status:statusor",
        "@com_github_absl//absl/strings:strings",
        "@com_github_absl//absl/synchronization:synchronization",
        "@com_github_absl//absl/time:time",
        "@com_github_absl//absl/types:optional",
        "@com_github_absl//absl/types:span",
        "@com_github_absl//absl/utility:utility",
    ],
)

cc_library(
    name = "protobuf_headers",
    visibility = ["//visibility:public"],
    deps = ["@com_github_protobuf//:protobuf_headers"],
)

cc_library(
    name = "grpc++_public",
    hdrs = glob([
        "include/grpc/**/*.h",
        "include/grpc++/**/*.h",
        "include/grpcpp/**/*.h",
    ]),
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [
        ":grpc_public",
        ":absl_headers",
        ":protobuf_headers",
        "@//third_party:grpc_prebuilt_runtime",
    ],
    linkopts = ["-Wl,--allow-shlib-undefined"],
)

cc_library(
    name = "grpc_public",
    hdrs = glob([
        "include/grpc/**/*.h",
    ]),
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [
        ":absl_headers",
        ":protobuf_headers",
    ],
)

cc_library(
    name = "grpcpp_reflection",
    hdrs = glob([
        "include/grpcpp/ext/*.h",
    ]),
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [":grpc++_public"],
)

# protobuf runtime and headers vendored under the local gRPC checkout.
cc_library(
    name = "protobuf",
    visibility = ["//visibility:public"],
    deps = [
        "@com_github_protobuf//:protobuf",
    ],
)
