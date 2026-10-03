/* PS1B-425: the rows of the MDEC source-profile FIFO spec
 * (MDEC-SOURCE-PROFILE-FIFO-SPEC.md), one by one, through the nine functions
 * of source_mdec_fifo.h with authored callbacks. No decode arithmetic, no
 * media, no checkpoint.
 *
 * Every expected value comes from a spec row; each check names its row. One
 * expectation goes beyond the row text: a block also ends when its
 * coefficient reaches 64 (row B2 gives only the limit). PSX-SPX "EOB (End Of
 * Block)" says that the end code is not required for a fully defined block,
 * and the b53_replay_driver hash of the README holds it.
 *
 * It uses the public names of the header only: the nine functions, the power
 * initializer, the state fields and the phase enum. It builds against any
 * source_mdec_fifo.h with that interface:
 *   cc -std=c11 -O2 -Itree/runtime/include \
 *      tree/runtime/tests/test_mdec_source_fifo_rows.c -o mdec_source_fifo_rows
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "source_mdec_fifo.h"

static unsigned checks, failures;
#define EXPECT(cond, ...) do { checks++; if (!(cond)) { failures++; \
    if (failures <= 60) { fprintf(stderr, "FAIL line %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } } while (0)

/* The owner: records what the header hands to it. */
typedef struct {
    unsigned blocks, give, tables;
    uint32_t command;
    unsigned index, count;
    uint16_t encoded[64];
    unsigned table_op[40], table_index[40];
    uint32_t table_word[40];
} Owner;
static Owner owner;
static SourceMDEC s;

static uint32_t pixel_value(unsigned block_number, unsigned i) { return 0xA0000000u + (block_number << 8) + i; }
static unsigned on_block(void *context, uint32_t command, unsigned index, const uint16_t *encoded, unsigned count, uint32_t *pixels)
{
    Owner *o = context;
    o->blocks++;
    o->command = command; o->index = index; o->count = count;
    memcpy(o->encoded, encoded, (count < 64 ? count : 64) * sizeof encoded[0]);
    for (unsigned i = 0; i < o->give && i < 48; ++i) pixels[i] = pixel_value(o->blocks, i);
    return o->give;
}
static void on_table(void *context, unsigned operation, unsigned index, uint32_t word)
{
    Owner *o = context;
    if (o->tables < 40) { o->table_op[o->tables] = operation; o->table_index[o->tables] = index; o->table_word[o->tables] = word; }
    o->tables++;
}
static void power(void)
{
    memset(&owner, 0, sizeof owner);
    owner.give = 8;
    source_mdec_power(&s, on_block, on_table, &owner);
}
static int equal_but_control(const SourceMDEC *a, const SourceMDEC *b)
{
    return !memcmp(a->in, b->in, sizeof a->in) && !memcmp(a->out, b->out, sizeof a->out) &&
           !memcmp(a->pixels, b->pixels, sizeof a->pixels) && !memcmp(a->encoded, b->encoded, sizeof a->encoded) &&
           a->in_at == b->in_at && a->in_count == b->in_count && a->out_at == b->out_at && a->out_count == b->out_count &&
           a->command == b->command && a->credit == b->credit && a->remaining == b->remaining && a->phase == b->phase &&
           a->busy == b->busy && a->coefficient == b->coefficient && a->encoded_count == b->encoded_count &&
           a->block == b->block && a->pixel_count == b->pixel_count && a->pixel_at == b->pixel_at &&
           a->quant_index == b->quant_index && a->matrix_index == b->matrix_index && a->row == b->row &&
           a->word_in_row == b->word_in_row && a->row_words == b->row_words && a->decode == b->decode &&
           a->table == b->table && a->context == b->context && a->error == b->error && a->block_cycles == b->block_cycles;
}

