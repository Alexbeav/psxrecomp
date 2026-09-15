/* Exercise the actual linked device readers without executing retail code. */
#define main psx_fixture_runtime_main
#include PSX_RUNTIME_MAIN_SOURCE
#undef main
#include "pst_wire.h"
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
    if(argc!=2)return 2;
    memory_init(argv[1]);gr_set_backend(GR_BACKEND_SOFTWARE);gpu_init();dma_init();
    mdec_init();timers_init();interrupts_init();sio_init();spu_init();cdrom_init(nullptr);
    CPUState cpu={};cpu.read_word=psx_read_word;cpu.write_word=psx_write_word;
    cpu.read_half=psx_read_half;cpu.write_half=psx_write_half;
    cpu.read_byte=psx_read_byte;cpu.write_byte=psx_write_byte;
    cpu.pc=0x80100000;cpu.gpr[29]=0x801fff00;gte_canonicalize_cpu_state(&cpu);
    psx_cycle_count=123456789;
    cpu.gpr[5]=0x11111111;memory_get_ram_ptr()[0x18000]=0x42;
    const Bytes incoming=save(cpu);const auto sections=split(incoming);
    int failures=0,checks=0;
    auto load=[&](const Bytes &b){return boot_state_load_buffer(b.data(),b.size(),0x12345678,0x80100000,&cpu);};
    auto reject=[&](const char *label,const Bytes &bad,int allocation=-1) {
        cpu.gpr[5]=0x22222222;memory_get_ram_ptr()[0x18000]=0x99;
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
            if(sections[i].tag==BS_SEC_ICACHE)continue;
            auto missing=sections;missing.erase(missing.begin()+i);
            reject("missing",pack(incoming,missing,compressed));
            auto duplicate=sections;duplicate.push_back(sections[i]);
            reject("duplicate",pack(incoming,duplicate,compressed));
        }
        auto bad=sections;
        for(auto &s:bad)if(s.tag==BS_SEC_MDEC)put32(s.data,0,0xffffffff);
        reject("late_mdec_version",pack(incoming,bad,compressed));
        auto reversed=sections;std::reverse(reversed.begin(),reversed.end());
        Bytes valid=pack(incoming,reversed,compressed);
        ++checks;if(!load(valid)||save(cpu)!=incoming){++failures;puts("FAIL valid reversed roundtrip");}
        valid=pack(incoming,sections,compressed);
        fail_at=1000000;calls=0;int ok=load(valid);int count=calls;fail_at=-1;
        if(!ok)return 2;
        for(int i=0;i<count;i++)reject("allocation",valid,i);
        ++checks;if(!load(valid)||save(cpu)!=incoming){++failures;puts("FAIL final valid roundtrip");}
    }
    printf("state load atomicity: checks=%d failures=%d\n",checks,failures);
    return failures?1:0;
}
