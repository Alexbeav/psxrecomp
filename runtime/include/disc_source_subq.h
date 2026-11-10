/* Authored raw-Q wire encoder. Layout, pregap countdown and CRC byte order
 * follow our accepted source-disc-subq-pregap behavior and black-box fixtures
 * in runtime/tests/source_disc_subq_fixtures.json. No leadout synthesis. */
#ifndef PSX_DISC_SOURCE_SUBQ_H
#define PSX_DISC_SOURCE_SUBQ_H
#include <stdint.h>
#include <string.h>
static inline uint8_t psx_source_q_bcd(uint32_t n) {
    return (uint8_t)(((n/10u)<<4)|(n%10u));
}
static inline void psx_source_subq(uint32_t lba,uint32_t start,unsigned track,
                                  int audio,uint8_t q[12]) {
    uint32_t relative=lba<start?start-lba:lba-start,absolute=lba+150u;
    memset(q,0,12);q[0]=audio?1u:0x41u;q[1]=psx_source_q_bcd(track);
    q[2]=lba<start?0u:1u;
    q[3]=psx_source_q_bcd(relative/4500u);q[4]=psx_source_q_bcd(relative/75u%60u);
    q[5]=psx_source_q_bcd(relative%75u);
    q[7]=psx_source_q_bcd(absolute/4500u);q[8]=psx_source_q_bcd(absolute/75u%60u);
    q[9]=psx_source_q_bcd(absolute%75u);
    uint16_t crc=0;
    for(unsigned i=0;i<10;i++) {
        crc^=(uint16_t)q[i]<<8;
        for(unsigned bit=0;bit<8;bit++)crc=(uint16_t)((crc<<1)^((crc&0x8000u)?0x1021u:0u));
    }
    crc=(uint16_t)~crc;q[10]=(uint8_t)(crc>>8);q[11]=(uint8_t)crc;
}
#endif