/* ---- S6, S1, S2 and the input queue ---------------------------------------- */
static void test_power_status_requests(void)
{
    power();
    SourceMDEC zero;
    memset(&zero, 0, sizeof zero);
    zero.block_cycles = 474; zero.decode = on_block; zero.table = on_table; zero.context = &owner;
    EXPECT(equal_but_control(&s, &zero) && s.control == 0, "S6 power clears everything, sets 474 and the callbacks");
    /* S1: after power only "output queue empty" is set (spec section 1: this
     * profile's reset status is 80000000h). */
    EXPECT(source_mdec_status(&s) == 0x80000000u, "S1 status after power %08X", source_mdec_status(&s));
    s.out_count = 1;
    EXPECT(source_mdec_status(&s) == 0, "S1 bit 31 only with an empty output queue: %08X", source_mdec_status(&s));
    s.in_count = 31;
    EXPECT(source_mdec_status(&s) == 0, "S1 bit 30 not at 31 input words");
    s.in_count = 32;
    EXPECT(source_mdec_status(&s) == 0x40000000u, "S1 bit 30 at 32 input words: %08X", source_mdec_status(&s));
    s.in_count = 0; s.busy = 1;
    EXPECT(source_mdec_status(&s) == 0x20000000u, "S1 bit 29 follows busy: %08X", source_mdec_status(&s));
    s.busy = 0; s.remaining = 0xBEEF;
    EXPECT(source_mdec_status(&s) == 0x0000BEEFu, "S1 bits 15-0 are the remaining count: %08X", source_mdec_status(&s));
    s.remaining = 0; s.block = 5;
    EXPECT(source_mdec_status(&s) == 0, "S1 bits 22-16 are zero whatever the block: %08X", source_mdec_status(&s));
    for (unsigned bit = 25; bit <= 28; ++bit) {
        s.command = 1u << bit;
        EXPECT(source_mdec_status(&s) == 1u << (bit - 2), "S1 command bit %u goes to status bit %u: %08X", bit, bit - 2,
               source_mdec_status(&s));
    }
    s.command = 0xE1FFFFFFu;
    EXPECT(source_mdec_status(&s) == 0, "S1 no other command bit shows: %08X", source_mdec_status(&s));
    /* S2 and S1 bits 28, 27: the two requests. */
    for (unsigned mask = 0; mask < 96; ++mask) {
        static const uint16_t counts[3] = { 0xFFFFu, 0, 5 };
        static const unsigned out_counts[4] = { 0, 1, 31, 32 };
        power();
        s.control = (mask & 1u ? 1u << 30 : 0) | (mask & 2u ? 1u << 29 : 0) | 0x1234u;
        s.busy = mask >> 2 & 1u;
        s.in_count = mask >> 3 & 1u;
        s.remaining = counts[(mask >> 4) % 3u];
        s.out_count = out_counts[mask >> 2 & 3u];
        int in_request = (mask & 1u) && s.busy && s.in_count == 0 && s.remaining != 0xFFFFu;
        int out_request = (mask & 2u) && s.out_count == 32;
        EXPECT(source_mdec_can_write(&s) == in_request, "S2 input request, case %u", mask);
        EXPECT(source_mdec_can_read(&s) == out_request, "S2 output request, case %u", mask);
        EXPECT((source_mdec_status(&s) >> 28 & 1u) == (unsigned)in_request && (source_mdec_status(&s) >> 27 & 1u) == (unsigned)out_request,
               "S1 bits 28 and 27 follow the requests, case %u", mask);
    }
    /* Section 2: the oldest input word first; indices are modulo 32. */
    power();
    s.in[31] = 0x11; s.in[0] = 0x22; s.in_at = 31; s.in_count = 2;
    EXPECT(source_mdec_pop_input(&s) == 0x11 && s.in_at == 0 && s.in_count == 1, "pop_input takes the oldest word");
    EXPECT(source_mdec_pop_input(&s) == 0x22 && s.in_at == 1 && s.in_count == 0, "pop_input wraps at 32");
}

