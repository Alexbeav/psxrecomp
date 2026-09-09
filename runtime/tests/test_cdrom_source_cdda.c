/* Authored disc/command transcript driver. Includes production controller.
 * Source oracle and declared projection are bound in the adjacent JSON.
 * Opcodes: 0 update absolute clock, 1 MMIO write, 2 read and emit, 3 emit
 * 13-field projection, 4 warm-up read without comparison, 5 emit fresh PCM.
 * Input/output are little-endian. Warm-up uses ordinary Reset/GetStat;
 * initial tray/spin image and downstream SPU mixing are not unit claims. */
/* Controller-level regression. Synthetic sectors only; no BIOS/game data.
 * Include the production unit to seed the laser position without reading an
 * entire synthetic disc. All commands and advancement use its real MMIO API.
 */
#include "../src/cdrom.c"

uint64_t psx_cycle_count, s_frame_count;
uint32_t i_stat, g_debug_current_func_addr, g_debug_last_store_pc;
uint32_t debug_guest_ra(void) { return 0; }
static int reads, failures, irq_raises;
void psx_irq_raise(uint32_t a, uint32_t b) { (void)b; i_stat |= 1u << a; irq_raises++; }
void event_ring_record(uint16_t a, uint8_t b) { (void)a; (void)b; }
void event_ring_record_aux(uint16_t a, uint8_t b, uint32_t c) { (void)a; (void)b; (void)c; }
void audio_trace_event(uint16_t a, uint32_t b, uint32_t c) { (void)a; (void)b; (void)c; }
uint32_t interrupts_get_cycles_since_vblank(void) { return 0; }
int dma_cdrom_transfer_active(void) { return 0; }
static int16_t last_pcm[1176];static uint64_t audio_frames;static uint32_t audio_hash=2166136261u;
void spu_cd_audio_push(const int16_t *a,int n){if(n!=588)abort();memcpy(last_pcm,a,sizeof(last_pcm));for(int i=0;i<n*2;i++){uint16_t v=(uint16_t)a[i];audio_hash=(audio_hash^(v&255))*16777619u;audio_hash=(audio_hash^(v>>8))*16777619u;}audio_frames+=n;}
void spu_cd_audio_reset(void){}
int psx_netplay_active(void) { return 0; }
int psx_netplay_cd_bisect_active(void) { return 0; }
int psx_netplay_is_resimulating(void) { return 0; }
uint32_t psx_netplay_sim_tick(void) { return 0; }
void *iso_open(const char *p) { (void)p; return (void *)1; }
void iso_close(void *p) { (void)p; }
int iso_read_sector(void *p, uint32_t lba, uint8_t *b, int n) {
    (void)p; memset(b, (int)(lba & 255u), (size_t)n); reads++; return 1;
}

