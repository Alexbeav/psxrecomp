/* Isolate the pre-existing globally enabled title repair using production SIO.
 * Synthetic unrelated RAM happens to match the repair's fixed-address probes.
 * No BIOS, disc, guest execution or real memory card is required. */
#define main unused_ack_fixture_main
#include "test_sio_ack_timing.c"
#undef main
int main(void) {
    for(int enabled=-1;enabled<2;enabled++) {
#ifdef _WIN32
        _putenv_s("PSX_APE_CARD_UNSTICK",enabled<0?"":enabled?"1":"0");
#else
        if(enabled<0) unsetenv("PSX_APE_CARD_UNSTICK");
        else setenv("PSX_APE_CARD_UNSTICK",enabled?"1":"0",1);
#endif
        s_ape_unstick_env=-1;s_ape_unstick_pending=0;s_ape_unstick_cool_cyc=0;
        setup(1,0);i_mask=0x0D;i_stat=1;test_unrelated_card_words=1;
        sio_write(0x1F801040,0x81);jump(1088);
        assert(memcard_is_present(0)==0 && !(sio_peek_stat()&0x80));
        sio_write(0x1F80104A,0x40); /* abort the absent-card probe */
        printf("repair=%d, no card, CTRL reset: I_MASK=%02X I_STAT=%02X\n",enabled,i_mask,i_stat);
        assert(i_mask==(enabled>0?0x8D:0x0D));
        assert(i_stat==(enabled>0?0x81:0x01));
    }
    puts("Legacy card repair: unrelated RAM can inject IRQ7/mask; existing off switch prevents it PASS");
    return 0;
}
