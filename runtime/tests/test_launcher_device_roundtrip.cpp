#include "launcher_device.h"

#include <cassert>

int main() {
    using PSXRecompV4::launcher_device_from_source;
    using PSXRecompV4::launcher_source_from_device;

    assert(launcher_source_from_device("") == 0);
    assert(launcher_source_from_device(" none ") == 0);
    assert(launcher_source_from_device("Keyboard") == 1);
    assert(launcher_source_from_device("gamepad") == 2);
    assert(launcher_source_from_device("030000005e0400008e02000000000000") == 2);

    assert(launcher_device_from_source(0, "keyboard") == "none");
    assert(launcher_device_from_source(1, "none") == "keyboard");
    assert(launcher_device_from_source(2, "keyboard") == "gamepad");
    assert(launcher_device_from_source(2, "none") == "gamepad");
    assert(launcher_device_from_source(2, " auto ") == "auto");
    assert(launcher_device_from_source(
        2, " 030000005e0400008e02000000000000 ") ==
        "030000005e0400008e02000000000000");

    /* PS1 Mouse seat: shown as None, kept as mouse if left there. */
    assert(PSXRecompV4::launcher_device_is_mouse(" Mouse "));
    assert(!PSXRecompV4::launcher_device_is_mouse("keyboard"));
    assert(launcher_source_from_device("mouse") == 0);
    assert(launcher_device_from_source(0, "mouse") == "mouse");
    assert(launcher_device_from_source(1, "mouse") == "keyboard");
    assert(launcher_device_from_source(2, "mouse") == "gamepad");

    /* A launcher with the "PS1 Mouse" source (RECOMP_LAUNCHER_HAS_MOUSE_SOURCE):
     * the seat is source 3 both ways, and None now really means none. */
    using PSXRecompV4::kLauncherSourceMouse;
    assert(launcher_source_from_device("mouse", true) == kLauncherSourceMouse);
    assert(launcher_source_from_device("keyboard", true) == 1);
    assert(launcher_device_from_source(kLauncherSourceMouse, "keyboard", true) == "mouse");
    assert(launcher_device_from_source(kLauncherSourceMouse, "none", true) == "mouse");
    assert(launcher_device_from_source(0, "mouse", true) == "none");
    assert(launcher_device_from_source(1, "mouse", true) == "keyboard");

    /* GunCon seat (PS1B-305): an older launcher shows it as None and None
     * keeps it; it is never mistaken for a pad GUID. */
    using PSXRecompV4::kLauncherSourceGuncon;
    assert(PSXRecompV4::launcher_device_is_guncon(" GunCon "));
    assert(launcher_source_from_device("guncon") == 0);
    assert(launcher_source_from_device("guncon", true) == 0);
    assert(launcher_device_from_source(0, "guncon") == "guncon");
    assert(launcher_device_from_source(0, "guncon", true) == "guncon");
    assert(launcher_device_from_source(2, "guncon") == "gamepad");
    assert(launcher_device_from_source(1, "guncon") == "keyboard");
    /* A launcher with the "GunCon" source (RECOMP_LAUNCHER_HAS_GUNCON_SOURCE):
     * source 4 both ways, and None means none. */
    assert(launcher_source_from_device("guncon", true, true) == kLauncherSourceGuncon);
    assert(launcher_source_from_device("mouse", true, true) == kLauncherSourceMouse);
    assert(launcher_device_from_source(kLauncherSourceGuncon, "keyboard", true, true) == "guncon");
    assert(launcher_device_from_source(0, "guncon", true, true) == "none");
    assert(launcher_device_from_source(kLauncherSourceMouse, "guncon", true, true) == "mouse");
    /* A mouse-only launcher keeps a GunCon seat and returns a mouse seat. */
    assert(launcher_device_from_source(0, "guncon", true, false) == "guncon");
    assert(launcher_device_from_source(0, "mouse", false, true) == "mouse");

    /* neGcon pad type (PS1B-304). A launcher with the mode
     * (RECOMP_LAUNCHER_HAS_NEGCON_MODE) passes 3 through both ways. */
    using PSXRecompV4::launcher_pad_mode_from_launcher;
    using PSXRecompV4::launcher_pad_mode_to_launcher;
    constexpr int kAnalog = 1, kDigital = 2, kNegcon = 3;
    assert(launcher_pad_mode_to_launcher(kNegcon, true) == kNegcon);
    assert(launcher_pad_mode_from_launcher(kNegcon, kAnalog, true) == kNegcon);
    assert(launcher_pad_mode_from_launcher(kAnalog, kNegcon, true) == kAnalog);
    /* An older launcher never sees 3: the seat goes in as Analog, and an
     * Analog coming back keeps the neGcon; D-Pad replaces it. */
    assert(launcher_pad_mode_to_launcher(kNegcon, false) == kAnalog);
    assert(launcher_pad_mode_from_launcher(kAnalog, kNegcon, false) == kNegcon);
    assert(launcher_pad_mode_from_launcher(kDigital, kNegcon, false) == kDigital);
    /* Analog and digital seats are untouched either way. */
    for (bool with_mode : { false, true }) {
        assert(launcher_pad_mode_to_launcher(kAnalog, with_mode) == kAnalog);
        assert(launcher_pad_mode_to_launcher(kDigital, with_mode) == kDigital);
        assert(launcher_pad_mode_from_launcher(kAnalog, kDigital, with_mode) == kAnalog);
        assert(launcher_pad_mode_from_launcher(kDigital, kAnalog, with_mode) == kDigital);
    }
    return 0;
}
