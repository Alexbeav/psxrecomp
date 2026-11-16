/* Integration of accepted authored source voice rows with the modern guest
 * clock, independent sample banks/private voices and separate CD snapshot.
 * Synthetic ADPCM and PCM only; no oracle/hardware qualification. */
#define main source_rows_main
#include "test_spu_source_voice_rows.c"
#undef main

static void run_prefix(int16_t pcm[96][2]) {
    for (unsigned i=0;i<96;i++) { tick(); pcm[i][0]=out_l; pcm[i][1]=out_r; }
}
int main(void) {
#ifdef _WIN32
    _putenv("PSX_GPU_DMA_MODEL=octoshock-2.2.2-bounded-quad");
#else
    setenv("PSX_GPU_DMA_MODEL","octoshock-2.2.2-bounded-quad",1);
#endif
    for(unsigned point=0;point<16;point++) {
#ifdef _WIN32
        _putenv(point&1u ? "PSX_GPU_DMA_MODEL=octoshock-2.2.2-bounded-quad" :
                          "PSX_GPU_DMA_MODEL=octoshock-2.2.2-bounded-linked-list");
#else
        setenv("PSX_GPU_DMA_MODEL",point&1u ? "octoshock-2.2.2-bounded-quad" :
                                          "octoshock-2.2.2-bounded-linked-list",1);
#endif
        power_on(0xC001u);
        put_block(0x1000,0,1,7);
        voice_setup(1,0x1000u,0x1000,HOLD_LO,HOLD_HI);
        uint8_t bank[32]={0}; bank[1]=7; memset(bank+2,0x33,14);
        CHECK(spu_register_sample_bank(1,bank,sizeof bank),"bank registered");
        uint16_t regs[8]={0x3FFF,0x3FFF,0x1000,0,HOLD_LO,HOLD_HI,0,0};
        CHECK(spu_private_voice_play(0,1,regs,0),"private voice started");
        voice_setup(2,0x1000u,0,HOLD_LO,HOLD_HI);
        CHECK(spu_bind_next_voice_bank(2,1),"hardware bank queued");
        int16_t cd[384]; for(unsigned i=0;i<384;i++) cd[i]=(int16_t)(i*19u);
        spu_cd_audio_push(cd,192);
        write_key_on(6); ticks((int)point);
        test_clock += 173u; /* retain a nonzero modern phase */
        write_key_on(point & 1u ? 2u : 0u);
        write_key_off(point & 2u ? 2u : 0u);
        uint32_t n=spu_snapshot_bytes(),cn=spu_cd_snapshot_bytes();
        uint8_t *saved=malloc(n),*again=malloc(n),*after=malloc(n),*cdwire=malloc(cn);
        uint8_t *savedram=malloc(SPU_RAM_SIZE),*endram=malloc(SPU_RAM_SIZE);
        CHECK(saved&&again&&after&&cdwire&&savedram&&endram,"allocations");
        if(!(saved&&again&&after&&cdwire&&savedram&&endram)) return 1;
        spu_snapshot_write(saved); spu_cd_snapshot_write(cdwire);
        memcpy(savedram,spu_ram,SPU_RAM_SIZE); uint64_t clock=test_clock;
        int16_t a[96][2],b[96][2]; run_prefix(a); spu_snapshot_write(after);
        memcpy(endram,spu_ram,SPU_RAM_SIZE);
        test_clock=clock; memcpy(spu_ram,savedram,SPU_RAM_SIZE);
        CHECK(spu_snapshot_read(saved,n),"source/private snapshot accepted point%u",point);
        CHECK(spu_cd_snapshot_read(cdwire,cn),"CD guest queue restored");
        run_prefix(b); spu_snapshot_write(again);
        CHECK(!memcmp(a,b,sizeof a),"PCM continuation point%u",point);
        CHECK(!memcmp(after,again,n),"complete state continuation point%u",point);
        CHECK(!memcmp(endram,spu_ram,SPU_RAM_SIZE),"RAM/capture continuation point%u",point);
        CHECK(voices[2].sample_bank==(point ? 1u : 0u) && voices[2].pending_bank==(point ? 0u : 1u) && voices[SPU_VOICE_COUNT].sample_bank==1,
              "bank and private bindings survive");
        if(point) CHECK(source_decode[2].samples[0]==12288,
                        "hardware source decoder reads immutable bank samples");
        /* Source footer is after the modern private records, before phase. */
        const unsigned offset=SPU_SNAPSHOT_BASE_BYTES+SPU_PRIVATE_WIRE_BYTES;
        const unsigned positions[]={0,4+3,8+3,12,36+64,36+65,36+66,36+67,36+68,36+69};
        for(unsigned i=0;i<sizeof positions/sizeof positions[0];i++) {
            memcpy(saved,again,n); saved[offset+positions[i]]=255;
            CHECK(!spu_snapshot_read(saved,n),"corrupt footer refused field%u",i);
            spu_snapshot_write(saved);
            CHECK(!memcmp(saved,again,n),"corrupt source refusal atomic field%u",i);
        }
        free(saved);free(again);free(after);free(cdwire);free(savedram);free(endram);
    }
    printf("source SPK2 + modern phase/private/banks/CD: %u checks, %u failures\n",checks,failures);
    return failures!=0;
}
