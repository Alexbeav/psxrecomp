# Explicit CD seek ownership

SeekL and SeekP cancel an active ReadN or ReadS stream before starting the
seek. The controller clears the old read deadline, pending data-ready event,
sector slots and data request. The next read starts at the requested target.
Repeated reads without an explicit seek retain the existing stream.

This correction restores the explicit-seek part of PSX-CD-006. It does not
change implicit seek timing or first-sector delay. Broader timing changes
previously regressed Ape Escape and need separate evidence.

The hardware command contract is described in
[PSX-SPX](https://psx-spx.consoledev.net/cdromdrive/).
The source-owned regression includes the actual controller with synthetic
state: [test_cdrom_explicit_seek.c](../runtime/tests/test_cdrom_explicit_seek.c).
Run `ctest -R cdrom_explicit_seek_test --output-on-failure` in a configured
build with `BUILD_TESTING=ON`. The test covers four read/seek combinations,
stale data cancellation, target restart and repeated-read preservation.

G-Police USA Disc 1 reproduced this defect on source 63446a28: a SeekL to
07:41:31 during an existing 32:47 stream was followed by ReadS continuing
32:47. The result was a black opening with no new video packets. Restoring
explicit seek ownership reaches the normal menu and controlled Mission 1.
Full campaign, Disc 2 progression and audible acceptance remain separate.
