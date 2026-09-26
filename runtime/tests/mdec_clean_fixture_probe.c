/* Authored fixture adapter. This includes only the new clean implementation. */
#define PSX_NO_DEBUG_TOOLS 1
#include "../src/mdec.c"
uint64_t s_frame_count;
uint64_t psx_cycle_count;
#ifdef _WIN32
#define PROBE_API __declspec(dllexport)
#else
#define PROBE_API
#endif
PROBE_API int mdec_fixture_decode(uint32_t command, const uint8_t *quant,
        const int16_t *scale, const uint16_t *input, unsigned count,
        uint8_t *output, unsigned capacity, unsigned source) {
    static int initialized;
    if (!initialized) { mdec_init(); initialized = 1; }
    mdec_write(4, 0xe0000000u);
    if (count & 1) return -1;
    mdec_write(0, 0x40000001u);
    for (unsigned i = 0; i < 128; i += 4)
        mdec_write(0, (uint32_t)quant[i] | ((uint32_t)quant[i+1]<<8)
            | ((uint32_t)quant[i+2]<<16) | ((uint32_t)quant[i+3]<<24));
    mdec_write(0, 0x60000000u);
    for (unsigned i = 0; i < 64; i += 2)
        mdec_write(0, (uint16_t)scale[i] | ((uint32_t)(uint16_t)scale[i+1]<<16));
    if (!source) {
        mdec_write(0, (command & 0xffff0000u) | (count / 2));
        for (unsigned i = 0; i < count; i += 2)
            mdec_dma_write_word(input[i] | ((uint32_t)input[i+1]<<16));
        unsigned bytes = 0;
        while (mdec_dma_read_ready()) {
            if (bytes + 4 > capacity) return -2;
            uint32_t word = mdec_dma_read_word();
            for (unsigned b = 0; b < 4; ++b) output[bytes++] = (uint8_t)(word >> (b * 8));
        }
        if (mdec_read(4) & (1u<<29)) return -3;
        return (int)bytes;
    }
    /* Exercise the production callback and source table-loading seams. */
    memset(mdec.y_quant, 0, 64); memset(mdec.uv_quant, 0, 64); memset(mdec.scale, 0, 128);
    for (unsigned i = 0; i < 128; i += 4)
        source_table_word(0, 2, i, (uint32_t)quant[i] | ((uint32_t)quant[i+1]<<8)
            | ((uint32_t)quant[i+2]<<16) | ((uint32_t)quant[i+3]<<24));
    for (unsigned i = 0; i < 64; i += 2)
        source_table_word(0, 3, i, (uint16_t)scale[i] | ((uint32_t)(uint16_t)scale[i+1]<<16));
    unsigned pos = 0, block = 0, bytes = 0, depth = (command >> 27) & 3;
    while (pos < count) {
        while (pos < count && input[pos] == 0xfe00) ++pos;
        if (pos == count) break;
        unsigned begin = pos++, coefficient = 0;
        while (pos < count && coefficient < 64) coefficient += (input[pos++] >> 10) + 1;
        uint32_t pixels[48];
        unsigned words = source_decode_block(0, command, depth < 2 ? 2 : block, input+begin, pos-begin, pixels);
        if (depth < 2) {
            if (bytes + words * 4 > capacity) return -4;
            for (unsigned i = 0; i < words * 4; ++i) output[bytes++] = (uint8_t)(pixels[i/4] >> ((i%4)*8));
        } else {
            unsigned bpp = depth == 2 ? 3 : 2;
            if (bytes + 256 * bpp > capacity) return -5;
            if (block >= 2) {
                unsigned ox = ((block-2)&1)*8, oy = ((block-2)>>1)*8;
                for (unsigned i = 0; i < words * 4; ++i) {
                    unsigned pixel = i/bpp;
                    unsigned at = bytes + ((oy+pixel/8)*16+ox+pixel%8)*bpp+i%bpp;
                    output[at] = (uint8_t)(pixels[i/4] >> ((i%4)*8));
                }
            }
            if (++block == 6) { block = 0; bytes += 256*bpp; }
        }
    }
    return (int)bytes;
}