/* ---- B2, B3, B4: one encoded halfword ------------------------------------------ */
static void complete_block(void) { source_mdec_half(&s, 0x0001); source_mdec_half(&s, 0xFE00); }
static void test_half(void)
{
    power();
    s.command = 0x30000000u;                      /* bit 28: a colour command */
    s.pixel_count = 33;
    EXPECT(source_mdec_half(&s, 0xFE00) == 0 && s.encoded_count == 0 && s.coefficient == 0 && owner.blocks == 0,
           "B2 FE00h before a block is padding");
    EXPECT(source_mdec_half(&s, 0x1234) == 0 && s.encoded_count == 1 && s.encoded[0] == 0x1234 && s.coefficient == 1,
           "B2 the first halfword: count %u coefficient %u", s.encoded_count, s.coefficient);
    EXPECT(source_mdec_half(&s, 0x0801) == 0 && s.encoded_count == 2 && s.coefficient == 4,
           "B2 a later halfword moves by 1 plus its high six bits: coefficient %u, want 4", s.coefficient);
    int cost = source_mdec_half(&s, 0xFE00);
    EXPECT(cost == 474 && owner.blocks == 1, "B3/B4 FE00h ends the block for the cost 474: cost %d, calls %u", cost, owner.blocks);
    EXPECT(owner.command == 0x30000000u && owner.index == 0 && owner.count == 3 && owner.encoded[0] == 0x1234 &&
           owner.encoded[1] == 0x0801 && owner.encoded[2] == 0xFE00, "B3 the owner gets command, block and the encoded halfwords");
    EXPECT(s.coefficient == 0 && s.encoded_count == 0 && s.block == 1, "B3/B4 after the block: coefficient %u count %u block %u",
           s.coefficient, s.encoded_count, s.block);
    EXPECT(s.pixel_count == 33, "B4 block 0 gives no output count: %u", s.pixel_count);
    complete_block();
    EXPECT(s.block == 2 && s.pixel_count == 33, "B4 block 1 gives no output count either: block %u count %u", s.block, s.pixel_count);
    owner.give = 16;
    complete_block();
    EXPECT(s.block == 3 && s.pixel_count == 16, "B4 block 2 gives the returned count: block %u count %u", s.block, s.pixel_count);
    complete_block(); complete_block();
    EXPECT(s.block == 5, "B4 colour blocks count up: %u", s.block);
    complete_block();
    EXPECT(s.block == 0 && owner.blocks == 6 && owner.index == 5, "B4 after block 5 comes block 0: %u", s.block);
    /* B4: without bit 28, block 2 repeats. The cost is block_cycles. */
    power();
    s.command = 0x20000000u; s.block = 2; s.block_cycles = 512;
    cost = source_mdec_half(&s, 0x0001) + source_mdec_half(&s, 0xFE00);
    EXPECT(cost == 512 && s.block == 2 && s.pixel_count == 8 && owner.index == 2, "B4 mono: cost %d block %u count %u", cost, s.block, s.pixel_count);
    /* B2 with PSX-SPX "EOB (End Of Block)": at 64 the block is complete
     * without an end code. 1 + (1 + 61) = 63 is not yet the end. */
    power();
    s.command = 0x20000000u; s.block = 2;
    EXPECT(source_mdec_half(&s, 0x0001) == 0 && source_mdec_half(&s, 0xF400) == 0 && s.coefficient == 63 && owner.blocks == 0,
           "B2 coefficient 63 does not end the block: %u", s.coefficient);
    cost = source_mdec_half(&s, 0x0005);
    EXPECT(cost == 474 && owner.blocks == 1 && owner.count == 3 && s.coefficient == 0 && s.encoded_count == 0,
           "B2 coefficient 64 ends the block: cost %d calls %u count %u", cost, owner.blocks, owner.count);
    EXPECT(source_mdec_half(&s, 0xFE00) == 0 && owner.blocks == 1 && s.encoded_count == 0, "B2 FE00h after such a block is padding");
    source_mdec_half(&s, 0x0001);
    cost = source_mdec_half(&s, 0xFC01);          /* 1 + (1 + 63) is above 64 */
    EXPECT(cost == 474 && owner.blocks == 2 && owner.count == 2, "B2 a step past 64 ends the block: cost %d calls %u", cost, owner.blocks);
    /* B3: the 65th halfword of a block is an error with no cost. */
    power();
    s.encoded_count = 64; s.coefficient = 1;
    cost = source_mdec_half(&s, 0x0001);
    EXPECT(cost == 0 && s.error == 1 && s.encoded_count == 64 && s.coefficient == 1 && owner.blocks == 0,
           "B3 more than 64 halfwords: cost %d error %d count %u coefficient %u", cost, s.error, s.encoded_count, s.coefficient);
    /* B3: more than 48 output words is an error; block and count stay. 48 is allowed. */
    power();
    s.command = 0x20000000u; s.block = 2; owner.give = 49;
    source_mdec_half(&s, 0x0001);
    cost = source_mdec_half(&s, 0xFE00);
    EXPECT(cost == 0 && s.error == 1 && s.block == 2 && s.encoded_count == 2 && s.coefficient == 0 && owner.blocks == 1,
           "B3 49 output words: cost %d error %d block %u count %u coefficient %u", cost, s.error, s.block, s.encoded_count, s.coefficient);
    power();
    s.command = 0x20000000u; s.block = 2; owner.give = 48;
    source_mdec_half(&s, 0x0001);
    cost = source_mdec_half(&s, 0xFE00);
    EXPECT(cost == 474 && s.error == 0 && s.pixel_count == 48, "B3 48 output words are allowed");
}

/* ---- T1: elapsed time ---------------------------------------------------------------- */
static void test_time(void)
{
    power();
    source_mdec_run(&s, 1000000);
    EXPECT(s.credit == 128 && s.error == 0, "T1 1,000,000 clocks are allowed and the credit stops at 128: %d", s.credit);
    source_mdec_run(&s, 1000001);
    EXPECT(s.credit == 128 && s.error == 1, "T1 more than 1,000,000 clocks is an error and changes nothing else");
    s.credit = 5;
    source_mdec_run(&s, 7);
    EXPECT(s.credit == 5, "T1 with the error set, service changes nothing");
    power();
    s.credit = -300;
    source_mdec_run(&s, 100);
    EXPECT(s.credit == -200, "T1 a debt is repaid by later clocks: %d", s.credit);
    source_mdec_run(&s, 128); source_mdec_run(&s, 128);
    EXPECT(s.credit == 56, "T1 credit %d, want 56", s.credit);
    source_mdec_run(&s, 128);
    EXPECT(s.credit == 128, "T1 the positive result is limited to 128: %d", s.credit);
    /* T2, T6: an idle controller that is served is not busy. */
    power();
    s.busy = 1;
    source_mdec_run(&s, 0);
    EXPECT(s.busy == 0 && s.phase == SMDEC_IDLE, "T2 idle service clears busy");
}

