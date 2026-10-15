// SYSTEM.CNF beyond 16 MiB wins over unrelated early cdrom: strings.
// The title's netplay disc rule holds for a .chd as for a cue (PS1B-294).
// The launcher's SBI row and the missing-SBI warning agree (PS1B-495).
#include "disc_identity.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs=std::filesystem;
static void both32(uint8_t* p,uint32_t value) {
    for(unsigned i=0;i<4;i++) {p[i]=(uint8_t)(value>>(8*i));p[7-i]=p[i];}
}
// boot is the SYSTEM.CNF file name. With decoy, an unrelated "cdrom:" string
// sits in the early image; without it, a copy of the real line does, so an
// image whose TOC cannot be mounted still names its serial to the early scan.
static void fixture(const fs::path& path,bool raw,const char* boot="SLPS_009.98",bool decoy=true) {
    const unsigned unit=raw?2352:2048,offset=raw?24:0;
    std::ofstream file(path,std::ios::binary);
    file.seekp(10000ull*unit-1);file.put(0);
    if(raw) {
        uint8_t sync[16]={};std::memset(sync+1,255,10);sync[15]=2;
        file.seekp(0);file.write((char*)sync,sizeof sync);
    }
    auto write=[&](unsigned lba,const std::vector<uint8_t>& data) {
        file.seekp((uint64_t)lba*unit+offset);
        file.write((const char*)data.data(),data.size());
    };
    std::vector<uint8_t> pvd(2048);
    pvd[0]=1;std::memcpy(pvd.data()+1,"CD001",5);pvd[6]=1;
    auto* root=pvd.data()+156;root[0]=34;both32(root+2,20);both32(root+10,2048);
    root[25]=2;root[28]=root[31]=root[32]=1;
    write(16,pvd);
    const std::string cnf=std::string("BOOT = cdrom:")+boot+";1\r\n";
    std::vector<uint8_t> directory(2048);
    directory[0]=46;both32(directory.data()+2,9000);both32(directory.data()+10,(uint32_t)cnf.size());
    directory[28]=directory[31]=1;directory[32]=12;
    std::memcpy(directory.data()+33,"SYSTEM.CNF;1",12);write(20,directory);
    write(9000,std::vector<uint8_t>(cnf.begin(),cnf.end()));
    const std::string early=decoy?"BOOT = cdrom:SLUS_999.99;1\r\n":cnf;
    write(40,std::vector<uint8_t>(early.begin(),early.end()));
}
// A mounted image as identify_disc describes it, without a file: the rule
// reads only these fields. No .chd can be made here (libchdr only reads), so
// the rule is exercised on the identity and the branch on a broken file.
static PSXRecompV4::DiscIdentity mounted(bool chd,bool cue,int tracks,const char* fp) {
    PSXRecompV4::DiscIdentity id;
    id.opened=id.has_header=id.toc_opened=true;
    id.from_chd=chd;id.from_cue=cue;id.track_count=tracks;id.disc_fp=fp;
    return id;
}
static void netplay_rule() {
    using namespace PSXRecompV4;
    const char* good="ab77cc9d";const char* other="3ad066bd";
    const std::string wrong_dump="Disc TOC fingerprint does not match this build's required dump.";
    NetplayDiscExpect kit;                 // what every netplay kit writes
    kit.require_cue=true;kit.required_tracks=1;kit.required_disc_fp=good;

    auto chd=mounted(true,false,1,good);   // the right .chd: accepted
    apply_netplay_disc_expect(chd,kit);
    assert(chd.netplay_ok && chd.netplay_detail.empty());

    chd=mounted(true,false,1,other);       // a wrong .chd: the sentence a wrong cue gets
    apply_netplay_disc_expect(chd,kit);
    assert(!chd.netplay_ok && chd.netplay_detail==wrong_dump);

    auto cue=mounted(false,true,1,good);   // cue: unchanged
    apply_netplay_disc_expect(cue,kit);
    assert(cue.netplay_ok);
    cue=mounted(false,true,1,other);
    apply_netplay_disc_expect(cue,kit);
    assert(!cue.netplay_ok && cue.netplay_detail==wrong_dump);

    auto bin=mounted(false,false,1,good);  // a bare .bin: still refused by require_cue
    apply_netplay_disc_expect(bin,kit);
    assert(!bin.netplay_ok && bin.netplay_detail.find("requires the full .cue")!=std::string::npos);
    cue=mounted(false,true,1,good);cue.cue_fallback=true;
    apply_netplay_disc_expect(cue,kit);
    assert(!cue.netplay_ok);

    NetplayDiscExpect many=kit;many.required_tracks=27;
    chd=mounted(true,false,27,good);       // a multi-track .chd: accepted
    apply_netplay_disc_expect(chd,many);
    assert(chd.netplay_ok);
    chd=mounted(true,false,1,good);        // a .chd with too few tracks: refused
    apply_netplay_disc_expect(chd,many);
    assert(!chd.netplay_ok && chd.netplay_detail.find("27")!=std::string::npos);

    chd=mounted(true,false,1,good);chd.toc_opened=false;
    apply_netplay_disc_expect(chd,kit);    // a .chd whose TOC did not open: refused
    assert(!chd.netplay_ok);
}
// identify_disc must reach the rule from its .chd branch: a file that is not
// a CHD cannot be opened, and with a rule given it is not valid for netplay.
static void broken_chd(const fs::path& root) {
    const auto image=root/"broken.chd";
    {std::ofstream file(image,std::ios::binary);file<<"not a chd";}
    PSXRecompV4::NetplayDiscExpect kit;
    kit.require_cue=true;kit.required_tracks=1;kit.required_disc_fp="ab77cc9d";
    auto id=PSXRecompV4::identify_disc(image,"",0,false,false,&kit);
    assert(id.from_chd && !id.opened && !id.netplay_ok && !id.netplay_detail.empty());
    id=PSXRecompV4::identify_disc(image,"",0,false,false);
    assert(id.from_chd && !id.netplay_ok);
    fs::remove(image);
}
// The launcher's SBI File row is OK only when the disc mounted with its SBI
// loaded (ae_disc_verify: toc_opened && sbi_loaded). The missing-SBI warning
// must say the same, whether the disc is unlisted, has no .sbi, has a good one,
// or has one the reader cannot take (empty, wrong magic, cut short): that
// last case leaves the TOC unmounted, and the warning used to read it as
// "loaded" (PS1B-495).
enum class Companion {none,good,empty,bad_magic,cut_short};
static void write_companion(const fs::path& path,Companion kind) {
    std::vector<uint8_t> bytes;
    switch(kind) {
    case Companion::none: return;
    case Companion::good:       // SBI\0, then one record: MSF 00:02:20, type 1, ten Q bytes
        bytes={'S','B','I',0,0x00,0x02,0x20,0x01,0x41,0x01,0x01,0x00,0x02,0x20,0x00,0x00,0x00,0x00};break;
    case Companion::empty:      bytes={'S','B','I',0};break;
    case Companion::bad_magic:  bytes={'N','O','P','E',0x00,0x02,0x20,0x01,0x41,0x01,0x01,0x00,0x02,0x20,0x00,0x00,0x00,0x00};break;
    case Companion::cut_short:  bytes={'S','B','I',0,0x00,0x02,0x20,0x01,0x41};break;
    }
    std::ofstream file(path,std::ios::binary);
    file.write((const char*)bytes.data(),bytes.size());
}
static void sbi_row_and_warning(const fs::path& root) {
    struct Case {const char* name;bool listed;Companion companion;bool extra_track;
                 int toc;int loaded;};   // toc, loaded: 1/0 expected, -1 = only agreement
    const Case cases[]={
        {"unlisted-no-sbi",    false,Companion::none,     false, 1,0},
        {"no-sbi",             true, Companion::none,     false, 1,0},
        {"good-sbi",           true, Companion::good,     false, 1,1},
        {"empty-sbi",          true, Companion::empty,    false, 0,0},
        {"bad-magic-sbi",      true, Companion::bad_magic,false, 0,0},
        {"cut-short-sbi",      true, Companion::cut_short,false, 0,0},
        {"unlisted-empty-sbi", false,Companion::empty,    false, 0,0},
        {"track-missing",      true, Companion::none,     true, -1,-1},
        {"track-missing-sbi",  true, Companion::good,     true, -1,-1},
    };
    // The listed serial is Soul Reaver (Europe), SLES-01301, on kKnownLibcryptSerials.
    std::setvbuf(stdout,nullptr,_IONBF,0);
    int misses=0;
    const auto listed=root/"listed.bin",plain=root/"plain.bin";
    fixture(listed,true,"SLES_013.01",false);
    fixture(plain,true,"SLPS_009.98",false);
    const std::string cue_main="FILE \"disc.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n";
    const std::string cue_track="FILE \"disc.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n"
                                "FILE \"missing track 02.bin\" BINARY\n  TRACK 02 AUDIO\n    INDEX 01 00:00:00\n";
    for(const Case& c:cases) {
        const auto dir=root/c.name;fs::create_directory(dir);
        fs::copy_file(c.listed?listed:plain,dir/"disc.bin");
        {std::ofstream cue(dir/"disc.cue",std::ios::binary);cue<<(c.extra_track?cue_track:cue_main);}
        write_companion(dir/"disc.sbi",c.companion);
        for(const char* pick:{"disc.cue","disc.bin"}) {
            const auto id=PSXRecompV4::identify_disc(dir/pick,"",0,false,false);
            const bool row_ok=id.toc_opened&&id.sbi_loaded;   // the row ae_disc_verify draws
            std::printf("sbi case %-18s pick=%-8s toc_opened=%d sbi_loaded=%d row_ok=%d warning=%s\n",
                        c.name,pick,id.toc_opened,id.sbi_loaded,row_ok,
                        id.sbi_warning.empty()?"(none)":id.sbi_warning.substr(0,22).c_str());
            // Count every miss so that one run lists them all, then assert once.
            auto expect=[&](bool ok,const char* what) {
                if(!ok) {++misses;std::printf("  MISS %s\n",what);}
            };
            expect(id.opened && id.has_header,"image opens with an ISO header");
            expect(id.detected_serial==(c.listed?"SLES-01301":"SLPS-00998"),"boot serial");
            if(c.toc>=0) expect(id.toc_opened==(c.toc==1),"toc_opened");
            if(c.loaded>=0) expect(id.sbi_loaded==(c.loaded==1),"sbi_loaded");
            if(c.listed) {   // warning exactly when the row is not OK
                expect(id.sbi_warning.empty()==row_ok,"row and warning agree");
                if(!row_ok) expect(id.sbi_warning.find("Missing SBI: SLES-01301")==0,"warning text");
            } else {
                expect(id.sbi_warning.empty(),"no warning for a serial off the list");
            }
        }
        fs::remove_all(dir);
    }
    fs::remove(listed);fs::remove(plain);
    assert(misses==0);
}
int main() {
    auto root=fs::temp_directory_path()/ ("psx-disc-identity-"+std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    for(bool raw:{false,true}) {
        auto image=root/(raw?"disc.bin":"disc.iso");fixture(image,raw);
        auto id=PSXRecompV4::identify_disc(image,"",0,false,false);
        assert(id.opened && id.has_header && id.detected_serial=="SLPS-00998" && id.region=="NTSC-J");
    }
    netplay_rule();broken_chd(root);sbi_row_and_warning(root);
    fs::remove(root/"disc.iso");fs::remove(root/"disc.bin");fs::remove(root);
}
