#include "cdrom_random_tape.h"
static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); failures++; } } while(0)
int main(int argc,char **argv) {
    CdRandomTape tape={0}; uint32_t value=99;
    static uint8_t authored[]={3,0,0,0,2,0,0,0,0,0,0,0};
    tape.bytes=malloc(sizeof(authored));memcpy(tape.bytes,authored,sizeof(authored));tape.count=3;
    CHECK(cd_tape_bounded(&tape,2,&value) && value==2 && tape.cursor==2);
    CHECK(cd_tape_bounded(&tape,0,&value) && value==0 && tape.cursor==3);
    value=99;CHECK(!cd_tape_bounded(&tape,5,&value) && value==99 && tape.cursor==3);
    uint8_t changed[32]={1};
    CHECK(!cd_tape_restore(&tape,changed,3,1) && tape.cursor==3);
    CHECK(!cd_tape_restore(&tape,tape.sha256,2,1) && tape.cursor==3);
    CHECK(!cd_tape_restore(&tape,tape.sha256,3,4) && tape.cursor==3);
    CHECK(cd_tape_restore(&tape,tape.sha256,3,1) && tape.cursor==1);
    CHECK(cd_tape_bounded(&tape,2,&value) && value==2 && tape.cursor==2);
    cd_tape_reset(&tape);CHECK(tape.cursor==0);
    CHECK(cd_tape_bounded(&tape,UINT32_MAX,&value) && value==3 && tape.cursor==1);
    cd_tape_free(&tape);
    if (argc==3 && !strcmp(argv[1],"--reject")) {
        CHECK(!cd_tape_load(&tape,argv[2]));
    }
    if (argc==2) {
        CHECK(cd_tape_load(&tape,argv[1]));
        printf("tape SHA256 ");for(unsigned i=0;i<32;i++)printf("%02x",tape.sha256[i]);puts("");
        static const uint32_t vectors[][3]={
            {3000,1137,3},{3000,103,1},{3000,900,1},{3000,952,2},{3000,2893,1},
            {25000,24420,1},{3000,2573,1},{3000,637,1},{3000,1936,2},{3000,2431,3},
            {25000,5306,5},{3000,2586,1},{3000,2704,1},{25000,22008,1},{3000,1775,1},
            {3000,2376,1},{3000,2138,2},{25000,19069,1},{3000,1518,2},{3000,1518,1},
            {25000,9362,3},{3000,1698,1}};
        for (unsigned i=0;i<sizeof(vectors)/sizeof(vectors[0]);i++) {
            uint32_t before=tape.cursor;
            CHECK(cd_tape_bounded(&tape,vectors[i][0],&value));
            CHECK(value==vectors[i][1] && tape.cursor-before==vectors[i][2]);
        }
        uint32_t cursor=tape.cursor;
        CHECK(cd_tape_load(&tape,argv[1]) && tape.cursor==0);
        CHECK(cd_tape_restore(&tape,tape.sha256,tape.count,cursor));
        printf("source vectors 22, raw cursor %u\n",cursor);
    }
    cd_tape_free(&tape);
    printf("random tape fixture: %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