/* ---- S3, T2 to T4: writes, command acceptance and the header -------------------------- */
static void test_write_and_header(void)
{
    /* S3, T2: a CPU write to an idle controller lifts the credit to 1; the
     * word becomes the command for one cycle; the header waits. */
    power();
    source_mdec_write(&s, 0x20000002u, 0);
    EXPECT(s.command == 0x20000002u && s.busy == 1 && s.credit == 0 && s.phase == SMDEC_HEADER_WAIT && s.in_count == 0,
           "S3/T2 CPU command write: credit %d phase %u", s.credit, s.phase);
    source_mdec_run(&s, 0);
    EXPECT(s.phase == SMDEC_HEADER_WAIT, "T2 the header waits at credit 0");
    source_mdec_run(&s, 1);
    EXPECT(s.phase == SMDEC_INPUT && s.remaining == 1 && s.credit == 1, "T3/T5 header done: phase %u remaining %u credit %d",
           s.phase, s.remaining, s.credit);
    /* S3: a DMA write does not lift the credit. */
    power();
    source_mdec_write(&s, 0x20000002u, 1);
    EXPECT(s.busy == 1 && s.credit == -1 && s.phase == SMDEC_HEADER_WAIT, "S3/T2 DMA command write: credit %d", s.credit);
    source_mdec_run(&s, 1);
    EXPECT(s.phase == SMDEC_HEADER_WAIT && s.credit == 0, "T2 credit 0 is not enough");
    /* S3: a CPU write to a busy controller does not lift the credit. */
    source_mdec_write(&s, 0xFE00FE00u, 0);
    EXPECT(s.credit == 0 && s.in_count == 1, "S3 CPU write while busy: credit %d", s.credit);
    s.credit = -7;
    source_mdec_write(&s, 0xFE00FE00u, 0);
    EXPECT(s.credit == -7 && s.in_count == 2, "S3 CPU write while busy keeps a debt: credit %d", s.credit);
    /* S3: a credit above 1 is not lowered. */
    power();
    s.credit = 5;
    source_mdec_write(&s, 0x00000000u, 0);
    EXPECT(s.credit == 4, "S3 CPU write with credit 5: %d, want 4", s.credit);
    /* S3: a full input queue. The header waits (credit below 1), so nothing is consumed. */
    power();
    source_mdec_write(&s, 0x20000040u, 1);
    for (unsigned i = 0; i < 32; ++i) source_mdec_write(&s, 0x1000u + i, 1);
    EXPECT(s.in_count == 32 && (source_mdec_status(&s) >> 30 & 1u), "S3/S1 32 queued words");
    SourceMDEC before = s;
    source_mdec_write(&s, 0xDEADu, 1);
    source_mdec_write(&s, 0xBEEFu, 0);
    EXPECT(equal_but_control(&s, &before) && s.control == before.control, "S3 a write to a full queue changes nothing");
    EXPECT(s.in[(s.in_at + 31u) & 31u] == 0x101Fu, "S3 the queue keeps its 32 words");

    /* T3: the decode header, for the four depths. */
    for (unsigned depth = 0; depth < 4; ++depth) {
        unsigned row_words = depth == 2 ? 6 : depth == 3 ? 4 : 0;
        power();
        s.out_count = 3; s.out_at = 7; s.pixel_count = 5; s.coefficient = 9; s.encoded_count = 4;
        s.row = 3; s.word_in_row = 1; s.block = 4;
        source_mdec_write(&s, 0x20000010u | depth << 27, 0);
        source_mdec_run(&s, 1);
        EXPECT(s.phase == SMDEC_INPUT && s.busy == 1 && s.remaining == 0x000F, "T3/T5 depth %u: phase %u remaining %04X", depth,
               s.phase, s.remaining);
        EXPECT(s.out_at == 0 && s.out_count == 0 && s.pixel_count == 0 && s.coefficient == 0 && s.encoded_count == 0,
               "T3 depth %u: the output queue and the block fields are cleared", depth);
        EXPECT(s.block == (depth >= 2 ? 0u : 2u), "T3 depth %u: first block %u", depth, s.block);
        EXPECT(s.row_words == row_words && s.row == 0 && s.word_in_row == row_words, "T3 depth %u: row_words %u row %u word_in_row %u",
               depth, s.row_words, s.row, s.word_in_row);
    }
    /* T3, T5, S2 as the rows read (SPEC-QUESTIONS 7): a decode command with a
     * count of 0 leaves FFFFh at the header, asks for no input, and still
     * takes data words. */
    power();
    source_mdec_control(&s, 0x40000000u);
    source_mdec_write(&s, 0x20000000u, 0);
    source_mdec_run(&s, 1);
    EXPECT(s.phase == SMDEC_INPUT && s.busy == 1 && s.remaining == 0xFFFF && !source_mdec_can_write(&s),
           "T3/T5/S2 decode with count 0: phase %u remaining %04X", s.phase, s.remaining);
    source_mdec_write(&s, 0xFE00FE00u, 0);
    EXPECT(s.remaining == 0xFFFE && s.in_count == 0 && s.phase == SMDEC_INPUT && s.busy == 1 && source_mdec_can_write(&s),
           "T5 decode with count 0 takes a data word: phase %u remaining %04X", s.phase, s.remaining);
    /* T4: the table headers keep the output queue and the decode fields. */
    for (unsigned colour = 0; colour < 2; ++colour) {
        power();
        s.quant_index = 77; s.out_count = 3; s.pixel_count = 5; s.coefficient = 9; s.encoded_count = 4; s.block = 4;
        source_mdec_write(&s, 0x40000000u | colour, 0);
        source_mdec_run(&s, 1);
        EXPECT(s.phase == SMDEC_INPUT && s.remaining == (colour ? 31 : 15) && s.quant_index == 0,
               "T4/T5 quant header, colour %u: remaining %u index %u", colour, s.remaining, s.quant_index);
        EXPECT(s.out_count == 3 && s.pixel_count == 5 && s.coefficient == 9 && s.encoded_count == 4 && s.block == 4,
               "T4 a quant header clears nothing else");
    }
    power();
    s.matrix_index = 33; s.out_count = 3; s.pixel_count = 5;
    source_mdec_write(&s, 0x60000000u, 0);
    source_mdec_run(&s, 1);
    EXPECT(s.phase == SMDEC_INPUT && s.remaining == 31 && s.matrix_index == 0 && s.out_count == 3 && s.pixel_count == 5,
           "T4/T5 scale header: remaining %u index %u", s.remaining, s.matrix_index);
    /* T4: operations 0 and 4 to 7 have no data phase. */
    static const unsigned plain[5] = { 0, 4, 5, 6, 7 };
    for (unsigned i = 0; i < 5; ++i) {
        power();
        source_mdec_write(&s, plain[i] << 29 | 0x1234u, 0);
        source_mdec_run(&s, 1);
        EXPECT(s.phase == SMDEC_IDLE && s.busy == 0 && s.remaining == 0x1234 && s.credit == 1,
               "T4 operation %u: phase %u busy %u remaining %04X credit %d", plain[i], s.phase, s.busy, s.remaining, s.credit);
    }
    /* T4, T2: a queued next command is accepted in the same service, for one cycle more. */
    power();
    source_mdec_write(&s, 0x00001234u, 1);        /* accepted: credit -1 */
    source_mdec_write(&s, 0x40000000u, 1);        /* queued behind the waiting header */
    EXPECT(s.in_count == 1 && s.phase == SMDEC_HEADER_WAIT, "T2 the second command waits in the queue");
    source_mdec_run(&s, 5);
    EXPECT(s.command == 0x40000000u && s.phase == SMDEC_INPUT && s.busy == 1 && s.credit == 3 && s.remaining == 15 && s.in_count == 0,
           "T4 next command in the same service: command %08X phase %u credit %d remaining %u", s.command, s.phase, s.credit, s.remaining);
}

