# Software texture mask

The default software renderer preserves bit 15 of each nonzero texture pixel.
Bit 15 is the mask bit stored in video memory.
The GP0 E6h force-mask setting can also set that bit.
This rule applies after color modulation and semi-transparent blending.
The destination mask check still rejects a write to a protected pixel.
A zero texture word stays transparent even with force-mask enabled.
A texture word of `0x8000` draws black with its mask bit set.

The shared `put_textured` function applies the rule to textured triangles,
shaded textured triangles, rectangles, and scaled rectangles.
The regression `gpu_sw_texture_mask_test` checks these public software drawing
functions and reads each result through `sw_vram_transfer_out`.
It covers raw and modulated colors, all four blend modes, source and destination
mask combinations, transparent zero, masked black, and software scales 1 and 4.
All test pixels are authored data.

Hardware rule: [PSX-SPX, rendering attributes](https://psx-spx.consoledev.net/ps1/gpu/rendering-attributes/), GP0 E6h.
Task: [PS1B-447](https://youtrack.crosstalkis.com/issue/PS1B-447), ordered step 1.
This correction does not establish complete renderer agreement.
Color arithmetic, texel selection, title replay boundaries, and replay canaries
retain their separate evidence requirements.
