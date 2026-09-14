/* Authored memory-card traffic; no BIOS, media, game data or state load. */
#include <src/psx/psx.h>
#include <src/psx/frontio.h>
#include <src/psx/input/memcard.h>
#include <src/state.h>
#include <cstdio>
#include <cstdlib>
bool LagFlag = true;
void (*InputCallback)() = nullptr;
extern "C" int trio_snprintf(char*, size_t, const char*, ...) { std::abort(); }
namespace Mednafen {
bool MDFNSS_StateAction(StateMem*, const unsigned, const bool, const SFORMAT*, const char*, const bool) noexcept {
    std::abort(); // State APIs are forbidden in this serial-only device fixture.
}
}
using MDFN_IEN_PSX::InputDevice;
static InputDevice *card;
static uint64_t nv_hash(void) {
    uint64_t hash=UINT64_C(14695981039346656037);
    for (unsigned i=0;i<card->GetNVSize();++i) { hash^=card->ReadNV()[i];hash*=UINT64_C(1099511628211); }
    return hash;
}
static void card_transaction(const char *name,const uint8_t *tx,unsigned count) {
    card->SetDTR(true);
    printf("{\"case\":\"%s\",\"bytes\":[",name);
    for (unsigned byte=0;byte<count;++byte) {
        unsigned rx=0; int32 delay=0;
        for (unsigned bit=0;bit<8;++bit) {
            int32 pulse=0;
            rx|=(unsigned)card->Clock((tx[byte]>>bit)&1,pulse)<<bit;
            if (bit!=7 && pulse) std::abort();
            delay=pulse;
        }
        printf("%s[%u,%u,%d]",byte?",":"",tx[byte],rx,delay);
    }
    card->SetDTR(false);
    printf("],\"nv_fnv1a64\":\"%016llx\"}\n",(unsigned long long)nv_hash());
}
static void card_power(void) { card->Power(); }
#include "nymashock_card_cases.h"
static void dump(const char *path) {
    FILE *f=fopen(path,"wb");
    if (!f || fwrite(card->ReadNV(),1,card->GetNVSize(),f)!=131072 || fclose(f)) std::abort();
}
int main(int argc,char **argv) {
    if (argc!=3) return 2;
    std::unique_ptr<InputDevice> owner(MDFN_IEN_PSX::Device_Memcard_Create());
    card=owner.get();
    dump(argv[1]); card_cases(); dump(argv[2]);
    return 0;
}
