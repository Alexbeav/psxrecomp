# Counted MIPS relocation containers

`tools/counted_mips_relocations.py` reads image containers with this layout:

| Area | Little-endian words |
| --- | --- |
| Header | Member count |
| Records | Image offset, image size, relocation-stream offset for each member |
| Images | Concatenated image bytes |
| Streams | Two-bit tagged relocation words, each stream ending in `0xFFFFFFFF` |

Image offsets are relative to the image area. Stream offsets are relative to
the area following the **sum** of the image sizes. Both areas must partition
exactly; overlapping members, invalid targets, duplicate writes, missing
terminators and unaccounted bytes are rejected.

The tag's low two bits select the operation; the remaining bits are the aligned
image offset. Offset zero is valid. Kind 1 consumes an additional addend word.

| Kind | Operation |
| --- | --- |
| 0 | Add the image's RAM base to an absolute 32-bit word |
| 1 | Preserve the instruction's upper half; insert `(base + addend + 0x8000) >> 16`, with 32-bit wrapping |
| 2 | Preserve the instruction's upper half; add the base to its low half |
| 3 | Preserve the opcode; relocate the masked 26-bit jump target |

```python
from counted_mips_relocations import parse, relocate

members = parse(container_bytes)
image = relocate(members[0], independently_verified_image_base)
```

This is a separate contract from `mips_tagged_relocations.py`: that format's
leading image-size word and full-instruction jump addition do not describe
these containers. Neither reader decompresses data.

The container has **no destination addresses**. `relocate` takes an aligned
KSEG0 main-RAM image base; this is not the allocation base of a container whose
headers remain in memory. Callers must establish placement and executable/data
boundaries from the original loader before constructing native AOT producers.
The original-disc AOT pipeline accepts `counted_relocated_members` with
`containers: [{file, sha256, members: [...]}]`. Each member pins its `index`,
`image_size`, `relocation_count` and nonempty `placements: [{load_addr, ...}]`.
Every member must be classified, including an explicit reason for exclusions.
The placement is the image address, which can differ from the container's
allocation address by the header size. Native emission uses relocated original
bytes and the normal guarded dispatcher. A newly encountered heap placement
continues through the interpreter until its own native variant is established.

`entry_pointer_offsets` declares image-relative callback fields proven by the
loader's consumers. Reading those fields after relocation lets the same export
declaration apply at multiple bases. Null callbacks are ignored; unaligned,
out-of-image and excluded required entries fail extraction. Script pointers and
other data fields must not be nominated as MIPS entry points merely because they
look like addresses.

The generic disc discoverer now recognizes complete counted containers and
writes `<output>.relocatable.json` alongside its positioned recipes. The sidecar
records original hashes, member extents, sizes and relocation counts with
`placement_required: true`. It does not infer a heap address or claim native
coverage. This exposes unresolved loader work during initial discovery instead
of waiting for gameplay to reveal interpreter callbacks.

The implementation was compared byte for byte with the owned USA MediEvil II
relocation routine (`0x80078328`) for 24 modules at two distinct RAM bases:
48 complete-image comparisons passed. Owned bytes and diagnostic captures are
not part of this repository. Synthetic contract tests cover multiple members,
all four relocation kinds, opcode preservation and malformed inputs:

```sh
python -m unittest discover -s tools/tests -p test_counted_mips_relocations.py
python -m unittest discover -s tools/tests -p test_counted_relocation_discovery.py
python -m unittest discover -s tools/tests -p test_aot_overlay_pipeline.py
```
