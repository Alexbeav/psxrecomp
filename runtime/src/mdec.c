#include "mdec.h"
#include "psx_align.h"
#include "pst_wire.h"
#include "psx_cycles.h"
#include "source_mdec_fifo.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


extern uint64_t s_frame_count;




static uint64_t mdec_last_color_decode_frame = (uint64_t)0 - 1000u;


static uint64_t mdec_last_color_decode_cycle = (uint64_t)0 - 1000u;

enum {
    MDEC_CMD_NOP = 0,
    MDEC_CMD_DECODE = 1,
    MDEC_CMD_SET_QUANT = 2,
    MDEC_CMD_SET_SCALE = 3
};

enum {
    MDEC_EVT_RESET = 1,
    MDEC_EVT_CTRL_WRITE,
    MDEC_EVT_CMD_BEGIN,
    MDEC_EVT_CMD_DONE,
    MDEC_EVT_DECODE_DONE,
    MDEC_EVT_DMA_IN_START,
    MDEC_EVT_DMA_IN_END,
    MDEC_EVT_DMA_OUT_START,
    MDEC_EVT_DMA_OUT_END,
    MDEC_EVT_OUTPUT_DRAINED,
    MDEC_EVT_READ_UNDERFLOW
};

enum {
    MDEC_STOP_NONE = 0,
    MDEC_STOP_INPUT_END = 1,
    MDEC_STOP_CR = 2,
    MDEC_STOP_CB = 3,
    MDEC_STOP_Y0 = 4,
    MDEC_STOP_Y1 = 5,
    MDEC_STOP_Y2 = 6,
    MDEC_STOP_Y3 = 7
};


typedef struct MDECState {
    uint32_t command;
    uint32_t expected_halfwords;
    uint32_t input_count;
    uint16_t *input;
    uint32_t input_cap;

    uint8_t *output;
    uint32_t output_size;
    uint32_t output_pos;
    uint32_t output_cap;

    uint8_t y_quant[64];
    uint8_t uv_quant[64];
    int16_t scale[64];

    uint8_t output_bit15;
    uint8_t output_signed;
    uint8_t output_depth;
    uint8_t current_block;
    uint8_t busy;
    uint8_t input_full;
    uint8_t enable_dma_in;
    uint8_t enable_dma_out;

    uint32_t last_status;
    uint32_t decode_macroblocks;
    uint32_t decode_blocks;
    uint32_t decode_stop_reason;
    uint32_t decode_input_pos;
    uint32_t decode_input_end;
    uint32_t dma_in_words;
    uint32_t dma_out_words;
    uint32_t dma_read_underflows;
} MDECState;

static MDECState mdec;
static SourceMDEC source_mdec;
static int source_mdec_enabled;
static int16_t source_cr[64],source_cb[64];

#define MDEC_TRACE_CAP 4096u
static MDECDebugEvent mdec_trace[MDEC_TRACE_CAP];
static uint64_t mdec_trace_seq;
static uint32_t mdec_trace_head;

static void trace_event(uint32_t kind, uint32_t value) {
#ifdef PSX_NO_DEBUG_TOOLS
    (void)kind;
    (void)value;
    return;
#else
    extern int debug_server_fmv_quiet(void);
    if (debug_server_fmv_quiet()) return;
    MDECDebugEvent *e = &mdec_trace[mdec_trace_head];
    e->seq = mdec_trace_seq++;
    e->frame = (uint32_t)s_frame_count;
    e->kind = kind;
    e->value = value;
    e->command = mdec.command;
    e->input_count = mdec.input_count;
    e->expected_halfwords = mdec.expected_halfwords;
    e->output_size = mdec.output_size;
    e->output_pos = mdec.output_pos;
    e->macroblocks = mdec.decode_macroblocks;
    e->blocks = mdec.decode_blocks;
    e->stop_reason = mdec.decode_stop_reason;
    e->underruns = mdec.dma_read_underflows;
    mdec_trace_head = (mdec_trace_head + 1u) % MDEC_TRACE_CAP;
#endif
}


static void clear_output(void) {
    mdec.output_size = 0;
    mdec.output_pos = 0;
}

static int ensure_input_capacity(uint32_t halfwords) {
    if (halfwords <= mdec.input_cap) return 1;
    uint32_t new_cap = mdec.input_cap ? mdec.input_cap : 256u;
    while (new_cap < halfwords) new_cap *= 2u;
    uint16_t *new_input = (uint16_t *)realloc(mdec.input, new_cap * sizeof(uint16_t));
    if (!new_input) return 0;
    mdec.input = new_input;
    mdec.input_cap = new_cap;
    return 1;
}

static int ensure_output_capacity(uint32_t bytes) {
    if (bytes <= mdec.output_cap) return 1;
    uint32_t new_cap = mdec.output_cap ? mdec.output_cap : 4096u;
    while (new_cap < bytes) new_cap *= 2u;
    uint8_t *new_output = (uint8_t *)realloc(mdec.output, new_cap);
    if (!new_output) return 0;
    mdec.output = new_output;
    mdec.output_cap = new_cap;
    return 1;
}

