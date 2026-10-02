/* A BREAK enters the exception vector only when the guest owns that vector
 * (PS1G-74). No guest memory but the three vector words is read. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_state.h"
#include "../src/traps.c"

static uint32_t vector[3];
static int in_exception, source_profile;
int source_gpu_runtime_active(void) { return source_profile; }
int psx_get_in_exception(void) { return in_exception; }
static uint32_t read_vector(uint32_t addr) {
    assert(addr >= 0x80000080u && addr <= 0x80000088u && !(addr & 3u));
    return vector[(addr - 0x80000080u) >> 2];
}

static CPUState state(void) {
    CPUState c = {0};
    for (unsigned i = 0; i < 32; i++) c.gpr[i] = 0x1000 + i;
    c.pc = 0x8001015Cu; c.read_word = read_vector;
    c.cop0[12] = 0x40000401; c.cop0[13] = 0xf000057c; c.cop0[14] = 0x1234;
    return c;
}

static void set_vector(uint32_t w0, uint32_t w1, uint32_t w2) {
    vector[0] = w0; vector[1] = w1; vector[2] = w2;
}

static void refused(void) {
    CPUState c = state(), e = c;
    assert(enter_guest_break_exception(&c, 0x80010160u) == 0);
    assert(!memcmp(&c, &e, sizeof(c)));
}

static void entered(uint32_t cause) {
    CPUState c = state(), e = c;
    e.pc = 0x80000080u; e.cop0[14] = 0x80010160u;
    e.cop0[12] = 0x40000404; e.cop0[13] = cause;
    assert(enter_guest_break_exception(&c, 0x80010160u) == 1);
    assert(!memcmp(&c, &e, sizeof(c)));
}

int main(void) {
    /* The guest's stub: lui k0,0x8001 ; ori k0,k0,0x334c ; jr k0. */
    set_vector(0x3c1a8001u, 0x375a334cu, 0x03400008u);
    entered(0x70000524u);              /* code 9, BD clear, the rest kept */
    source_profile = 1; entered(0x00000524u); source_profile = 0;
    in_exception = 1; refused(); in_exception = 0;
    { CPUState c = state(), e; c.cop0[12] |= 0x00400000u; e = c;   /* SR.BEV */
      assert(enter_guest_break_exception(&c, 0x80010160u) == 0);
      assert(!memcmp(&c, &e, sizeof(c))); }

    /* The same target built with addiu: lui at,0x8002 ; addiu at,at,-0x7000. */
    set_vector(0x3c018002u, 0x24219000u, 0x00200008u);
    entered(0x70000524u);

    /* The BIOS's stub: lui k0,0 ; addiu k0,k0,0xc80 ; jr k0. */
    set_vector(0x3c1a0000u, 0x275a0c80u, 0x03400008u);
    refused();
    /* No vector, a ROM target, a jump through another register. */
    set_vector(0u, 0u, 0u); refused();
    set_vector(0x3c1abfc0u, 0x375a0180u, 0x03400008u); refused();
    set_vector(0x3c1a8001u, 0x375a334cu, 0x03600008u); refused();
    return 0;
}
