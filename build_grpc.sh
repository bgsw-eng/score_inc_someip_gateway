#!/bin/bash
set -e

WORKSPACE=/workspaces/score_inc_someip_gateway
GRPC_SRC=/workspaces/grpc

cd $WORKSPACE

# Full clean
rm -rf artifacts/grpc-shared artifacts/grpc-host deploy/grpc-aarch64

echo "=== Step 1: Build host tools ==="
cmake -S $GRPC_SRC \
      -B artifacts/grpc-host \
      -GNinja \
      -DCMAKE_BUILD_TYPE=Release \
      -DgRPC_BUILD_TESTS=OFF \
      -DgRPC_ABSL_PROVIDER=module \
      -DgRPC_CARES_PROVIDER=module \
      -DgRPC_PROTOBUF_PROVIDER=module \
      -DgRPC_RE2_PROVIDER=module \
      -DgRPC_SSL_PROVIDER=module \
      -DgRPC_ZLIB_PROVIDER=module

cmake --build artifacts/grpc-host -j$(nproc) --target grpc_cpp_plugin protoc

echo "=== Step 2: Cross compile for aarch64 ==="
cmake -S $GRPC_SRC \
      -B artifacts/grpc-shared \
      -GNinja \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=ON \
      -DCMAKE_SYSTEM_NAME=Linux \
      -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
      -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc-11 \
      -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++-11 \
      -DCMAKE_SYSROOT=/usr/aarch64-linux-gnu \
      -DCMAKE_FIND_ROOT_PATH=/usr/aarch64-linux-gnu \
      -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
      -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
      -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
      -DCMAKE_INSTALL_PREFIX=$(pwd)/deploy/grpc-aarch64 \
      -DgRPC_INSTALL=ON \
      -DgRPC_BUILD_TESTS=OFF \
      -DgRPC_BUILD_CODEGEN=ON \
      -DgRPC_BUILD_GRPC_CPP_PLUGIN=OFF \
      -DgRPC_ABSL_PROVIDER=module \
      -DgRPC_CARES_PROVIDER=module \
      -DgRPC_PROTOBUF_PROVIDER=module \
      -Dprotobuf_BUILD_TESTS=OFF \
      -DgRPC_RE2_PROVIDER=module \
      -DgRPC_SSL_PROVIDER=module \
      -DgRPC_ZLIB_PROVIDER=module \
      -D_gRPC_CPP_PLUGIN=$(pwd)/artifacts/grpc-host/grpc_cpp_plugin \
      -D_gRPC_PROTOBUF_PROTOC=$(pwd)/artifacts/grpc-host/third_party/protobuf/protoc

cmake --build artifacts/grpc-shared -j$(nproc)
cmake --install artifacts/grpc-shared

echo "=== Step 3: Verify GLIBC versions ==="
echo "--- Checking all libs ---"
for lib in deploy/grpc-aarch64/lib/*.so*; do
  result=$(strings "$lib" 2>/dev/null | grep "GLIBC_" | sort -V | tail -1)
  echo "$lib → $result"
done
