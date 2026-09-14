# Nymashock 1.29.0 memory-card compatibility

`PSX_INPUT_ROUTE_CARD_MODEL=nymashock-1.29.0` selects an explicit source
compatibility profile. It does not enable a card or select its initial bytes.
The native launcher must separately stage and bind the source's raw 128 KiB
card, enable only the intended slots, and verify the loaded peripheral buffer.
Native blank formatting differs from the source constructor, so creating an
arbitrary new card is not an equivalent initial condition.

The source is BizHawk `745efb1dd8eb82f31ba9201a79cdfc5bcaf1f5d1`, Nyma
Mednafen submodule `ddf225cf63b7b355cb2ac7772450cf473f4b53ac`. Reproduce its
authored device transactions with:

```text
python tools/tasreplays/collect_nymashock_card.py PATH_TO_MEDNAFEN FRESH_OUTPUT --compiler g++
```

The collector compiles the unmodified complete source `memcard.cpp`, its
exact `InputDevice` base-method region from `frontio.cpp`, and the authored
serial-input fixture at O0 and O2. State serializer calls abort. No BIOS,
disc, retail input, save data or full emulator execution is involved. The
collector verifies both repository revisions and clean trees and records
compiler/source/output hashes. It compares normalized transaction output
and raw constructor/final bytes against fixed identities.

The 28 cases cover cold reads, boundary/invalid addresses, writes and
readback, repeated identical writes, checksum errors, simultaneous address
and checksum errors, deselection after each of five write boundaries,
unsupported commands, and power reset retaining nonvolatile contents.
Every input byte, response byte and requested ACK delay is compared.
Each transaction also records the nonvolatile buffer's FNV-1a hash; the
final complete 128 KiB buffer is checked with SHA-256.

The source profile corrects these differences from the existing default:

- ACK requests use 256 cycles; DSR stays active for 32 cycles independently
  of status reads. The source `FrontIO::Update` supplies that pulse-width
  contract. The native clock fixture checks boundaries, oversized advances,
  an already-pending INTC IRQ, IRQ-disabled pulses, and DTR cancellation.
- Reads echo the high address byte. Invalid reads echo `FFFF`, then stop
  responding after the low address byte without an ACK.
- Writes echo the preceding payload byte. Data commits while transmitting
  the second confirmation, two byte times after receiving the checksum.
  Deselecting earlier leaves the card unchanged.
- A valid write clears the new-card flag when it commits. Failed writes
  preserve the flag. Checksum failure takes precedence over invalid address.
- Unsupported commands still return the previously queued flag byte but
  request no ACK. Completed/failed transactions remain silent until DTR drops.
- Source timing does not flush pending card IRQs on deselection or postpone
  a DSR pulse until the guest clears an earlier INTC IRQ.

The diagnostic delay field can represent 256 cycles. Existing snapshot wire
format is unchanged; the source profile is cold-boot-only and rejects snapshot
use. The default model is compared to a frozen pre-correction baseline, and
the earlier controller profiles retain their own tests.

This is device-component qualification. The source fixture does not execute
the complete FrontIO scheduler, all device combinations, full game timing,
or a Bio Hazard TAS/progression-save gate. Source/native full RAM and clock
comparisons remain required before any title-level pass.
