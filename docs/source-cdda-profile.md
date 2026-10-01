# Octoshock 2.3 CD-audio comparison profile

`PSX_CD_CDDA_MODEL=octoshock-2.3` requires the explicit source command-clock
tape. The Pepsiman adapter selects it; the default and Tekken profiles do not.
The original movie and its frame count are unchanged.

CLAIM: CDDA uses the source drive seek deadline and two physical sector buffers
before decoded audio reaches the existing native SPU queue. Position/report
SubQ belongs to the physical read head. Reports replace one pending asynchronous
response and wait for IRQ acknowledgement plus the 2000-clock receive interval.
Pause acknowledges the old status, rewinds up to four physical reads, stops the
producer immediately and preserves already queued audio. Stop and MotorOn use
the source phase-one state and second-response delays. This is oracle
compatibility, not measured hardware timing.

ORACLE CORE: BizHawk tag 2.3 (Octoshock), commit
`a15b31a46bdac27d843d3ebbc5a860012d8452fb`.
The oracle controller remains in a separate external GPL oracle; it is never
linked into the native runtime or used as a movie replay core. The runtime's
CD-DA start, service, notification and peek code was rewritten from black-box
observations in T172 (`runtime/tests/cdda_*_provenance*.json`). The Pause
move-back is specified in
`recomp-corpus/references/ps1/CDROM-SOURCE-PROFILE-SPEC.md`, row P3.

ORACLE: `runtime/tests/cdrom_source_cdda_fixtures.json` binds four complete
synthetic-disc transcripts for modes 0/2/4/6: explicit track play, seek boundary,
position query, pause/resume, already-playing Play, report retention/replacement,
track pregap autopause, leadout, clamped track argument, pause during seek,
Stop and MotorOn including repeated commands. Every post-warm-up response byte,
13 declared controller values at every observation, and each fresh 588-frame
stereo PCM sector are compared. These are 1,117,988 little-endian output words
per optimization mode, not inferred parity from the game's image. The authored
disc has three tracks, deterministic PCM, generated CRC-valid SubQ and no retail
content. The cold tray/motor image differs between isolated host initialization
and production native boot; ordinary Reset/GetStat establishes the unit starting
condition and those warm-up response bytes are explicitly excluded. Inactive
source timers, unused internal state and downstream SPU mixing are not projected.

REPRODUCE: configure the runtime CMake tests with GNU C and run
`ctest --test-dir <build> -R cdrom_source_cdda --output-on-failure`.
O0 and O2 replay each complete transcript and check rejection of scan commands,
double-speed playback and active data-read to Play. Each transcript also runs
with a snapshot round trip after every operation, including queued PCM and
pending reports. See [TAS checkpoints](TAS_CHECKPOINTS.md) for replay qualification.
Fixtures bind original upstream, external oracle source head/tree/core hash,
authoring driver, random-word tape, complete operation stream and output hash.
External build/authoring recipes are retained in the campaign recovery evidence.

Scope: single-speed audio tracks with valid SubQ, ordinary authored commands,
cold source-clock selection. Double speed, Forward/Backward, playing data tracks,
active data-read to Play and hardware timing are not
qualified. Full Pepsiman RAM/clock parity and witnessed ending remain separate
acceptance gates, as does the unchanged Tekken regression after shared changes.
