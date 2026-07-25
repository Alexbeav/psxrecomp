# Which BIOS a build uses

A PlayStation game needs a BIOS. PSXRecomp can supply one — **OpenBIOS**, an
MIT-licensed, from-scratch PS1 BIOS from the PCSX-Redux project that we are
allowed to redistribute — so a player can be handed a build and a disc image and
just play. A player who prefers their own dumped retail BIOS can use that
instead.

Both BIOSes are compiled into every build. Which one runs is decided when the
game launches, not when it is built.

## The rule

> **No BIOS chosen → OpenBIOS. A BIOS the player explicitly chose → that BIOS.**

That is the whole user-facing contract. A player never has to find a BIOS file
to start playing, and never loses the ability to use their own.

A developer can switch this off for a title that is known not to work on
OpenBIOS; then a retail BIOS is required and the player is prompted for one.

## What counts as "chosen"

Only a deliberate act by the player:

- `--bios <path>` on the command line, or
- a BIOS selected in the launcher / settings, which is then remembered.

**Finding a BIOS file on disk does not count.** Earlier versions searched
directories near the executable and would quietly adopt whatever they found.
That made behaviour depend on what happened to be lying around — two players
with identical builds could get different BIOSes. Discovery is no longer a
selection mechanism.

A player who has chosen a BIOS can clear that choice and go back to OpenBIOS;
the choice is not permanent.

## "A retail BIOS" means one specific image

A build compiles in exactly one retail image (currently SCPH-1001) alongside
OpenBIOS. The compiled-in code is generated *from that exact image*, so a
different dump — SCPH-101, SCPH-5552, a bad rip — cannot be substituted: the
recompiled code would execute against data it does not match, and crash.

Every build therefore checks the identity (size, CRC32, SHA-256) of a chosen
BIOS before using it, and says which image it expects if the check fails. This
is not DRM and not a preference; it is the difference between running and
wild-jumping.

If a chosen BIOS does not match:

- with OpenBIOS available, the player is told why and can continue on OpenBIOS;
- with OpenBIOS disabled for the title, the choice is rejected and re-prompted.

## Developer setting

One optional line in `game.toml`:

```toml
[runtime]
openbios = true    # default. false = this title requires a retail BIOS.
```

Set `openbios = false` only for a title with a **verified** OpenBIOS
incompatibility. Per-title compatibility is not implied by the framework
supporting OpenBIOS — verify a title before shipping it that way.

This setting is deliberately **not** overridable by a player's `settings.toml`,
unlike most runtime options. It records a developer's compatibility finding, not
a preference. A player re-enabling OpenBIOS on a title known to break under it
produces confusing bug reports and no benefit.

There is no third mode. A title either allows OpenBIOS (and still honours an
explicit retail choice) or requires retail. "OpenBIOS only, retail forbidden"
would take away a working option from players for no reason.

## Save data

**Memory cards are unaffected.** They are card images; they carry no BIOS state
and move freely between BIOSes and between builds.

**Savestates are BIOS-specific.** The kernel occupies RAM, and OpenBIOS and
retail lay that RAM out differently, so a savestate captured under one BIOS is
not valid under the other. Savestates therefore record which BIOS was active and
refuse to load under a different one. Without that check, switching BIOS would
silently corrupt a restored state — the only quiet failure mode in this design,
which is why it is enforced rather than warned about.

## Why both are compiled in

The alternative — a build flavour per BIOS — was the original design, and it
required a game to state its BIOS in three places that nothing cross-checked
(`bios_config` in `game.toml`, plus two CMake variables). Setting one and
forgetting another produced a build that linked one BIOS's code against another
BIOS's generated game code, and it compiled cleanly.

Carrying both costs roughly 20 MB of binary and removes that entire class of
mistake: one build configuration, one source of truth, the choice made at
runtime where it can be validated.

It is affordable because the game's own recompiled code is **identical**
regardless of which BIOS it was generated against — verified byte-for-byte on
Tomba!, Tomba! 2 and Ape Escape. Only the BIOS code is duplicated, and the two
images collide on just 22 symbols, which are namespaced per BIOS.

## Attribution

OpenBIOS is MIT-licensed. Its notice is vendored at `bios/OpenBIOS.LICENSE`,
with the upstream source pin and build recipe in `bios/OpenBIOS.toml` and
attribution in `THIRD_PARTY_ATTRIBUTION.md`. Builds that ship it credit the
PCSX-Redux authors in the launcher, whether or not the licence compels it.

Retail BIOS images are **not** redistributable and are never shipped. A player
using one supplies their own dump.
