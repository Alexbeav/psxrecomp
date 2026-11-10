# Default CD Pause/Read position

An active ReadN or ReadS that receives a sector and is then paused keeps that
last received sector as its resume position. A read without a new Setloc repeats
it. GetlocP while paused reports the same held position. A new Setloc or a
completed explicit seek still supplies its own destination.

The controller records whether the current read has received a sector.
Starting or stopping a read and controller initialization clear that flag.
Successful data and XA sector reads set it. Pause changes the held cursor only
for an active read with this flag; completed resident reads and reads paused
before their first sector retain their existing cursor. Repeated Pause cannot
move the cursor backward again.

Pause computes its completion delay before changing this cursor. Existing fast
command acknowledgement and delay policy is unchanged.

CD snapshots append a CDF4 marker and one boolean, five bytes in total, after
the existing CDF3 extension. This preserves current-read eligibility during
restore. CDF3, CDF2 and legacy states remain readable; they contain no such
flag, so restoring them clears eligibility rather than inferring it from stale
last-sector metadata. Their saved cursor is retained until another sector is
received. Older executables cannot read the new extension.

`cdrom_pause_preservation_test` covers data and XA, paused/active restore,
repeated Pause, new targets, resident completion, explicit seek, malformed
extensions and older state readers. Its standalone driver also builds at O0
and O2. These checks establish controller behavior with authored sectors;
they do not establish audible playback, whole-game or strict fleet acceptance.

The opt-in [source CD profiles](source-cd-profile.md) retain their own clock,
physical head and audio queue state in a separate optional appendix. The
CDF4 default and legacy-reader behavior described here remain unchanged.
