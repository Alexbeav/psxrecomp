# Source polygon traversal order

The optional source-profile triangle walker now visits the two triangle halves
in the order anchored at the selected leftmost core vertex. A top core visits
the upper half first; a middle or bottom core visits the lower half first.
Direction and coverage inside each half are unchanged. This shared walker
feeds both geometry work and the source software renderer. Texture-cache tags
and texture reads overlapping the draw destination observe traversal order.
No command, coordinate, reset, input or comparison guard is relaxed.

CLAIM / DERIVED-FROM: original Octoshock2.3 gpu_polygon.cpp DrawTriangle,
tripart[vo] / tripart[vo^1], upstreama15b31a46bdac27d843d3ebbc5a860012d8452fb.
This is compatibility with that source, not a new hardware timing claim.

The authored fixture covers three core positions and three texture depths,
with separate texture data and texture data overlapping the draw destination.
Each case draws twice with the cache retained. All18 complete final1MiB VRAM
hashes come from unmodified stock2.3 and equal the passive observer's output.
The separately recorded original work provides36 command-work values. The
previous walker fails image and work comparisons; the corrected walker matches
all values atO0 andO2. The oracle uses an authored ROM and RAM program executing
ordinary MMIO, with an explicit completion marker. No retail state is imported.
Run CTest -R tas_gpu_polygon_order to reproduce the native contracts.

Passive Pepsiman diagnosis first observed four extra GPU work units at
clock936685440, then the128-clock DMA completion difference that changes one
BIOS saved-context byte at return1656. Both traced and untraced captures were
identical before this correction. The full movie must establish whether this
correction resolves that boundary; native ending and Tekken regression remain
independent acceptance gates.
