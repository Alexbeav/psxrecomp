/* Exercise the actual linked device readers without executing retail code. */
#define main psx_fixture_runtime_main
#include PSX_RUNTIME_MAIN_SOURCE
#undef main
#include "pst_wire.h"
#include "mod_memory.h"
#include <algorithm>
#include <cstring>
#include <zlib.h>
extern "C" uint8_t *memory_get_ram_ptr(void);
extern "C" void *__real_malloc(size_t);
extern "C" void *__real_realloc(void *, size_t);
static int fail_at=-1, calls=0, hits=0;
extern "C" void *__wrap_malloc(size_t n) {
    if (fail_at>=0 && calls++==fail_at) { ++hits; return nullptr; }
    return __real_malloc(n);
}
extern "C" void *__wrap_realloc(void *p,size_t n) {
    if (fail_at>=0 && calls++==fail_at) { ++hits; return nullptr; }
    return __real_realloc(p,n);
}
using Bytes=std::vector<uint8_t>;
struct Section { uint32_t tag; Bytes data; };
static void put32(Bytes &b,size_t at,uint32_t v) {
    for(int i=0;i<4;i++) b.at(at+i)=uint8_t(v>>(8*i));
}
static Bytes save(CPUState &cpu) {
    uint8_t *p=nullptr; size_t n=0;
    if(!boot_state_save_buffer_raw(&cpu,0x12345678,0x80100000,&p,&n)) std::exit(2);
    Bytes b(p,p+n); std::free(p); return b;
}
static std::vector<Section> split(const Bytes &b) {
    PstR r; pst_r_init(&r,b.data()+36,b.size()-36);
    std::vector<Section> result;
    while(r.p!=r.end) {
        uint32_t tag,flags; uint64_t len;
        if(!pst_r_u32(&r,&tag)||!pst_r_u32(&r,&flags)||!pst_r_u64(&r,&len)
            ||flags||len>size_t(r.end-r.p)) std::exit(2);
        result.push_back({tag,Bytes(r.p,r.p+len)});r.p+=len;
    }
    return result;
}
static Bytes pack(const Bytes &header,const std::vector<Section> &sections,bool compressed) {
    Bytes b(header.begin(),header.begin()+36);put32(b,28,unsigned(sections.size()));
    for(const auto &s:sections) {
        Bytes payload=s.data;
        if(compressed) {
            uLongf n=compressBound(payload.size());Bytes z(n+4);
            put32(z,0,unsigned(payload.size()));
            if(compress2(z.data()+4,&n,payload.data(),payload.size(),Z_BEST_SPEED)!=Z_OK)std::exit(2);
            z.resize(n+4);payload=std::move(z);
        }
        size_t at=b.size();b.resize(at+16);
        put32(b,at,s.tag);put32(b,at+4,compressed?1:0);put32(b,at+8,unsigned(payload.size()));put32(b,at+12,0);
        b.insert(b.end(),payload.begin(),payload.end());
    }
    return b;
}
int main(int argc,char **argv) {
    if(argc!=2 && argc!=3)return 2;
    const bool source_profile=argc==3;
    if(source_profile) {
        _putenv_s("PSX_INPUT_ROUTE_FILE","fixture-no-guest-execution");
        _putenv_s("PSX_GPU_DMA_MODEL","octoshock-2.2.2-bounded-linked-list");
        _putenv_s("PSX_INPUT_ROUTE_FIELD_MODEL","octoshock-2.2.2-ntsc-raster");
        _putenv_s("PSX_TIMER1_MODEL","octoshock-2.2.2");
        _putenv_s("PSX_TIMER2_MODEL","octoshock-2.2.2");
        _putenv_s("PSX_INPUT_ROUTE_PAD_ACK_MODEL","nymashock-1.29.0-dualshock");
        _putenv_s("PSX_INPUT_ROUTE_CARD_MODEL","nymashock-1.29.0");
    }
    memory_init(argv[1]);gr_set_backend(GR_BACKEND_SOFTWARE);gpu_init();dma_init();
    mdec_init();timers_init();interrupts_init();sio_init();spu_init();cdrom_init(nullptr);
    if(source_profile) {
        source_gpu_runtime_init();
        if(!psx_mod_memory_alloc(256,16) || !psx_mod_gpu_dma_memory_alloc(256,16))return 2;
    }
    CPUState cpu={};cpu.read_word=psx_read_word;cpu.write_word=psx_write_word;
    cpu.read_half=psx_read_half;cpu.write_half=psx_write_half;
    cpu.read_byte=psx_read_byte;cpu.write_byte=psx_write_byte;
    cpu.pc=0x80100000;cpu.gpr[29]=0x801fff00;gte_canonicalize_cpu_state(&cpu);
    psx_cycle_count=123456789;
    cpu.gpr[5]=0x11111111;memory_get_ram_ptr()[0x18000]=0x42;
    psx_write_word(0x110,0x80010000);psx_write_word(0x114,0xC0);
    Bytes scheduler(PSX_SCHEDULER_SNAPSHOT_BYTES);put32(scheduler,0,0x80010000);
    if(!psx_scheduler_snapshot_read(scheduler.data(),scheduler.size(),&cpu))return 2;
    const Bytes incoming=save(cpu);const auto sections=split(incoming);
    int failures=0,checks=0;
    auto load=[&](const Bytes &b){return boot_state_load_buffer(b.data(),b.size(),0x12345678,0x80100000,&cpu);};
    auto reject=[&](const char *label,const Bytes &bad,int allocation=-1) {
        cpu.gpr[5]=0x22222222;memory_get_ram_ptr()[0x18000]=0x99;
        // The incoming scheduler is valid only against its own RAM table.
        psx_write_word(0x110,0x80020000);
        const Bytes before=save(cpu);
        fail_at=allocation;calls=hits=0;int accepted=load(bad);fail_at=-1;
        const Bytes after=save(cpu);
        ++checks;
        if(accepted||before!=after||(allocation>=0&&hits!=1)) {
            ++failures;printf("FAIL %s accepted=%d unchanged=%d hits=%d\n",label,accepted,before==after,hits);
        }
    };
    for(int compressed=0;compressed<2;compressed++) {
        for(size_t i=0;i<sections.size();i++) {
            auto missing=sections;missing.erase(missing.begin()+i);
            reject("missing",pack(incoming,missing,compressed));
            auto duplicate=sections;duplicate.push_back(sections[i]);
            reject("duplicate",pack(incoming,duplicate,compressed));
        }
        auto bad=sections;
        for(auto &s:bad)if(s.tag==BS_SEC_MDEC)put32(s.data,0,0xffffffff);
        reject("late_mdec_version",pack(incoming,bad,compressed));
        // Late content errors must not commit an earlier CPU/RAM/device section.
        auto corrupt=[&](uint32_t tag,size_t offset,uint32_t value) {
            auto altered=sections;
            for(auto &section:altered)if(section.tag==tag) {
                put32(section.data,offset,value);
                reject("invalid content",pack(incoming,altered,compressed));
                return;
            }
        };
        corrupt(BS_SEC_MODMEM,0,0);
        corrupt(BS_SEC_MODMEM,8,0);
        corrupt(BS_SEC_CPU,572,0xffffffffu);
        corrupt(BS_SEC_CPU_EXEC,0,2);
        corrupt(BS_SEC_CPU_EXEC,4,3);
        corrupt(BS_SEC_CPU_EXEC,20,32);
        corrupt(BS_SEC_SCHED,4,1);
        corrupt(BS_SEC_SCHED,0,0x81234560);
        corrupt(BS_SEC_BOOTFLOW,0,2);
        for(const auto &section:sections)if(section.tag==BS_SEC_SPU) {
            corrupt(BS_SEC_SPU,section.data.size()-36,0xffffffffu);
            // CD ring has eight seconds of 44100 Hz stereo frames, then 36 bytes of queue metadata.
            const size_t cd_bytes=44100u*8u*4u+36u;
            corrupt(BS_SEC_SPU,section.data.size()-cd_bytes-8,768);
            if(source_profile) {
                const size_t keys=section.data.size()-cd_bytes-16-(36+24*70);
                corrupt(BS_SEC_SPU,keys,0);
                corrupt(BS_SEC_SPU,keys+12,5);
                corrupt(BS_SEC_SPU,keys+36+64,32);
            }
        }
        if(source_profile) {
            for(unsigned i=0;i<3;i++)corrupt(BS_SEC_RASTER,i*80+16,1);
            corrupt(BS_SEC_GPU_SERVICE,48,33);
            for(const auto &section:sections)if(section.tag==BS_SEC_SIO)
                corrupt(BS_SEC_SIO,section.data.size()-4,0xffffffffu);
        } else {
            auto mismatch=sections;mismatch.push_back({BS_SEC_GPU_SERVICE,Bytes(SOURCE_GPU_SERVICE_WIRE_BYTES)});
            reject("inactive profile section",pack(incoming,mismatch,compressed));
        }
        Bytes truncated=pack(incoming,sections,compressed);truncated.pop_back();
        reject("truncated stream",truncated);
        auto unknown=sections;unknown.push_back({0x80,Bytes{1,2,3,4}});
        ++checks;if(!load(pack(incoming,unknown,compressed))||save(cpu)!=incoming) {
            ++failures;puts("FAIL unknown section compatibility");
        }
        auto trailing=sections;
        for(auto &section:trailing)if(section.tag==BS_SEC_MDEC)section.data.push_back(0);
        reject("MDEC trailing data",pack(incoming,trailing,compressed));
        // Admit different FIFO sizes; live MDEC size is not an input-shape rule.
        auto grown=sections;
        for(auto &section:grown)if(section.tag==BS_SEC_MDEC) {
            put32(section.data,324,5000);put32(section.data,328,50000);
            section.data.insert(section.data.end(),60000,0);
        }
        const Bytes grown_blob=pack(incoming,grown,compressed);
        mdec_init();fail_at=1000000;calls=0;
        int grown_ok=load(grown_blob),grown_allocations=calls;fail_at=-1;
        ++checks;if(!grown_ok||save(cpu)!=pack(incoming,grown,false)) {
            ++failures;puts("FAIL different MDEC FIFO sizes");
        }
        for(int i=0;i<grown_allocations;i++) {
            mdec_init();reject("MDEC growth allocation",grown_blob,i);
        }
        auto reversed=sections;std::reverse(reversed.begin(),reversed.end());
        Bytes valid=pack(incoming,reversed,compressed);
        ++checks;if(!load(valid)||save(cpu)!=incoming){++failures;puts("FAIL valid reversed roundtrip");}
        valid=pack(incoming,sections,compressed);
        fail_at=1000000;calls=0;int ok=load(valid);int count=calls;fail_at=-1;
        if(!ok)return 2;
        for(int i=0;i<count;i++)reject("allocation",valid,i);
        ++checks;if(!load(valid)||save(cpu)!=incoming){++failures;puts("FAIL final valid roundtrip");}
    }
    printf("state load atomicity (%s): checks=%d failures=%d\n",source_profile?"source":"default",checks,failures);
    return failures?1:0;
}
