#!/usr/bin/env bash

cmake -S recompiler -B recompiler/build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build recompiler/build
