/* Accepted authored ac56b624 controller/clean arithmetic; observation limits
 * remain in source_mdec_fifo.h and docs/source-mdec-profile.md. Modern default
 * SIMD decode and snapshot prefix are independent. No reference body copied. */
#include "source_mdec_fifo.h"
static SourceMDEC source_mdec;
static int source_mdec_enabled;
static int16_t source_cr[64],source_cb[64];
/* Clean derivation: PSX-SPX "MDEC Decompression" and the authored M1-M11
 * observations. No reference decoder code or prior decode bodies were used.
 * Arithmetic and receipt identities: docs/testing/MDEC_CLEAN_DECODE.md. */
static int source_clean_floor_div(int value, int divisor) {
    int quotient = value / divisor;
    return quotient - (value % divisor < 0);
}
static int source_clean_clamp_int(int value, int lo, int hi) {
    return value < lo ? lo : value > hi ? hi : value;
}
static int source_clean_sign_extend_10(uint16_t value) {
    int low = value & 1023;
    return low >= 512 ? low - 1024 : low;
}
static int source_clean_coefficient_sign(int value) {
    return (value > 0) - (value < 0);
}
static int source_clean_mask9_clamp_s8(int value) {
    int low = (int)((unsigned)value & 511u);
    if (low >= 256) low -= 512;
    return source_clean_clamp_int(low, -128, 127);
}
/* Inverse of the zigzag array in PSX-SPX "MDEC Decompression". */
static const uint8_t source_clean_coefficient_order[64] = {
    0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,
    12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
};
static void source_clean_idct_block(int16_t block[64]) {
    int16_t next[64];
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned y = 0; y < 8; ++y) {
            for (unsigned x = 0; x < 8; ++x) {
                int sum = 0;
                for (unsigned z = 0; z < 8; ++z)
                    sum += block[y + z * 8] * source_clean_floor_div(mdec.scale[x + z * 8], 8);
                next[x + y * 8] = (int16_t)source_clean_floor_div(sum + 16384, 32768);
            }
        }
        memcpy(block, next, sizeof(next));
    }
    for (unsigned i = 0; i < 64; ++i) block[i] = (int16_t)source_clean_mask9_clamp_s8(block[i]);
}
static int source_clean_decode_rle_block_from(const uint16_t *encoded, int16_t *block,
                               const uint8_t *quant, uint32_t *pos, uint32_t end) {
    while (*pos < end && encoded[*pos] == 0xfe00u) ++*pos;
    if (*pos == end) return 0;
    memset(block, 0, 64 * sizeof(*block));
    uint16_t entry = encoded[(*pos)++];
    unsigned q = entry >> 10;
    int product = source_clean_sign_extend_10(entry) * quant[0];
    int dc = quant[0] ? 16 * product - 8 * source_clean_coefficient_sign(product) : 32 * source_clean_sign_extend_10(entry);
    block[0] = (int16_t)source_clean_clamp_int(dc, -16384, 16383);
    unsigned index = 0;
    while (*pos < end) {
        entry = encoded[(*pos)++];
        index += (entry >> 10) + 1;
        if (index >= 64) break;
        int level = source_clean_sign_extend_10(entry);
        product = level * quant[index] * (int)q;
        int value = q && quant[index] ? 16 * source_clean_floor_div(product, 8) - 8 * source_clean_coefficient_sign(product) : 32 * level;
        block[source_clean_coefficient_order[index]] = (int16_t)source_clean_clamp_int(value, -16384, 16383);
    }
    source_clean_idct_block(block);
    return 1;
}
static unsigned source_clean_mono_bytes(const int16_t *block, unsigned depth, unsigned is_signed,
                           uint8_t *output) {
    for (unsigned i = 0; i < 64; ++i) {
        unsigned value = (unsigned)(block[i] + 128);
        if (depth == 1) output[i] = (uint8_t)(value ^ (is_signed ? 128u : 0u));
        else {
            unsigned nibble = (unsigned)source_clean_clamp_int((int)(value + 8) / 16, 0, 15);
            nibble ^= is_signed ? 8u : 0u;
            if ((i & 1u) == 0) output[i / 2] = (uint8_t)nibble;
            else output[i / 2] |= (uint8_t)(nibble << 4);
        }
    }
    return depth == 1 ? 64 : 32;
}
static unsigned source_clean_color_pixel(int y, int cb, int cr, uint32_t command, uint8_t *out) {
    int r = source_clean_mask9_clamp_s8(y + source_clean_floor_div(359 * cr + 128, 256)) + 128;
    int b = source_clean_mask9_clamp_s8(y + source_clean_floor_div(454 * cb + 128, 256)) + 128;
    int green = source_clean_floor_div(source_clean_floor_div(-88 * cb, 32) + source_clean_floor_div(-183 * cr, 32) + 4, 8);
    int g = source_clean_mask9_clamp_s8(y + green) + 128;
    unsigned is_signed = (command >> 26) & 1;
    if (((command >> 27) & 3) == 2) {
        unsigned toggle = is_signed ? 128u : 0u;
        out[0] = (uint8_t)((unsigned)r ^ toggle);
        out[1] = (uint8_t)((unsigned)g ^ toggle);
        out[2] = (uint8_t)((unsigned)b ^ toggle);
        return 3;
    }
    unsigned pixel = (unsigned)source_clean_clamp_int((r + 4) / 8, 0, 31)
                   | ((unsigned)source_clean_clamp_int((g + 4) / 8, 0, 31) << 5)
                   | ((unsigned)source_clean_clamp_int((b + 4) / 8, 0, 31) << 10);
    pixel ^= is_signed ? 0x4210u : 0u;
    pixel |= ((command >> 25) & 1u) << 15;
    out[0] = (uint8_t)pixel; out[1] = (uint8_t)(pixel >> 8);
    return 2;
}
static unsigned source_decode_block(void *context, uint32_t command, unsigned block,
                                    const uint16_t *encoded, unsigned count, uint32_t *pixels) {
    (void)context;
    unsigned depth = (command >> 27) & 3;
    uint32_t pos = 0;
    int16_t decoded[64];
    if (!source_clean_decode_rle_block_from(encoded, decoded, depth >= 2 && block < 2 ? mdec.uv_quant : mdec.y_quant,
                               &pos, count)) return 0;
    uint8_t output[192];
    unsigned bytes;
    if (depth < 2) bytes = source_clean_mono_bytes(decoded, depth, (command >> 26) & 1, output);
    else if (block < 2) {
        memcpy(block == 0 ? source_cr : source_cb, decoded, sizeof(decoded));
        return 0;
    } else {
        if (block == 2u) {
            mdec_last_color_decode_cycle = psx_cycle_count;
            g_mdec_decode_count++;
        }
        unsigned origin_x = ((block - 2) & 1) * 8;
        unsigned origin_y = ((block - 2) >> 1) * 8;
        bytes = 0;
        for (unsigned y = 0; y < 8; ++y) {
            for (unsigned x = 0; x < 8; ++x) {
                unsigned c = ((y + origin_y) / 2) * 8 + (x + origin_x) / 2;
                bytes += source_clean_color_pixel(decoded[y * 8 + x], source_cb[c], source_cr[c], command, output + bytes);
            }
        }
    }
    for (unsigned i = 0; i < bytes / 4; ++i)
        pixels[i] = (uint32_t)output[i * 4] | ((uint32_t)output[i * 4 + 1] << 8)
                  | ((uint32_t)output[i * 4 + 2] << 16) | ((uint32_t)output[i * 4 + 3] << 24);
    return bytes / 4;
}
static void source_table_word(void *context, unsigned kind, unsigned index, uint32_t value) {
    (void)context;
    if (kind == MDEC_CMD_SET_QUANT) {
        for (unsigned i = 0; i < 4; ++i) {
            unsigned at = (index + i) & 127;
            (at < 64 ? mdec.y_quant : mdec.uv_quant)[at & 63] = (uint8_t)(value >> (i * 8));
        }
    } else if (kind == MDEC_CMD_SET_SCALE) {
        for (unsigned i = 0; i < 2; ++i) {
            unsigned half = (value >> (i * 16)) & 65535u;
            mdec.scale[(index + i) & 63] = (int16_t)(half >= 32768 ? (int)half - 65536 : (int)half);
        }
    }
}

