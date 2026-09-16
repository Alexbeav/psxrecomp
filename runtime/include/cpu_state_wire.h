#ifndef PSX_CPU_STATE_WIRE_H
#define PSX_CPU_STATE_WIRE_H
#include "cpu_state.h"
#include "pst_wire.h"

/* Architectural registers plus guest-clock pipeline timing, without pointers. */
#define CPU_STATE_WIRE_BYTES 580u
static inline int cpu_state_wire_write(uint8_t *out, const CPUState *cpu) {
    PstW w; pst_w_init(&w, out, CPU_STATE_WIRE_BYTES);
    for (unsigned i=0;i<32;i++) pst_w_u32(&w,cpu->gpr[i]);
    pst_w_u32(&w,cpu->pc); pst_w_u32(&w,cpu->hi); pst_w_u32(&w,cpu->lo);
    for (unsigned i=0;i<32;i++) pst_w_u32(&w,cpu->cop0[i]);
    for (unsigned i=0;i<32;i++) pst_w_u32(&w,cpu->gte_data[i]);
    for (unsigned i=0;i<32;i++) pst_w_u32(&w,cpu->gte_ctrl[i]);
    pst_w_u64(&w,cpu->muldiv_ts_done); pst_w_u64(&w,cpu->gte_ts_done);
    pst_w_bytes(&w,cpu->read_absorb,33);
    pst_w_u8(&w,cpu->read_absorb_which); pst_w_u8(&w,cpu->read_fudge);
    pst_w_u8(&w,cpu->ld_which_t); pst_w_u32(&w,cpu->ld_absorb);
    return w.written==CPU_STATE_WIRE_BYTES;
}
static inline int cpu_state_wire_read(const uint8_t *in, uint32_t len, CPUState *cpu) {
    if(len!=CPU_STATE_WIRE_BYTES || in[573]>32u || in[574]>32u || in[575]>32u) return 0;
    PstR r; pst_r_init(&r,in,len);
    for (unsigned i=0;i<32;i++) pst_r_u32(&r,&cpu->gpr[i]);
    pst_r_u32(&r,&cpu->pc); pst_r_u32(&r,&cpu->hi); pst_r_u32(&r,&cpu->lo);
    for (unsigned i=0;i<32;i++) pst_r_u32(&r,&cpu->cop0[i]);
    for (unsigned i=0;i<32;i++) pst_r_u32(&r,&cpu->gte_data[i]);
    for (unsigned i=0;i<32;i++) pst_r_u32(&r,&cpu->gte_ctrl[i]);
    pst_r_u64(&r,&cpu->muldiv_ts_done); pst_r_u64(&r,&cpu->gte_ts_done);
    pst_r_bytes(&r,cpu->read_absorb,33);
    pst_r_u8(&r,&cpu->read_absorb_which); pst_r_u8(&r,&cpu->read_fudge);
    pst_r_u8(&r,&cpu->ld_which_t); pst_r_u32(&r,&cpu->ld_absorb);
    return 1;
}
#endif
