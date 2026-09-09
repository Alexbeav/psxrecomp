# Recorded-sector SubQ and CD-DA pregaps

Pepsiman's native replay first differed at return 2,366. Passive CPU traces
matched 171,287 pre-fetch PC/clock records, then native took a CD-ROM interrupt
at clock 1,338,890,753. All 277 non-null GPU command dispatch records in the
window matched. Native received INT4 with playing status 0x82; the source
continued playing. The source's terminal CDC state showed track match 3 and
the physical head at sector 57,592.

The production disc reader assigned pregap sectors to the preceding track.
At sector 57,525 it returned track 2/index 1; stock BizHawk returned track
3/index 0 with 66 sectors remaining. Native therefore established the wrong
auto-pause track and stopped upon reaching sector 57,591. The corrected reader
uses the track's pregap boundary and emits the decreasing INDEX 00 position.

The raw twelve-byte SubQ record also needs a reserved zero at byte 6, absolute
MSF at bytes 7–9 and the checksum most significant byte first. These fields
are corrected for synthesized recorded sectors. SBI payload bytes remain
unchanged and its intentionally invalid checksum retains the same wire byte
order. GetlocP now skips the reserved byte on the SBI path, as the source CD-DA
path already does. Invalid Q still preserves the last valid position.

An authored three-track CUE supplies 1,500 complete raw Q records from the
unchanged stock BizHawk 2.3 DiscSystem assembly. The previous implementation
differs on all 1,500; corrected production ISOReader matches every byte at O0
and O2. Five read-only Pepsiman boundary positions also match stock after the
correction. Source lineage is a15b31a46bdac27d843d3ebbc5a860012d8452fb;
the fixture manifest records assembly and driver hashes. No retail input is
included in the tracked fixtures.

Run CTest with `-R 'tas_(iso_|cdrom_subq)'` for the reader, SBI and position
response checks. Disc-reader tests require PSXRECOMP_ENABLE_CHD; the controller
response checks do not. Leadout synthesis is outside this recorded-sector
correction and remains unqualified. The diagnostic stock capture includes
three leadout probes whose differences are retained explicitly. Full Pepsiman
and the unchanged Tekken regressions remain separate acceptance gates.
