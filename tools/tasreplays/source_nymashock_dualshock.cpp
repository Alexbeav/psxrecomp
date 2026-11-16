/* Authored controller transactions. No BIOS, disc, game data or state load. */
#include <src/psx/psx.h>
#include <src/psx/frontio.h>
#include <src/psx/input/dualshock.h>
#include <src/state.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
bool LagFlag = true;
void (*InputCallback)() = nullptr;
extern "C" int trio_snprintf(char*, size_t, const char*, ...) {
    std::abort(); // Only the unexercised state serializer calls this formatter.
}
namespace Mednafen {
bool MDFNSS_StateAction(StateMem*, const unsigned, const bool, const SFORMAT*, const char*, const bool) noexcept {
    std::abort(); // An unexpected state operation invalidates this input-only fixture.
}
}
using MDFN_IEN_PSX::InputDevice;
static void controls(InputDevice* pad, unsigned v) {
    uint8 input[32] = {};
    // Source thunk writes the high byte. Nyma field order: RX,RY,LX,LY.
    input[0] = 0x10;
    for (unsigned i=0; i<4; ++i) input[4+i*2] = (uint8)((v+i*53)&255);
    pad->UpdateInput(input);
}
static void transaction(InputDevice* pad, const char* name, const std::vector<uint8>& tx) {
    pad->SetDTR(true);
    printf("{\"case\":\"%s\",\"bytes\":[", name);
    for (unsigned byte=0; byte<tx.size(); ++byte) {
        unsigned rx=0; int32 delay=0;
        for (unsigned bit=0; bit<8; ++bit) {
            int32 pulse=0;
            rx |= (unsigned)pad->Clock((tx[byte]>>bit)&1, pulse)<<bit;
            if (bit != 7 && pulse) std::abort();
            delay=pulse;
        }
        printf("%s[%u,%u,%d]", byte?",":"", tx[byte],rx,delay);
    }
    printf("]}\n");
    pad->SetDTR(false);
}
int main() {
    std::unique_ptr<InputDevice> pad(MDFN_IEN_PSX::Device_DualShock_Create());
    controls(pad.get(), 0);
    transaction(pad.get(), "cold-digital", {1,0x42,0,0,0});
    transaction(pad.get(), "enter-config", {1,0x43,0,1,0,0,0,0,0});
    transaction(pad.get(), "set-analog-locked", {1,0x44,0,1,3,0,0,0,0});
    transaction(pad.get(), "leave-config", {1,0x43,0,0,0,0,0,0,0});
    for (unsigned v=0; v<256; ++v) {
        char name[32]; snprintf(name,sizeof name,"axis-%03u",v);
        controls(pad.get(), v);
        transaction(pad.get(), name, {1,0x42,0,0,0,0,0,0,0});
    }
    return 0;
}