static void append_byte(uint8_t value) {
    if (!ensure_output_capacity(mdec.output_size + 1u)) return;
    mdec.output[mdec.output_size++] = value;
}



static uint8_t *output_reserve(uint32_t bytes) {
    if (!ensure_output_capacity(mdec.output_size + bytes)) return NULL;
    return mdec.output + mdec.output_size;
}

static uint8_t input_byte(uint32_t byte_index) {
    uint16_t hw = mdec.input[byte_index >> 1];
    return (byte_index & 1u) ? (uint8_t)(hw >> 8) : (uint8_t)hw;
}

static void finish_command(void) {
    mdec.expected_halfwords = 0;
    mdec.input_count = 0;
    mdec.busy = 0;
    mdec.input_full = 0;
    trace_event(MDEC_EVT_CMD_DONE, mdec.command);
}

static void soft_reset(void) {
    uint16_t *input = mdec.input;
    uint32_t input_cap = mdec.input_cap;
    uint8_t *output = mdec.output;
    uint32_t output_cap = mdec.output_cap;
    uint8_t y_quant[64];
    uint8_t uv_quant[64];
    int16_t scale[64];

    memcpy(y_quant, mdec.y_quant, sizeof(y_quant));
    memcpy(uv_quant, mdec.uv_quant, sizeof(uv_quant));
    memcpy(scale, mdec.scale, sizeof(scale));

    memset(&mdec, 0, sizeof(mdec));
    mdec.input = input;
    mdec.input_cap = input_cap;
    mdec.output = output;
    mdec.output_cap = output_cap;
    memcpy(mdec.y_quant, y_quant, sizeof(mdec.y_quant));
    memcpy(mdec.uv_quant, uv_quant, sizeof(mdec.uv_quant));
    memcpy(mdec.scale, scale, sizeof(mdec.scale));
    mdec.output_depth = 3;
    mdec.current_block = 4;
}




/* Clean derivation: PSX-SPX "MDEC Decompression" and the authored M1-M11
 * observations. No reference decoder code or prior decode bodies were used.
 * Arithmetic and receipt identities: docs/testing/MDEC_CLEAN_DECODE.md. */
