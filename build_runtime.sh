#!/usr/bin/env bash

cmake -S runtime    -B runtime/build    -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build runtime/build --target psx-runtime