/* ---- B1, T5, T6: table words ------------------------------------------------------------ */
static void test_tables(void)
{
    static const struct { uint32_t command; unsigned words, operation, step, wrap; } table[3] = {
        { 0x40000000u, 16, 2, 4, 128 }, { 0x40000001u, 32, 2, 4, 128 }, { 0x60000000u, 32, 3, 2, 64 } };
    for (unsigned t = 0; t < 3; ++t) {
        power();
        source_mdec_write(&s, table[t].command, 0);
        source_mdec_run(&s, 1);
        s.credit = -50;                           /* T5: input needs no credit and costs none */
        for (unsigned i = 0; i < table[t].words; ++i) {
            EXPECT(s.phase == SMDEC_INPUT && s.busy == 1, "T6 table %u still takes words before word %u", t, i);
            source_mdec_write(&s, 0xC0DE0000u + i, 1);
        }
        int right = owner.tables == table[t].words;
        for (unsigned i = 0; right && i < table[t].words; ++i)
            right = owner.table_op[i] == table[t].operation && owner.table_index[i] == i * table[t].step &&
                    owner.table_word[i] == 0xC0DE0000u + i;
        EXPECT(right, "B1 table %u: %u calls with operation, index and word", t, owner.tables);
        unsigned index = table[t].operation == 2 ? s.quant_index : s.matrix_index;
        EXPECT(index == table[t].words * table[t].step % table[t].wrap, "B1 table %u: index %u after the last word", t, index);
        EXPECT(s.remaining == 0xFFFF && s.phase == SMDEC_IDLE && s.busy == 0 && s.in_count == 0,
               "T6 table %u: remaining %04X phase %u busy %u", t, s.remaining, s.phase, s.busy);
        EXPECT(s.credit == -50, "T5 table input is not charged: credit %d", s.credit);
        EXPECT(owner.blocks == 0, "B1 a table word is no decode data");
    }
}