static void source_mdec_require(void){
    if(source_mdec.error){fprintf(stderr,"[mdec-source] unqualified operation or decode\n");exit(2);}
}
int mdec_source_active(void){return source_mdec_enabled;}
void mdec_source_advance(uint32_t clocks){
    if(source_mdec_enabled){source_mdec_run(&source_mdec,clocks);source_mdec_require();}
}
uint32_t mdec_source_dma_read(uint32_t *word_offset){
    if(!source_mdec_enabled)abort();
    uint32_t value=source_mdec_read(&source_mdec,1,word_offset);source_mdec_require();return value;
}




#define SOURCE_MDEC_TAIL_BYTES 913u
#define SOURCE_MDEC_SCALARS(X) \
    X(in_at) X(in_count) X(out_at) X(out_count) X(command) X(control) \
    X(phase) X(busy) X(coefficient) X(encoded_count) X(block) \
    X(pixel_count) X(pixel_at) X(quant_index) X(matrix_index)
static int mdec_source_snap_emit(PstW *w) {
    SourceMDEC *s=&source_mdec;
    for (unsigned i=0;i<32;i++) if (!pst_w_u32(w,s->in[i])) return 0;
    for (unsigned i=0;i<32;i++) if (!pst_w_u32(w,s->out[i])) return 0;
    for (unsigned i=0;i<48;i++) if (!pst_w_u32(w,s->pixels[i])) return 0;
#define EMIT(f) if (!pst_w_u32(w,s->f)) return 0;
    SOURCE_MDEC_SCALARS(EMIT)
#undef EMIT
    if (!pst_w_i32(w,s->credit) || !pst_w_u16(w,s->remaining)) return 0;
    for (unsigned i=0;i<64;i++) if (!pst_w_u16(w,s->encoded[i])) return 0;
    if (!pst_w_u8(w,s->row) || !pst_w_u8(w,s->word_in_row) ||
        !pst_w_u8(w,s->row_words) || !pst_w_i32(w,s->error)) return 0;
    for (unsigned i=0;i<64;i++) if (!pst_w_i16(w,source_cr[i])) return 0;
    for (unsigned i=0;i<64;i++) if (!pst_w_i16(w,source_cb[i])) return 0;
    return 1;
}
static int mdec_source_snap_parse(PstR *r, int apply) {
    SourceMDEC s=source_mdec;
    int16_t cr[64],cb[64];int32_t error;
    for (unsigned i=0;i<32;i++) if (!pst_r_u32(r,&s.in[i])) return 0;
    for (unsigned i=0;i<32;i++) if (!pst_r_u32(r,&s.out[i])) return 0;
    for (unsigned i=0;i<48;i++) if (!pst_r_u32(r,&s.pixels[i])) return 0;
#define READ(f) if (!pst_r_u32(r,&s.f)) return 0;
    SOURCE_MDEC_SCALARS(READ)
#undef READ
    if (!pst_r_i32(r,&s.credit) || !pst_r_u16(r,&s.remaining)) return 0;
    for (unsigned i=0;i<64;i++) if (!pst_r_u16(r,&s.encoded[i])) return 0;
    if (!pst_r_u8(r,&s.row) || !pst_r_u8(r,&s.word_in_row) ||
        !pst_r_u8(r,&s.row_words) || !pst_r_i32(r,&error)) return 0;
    for (unsigned i=0;i<64;i++) if (!pst_r_i16(r,&cr[i])) return 0;
    for (unsigned i=0;i<64;i++) if (!pst_r_i16(r,&cb[i])) return 0;
    if (s.in_at>31 || s.out_at>31 || s.in_count>32 || s.out_count>32 ||
        s.phase>SMDEC_OUTPUT || s.busy>1 || s.coefficient>64 || s.encoded_count>64 ||
        s.credit < -(int32_t)s.block_cycles || s.credit > 128 ||
        s.block>5 || s.pixel_count>48 || s.pixel_at>48 ||
        s.quant_index>127 || s.matrix_index>63 || error) return 0;
    s.error=error;
    if (apply) {source_mdec=s;memcpy(source_cr,cr,sizeof cr);memcpy(source_cb,cb,sizeof cb);}
    return 1;
}