int iso_read_raw_sector(void *p,uint32_t lba,uint8_t *b,int n){(void)p;if(n!=2352)return 0;for(unsigned i=0;i<588;i++){uint16_t l=(uint16_t)(lba*17+i*3),r=(uint16_t)(lba*31-i*7);b[i*4]=l;b[i*4+1]=l>>8;b[i*4+2]=r;b[i*4+3]=r>>8;}return 1;}
static const unsigned starts[]={0,0,240,600,960};
static void q_msf(unsigned n,uint8_t *b){b[0]=bin_to_bcd(n/4500);b[1]=bin_to_bcd(n/75%60);b[2]=bin_to_bcd(n%75);}
int iso_read_subq(void *p,uint32_t lba,uint8_t *q,int n,int *v){(void)p;if(n!=12)return 0;int track=lba<90?1:lba<450?2:lba<960?3:100;int index=track==100||lba>=starts[track];memset(q,0,12);q[0]=track==1?0x41:1;q[1]=track==100?0xaa:bin_to_bcd(track);q[2]=index;q_msf(track==100?0:abs((int)lba-(int)starts[track]),q+3);q_msf(lba+150,q+7);unsigned crc=0;for(int i=0;i<10;i++){crc^=(unsigned)q[i]<<8;for(int j=0;j<8;j++)crc=((crc<<1)^((crc&0x8000)?0x1021:0))&65535;}q[10]=(~crc)>>8;q[11]=~crc;*v=1;return 1;}
int iso_has_subq_replacements(void *p){(void)p;return 0;}
uint32_t iso_sector_count(void *p){(void)p;return 960;}
int iso_track_count(void *p){(void)p;return 3;}
uint32_t iso_track_start_lba(void *p,int t){(void)p;return t==0?960:starts[t];}
uint32_t iso_track_pregap_lba(void *p,int t){(void)p;return t==2?90:t==3?450:0;}
int iso_track_is_audio(void *p,int t){(void)p;return t==2||t==3;}
static void env(const char *k,const char *v){
#ifdef _WIN32
_putenv_s(k,v);
#else
setenv(k,v,1);
#endif
}
static void out(FILE *f,uint32_t v){uint8_t b[]={v,v>>8,v>>16,v>>24};if(fwrite(b,1,4,f)!=4)exit(3);}
int main(int argc,char **argv){if(argc!=4 && argc!=5)return 2;
env("PSX_CD_EXPLICIT_SEEK_MODEL","octoshock-2.2.2");env("PSX_CD_TOC_SEEK_MODEL","octoshock-2.2.2");env("PSX_CD_READ_START_MODEL","octoshock-2.2.2-pipeline");env("PSX_CD_COLD_STATUS_MODEL","octoshock-2.2.2");env("PSX_CD_SOURCE_CLOCK_TAPE",argv[1]);env("PSX_CD_CDDA_MODEL","octoshock-2.3");
cdrom_init("authored");
/* Match the isolated oracle's explicit powered, disc-poked STOPPED head.
 * This unit precondition is not the runtime's production cold-boot image. */
if(argc==5){if(!strcmp(argv[4],"capture"))cdrom_snapshot_bytes();else if(!strcmp(argv[4],"write-state"))cdrom_snapshot_write(NULL);else if(!strcmp(argv[4],"scan"))cdrom_write(0x1f801801,4);else if(!strcmp(argv[4],"restore"))return cdrom_snapshot_read(NULL,0)?1:0;else if(!strcmp(argv[4],"double-speed")){mode_reg=0x80;start_source_cdda(2);}else if(!strcmp(argv[4],"active-read")){reading=1;start_source_cdda(2);}return 1;}
stat_reg=CDSTAT_SHELL;s_source_seek_paused=0;read_sec=2;setloc_pending=1;s_setloc_lba=0;
FILE *in=fopen(argv[2],"rb"),*f=fopen(argv[3],"wbx");if(!in||!f)return 3;uint8_t bytes[16];
while(fread(bytes,1,16,in)==16){uint32_t o[4];for(int i=0;i<4;i++)o[i]=cd_tape_le32(bytes+i*4);
if(o[0]==0){if(o[1]<psx_cycle_count)return 4;uint32_t delta=o[1]-psx_cycle_count;psx_cycle_count=o[1];cdrom_advance(delta);}
else if(o[0]==1)cdrom_write(0x1f801800+o[2],o[3]);
else if(o[0]==2)out(f,cdrom_read(0x1f801800+o[2]));
else if(o[0]==4)(void)cdrom_read(0x1f801800+o[2]);
else if(o[0]==5){for(unsigned i=0;i<588;i++)out(f,(uint16_t)last_pcm[i*2]|((uint32_t)(uint16_t)last_pcm[i*2+1]<<16));}
else if(o[0]==3){out(f,stat_reg);out(f,source_cdda.seeking?1:cdda_playing?4:!(stat_reg&2)?0:s_source_seek_paused?~0u:~1u);out(f,(uint32_t)msf_to_lba(read_min,read_sec,read_sect));out(f,source_cdda.sectors_read);out(f,cdda_playing?cdda_delay:0);out(f,mode_reg);out(f,irq_flag);out(f,source_cdda.pipe_count);out(f,source_cdda.report_last_tens);out(f,source_cdda.play_track_match);out(f,source_cdda.async_type);out(f,source_cdda.async_type?source_cdda.async_count:0);out(f,response_count-response_read);}
else return 5;}
fclose(in);fclose(f);return 0;}