static int floor_div(int value, int divisor) {
    int quotient = value / divisor;
    return quotient - (value % divisor < 0);
}
static int clamp_int(int value, int lo, int hi) {
    return value < lo ? lo : value > hi ? hi : value;
}
static int sign_extend_10(uint16_t value) {
    int low = value & 1023;
    return low >= 512 ? low - 1024 : low;
}
static int coefficient_sign(int value) {
    return (value > 0) - (value < 0);
}
static int mask9_clamp_s8(int value) {
    int low = (int)((unsigned)value & 511u);
    if (low >= 256) low -= 512;
    return clamp_int(low, -128, 127);
}
/* Inverse of the zigzag array in PSX-SPX "MDEC Decompression". */
static const uint8_t coefficient_order[64] = {
    0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,
    12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
};
static void idct_block(int16_t block[64]) {
    int16_t next[64];
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned y = 0; y < 8; ++y) {
            for (unsigned x = 0; x < 8; ++x) {
                int sum = 0;
                for (unsigned z = 0; z < 8; ++z)
                    sum += block[y + z * 8] * floor_div(mdec.scale[x + z * 8], 8);
                next[x + y * 8] = (int16_t)floor_div(sum + 16384, 32768);
            }
        }
        memcpy(block, next, sizeof(next));
    }
    for (unsigned i = 0; i < 64; ++i) block[i] = (int16_t)mask9_clamp_s8(block[i]);
}
static int decode_rle_block_from(const uint16_t *encoded, int16_t *block,
                               const uint8_t *quant, uint32_t *pos, uint32_t end) {
    while (*pos < end && encoded[*pos] == 0xfe00u) ++*pos;
    if (*pos == end) return 0;
    memset(block, 0, 64 * sizeof(*block));
    uint16_t entry = encoded[(*pos)++];
    unsigned q = entry >> 10;
    int product = sign_extend_10(entry) * quant[0];
    int dc = quant[0] ? 16 * product - 8 * coefficient_sign(product) : 32 * sign_extend_10(entry);
    block[0] = (int16_t)clamp_int(dc, -16384, 16383);
    unsigned index = 0;
    while (*pos < end) {
        entry = encoded[(*pos)++];
        index += (entry >> 10) + 1;
        if (index >= 64) break;
        int level = sign_extend_10(entry);
        product = level * quant[index] * (int)q;
        int value = q && quant[index] ? 16 * floor_div(product, 8) - 8 * coefficient_sign(product) : 32 * level;
        block[coefficient_order[index]] = (int16_t)clamp_int(value, -16384, 16383);
    }
    idct_block(block);
    return 1;
}
static int decode_rle_block(int16_t *block, const uint8_t *quant,
                           uint32_t *pos, uint32_t end) {
    int decoded = decode_rle_block_from(mdec.input, block, quant, pos, end);
    mdec.decode_blocks += (unsigned)decoded;
    return decoded;
}
static unsigned mono_bytes(const int16_t *block, unsigned depth, unsigned is_signed,
                           uint8_t *output) {
    for (unsigned i = 0; i < 64; ++i) {
        unsigned value = (unsigned)(block[i] + 128);
        if (depth == 1) output[i] = (uint8_t)(value ^ (is_signed ? 128u : 0u));
        else {
            unsigned nibble = (unsigned)clamp_int((int)(value + 8) / 16, 0, 15);
            nibble ^= is_signed ? 8u : 0u;
            if ((i & 1u) == 0) output[i / 2] = (uint8_t)nibble;
            else output[i / 2] |= (uint8_t)(nibble << 4);
        }
    }
    return depth == 1 ? 64 : 32;
}
static unsigned color_pixel(int y, int cb, int cr, uint32_t command, uint8_t *out) {
    int r = mask9_clamp_s8(y + floor_div(359 * cr + 128, 256)) + 128;
    int b = mask9_clamp_s8(y + floor_div(454 * cb + 128, 256)) + 128;
    int green = floor_div(floor_div(-88 * cb, 32) + floor_div(-183 * cr, 32) + 4, 8);
    int g = mask9_clamp_s8(y + green) + 128;
    unsigned is_signed = (command >> 26) & 1;
    if (((command >> 27) & 3) == 2) {
        unsigned toggle = is_signed ? 128u : 0u;
        out[0] = (uint8_t)((unsigned)r ^ toggle);
        out[1] = (uint8_t)((unsigned)g ^ toggle);
        out[2] = (uint8_t)((unsigned)b ^ toggle);
        return 3;
    }
    unsigned pixel = (unsigned)clamp_int((r + 4) / 8, 0, 31)
                   | ((unsigned)clamp_int((g + 4) / 8, 0, 31) << 5)
                   | ((unsigned)clamp_int((b + 4) / 8, 0, 31) << 10);
    pixel ^= is_signed ? 0x4210u : 0u;
    pixel |= ((command >> 25) & 1u) << 15;
    out[0] = (uint8_t)pixel; out[1] = (uint8_t)(pixel >> 8);
    return 2;
}
static void append_luma_block(const int16_t *block) {
    uint8_t *output = output_reserve(mdec.output_depth ? 64 : 32);
    if (output) mdec.output_size += mono_bytes(block, mdec.output_depth, mdec.output_signed, output);
}
static void append_color_macroblock(const int16_t *cr, const int16_t *cb,
                                   const int16_t yblocks[4][64]) {
    unsigned bytes = mdec.output_depth == 2 ? 768 : 512;
    uint8_t *output = output_reserve(bytes);
    if (!output) return;
    unsigned offset = 0;
    for (unsigned y = 0; y < 16; ++y) {
        for (unsigned x = 0; x < 16; ++x) {
            unsigned chroma = (y / 2) * 8 + x / 2;
            int luma = yblocks[(y / 8) * 2 + x / 8][(y % 8) * 8 + x % 8];
            offset += color_pixel(luma, cb[chroma], cr[chroma], mdec.command, output + offset);
        }
    }
    mdec.output_size += bytes;
}
static unsigned source_decode_block(void *context, uint32_t command, unsigned block,
                                    const uint16_t *encoded, unsigned count, uint32_t *pixels) {
    (void)context;
    unsigned depth = (command >> 27) & 3;
    uint32_t pos = 0;
    int16_t decoded[64];
    if (!decode_rle_block_from(encoded, decoded, depth >= 2 && block < 2 ? mdec.uv_quant : mdec.y_quant,
                               &pos, count)) return 0;
    uint8_t output[192];
    unsigned bytes;
    if (depth < 2) bytes = mono_bytes(decoded, depth, (command >> 26) & 1, output);
    else if (block < 2) {
        memcpy(block == 0 ? source_cr : source_cb, decoded, sizeof(decoded));
        return 0;
    } else {
        unsigned origin_x = ((block - 2) & 1) * 8;
        unsigned origin_y = ((block - 2) >> 1) * 8;
        bytes = 0;
        for (unsigned y = 0; y < 8; ++y) {
            for (unsigned x = 0; x < 8; ++x) {
                unsigned c = ((y + origin_y) / 2) * 8 + (x + origin_x) / 2;
                bytes += color_pixel(decoded[y * 8 + x], source_cb[c], source_cr[c], command, output + bytes);
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




static volatile uint32_t g_mdec_decode_count = 0;
uint32_t mdec_get_decode_count(void) { return g_mdec_decode_count; }

static void execute_decode(void) {
    uint32_t pos = 0;
    uint32_t end = mdec.input_count;
    g_mdec_decode_count++;
    clear_output();
    mdec.decode_macroblocks = 0;
    mdec.decode_blocks = 0;
    mdec.decode_stop_reason = MDEC_STOP_NONE;
    mdec.decode_input_pos = 0;
    mdec.decode_input_end = end;
    mdec.dma_out_words = 0;
    mdec.dma_read_underflows = 0;

    if (mdec.output_depth < 2) {
        int16_t yblk[64];
        while (pos < end && decode_rle_block(yblk, mdec.y_quant, &pos, end)) {
            append_luma_block(yblk);
            mdec.decode_macroblocks++;
        }
        mdec.decode_stop_reason = (pos >= end) ? MDEC_STOP_INPUT_END : MDEC_STOP_Y0;
        mdec.decode_input_pos = pos;
        trace_event(MDEC_EVT_DECODE_DONE, mdec.output_size);
        return;
    }

    while (pos < end) {
        int16_t crblk[64];
        int16_t cbblk[64];
        int16_t yblk[4][64];
        if (!decode_rle_block(crblk, mdec.uv_quant, &pos, end)) { mdec.decode_stop_reason = MDEC_STOP_CR; break; }
        if (!decode_rle_block(cbblk, mdec.uv_quant, &pos, end)) { mdec.decode_stop_reason = MDEC_STOP_CB; break; }
        if (!decode_rle_block(yblk[0], mdec.y_quant, &pos, end)) { mdec.decode_stop_reason = MDEC_STOP_Y0; break; }
        if (!decode_rle_block(yblk[1], mdec.y_quant, &pos, end)) { mdec.decode_stop_reason = MDEC_STOP_Y1; break; }
        if (!decode_rle_block(yblk[2], mdec.y_quant, &pos, end)) { mdec.decode_stop_reason = MDEC_STOP_Y2; break; }
        if (!decode_rle_block(yblk[3], mdec.y_quant, &pos, end)) { mdec.decode_stop_reason = MDEC_STOP_Y3; break; }
        append_color_macroblock(crblk, cbblk, yblk);
        mdec.decode_macroblocks++;
    }
    if (pos >= end && mdec.decode_stop_reason == MDEC_STOP_NONE) {
        mdec.decode_stop_reason = MDEC_STOP_INPUT_END;
    }
    mdec.decode_input_pos = pos;


    mdec_last_color_decode_frame = s_frame_count;
    mdec_last_color_decode_cycle = psx_cycle_count;
    trace_event(MDEC_EVT_DECODE_DONE, mdec.output_size);
}

static void execute_command(void) {
    uint32_t op = mdec.command >> 29;

    if (op == MDEC_CMD_DECODE) {
        execute_decode();
    } else if (op == MDEC_CMD_SET_QUANT) {
        for (unsigned i = 0; i < 64; ++i) mdec.y_quant[i] = input_byte(i);
        if (mdec.command & 1u)
            for (unsigned i = 0; i < 64; ++i) mdec.uv_quant[i] = input_byte(i + 64);
    } else if (op == MDEC_CMD_SET_SCALE) {
        for (unsigned i = 0; i < 64; ++i) {
            unsigned value = mdec.input[i];
            mdec.scale[i] = (int16_t)(value >= 32768 ? (int)value - 65536 : (int)value);
        }
    }

    finish_command();
}

static void begin_command(uint32_t value) {
    mdec.command = value;
    mdec.output_bit15 = (uint8_t)((value >> 25) & 1u);
    mdec.output_signed = (uint8_t)((value >> 26) & 1u);
    mdec.output_depth = (uint8_t)((value >> 27) & 3u);
    mdec.current_block = 4;
    mdec.input_count = 0;
    mdec.input_full = 0;
    mdec.busy = 1;

    switch (value >> 29) {
        case MDEC_CMD_DECODE:
            mdec.expected_halfwords = (value & 0xFFFFu) * 2u;
            break;
        case MDEC_CMD_SET_QUANT:
            mdec.expected_halfwords = (value & 1u) ? 64u : 32u;
            break;
        case MDEC_CMD_SET_SCALE:
            mdec.expected_halfwords = 64u;
            break;
        default:
            mdec.expected_halfwords = 0;
            break;
    }

    if (mdec.expected_halfwords == 0 || !ensure_input_capacity(mdec.expected_halfwords)) {
        finish_command();
    } else {
        trace_event(MDEC_EVT_CMD_BEGIN, value);
    }
}

static void write_data(uint32_t value) {
    if (mdec.busy && mdec.input_count < mdec.expected_halfwords) {
        mdec.input[mdec.input_count++] = (uint16_t)value;
        if (mdec.input_count < mdec.expected_halfwords) {
            mdec.input[mdec.input_count++] = (uint16_t)(value >> 16);
        }
        if (mdec.input_count >= mdec.expected_halfwords) {
            execute_command();
        }
        return;
    }

    begin_command(value);
}

int mdec_recently_active(uint32_t within_frames) {



    const uint64_t cycles_per_frame = 338688ull;
    const uint64_t window =
        (uint64_t)within_frames * cycles_per_frame + (cycles_per_frame / 2ull);
    if (psx_cycle_count < mdec_last_color_decode_cycle)
        return 0;
    return (psx_cycle_count - mdec_last_color_decode_cycle) <= window;
}

uint64_t mdec_color_age_cycles(void) {
    if (mdec_last_color_decode_cycle == (uint64_t)0 - 1000u)
        return (uint64_t)0 - 1u;
    if (psx_cycle_count < mdec_last_color_decode_cycle)
        return (uint64_t)0 - 1u;
    return psx_cycle_count - mdec_last_color_decode_cycle;
}

void mdec_init(void) {
    memset(&mdec, 0, sizeof(mdec));
    memset(mdec_trace, 0, sizeof(mdec_trace));
    mdec_trace_seq = 0;
    mdec_trace_head = 0;


    mdec_last_color_decode_frame = (uint64_t)0 - 1000u;
    mdec_last_color_decode_cycle = (uint64_t)0 - 1000u;
    for (int i = 0; i < 64; i++) {
        mdec.y_quant[i] = 1;
        mdec.uv_quant[i] = 1;
    }
    mdec.output_depth = 3;
    mdec.current_block = 4;

    source_mdec_enabled=0;
    const char *source_mode=getenv("PSX_MDEC_SOURCE_MODEL");
    if(source_mode && *source_mode){
        if(strcmp(source_mode,"octoshock-2.3") && strcmp(source_mode,"nymashock-1.29.0") && strcmp(source_mode,"nymashock-1.32.1")){fprintf(stderr,"[mdec-source] unknown model\n");exit(2);}
        source_mdec_enabled=1;
        memset(mdec.y_quant,0,sizeof(mdec.y_quant));memset(mdec.uv_quant,0,sizeof(mdec.uv_quant));
        memset(mdec.scale,0,sizeof(mdec.scale));memset(source_cr,0,sizeof(source_cr));memset(source_cb,0,sizeof(source_cb));
        source_mdec_power(&source_mdec,source_decode_block,source_table_word,0);
        if(!strcmp(source_mode,"nymashock-1.29.0") || !strcmp(source_mode,"nymashock-1.32.1"))source_mdec.block_cycles=512;
    }
}

uint32_t mdec_read(uint32_t addr) {
    if(source_mdec_enabled)return (addr&4u)?source_mdec_status(&source_mdec):source_mdec_read(&source_mdec,0,0);
    uint32_t offset = addr & 7u;
    if (offset == 0) {
        return mdec_dma_read_word();
    }

    uint32_t remaining_words = 0;
    if (mdec.busy && mdec.expected_halfwords > mdec.input_count) {
        remaining_words = (mdec.expected_halfwords - mdec.input_count + 1u) / 2u;
    }

    uint32_t status = remaining_words ? ((remaining_words - 1u) & 0xFFFFu) : 0xFFFFu;
    status |= ((uint32_t)mdec.current_block & 7u) << 16;
    status |= ((uint32_t)mdec.output_bit15 & 1u) << 23;
    status |= ((uint32_t)mdec.output_signed & 1u) << 24;
    status |= ((uint32_t)mdec.output_depth & 3u) << 25;
    int write_ready = mdec_dma_write_ready();
    if (!write_ready) status |= 1u << 30;
    if (mdec.enable_dma_out && mdec_dma_read_ready()) status |= 1u << 27;
    if (mdec.enable_dma_in && write_ready) status |= 1u << 28;

    if (mdec.busy || mdec.output_pos < mdec.output_size) status |= 1u << 29;
    if (mdec.output_pos >= mdec.output_size) status |= 1u << 31;
    mdec.last_status = status;
    return status;
}

void mdec_write(uint32_t addr, uint32_t value) {
    if(source_mdec_enabled){
        if(addr&4u)source_mdec_control(&source_mdec,value);
        else source_mdec_write(&source_mdec,value,0);
        source_mdec_require();return;
    }
    uint32_t offset = addr & 7u;
    if (offset == 0) {
        write_data(value);
        return;
    }

    if (value & 0x80000000u) {
        soft_reset();
        trace_event(MDEC_EVT_RESET, value);
    }
    mdec.enable_dma_in = (uint8_t)((value >> 30) & 1u);
    mdec.enable_dma_out = (uint8_t)((value >> 29) & 1u);
    trace_event(MDEC_EVT_CTRL_WRITE, value);
}

void mdec_dma_write_word(uint32_t value) {
    if(source_mdec_enabled){source_mdec_write(&source_mdec,value,1);source_mdec_require();return;}
    write_data(value);
}




uint32_t mdec_dma_write_words(const uint32_t *src, uint32_t max_words) {
    if(source_mdec_enabled){unsigned n=0;while(n<max_words && source_mdec.in_count<32)mdec_dma_write_word(src[n++]);return n;}
    uint32_t moved = 0;
    while (moved < max_words) {
        if (!mdec_dma_write_ready()) break;


        if (mdec.busy && mdec.input_count < mdec.expected_halfwords) {
            uint32_t need_hw = mdec.expected_halfwords - mdec.input_count;
            uint32_t need_words = (need_hw + 1u) / 2u;
            uint32_t n = max_words - moved;
            if (n > need_words) n = need_words;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t value = src[moved++];
                mdec.input[mdec.input_count++] = (uint16_t)value;
                if (mdec.input_count < mdec.expected_halfwords) {
                    mdec.input[mdec.input_count++] = (uint16_t)(value >> 16);
                }
            }
            if (mdec.input_count >= mdec.expected_halfwords) {
                execute_command();
            }
            continue;
        }
        write_data(src[moved++]);
    }
    return moved;
}

uint32_t mdec_dma_read_word(void) {
    if(source_mdec_enabled){uint32_t offset;return mdec_source_dma_read(&offset);}
    uint32_t value = 0;
    uint32_t start_pos = mdec.output_pos;
    uint32_t avail = (start_pos < mdec.output_size)
                   ? (mdec.output_size - start_pos) : 0u;
    if (avail >= 4u) {
        memcpy(&value, mdec.output + start_pos, sizeof(value));
        mdec.output_pos = start_pos + 4u;
    } else {
        for (uint32_t i = 0; i < 4u; i++) {
            if (mdec.output_pos < mdec.output_size) {
                value |= (uint32_t)mdec.output[mdec.output_pos++] << (i * 8u);
            }
        }
    }
    mdec.dma_out_words++;
    if (start_pos >= mdec.output_size) {
        mdec.dma_read_underflows++;
        if (mdec.dma_read_underflows == 1u) {
            trace_event(MDEC_EVT_READ_UNDERFLOW, mdec.dma_out_words);
        }
    }
    if (mdec.output_pos >= mdec.output_size) {
        trace_event(MDEC_EVT_OUTPUT_DRAINED, mdec.output_size);
        clear_output();
    }
    return value;
}


uint32_t mdec_dma_read_words(uint32_t *dst, uint32_t max_words) {
    if(source_mdec_enabled){unsigned n=0;while(n<max_words && source_mdec.out_count)dst[n++]=mdec_dma_read_word();return n;}
    uint32_t moved = 0;
    while (moved < max_words && mdec.output_pos < mdec.output_size) {
        dst[moved] = mdec_dma_read_word();
        moved++;
    }
    return moved;
}

int mdec_dma_write_ready(void) {
    if(source_mdec_enabled)return source_mdec_can_write(&source_mdec);
    if (mdec.output_pos < mdec.output_size) return 0;
    return !mdec.busy || mdec.input_count < mdec.expected_halfwords;
}

int mdec_dma_read_ready(void) {
    if(source_mdec_enabled)return source_mdec_can_read(&source_mdec);
    return mdec.output_pos < mdec.output_size;
}

void mdec_debug_get_state(MDECDebugState *out) {
    if (!out) return;
    out->command = mdec.command;
    out->expected_halfwords = mdec.expected_halfwords;
    out->input_count = mdec.input_count;
    out->output_size = mdec.output_size;
    out->output_pos = mdec.output_pos;
    out->output_depth = mdec.output_depth;
    out->output_signed = mdec.output_signed;
    out->output_bit15 = mdec.output_bit15;
    out->busy = mdec.busy;
    out->input_full = mdec.input_full;
    out->enable_dma_in = mdec.enable_dma_in;
    out->enable_dma_out = mdec.enable_dma_out;
    out->last_status = mdec.last_status;
    out->decode_macroblocks = mdec.decode_macroblocks;
    out->decode_blocks = mdec.decode_blocks;
    out->decode_stop_reason = mdec.decode_stop_reason;
    out->decode_input_pos = mdec.decode_input_pos;
    out->decode_input_end = mdec.decode_input_end;
    out->dma_in_words = mdec.dma_in_words;
    out->dma_out_words = mdec.dma_out_words;
    out->dma_read_underflows = mdec.dma_read_underflows;
}

uint64_t mdec_debug_get_event_total(void) {
    return mdec_trace_seq;
}

uint32_t mdec_debug_copy_events(uint64_t seq_lo, uint64_t seq_hi,
                                MDECDebugEvent *out, uint32_t max_count) {
    if (!out || max_count == 0) return 0;
    uint64_t oldest = (mdec_trace_seq > MDEC_TRACE_CAP) ? mdec_trace_seq - MDEC_TRACE_CAP : 0;
    if (seq_lo < oldest) seq_lo = oldest;
    if (seq_hi > mdec_trace_seq) seq_hi = mdec_trace_seq;
    uint32_t n = 0;
    for (uint64_t seq = seq_lo; seq < seq_hi && n < max_count; seq++) {
        out[n++] = mdec_trace[seq % MDEC_TRACE_CAP];
    }
    return n;
}

void mdec_debug_clear(void) {
    memset(mdec_trace, 0, sizeof(mdec_trace));
    mdec_trace_seq = 0;
    mdec_trace_head = 0;
}

void mdec_debug_dma_in_start(uint32_t addr, uint32_t words) {
    (void)addr;
    trace_event(MDEC_EVT_DMA_IN_START, words);
}

void mdec_debug_dma_in_end(uint32_t addr, uint32_t words) {
    (void)addr;
    mdec.dma_in_words += words;
    trace_event(MDEC_EVT_DMA_IN_END, words);
}

void mdec_debug_dma_out_start(uint32_t addr, uint32_t words) {
    (void)addr;
    trace_event(MDEC_EVT_DMA_OUT_START, words);
}

void mdec_debug_dma_out_end(uint32_t addr, uint32_t words) {
    (void)addr;
    trace_event(MDEC_EVT_DMA_OUT_END, words);
}


#define MDEC_SNAP_VER 2u
#define MDEC_SNAP_INPUT_MAX  (4u * 1024u * 1024u)
#define MDEC_SNAP_OUTPUT_MAX (8u * 1024u * 1024u)


#define SOURCE_MDEC_WIRE_BYTES (905u+(source_mdec.block_cycles==512?4u:0u))
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
    if(s->block_cycles==512 && !pst_w_u32(w,s->block_cycles))return 0;
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
        s.block>5 || s.pixel_count>48 || s.pixel_at>48 ||
        s.quant_index>127 || s.matrix_index>63 || error) return 0;
    if(s.block_cycles==512) { uint32_t cycles; if(!pst_r_u32(r,&cycles) || cycles!=512)return 0; }
    s.error=error;
    if (apply) {source_mdec=s;memcpy(source_cr,cr,sizeof cr);memcpy(source_cb,cb,sizeof cb);}
    return 1;
}

static uint32_t mdec_snap_fixed_bytes(void) {

    return 4u +
           4u * 14u +
           1u * 8u +
           64u + 64u +
           64u * 2u +
           4u + 4u +
           8u;
}

uint32_t mdec_snapshot_bytes(void) {
    uint64_t n = (uint64_t)mdec_snap_fixed_bytes() +
                 (uint64_t)mdec.input_count * 2u +
                 (uint64_t)mdec.output_size + (source_mdec_enabled?SOURCE_MDEC_WIRE_BYTES:0u);
    if (n > 0xffffffffu) return 0;
    return (uint32_t)n;
}

void mdec_snapshot_write(uint8_t *p) {
    PstW w;
    uint32_t n = mdec_snapshot_bytes();
    if (!p || n == 0) return;
    pst_w_init(&w, p, n);
    (void)pst_w_u32(&w, MDEC_SNAP_VER);
    (void)pst_w_u32(&w, mdec.command);
    (void)pst_w_u32(&w, mdec.expected_halfwords);
    (void)pst_w_u32(&w, mdec.last_status);
    (void)pst_w_u32(&w, mdec.decode_macroblocks);
    (void)pst_w_u32(&w, mdec.decode_blocks);
    (void)pst_w_u32(&w, mdec.decode_stop_reason);
    (void)pst_w_u32(&w, mdec.decode_input_pos);
    (void)pst_w_u32(&w, mdec.decode_input_end);
    (void)pst_w_u32(&w, mdec.dma_in_words);
    (void)pst_w_u32(&w, mdec.dma_out_words);
    (void)pst_w_u32(&w, mdec.dma_read_underflows);
    (void)pst_w_u32(&w, mdec.output_pos);
    (void)pst_w_u32(&w, 0u);
    (void)pst_w_u32(&w, 0u);
    (void)pst_w_u8(&w, mdec.output_bit15);
    (void)pst_w_u8(&w, mdec.output_signed);
    (void)pst_w_u8(&w, mdec.output_depth);
    (void)pst_w_u8(&w, mdec.current_block);
    (void)pst_w_u8(&w, mdec.busy);
    (void)pst_w_u8(&w, mdec.input_full);
    (void)pst_w_u8(&w, mdec.enable_dma_in);
    (void)pst_w_u8(&w, mdec.enable_dma_out);
    (void)pst_w_bytes(&w, mdec.y_quant, 64u);
    (void)pst_w_bytes(&w, mdec.uv_quant, 64u);
    for (int i = 0; i < 64; i++)
        (void)pst_w_i16(&w, mdec.scale[i]);
    (void)pst_w_u32(&w, mdec.input_count);
    (void)pst_w_u32(&w, mdec.output_size);

    (void)pst_w_u64(&w, mdec_last_color_decode_cycle);
    for (uint32_t i = 0; i < mdec.input_count; i++)
        (void)pst_w_u16(&w, mdec.input ? mdec.input[i] : 0u);
    if (mdec.output_size && mdec.output)
        (void)pst_w_bytes(&w, mdec.output, mdec.output_size);
    if (source_mdec_enabled && (!mdec_source_snap_emit(&w) || w.written!=n)) abort();
}

static int mdec_snapshot_parse(const uint8_t *p, uint32_t len,
                               MDECState *next, PstR *payload,
                               uint64_t *out_last_decode_cycle) {
    PstR r;
    uint32_t ver = 0, input_count = 0, output_size = 0, reserved;
    uint64_t last_decode_cycle = 0;
    int16_t s16;
    if (!p || len < mdec_snap_fixed_bytes()) return 0;
    if (source_mdec_enabled) {
        if (len<mdec_snap_fixed_bytes()+SOURCE_MDEC_WIRE_BYTES) return 0;
        PstR source;
        pst_r_init(&source,p+len-SOURCE_MDEC_WIRE_BYTES,SOURCE_MDEC_WIRE_BYTES);
        if (!mdec_source_snap_parse(&source,0)) return 0;
    }
    pst_r_init(&r, p, len);
    if (!pst_r_u32(&r, &ver) || ver != MDEC_SNAP_VER) return 0;
    if (!pst_r_u32(&r, &next->command) ||
        !pst_r_u32(&r, &next->expected_halfwords) ||
        !pst_r_u32(&r, &next->last_status) ||
        !pst_r_u32(&r, &next->decode_macroblocks) ||
        !pst_r_u32(&r, &next->decode_blocks) ||
        !pst_r_u32(&r, &next->decode_stop_reason) ||
        !pst_r_u32(&r, &next->decode_input_pos) ||
        !pst_r_u32(&r, &next->decode_input_end) ||
        !pst_r_u32(&r, &next->dma_in_words) ||
        !pst_r_u32(&r, &next->dma_out_words) ||
        !pst_r_u32(&r, &next->dma_read_underflows) ||
        !pst_r_u32(&r, &next->output_pos) ||
        !pst_r_u32(&r, &reserved) ||
        !pst_r_u32(&r, &reserved))
        return 0;
    if (!pst_r_u8(&r, &next->output_bit15) ||
        !pst_r_u8(&r, &next->output_signed) ||
        !pst_r_u8(&r, &next->output_depth) ||
        !pst_r_u8(&r, &next->current_block) ||
        !pst_r_u8(&r, &next->busy) ||
        !pst_r_u8(&r, &next->input_full) ||
        !pst_r_u8(&r, &next->enable_dma_in) ||
        !pst_r_u8(&r, &next->enable_dma_out))
        return 0;
    if (!pst_r_bytes(&r, next->y_quant, 64u) ||
        !pst_r_bytes(&r, next->uv_quant, 64u))
        return 0;
    for (int i = 0; i < 64; i++) {
        if (!pst_r_i16(&r, &s16)) return 0;
        next->scale[i] = s16;
    }
    if (!pst_r_u32(&r, &input_count) || !pst_r_u32(&r, &output_size) ||
        !pst_r_u64(&r, &last_decode_cycle))
        return 0;
    if (input_count > MDEC_SNAP_INPUT_MAX || output_size > MDEC_SNAP_OUTPUT_MAX)
        return 0;
    if (next->output_pos > output_size) return 0;
    if ((size_t)(r.end - r.p) !=
        (size_t)input_count * 2u + (size_t)output_size +
        (source_mdec_enabled ? SOURCE_MDEC_WIRE_BYTES : 0u))
        return 0;
    next->input_count = input_count;
    next->output_size = output_size;
    *payload = r;
    *out_last_decode_cycle = last_decode_cycle;
    return 1;
}

int mdec_snapshot_prepare(const uint8_t *p, uint32_t len) {
    MDECState next = mdec;
    PstR payload;
    uint64_t last_decode_cycle;
    if (!mdec_snapshot_parse(p, len, &next, &payload, &last_decode_cycle)) return 0;

    return ensure_input_capacity(next.input_count ? next.input_count : 1u) &&
           ensure_output_capacity(next.output_size ? next.output_size : 1u);
}

int mdec_snapshot_read(const uint8_t *p, uint32_t len) {
    MDECState next = mdec;
    PstR r;
    uint64_t last_decode_cycle = 0, age;
    uint32_t input_count, output_size;
    if (!mdec_snapshot_parse(p, len, &next, &r, &last_decode_cycle)) return 0;
    input_count = next.input_count;
    output_size = next.output_size;
    if (!ensure_input_capacity(input_count ? input_count : 1u)) return 0;
    if (!ensure_output_capacity(output_size ? output_size : 1u)) return 0;
    next.input = mdec.input;
    next.input_cap = mdec.input_cap;
    next.output = mdec.output;
    next.output_cap = mdec.output_cap;
    for (uint32_t i = 0; i < input_count; i++) {
        uint16_t hw;
        if (!pst_r_u16(&r, &hw)) return 0;
        mdec.input[i] = hw;
    }
    if (output_size && !pst_r_bytes(&r, mdec.output, output_size))
        return 0;

    if (source_mdec_enabled && !mdec_source_snap_parse(&r,1)) return 0;
    if (r.p!=r.end) return 0;
    mdec_last_color_decode_cycle = last_decode_cycle;
    age = psx_cycle_count >= last_decode_cycle ? psx_cycle_count-last_decode_cycle : UINT64_MAX;
    mdec = next;


    {
        const uint64_t cycles_per_frame = 338688ull;
        uint64_t frames_ago = age / cycles_per_frame;
        if (frames_ago > 100000ull)
            frames_ago = 100000ull;
        if (frames_ago >= s_frame_count)
            mdec_last_color_decode_frame = 0;
        else
            mdec_last_color_decode_frame = s_frame_count - frames_ago;
    }
    return 1;
}