/* ---- B5, B6, T5, T6: decode data, cost and output ---------------------------------------- */
static void test_decode_mono(void)
{
    power();
    source_mdec_write(&s, 0x20000003u, 0);        /* depth 0, three data words */
    source_mdec_run(&s, 1);
    /* B2, B5: low halfword first. 0401h begins a block, FE00h ends it. */
    source_mdec_write(&s, 0xFE000401u, 1);
    EXPECT(owner.blocks == 1 && owner.count == 2 && owner.encoded[0] == 0x0401 && owner.encoded[1] == 0xFE00 && owner.index == 2,
           "B2/B5 the low halfword comes first");
    EXPECT(s.credit == 1 - 474 && s.phase == SMDEC_BLOCK_WAIT && s.remaining == 1 && s.pixel_count == 8 && s.out_count == 0,
           "B5 the cost comes off the credit: credit %d phase %u remaining %u", s.credit, s.phase, s.remaining);
    source_mdec_run(&s, 128); source_mdec_run(&s, 128); source_mdec_run(&s, 128);
    source_mdec_run(&s, 89);
    EXPECT(s.credit == 0 && s.phase == SMDEC_BLOCK_WAIT && s.out_count == 0, "B5 no output at credit 0: credit %d", s.credit);
    source_mdec_run(&s, 1);
    EXPECT(s.credit == 1 && s.out_count == 8 && s.pixel_at == 8 && s.phase == SMDEC_INPUT,
           "B5/B6 output at credit 1, at no cost: credit %d queued %u phase %u", s.credit, s.out_count, s.phase);
    /* T5, B5: a word of padding counts as a data word, clears the output
     * count and still passes through the block wait. */
    source_mdec_write(&s, 0xFE00FE00u, 1);
    EXPECT(s.remaining == 0 && s.pixel_count == 0 && owner.blocks == 1 && s.phase == SMDEC_INPUT && s.credit == 1 && s.busy == 1,
           "T5/B5 padding word: remaining %u count %u phase %u", s.remaining, s.pixel_count, s.phase);
    /* B5: zero-cost data waits for credit above zero like any other. */
    s.credit = 0;
    source_mdec_write(&s, 0xFE00FE00u, 1);
    EXPECT(s.remaining == 0xFFFF && s.phase == SMDEC_BLOCK_WAIT && s.busy == 1, "B5 zero-cost data enters the block wait: phase %u", s.phase);
    source_mdec_run(&s, 1);
    /* T6: after the last word's output the command is over; busy clears with output still queued. */
    EXPECT(s.phase == SMDEC_IDLE && s.busy == 0 && s.out_count == 8, "T6 idle after the last word: phase %u busy %u queued %u",
           s.phase, s.busy, s.out_count);
    /* B6, S4: the words come out in order; a CPU read reports no adjustment. */
    for (unsigned i = 0; i < 8; ++i) {
        uint32_t offset = 0xFEED, value = source_mdec_read(&s, 0, &offset);
        EXPECT(value == pixel_value(1, i) && offset == 0, "B6/S4 output word %u: %08X offset %u", i, value, offset);
    }
    uint32_t offset = 0xFEED;
    EXPECT(source_mdec_read(&s, 0, &offset) == 0 && offset == 0 && source_mdec_read(&s, 1, &offset) == 0 && offset == 0 &&
           s.out_count == 0, "S4 an empty read gives zero and offset zero");
    EXPECT(source_mdec_read(&s, 0, NULL) == 0, "S4 an empty read with no offset pointer");
}

static void test_decode_colour_backpressure(void)
{
    power();
    owner.give = 48;
    source_mdec_control(&s, 0x60000000u);
    source_mdec_write(&s, 0x30000008u, 0);        /* depth 2, eight data words */
    source_mdec_run(&s, 1);
    /* Blocks 0 and 1 give no output (B4). */
    for (unsigned i = 0; i < 2; ++i) {
        source_mdec_write(&s, 0xFE000001u, 1);
        source_mdec_run(&s, 1000000);
        EXPECT(s.out_count == 0 && s.phase == SMDEC_INPUT && s.block == i + 1, "B4/B6 block %u queues nothing", i);
    }
    /* Block 2 gives 48 words: 32 fit (B6). */
    source_mdec_write(&s, 0xFE000001u, 1);
    source_mdec_run(&s, 1000000);
    EXPECT(s.out_count == 32 && s.pixel_at == 32 && s.pixel_count == 48 && s.phase == SMDEC_OUTPUT && source_mdec_can_read(&s),
           "B6 a full queue keeps the rest: queued %u at %u phase %u", s.out_count, s.pixel_at, s.phase);
    EXPECT(source_mdec_can_write(&s) && s.busy == 1, "S2 the input request does not depend on the phase");
    /* S4, B6: a CPU read takes one word and refills nothing. */
    uint32_t offset = 0xFEED;
    uint32_t value = source_mdec_read(&s, 0, &offset);
    EXPECT(value == pixel_value(3, 0) && offset == 0 && s.out_count == 31 && s.pixel_at == 32 && s.word_in_row == 6 && s.row == 0,
           "S4/B6 CPU read: %08X queued %u at %u", value, s.out_count, s.pixel_at);
    source_mdec_run(&s, 0);                       /* a service refills (B6) */
    EXPECT(s.out_count == 32 && s.pixel_at == 33, "B6 a service refills: queued %u at %u", s.out_count, s.pixel_at);
    /* S4, B6, A2: a DMA read takes one word and its service refills at once. */
    value = source_mdec_read(&s, 1, &offset);
    EXPECT(value == pixel_value(3, 1) && offset == 0 && s.out_count == 32 && s.pixel_at == 34 && s.word_in_row == 5,
           "S4/B6 DMA read: %08X queued %u at %u", value, s.out_count, s.pixel_at);
    int order = 1;
    for (unsigned i = 2; i < 48; ++i) order = order && source_mdec_read(&s, 1, &offset) == pixel_value(3, i);
    EXPECT(order && s.out_count == 0 && s.phase == SMDEC_INPUT && s.remaining == 4, "B6 the 48 words come out in order; then input again");
    EXPECT(source_mdec_can_write(&s), "S2 input request: enabled, busy, empty input queue, words remaining");
}

