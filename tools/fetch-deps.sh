#!/bin/bash
# Fetch the exact public headers that match Opal.app's private libdepthai-core build
# (Luxonis depthai-core "audioproc" branch, v2.15.1, bootloader bb9e4c86...).
set -e
cd "$(dirname "$0")"
mkdir -p vendor && cd vendor
if [ ! -d depthai-core ]; then
  git clone -q --branch audioproc https://github.com/luxonis/depthai-core.git depthai-core
  git -C depthai-core checkout -q 703eda64e9b5262d7fbc57ce5622880aac5fb0cb
  git -C depthai-core submodule update --init shared/depthai-shared shared/depthai-bootloader-shared
  git -C depthai-core/shared/depthai-shared checkout -q 74e11ebeca9e7441f589cf957713bd998f5918ac
  git -C depthai-core/shared/depthai-bootloader-shared checkout -q 126f344155346836a6a2d67b0993c161b6b48729
fi
dl() { [ -d "$1" ] || { mkdir -p "$1" && curl -sfL "$2" | tar xz -C "$1" --strip-components 1; }; }
dl json   https://github.com/nlohmann/json/archive/v3.9.1.tar.gz
dl xlink  https://github.com/luxonis/XLink/archive/b7d8e6009ceb5b2278212e3f2a57f5f76e8df5d9.tar.gz
dl libnop https://github.com/luxonis/libnop/archive/ec8f75aa4df3454f73b5d7a7fe0680f3701b1744.tar.gz
mkdir -p gen/depthai/build && printf '#pragma once\n' > gen/depthai/build/config.hpp
echo "deps ready in $(pwd)"
