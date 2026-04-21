#!/bin/bash
# *******************************************************************************
# Copyright (c) 2025 Contributors to the Eclipse Foundation
#
# See the NOTICE file(s) distributed with this work for additional
# information regarding copyright ownership.
#
# This program and the accompanying materials are made available under the
# terms of the Apache License Version 2.0 which is available at
# https://www.apache.org/licenses/LICENSE-2.0
#
# SPDX-License-Identifier: Apache-2.0
# *******************************************************************************
#
# Sets up gRPC/Protobuf/Abseil headers and generates KUKSA proto stubs.
# Idempotent - safe to run multiple times.
# Called automatically via postCreateCommand in devcontainer.json.

set -euo pipefail

GRPC_INCLUDE="/workspaces/grpc/include"
GENERATED_DIR="/workspaces/Score_App/third_party/com_kukavalhal_databroker/generated"
PROTO_SRC=""

echo "[setup_grpc_deps] Starting gRPC dependency setup..."

# --- Step 1: Symlink abseil headers into /workspaces/grpc/include ---
if [ ! -e "${GRPC_INCLUDE}/absl" ]; then
    echo "[setup_grpc_deps] Symlinking abseil headers..."
    ln -s /usr/include/absl "${GRPC_INCLUDE}/absl"
else
    echo "[setup_grpc_deps] abseil symlink already exists, skipping."
fi

# --- Step 2: Symlink protobuf headers into /workspaces/grpc/include ---
if [ ! -e "${GRPC_INCLUDE}/google" ]; then
    echo "[setup_grpc_deps] Symlinking protobuf headers..."
    ln -s /usr/include/google "${GRPC_INCLUDE}/google"
else
    echo "[setup_grpc_deps] protobuf symlink already exists, skipping."
fi

# --- Step 3: Generate KUKSA databroker proto headers ---
# Find proto source from bazel cache or fallback to known path
BAZEL_CACHE_BASE="/var/cache/bazel"
PROTO_CACHE=$(find "${BAZEL_CACHE_BASE}" -maxdepth 5 \
    -path "*/external/+_repo_rules+com_kukavalhal_databroker/proto" \
    -type d 2>/dev/null | head -1)

if [ -n "${PROTO_CACHE}" ]; then
    PROTO_SRC="${PROTO_CACHE}"
fi

if [ -z "${PROTO_SRC}" ] || [ ! -d "${PROTO_SRC}" ]; then
    echo "[setup_grpc_deps] WARNING: Could not find kuksa proto sources in bazel cache."
    echo "[setup_grpc_deps] Run 'bazel build @com_kukavalhal_databroker//:val_cc_grpc' first to populate the cache, then re-run this script."
    exit 0
fi

if [ ! -f "${GENERATED_DIR}/kuksa/val/v1/val.grpc.pb.h" ]; then
    echo "[setup_grpc_deps] Generating KUKSA databroker proto headers..."
    mkdir -p "${GENERATED_DIR}"
    protoc -I"${PROTO_SRC}" \
        --cpp_out="${GENERATED_DIR}" \
        --grpc_out="${GENERATED_DIR}" \
        --plugin=protoc-gen-grpc=/usr/bin/grpc_cpp_plugin \
        "${PROTO_SRC}/kuksa/val/v1/types.proto" \
        "${PROTO_SRC}/kuksa/val/v1/val.proto" \
        "${PROTO_SRC}/sdv/databroker/v1/types.proto" \
        "${PROTO_SRC}/sdv/databroker/v1/collector.proto"
    echo "[setup_grpc_deps] Proto headers generated successfully."
else
    echo "[setup_grpc_deps] Proto headers already generated, skipping."
fi

echo "[setup_grpc_deps] Done."