/* ---- B5 with B3 and T1: a data word whose block ends in an error -------------------------- */
static void test_decode_error(void)
{
    power();
    owner.give = 49;                              /* B3: more than 48 output words is an error */
    source_mdec_write(&s, 0x20000003u, 0);
    source_mdec_run(&s, 1);
    s.credit = 0; s.pixel_count = 7; s.pixel_at = 5;
    source_mdec_write(&s, 0xFE000001u, 1);
    EXPECT(s.error == 1 && owner.blocks == 1 && owner.count == 2 && s.in_count == 0, "B3/B5 the data word reaches the owner and sets the error");
    EXPECT(s.phase == SMDEC_INPUT && s.credit == 0 && s.pixel_at == 5 && s.block == 2 && s.out_count == 0,
           "B5 an error stops before the phase changes: phase %u credit %d pixel_at %u", s.phase, s.credit, s.pixel_at);
    EXPECT(s.pixel_count == 0, "B5 the output count was cleared before the halves: %u", s.pixel_count);
    /* T1: from then on a service changes nothing. */
    source_mdec_write(&s, 0xFE000001u, 1);
    source_mdec_run(&s, 100);
    EXPECT(s.in_count == 1 && owner.blocks == 1 && s.credit == 0 && s.phase == SMDEC_INPUT, "T1 no service after the error: queued %u credit %d",
           s.in_count, s.credit);
}

/* ---- A1, A2: the DMA output-address adjustment ------------------------------------------ */
static void test_adjustment(void)
{
    static const struct { unsigned row; int32_t adjustment; } known[7] = {
        { 0, 0 }, { 1, 6 }, { 7, 42 }, { 8, -42 }, { 9, -36 }, { 15, 0 }, { 16, 0 } };
    for (unsigned row_words = 4; row_words <= 6; row_words += 2) {
        power();
        s.row_words = (uint8_t)row_words; s.word_in_row = (uint8_t)row_words;
        int right = 1;
        for (unsigned read = 0; read < 18 * row_words; ++read) {
            unsigned row = read / row_words;
            uint32_t want = (row & 7u) * row_words - ((row & 8u) ? 7u * row_words : 0u), offset = 0xFEED;
            if (!s.out_count) { s.out_count = 32; s.out_at = 0; }
            source_mdec_read(&s, 1, &offset);
            right = right && offset == want && s.row == (read + 1) / row_words &&
                    s.word_in_row == row_words - (read + 1) % row_words;
        }
        EXPECT(right, "A1/A2 18 rows of %u words", row_words);
    }
    for (unsigned i = 0; i < 7; ++i) {            /* A1 by hand, row_words 6 */
        uint32_t offset = 0xFEED;
        power();
        s.row_words = 6; s.word_in_row = 6; s.row = (uint8_t)known[i].row; s.out_count = 1;
        source_mdec_read(&s, 1, &offset);
        EXPECT(offset == (uint32_t)known[i].adjustment, "A1 row %u: adjustment %d, want %d", known[i].row, (int32_t)offset, known[i].adjustment);
    }
    /* A2: both counters are 8 bits wide; a CPU or empty read moves neither. */
    uint32_t offset;
    power();
    s.row_words = 6; s.word_in_row = 1; s.row = 255; s.out_count = 3;
    source_mdec_read(&s, 0, &offset);
    EXPECT(s.row == 255 && s.word_in_row == 1 && offset == 0, "A2 a CPU read moves no counter");
    source_mdec_read(&s, 1, &offset);
    EXPECT(s.row == 0 && s.word_in_row == 6, "A2 row wraps at 256: row %u word_in_row %u", s.row, s.word_in_row);
    power();
    s.out_count = 2;                              /* row_words 0, as for depths 0 and 1 */
    source_mdec_read(&s, 1, &offset);
    EXPECT(s.word_in_row == 255 && s.row == 0 && offset == 0, "A2 word_in_row wraps: %u", s.word_in_row);
    power();
    s.row_words = 6; s.word_in_row = 3; s.row = 9;
    source_mdec_read(&s, 1, &offset);
    EXPECT(s.word_in_row == 3 && s.row == 9 && offset == 0, "A1/A2 an empty DMA read moves no counter");
    /* S4: only a DMA read serves the controller. */
    power();
    s.out_count = 2; s.in[0] = 0x00000007u; s.in_count = 1;
    source_mdec_read(&s, 0, &offset);
    EXPECT(s.in_count == 1 && s.phase == SMDEC_IDLE, "S4 a CPU read does not serve the controller");
    source_mdec_read(&s, 1, &offset);
    EXPECT(s.in_count == 0 && s.phase == SMDEC_HEADER_WAIT && s.command == 7, "S4 a DMA read serves the controller");
}

