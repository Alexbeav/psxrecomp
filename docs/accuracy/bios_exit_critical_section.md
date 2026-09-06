# ExitCriticalSection register contract

`psx_syscall` handles BIOS SYS(02h) directly, including when optional BIOS HLE
services are disabled. It enables the current interrupt bit and the hardware
interrupt mask in COP0 SR. It preserves the caller's general registers.
In particular, it must not assign a return value to `v0`.

An SDK wrapper can call another BIOS service, then ExitCriticalSection, and
return the earlier result. Clearing `v0` turns a successful operation into a
false failure. This can prevent a caller from entering its video decode loop
even though disc streaming and audio have already started.

The hardware contract is documented in
[PSX-SPX, SYS(02h)](https://psx-spx.consoledev.net/kernelbios/#sys02h-exitcriticalsection-syscall-with-r402h).
The specification allows K0 to change; this direct handler preserves it too.
The host-only `cpu->pc = 0` continuation and C return value remain unchanged.

`exit_critical_section_test` compiles the real `traps.c` implementation with
GNU LTO and tests 16 combinations of incoming `v0` and SR. It checks the full
CPU state, allowing only the documented SR update and host continuation change.
The test uses no BIOS, generated retail code, or game data.
