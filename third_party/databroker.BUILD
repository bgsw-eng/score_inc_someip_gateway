# BUILD file for KUKSA databroker archive providing proto compilation targets

load("@rules_proto//proto:defs.bzl", "proto_library")
load("@rules_cc//cc:cc_proto_library.bzl", "cc_proto_library")
load("@com_github_grpc_grpc//bazel:cc_grpc_library.bzl", "cc_grpc_library")

# Databroker v1 types proto
proto_library(
    name = "types_proto",
    srcs = ["proto/sdv/databroker/v1/types.proto"],
    visibility = ["//visibility:public"],
)

cc_proto_library(
    name = "types_cc_proto",
    deps = [":types_proto"],
    visibility = ["//visibility:public"],
)

# Databroker v1 collector proto  
proto_library(
    name = "collector_proto",
    srcs = ["proto/sdv/databroker/v1/collector.proto"],
    deps = [":types_proto"],
    visibility = ["//visibility:public"],
)

cc_proto_library(
    name = "collector_cc_proto",
    deps = [":collector_proto"],
    visibility = ["//visibility:public"],
)

cc_grpc_library(
    name = "collector_cc_grpc",
    srcs = [":collector_proto"],
    grpc_only = True,
    deps = [":collector_cc_proto"],
    visibility = ["//visibility:public"],
)

# KUKSA VAL v1 types proto
proto_library(
    name = "val_types_proto",
    srcs = ["proto/kuksa/val/v1/types.proto"],
    visibility = ["//visibility:public"],
)

cc_proto_library(
    name = "val_types_cc_proto",
    deps = [":val_types_proto"],
    visibility = ["//visibility:public"],
)

# KUKSA VAL v1 service proto
proto_library(
    name = "val_proto",
    srcs = ["proto/kuksa/val/v1/val.proto"],
    deps = [":val_types_proto"],
    visibility = ["//visibility:public"],
)

cc_proto_library(
    name = "val_cc_proto",
    deps = [":val_proto"],
    visibility = ["//visibility:public"],
)

cc_grpc_library(
    name = "val_cc_grpc",
    srcs = [":val_proto"],
    grpc_only = True,
    deps = [":val_cc_proto"],
    visibility = ["//visibility:public"],
)

# Convenience aggregated target with all protos
cc_library(
    name = "databroker_cc",
    visibility = ["//visibility:public"],
    deps = [
        ":collector_cc_grpc",
        ":collector_cc_proto",
        ":types_cc_proto",
        ":val_cc_grpc",
        ":val_cc_proto",
        ":val_types_cc_proto",
    ],
)