/* ---- S5: control and reset -------------------------------------------------------------- */
static void test_control(void)
{
    power();
    memset(s.in, 0x5A, sizeof s.in); memset(s.out, 0x6B, sizeof s.out);
    memset(s.pixels, 0x7C, sizeof s.pixels); memset(s.encoded, 0x1D, sizeof s.encoded);
    s.in_at = 3; s.in_count = 4; s.out_at = 5; s.out_count = 6; s.command = 0x30001111u; s.credit = -99;
    s.remaining = 0x2222; s.phase = SMDEC_OUTPUT; s.busy = 1; s.coefficient = 7; s.encoded_count = 8; s.block = 3;
    s.pixel_count = 40; s.pixel_at = 12; s.quant_index = 20; s.matrix_index = 30; s.row = 9; s.word_in_row = 2; s.row_words = 6;
    s.error = 1; s.block_cycles = 512;
    SourceMDEC before = s;
    source_mdec_control(&s, 0x5FFF1234u);
    EXPECT(s.control == 0x5FFF1234u && equal_but_control(&s, &before), "S5 a control write without bit 31 keeps bits 30-0 and changes nothing else");
    source_mdec_control(&s, 0xE0001234u);
    EXPECT(s.control == 0x60001234u, "S5 control keeps bits 30-0: %08X", s.control);
    EXPECT(s.phase == SMDEC_IDLE && s.remaining == 0 && s.command == 0 && s.busy == 0 && s.pixel_count == 0 && s.credit == 0 &&
           s.quant_index == 0 && s.matrix_index == 0 && s.coefficient == 0 && s.encoded_count == 0 && s.block == 0 &&
           s.in_at == 0 && s.in_count == 0 && s.out_at == 0 && s.out_count == 0 && s.error == 0, "S5 reset clears the listed fields");
    EXPECT(!memcmp(s.in, before.in, sizeof s.in) && !memcmp(s.out, before.out, sizeof s.out) &&
           !memcmp(s.pixels, before.pixels, sizeof s.pixels) && !memcmp(s.encoded, before.encoded, sizeof s.encoded) &&
           s.pixel_at == 12 && s.row == 9 && s.word_in_row == 2 && s.row_words == 6 && s.decode == on_block &&
           s.table == on_table && s.context == &owner && s.block_cycles == 512, "S5/S6 reset keeps the arrays, the address state, the callbacks and block_cycles");
    EXPECT(source_mdec_status(&s) == 0x80000000u, "S1 status after reset %08X", source_mdec_status(&s));
    /* S5, T1: reset ends an error; S3: until then a write still queues its word. */
    power();
    source_mdec_run(&s, 1000001);
    source_mdec_write(&s, 0x40000000u, 0);
    EXPECT(s.error == 1 && s.in_count == 1 && s.credit == 1 && s.phase == SMDEC_IDLE && s.command == 0,
           "S3/T1 a write under an error queues the word and is not served: queued %u credit %d", s.in_count, s.credit);
    s.out_count = 1; s.out[0] = 0x77;
    EXPECT(source_mdec_read(&s, 1, NULL) == 0x77 && s.out_count == 0 && s.in_count == 1, "S4/T1 a read under an error still takes a word");
    source_mdec_control(&s, 0x80000000u);
    source_mdec_write(&s, 0x40000000u, 0);
    EXPECT(s.error == 0 && s.command == 0x40000000u && s.phase == SMDEC_HEADER_WAIT, "S5 after a reset the controller works again");
}

int main(void)
{
    test_power_status_requests();
    test_half();
    test_time();
    test_write_and_header();
    test_tables();
    test_decode_mono();
    test_decode_colour_backpressure();
    test_decode_error();
    test_adjustment();
    test_control();
    printf("MDEC source FIFO rows (spec S1-S6, T1-T6, B1-B6, A1-A2): %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
