// The GPU and SPU source headers are shared with C++ translation units
// (runtime/src/main.cpp includes source_gpu_runtime.h). Building this file as
// C++ fails on any C-only construct in that header chain, such as a bare
// _Static_assert, without needing a full runtime build.
#include "source_gpu_runtime.h"
#include "source_gpu_command_projection.h"
#include "source_gpu_command_timing.h"
#include "spu_gauss.h"

int main() { return 0; }
