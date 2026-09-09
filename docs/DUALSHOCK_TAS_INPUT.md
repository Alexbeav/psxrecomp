# DualShock TAS controller encoding

PSXRTI2 preserves one Nymashock2.9.1 P1 controller input per frontend
return. It is separate from the existing PSXRTI1 digital route. The exporter
rejects console/tray events, other ports, anchored movies, malformed rows,
extra payload members and streams outside the bounded route capacity.

The little-endian 24-byte header is `8sIIII`: magic `PSXRTI2\0`, version2,
record size12, frame count, reserved0. Every `IH6B` record contains the
one-based sequential frame, active-low button word, LY/LX/RY/RX bytes,
physical Analog button0/1, and reserved0. Limits are1,000,000 records and
4,096 distinct consecutive complete states. The parser stages the entire
stream and publishes counts only after EOF validation.

The canonical digest covers seven bytes per input: little-endian button
word followed by LY/LX/RY/RX/physical Analog. It is a lossless input digest,
not a claim about guest-visible controller bytes or execution equivalence.
Physical Analog is a button, not the guest-owned digital/analog mode.

The exact source bridge needs separate delivery qualification. In BizHawk
2.9.1 (`745efb1d`), Nymashock.AddAxis writes the log byte into the high byte
of a16-bit field. The pinned Nyma Mednafen submodule (`ddf225cf`) then maps
that16-bit value with `(value *255 +32767)/65535` in DualShock.UpdateInput.
Consequently raw values129..255 deliver128..254;0..128 remain unchanged.
The older Octoshock source tree is not evidence for this Nymashock bridge.
Power starts the DualShock in digital mode with mode locking disabled.

Authored Python format/negative cases and production C parsing at O0/O2
are registered tests. The private Bio Hazard export additionally reproduced
all227,202 original inputs against an independently expanded canonical
record:2,824 segments and28 noncenter rows. Original one-based noncenter
frames are44930..44952 and45232..45236. No physical Analog press occurs.
These codec checks do not qualify native SIO, core timing or a game ending.
