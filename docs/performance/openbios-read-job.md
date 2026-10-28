# Native OpenBIOS read request

Experimental, default OFF: configure `-DPSX_NATIVE_OPENBIOS_READ=ON` in an
ENHANCED build. REFERENCE and oracle builds retain the generated firmware.
The audited OpenBIOS image is
`fabe498fbf224e4721f12f31b6f5fe0659205e341dc4e5c5f91b9bd1a1011c57`.
Other BIOS images are unchanged; an unaudited OpenBIOS body fails configuration.

The common blocking CD request takes destination, LBA and sector count in
a0/a1/a2, returns destination or -1 in v0, and preserves the callee-saved ABI.
It owns MSF conversion, seek/read submission, retry budgets, completion/error
checks and return. A0 seek/read/error services and B0 TestEvent still execute
through the guest vectors. IRQ handlers, event consumption, callbacks and
actual CD/DMA transfers retain their working implementations. The unverified
DeliverEvent HLE anchor is neither invented nor bypassed.

After a complete unsuccessful event check, the native request advances to the
next observable device event, bounded to 16K cycles for host maintenance.
Intermediate device/audio events still run. This changes polling/timing, not
the CD-speed or host-loading setting. A modified B0 vector, handler or TestEvent
table entry disables deadline skipping and keeps calling the guest handler.
No runtime implementation selector or title loader is introduced.

Continuation PCs, retry state and the original 64-byte guest frame are the
entire request state. No private serializer section, live HLE/LLE conversion or
cross-build state promise is required. Existing generated continuation wrappers
enter the native body; only the original definition is renamed, so direct
callers do not accidentally enter LLE. Both source and binding recipe identify
the selected execution family for snapshots and common netplay profiles.
PGXP callee-save provenance is published/restored through the existing hooks.

The differential fixture compiles the actual generated original request beside
the native implementation. It compares caller results, MSF, command arguments,
payload, callee-saved values and error/completion outcomes for success, seek/read
submission exhaustion, seek failure, read retry, EOF, retry exhaustion and a
poll-driven custom handler. It also destroys/reconstitutes CPU state between
every native dispatch. This fixture alone does not validate real IRQ delivery,
current-build serialization, gameplay, PGXP rendering or performance.

```
python runtime/tests/test_openbios_read_job.py --generated generated/OpenBIOS_full.c --output <scratch> --cc <C compiler>
```

Qualification belongs to `beads-wuhs` and the optimization campaign
`beads-2wy8.1`. Actual matched built-game results must be recorded before
promotion; owner adoption checks remain bundled at the end of the burndown.
