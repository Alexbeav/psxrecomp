/* Synthetic-disc adapter: CDDA and data views share this authored backing
 * image. These are fixture-only ISO callbacks, not an original oracle. */
#ifndef CDROM_ISO_FIXTURE_ADAPTER_H
#define CDROM_ISO_FIXTURE_ADAPTER_H
extern uint32_t iso_track_start_lba(void *,int);
extern uint32_t iso_track_pregap_lba(void *,int);
extern int iso_track_is_audio(void *,int);
int iso_cdda_track_count(void *h) {return iso_track_count(h);}
uint32_t iso_cdda_track_start_lba(void *h,int t) {return iso_track_start_lba(h,t);}
uint32_t iso_cdda_track_pregap_lba(void *h,int t) {return iso_track_pregap_lba(h,t);}
int iso_cdda_track_is_audio(void *h,int t) {return iso_track_is_audio(h,t);}
uint32_t iso_cdda_sector_count(void *h) {return iso_sector_count(h);}
int iso_read_cdda_sector(void *h,uint32_t lba,uint8_t *p,int n) {return iso_read_raw_sector(h,lba,p,n);}
int iso_read_cdda_subq(void *h,uint32_t lba,uint8_t *p,int n,int *valid) {return iso_read_subq(h,lba,p,n,valid);}
void savestate_set_disc_scope(int disc) {(void)disc;}
#endif
