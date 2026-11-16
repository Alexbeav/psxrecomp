/* Public-command witness for the missing optional source clock. The backing
 * reader/FIFO stubs are our maintained authored synthetic controller fixture. */
#define main fifo_baseline_main
#include "test_cdrom_sector_fifo.c"
#undef main
static void select_model(const char *name,const char *value) {
#ifdef _WIN32
    if(_putenv_s(name,value)) abort();
#else
    if(setenv(name,value,1)) abort();
#endif
}
int main(int argc,char **argv) {
    CHECK(argc==2);
    select_model("PSX_CD_EXPLICIT_SEEK_MODEL","octoshock-2.2.2");
    select_model("PSX_CD_TOC_SEEK_MODEL","octoshock-2.2.2");
    select_model("PSX_CD_READ_START_MODEL","octoshock-2.2.2-pipeline");
    select_model("PSX_CD_SOURCE_CLOCK_TAPE",argv[1]);
    psx_cycle_count=0;cdrom_init("synthetic");i_stat=0;
    cdrom_write(0x1F801800,0);cdrom_write(0x1F801801,1);
    CHECK(irq_flag==0); /* zero tape: receive12315 + final8500 cycles */
    psx_cycle_count=12314;cdrom_advance(12314);CHECK(irq_flag==0);
    psx_cycle_count=12315;cdrom_advance(1);CHECK(irq_flag==0);
    psx_cycle_count=20814;cdrom_advance(8499);CHECK(irq_flag==0);
    psx_cycle_count=20815;cdrom_advance(1);CHECK(irq_flag==CDIRQ_ACK);
    CHECK(cdrom_read(0x1F801801)==CDSTAT_MOTOR);
    /* Retained source byte drain, separate from ordinary full-word DMA. */
    static const uint32_t padded[]={0,0x10,0x1110,0x121110,0x13121110};
    unsigned cases=0;
    for(int n=0;n<=4;n++) {
        RB_.size=n;RB_.pos=0;request_reg=CDROM_REQUEST_BFRD;
        for(int i=0;i<n;i++)RB_.data[i]=(uint8_t)(0x10+i);
        CHECK(cdrom_dma_read_padded()==padded[n] && RB_.pos==n);
        CHECK(cdrom_dma_read_padded()==0 && RB_.pos==n);
        RB_.pos=0;
        if(n<4)CHECK(cdrom_dma_read()==0 && RB_.pos==0);
        request_reg=0;
        CHECK(cdrom_dma_read_padded()==0 && RB_.pos==0);
        cases++;
    }
    printf("PASS: source GetStat admits at20815; padded byte-drain cases=%u; ordinary DMA/BFRD unchanged\n",cases);
    return 0;
}