static void mdec_source_profile_init(void) {
    source_mdec_enabled=0;
    memset(&source_mdec,0,sizeof(source_mdec));
    const char *mode=getenv("PSX_MDEC_SOURCE_MODEL");
    if (!mode || !*mode) return;
    if (strcmp(mode,"octoshock-2.3") && strcmp(mode,"nymashock-1.29.0") &&
        strcmp(mode,"nymashock-1.32.1") && strcmp(mode,"octoshock-2.4.2") &&
        strcmp(mode,"octoshock-2.6.2")) {
        fprintf(stderr,"[mdec-source] unknown model\n"); exit(2);
    }
    source_mdec_enabled=1;
    memset(mdec.y_quant,0,sizeof(mdec.y_quant));
    memset(mdec.uv_quant,0,sizeof(mdec.uv_quant));
    memset(mdec.scale,0,sizeof(mdec.scale));
    memset(source_cr,0,sizeof(source_cr));memset(source_cb,0,sizeof(source_cb));
    source_mdec_power(&source_mdec,source_decode_block,source_table_word,NULL);
    /* ---- Block cost ------------------------------------------------------
     * The controller charges per macroblock, not per word. When a block
     * completes, its halfword handler returns the profile's block cost and the
     * data phase subtracts that from the credit; the output phase then waits
     * for the credit to climb back above zero, so the cost is exactly the busy
     * window the status word shows for one block. The same number is written
     * into a snapshot and compared on restore, which is why it is part of the
     * profile identity rather than a tuning constant.
     *
     * Two figures are observed across the admitted profiles: the oldest one
     * keeps the 474 cycles that the power-on state installs, and every other
     * admitted profile charges 512. The 38-cycle difference sits inside the
     * observed busy window, so a route that crosses a block boundary at a
     * fixed return sees it. Only the figure is selected here; the accounting
     * stays in the FIFO controller. */
    enum { MDEC_LATER_PROFILE_BLOCK_CYCLES = 512 };
    if (strcmp(mode, "octoshock-2.3") != 0)
        source_mdec.block_cycles = MDEC_LATER_PROFILE_BLOCK_CYCLES;
}
static uint32_t mdec_source_tail_bytes(void) {
    return source_mdec_enabled ? SOURCE_MDEC_TAIL_BYTES : 0u;
}
static int mdec_source_tail_write(PstW *w) {
    if (!source_mdec_enabled) return 1;
    return pst_w_u32(w,0x31464d53u) && pst_w_u32(w,source_mdec.block_cycles) &&
           mdec_source_snap_emit(w);
}
static int mdec_source_tail_read(const uint8_t *p,uint32_t len,int apply) {
    if (!source_mdec_enabled) return len==0;
    if (!p || len!=SOURCE_MDEC_TAIL_BYTES) return 0;
    PstR r;uint32_t magic,cost;pst_r_init(&r,p,len);
    return pst_r_u32(&r,&magic) && magic==0x31464d53u &&
           pst_r_u32(&r,&cost) && cost==source_mdec.block_cycles &&
           mdec_source_snap_parse(&r,apply) && r.p==r.end;
}
