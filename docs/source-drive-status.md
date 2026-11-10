Source-drive status polling
===========================

When PSX_CD_DRIVE_MODEL is active, GetStat reports the current motor bit.
After Stop completes, polling GetStat keeps the motor off; MotorOn, ReadN,
Seek and Play retain their own start transitions. The path without a drive
profile retains its existing GetStat behavior, as does the 2.6.2 lid-status
branch. No command deadline or snapshot field changes.

cdrom_boot_model_test covers stopped polling, snapshot restoration, Pause,
no-disc status and subsequent start commands. Full FFIX 6726 replay
confirmation is separate from this controller regression.

Separate shipped route tray profiles
------------------------------------

The octoshock-2.4.2 selector preserves the shipped BizHawk2.4.2 Chrono4538
measurement at return988277: INT5 publishes16h/08h before the stopped shell
status becomes10h. The octoshock-2.7 selector follows the separately measured
BizHawk2.8 FFIX6726 return467782: opening raises no unsolicited IRQ and
holds status10h. The existing2.6.2 and default paths retain their responses.
Both selectors drop a pending second response and close into a one-second
motor-off startup. cdrom_boot_model_test exercises both real MMIO paths.

The startup command classification and routine structure are private
source-derived work in `cdrom.c`: the tray, startup window and seek behaviour were modelled from observed drive behaviour.
a15b31a46 psx/cdc.cpp95-149,1585-1598,1719-2424, GPL-2.0-or-later.
That reference lineage is not evidence that a15 models shipped2.4.2 tray
interrupts. The shipped2.4.2 response comes from the retained original state
and RAM observations; the separate2.7 no-IRQ response comes from its own
original state. Full-title confirmation remains bound to each exact source
and product. No shared-profile full pass is implied.
