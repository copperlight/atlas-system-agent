#!/bin/bash

set -ex

BUILD_DIR=cmake-build

export CC=gcc-11
export CXX=g++-11

echo "==== configure default profile ===="
rm -f $HOME/.conan/profiles/default
conan profile new default --detect
conan profile update settings.compiler.libcxx=libstdc++11 default

echo "==== install required dependencies ===="
conan install . --build --install-folder $BUILD_DIR --profile no-omit-frame-pointer

echo "==== install source dependencies ===="
conan source .

pushd $BUILD_DIR || exit 1
echo "==== generate build files ===="
cmake -DCMAKE_BUILD_TYPE=Release -DTITUS_SYSTEM_SERVICE="ON" ..

echo "==== build ===="
cmake --build .

echo "==== test ===="
GTEST_COLOR=1 ctest --verbose
popd || exit 1
