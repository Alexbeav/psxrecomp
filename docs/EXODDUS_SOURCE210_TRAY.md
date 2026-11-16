# Legacy Octoshock2.10 tray notification

This private opt-in keeps the existing measured2.7 drive timing and selects only
the retained2.10 physical tray notification with --cd-tray-model octoshock-2.10.
Default2.7,2.4.2 and Nyma profiles retain their existing tray paths.

The actual Exoddus diagnostic matched all2097152 bytes at179993 (93157da8).
The179994 state differs by17 bytes. From a unique32-instruction anchor at80069BA0,
8288 instruction addresses agree before source00002484/native000024A0 split.
The branch0000247C tests CD bit4: source I_STAT low5/mask0D, native1/mask0D.
Original stores at800665DC/80066634 record INT5 and16h/08h at8008C9DD/801ECFA0.
This proves a missing CD-notification path, not an exact source-cycle error.

Fresh ordinary LLVM disassembly of the retained ELF55667a44 supplies the model:
Pause stores DriveStatus=-1 at3119A. Missing media34C98/34E90 treats that as
nonzero and queues16h/08h, then stops the live drive. A later GetStat reads10h.
The failed449190e3 candidate excluded paused state and altered2.7 generally; it
is preserved as a failure and is not integrated here. No RCF code is ported.

Physical open records the notice. The next normal CD update services it.
A held IRQ/FIFO remains owned by its original response; the notice waits for ACK
and the existing2000-cycle receive deadline. New notice/payload bytes are added
only for this opt-in controller snapshot; existing-profile wire bytes stay the
same. The environment identity covers the new selector automatically.
No two-pad native resume, cross-profile cache restore or hardware proof is claimed.

The regression exercises paused notification, payload versus live status,
held-IRQ receive timing, queued controller roundtrip, stopped control and retained
2.4.2/2.7 controls. The identical fixture on c03 fails four source210 notification assertions at both
GCC O0 and O2; this candidate passes both, including retained older-profile controls.
The actual source210 ACK path now arms the existing receive deadline before
checking the queued notice; legacy profile ACK order stays unchanged.
Combined source GCC trees and full Exoddus confirmation remain pending; controller
tests alone are not a game-fix claim.
