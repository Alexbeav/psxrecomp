# Optional source CD profiles

The modern controller remains the default. Its independent data FIFO, decoder
ring, disc geometry checks, resident completion/feed APIs, stream generation,
default Getloc position behavior and CDF4 pause receipt remain in use. Source
comparison behavior is selected explicitly through the existing environment
settings; it does not change the default timing policy.

`PSX_CD_SOURCE_CLOCK_TAPE` names a finite local `PSX-CD-RNG1` word tape. The
clock requires `PSX_CD_EXPLICIT_SEEK_MODEL=octoshock-2.2.2`,
`PSX_CD_TOC_SEEK_MODEL=octoshock-2.2.2` and
`PSX_CD_READ_START_MODEL=octoshock-2.2.2-pipeline`. These independent selectors
may also be used without a clock tape. `PSX_CD_FIRMWARE_MODEL` and
`PSX_CD_COLD_STATUS_MODEL` accept `octoshock-2.2.2` separately. Unset, empty or
`default` retains their normal behavior.

With a tape, `PSX_CD_DRIVE_MODEL` accepts `nymashock-1.29.0`,
`nymashock-1.32.1`, `octoshock-2.4.2`, `octoshock-2.6.2` or
`octoshock-2.7`. The Octoshock selectors use jitter-first Init by default;
the Nymashock selectors use floor-first. Their measured tray behavior remains
title/provider-specific, rather than inferred from a shared core ancestry. `PSX_CD_INIT_DRAW_ORDER=jitter-first` or `floor-first` overrides that
order only with a drive model. `PSX_CD_CDDA_MODEL=octoshock-2.3` enables the
source physical two-sector CDDA pipeline and asynchronous reports. Firmware/seek/drive/CDDA validators reject unsupported values. A source clock
requires the named pipeline selector and valid tape. Tape exhaustion and the
retained unsupported operations stop with exit2; no generator or wraparound
is substituted. CDDA covers single-speed play: scan commands, double-speed
play and Play during a data read remain refused. Drive-model Init during CDDA
or an unfinished read seek also remains refused.

The source clock retains command/argument reception phases, the 2,000-cycle
post-acknowledgement window, scheduled Init and seek behavior, source read
position rules and optional idle-head state. The CDDA path retains physical
SubQ/report ownership, queue replacement/retention, data-track Play, Pause's
bounded physical read-ahead rewind and queued-audio preservation. Private
source helpers live in [cdrom_source_profile.inc](../runtime/src/cdrom_source_profile.inc)
and call the modern controller's shared operations; unchanged commands fall
through to the modern implementation. Modern XA decoding is retained.

The source-clock bridge uses `ISOReader::ReadSourceSubChannelQ` for INDEX00
pregaps, the reserved byte and big-endian CRC. Its encoder is
[disc_source_subq.h](../runtime/include/disc_source_subq.h). The normal reader
and normal bridge wire remain unchanged. Physical audio and virtual data/audio
geometry still come from the modern bridge. `iso_read_cdda_subq` keeps the audio
view separate from appended data tracks; ordinary source Q retains the data
view. Independent full-packet CRC vectors check both views and the original
default wire in the actual bridge. Invalid SBI packets remain
invalid and preserve the controller's last valid position. Recorded-sector
SubQ coverage does not qualify leadout synthesis.

The default device codec keeps its existing CDF4 prefix and accepts CDF3,
CDF2 and legacy input with the existing eligibility rules. An enabled profile
appends `SCD2` after CDF4. It records exact selector flags, paused/read-start
context, controller deadlines and lid state. Clock state adds tape identity,
count/cursor, call count and receive/reset deadlines; CDDA adds its two physical
sectors and pending report; a drive adds head/window/reset state. The source
FIFO selector adds its independent 4096-byte guest FIFO, latest 2340-byte sector
and bounded cursors. The 2.10 tray notification is retained separately.

The reader validates lengths, configuration and fields before mutation. A saved
clock may bind an exact original prefix of an independently extended loaded
tape. Restoring its cursor does not replace the loaded full tape's identity.
This does not invent further entropy or permit a different producer's tape.

The complete composed boot-state format is separately versioned as 17 without
host providers and 18 with trusted host state. It rejects both replay version16
and modern versions9/10. Device-codec legacy parsing does not imply that whole
older boot states can restore. See [the composition limits](private-replay-integration.md).

The original authored provenance is retained in `runtime/tests/cd*_provenance*.json`,
[source_disc_subq_fixtures.json](../runtime/tests/source_disc_subq_fixtures.json)
and both `cdrom_source_cdda_*fixtures.json` files. The private corpus references
`CDROM-SOURCE-PROFILE-SPEC.md` and `CD-ENTROPY-CONTRACT.md` distinguish observed,
kept and unobserved behavior. The original authored mechanics and later private source-derived additions
retain their distinct provenance. Private source-derived lines carry a
provenance note that marks them as still to be re-expressed; each must be
restated in our own form before any public export.
The source tape is bounded observation input, not a claim about a hidden random
generator or a hardware distribution.

The runtime suite registers command/drive rows, unsupported-scope controls,
optional snapshot corruption checks, both complete CDDA transcript variants at
O0/O2 and 1,500 recorded-sector SubQ records at O0/O2. Transcript checks include
controller snapshots after every operation and compare the full declared
response/state/PCM output. Cold tray/motor warm-up bytes and downstream SPU
mixing are outside that declared projection. These authored comparisons are not
new external-oracle runs, retail gameplay, listening tests, hardware parity or
strict/fleet qualification. The three historical disabled tests and the
separate GL scale stress exclusion remain separate limitations.

The [optional source bus](source-bus-profile.md) separately selects channel 3
DMA service with `PSX_CD_DMA_MODEL=octoshock-2.2.2`. Command/tape selection does
not enable that DMA selector. The real-CD/shared-DMA/event-clock fixture checks
synthetic FIFO bytes and per-operation restoration; command, drive and CDDA
transcripts retain their separate declared projections above.

The composed private selectors retain distinct measured tray policies:
2.4.2 Chrono opens with INT5 16h/08h then status10h; 2.7 FFIX opens without
an unsolicited IRQ. The separately selected 2.10 Exoddus notification includes
a paused/null tray operation. These are not interchangeable provider identities.
Stop in the 2.7 profile services head advances already due, then cancels its
cadence. No position or frame offset is substituted.
