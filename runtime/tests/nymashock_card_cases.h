/* Authored serial traffic shared by the source/native device fixtures.
 * Inputs only: replies, ACK deadlines and committed bytes come from each device. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static void card_read_case(const char *name, unsigned sector) {
    uint8_t tx[140] = {0x81,0x52,0,0};
    tx[4] = (uint8_t)(sector >> 8); tx[5] = (uint8_t)sector;
    card_transaction(name,tx,sizeof tx);
}
static void card_write_case(const char *name, unsigned sector, unsigned seed,
                            int bad_crc, unsigned count) {
    uint8_t tx[138] = {0x81,0x57,0,0};
    tx[4] = (uint8_t)(sector >> 8); tx[5] = (uint8_t)sector;
    uint8_t crc = tx[4] ^ tx[5];
    for (unsigned i=0;i<128;++i) { tx[6+i]=(uint8_t)(seed+i*53); crc^=tx[6+i]; }
    tx[134] = crc ^ (bad_crc ? 1 : 0);
    card_transaction(name,tx,count);
}
static void card_cases(void) {
    card_read_case("cold-header",0);
    card_read_case("cold-directory",1);
    card_read_case("cold-broken-sector-list",16);
    card_read_case("cold-last-sector",1023);
    card_read_case("invalid-read-0400",1024);
    card_read_case("invalid-read-ffff",65535);
    card_write_case("bad-checksum-before-first-write",63,17,1,138);
    card_read_case("bad-checksum-keeps-new-flag",63);
    card_write_case("invalid-write-before-first-write",1024,23,0,138);
    card_read_case("invalid-write-keeps-new-flag",63);
    card_write_case("valid-write",63,29,0,138);
    card_read_case("valid-write-readback",63);
    card_write_case("repeated-identical-write",63,29,0,138);
    card_write_case("bad-checksum-and-invalid-address",65535,31,1,138);
    for (unsigned cutoff=133;cutoff<=137;++cutoff) {
        char name[64];
        snprintf(name,sizeof name,"write-deselect-after-%u-bytes",cutoff);
        card_write_case(name,64+cutoff-133,41+cutoff,0,cutoff);
        snprintf(name,sizeof name,"read-after-%u-byte-write",cutoff);
        card_read_case(name,64+cutoff-133);
    }
    const uint8_t id[]={0x81,0x53,0,0,0,0,0,0,0x81,0x52,0,0};
    card_transaction("unsupported-S-until-deselect",id,sizeof id);
    const uint8_t unknown[]={0x81,0x99,0,0,0x81,0x52,0,0};
    card_transaction("unsupported-command-until-deselect",unknown,sizeof unknown);
    card_read_case("read-after-unsupported-command",63);
    card_power();
    card_read_case("power-restores-new-flag-retains-data",63);
}
