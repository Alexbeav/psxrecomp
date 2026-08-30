#!/usr/bin/env python3
"""Title-neutral Setloc plus ReadN/ReadS implicit-seek contract."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def function_body(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    end = source.index(next_signature, start)
    return source[start:end]


def require_in_order(body: str, *needles: str) -> None:
    position = -1
    for needle in needles:
        position = body.index(needle, position + 1)


def model_contract() -> None:
    target_lba = 900
    current_lba = 100
    setloc_pending = True
    seek_far = abs(target_lba - current_lba) > 16
    single_speed_sector_cycles = 451_584
    double_speed = True

    seek_cycles = single_speed_sector_cycles * 4 if seek_far else 0x800
    sector_cycles = (
        single_speed_sector_cycles // 2
        if double_speed
        else single_speed_sector_cycles
    )
    first_sector_delay = seek_cycles + sector_cycles

    status = "SEEK"
    elapsed = seek_cycles
    assert elapsed < first_sector_delay
    assert status == "SEEK"

    elapsed = first_sector_delay
    status = "READ"
    delivered_lba = target_lba
    setloc_pending = False
    assert elapsed == first_sector_delay
    assert status == "READ"
    assert delivered_lba == target_lba
    assert not setloc_pending


def model_explicit_seek_ownership() -> None:
    reading = True
    status = {"READ"}

    reading = False
    status.discard("READ")
    status.discard("PLAY")
    status.add("SEEK")

    assert not reading
    assert status == {"SEEK"}


def main() -> None:
    source = (ROOT / "runtime/src/cdrom.c").read_text(encoding="utf-8")
    start = function_body(source, "static void start_read_stream", "static void stop_read_stream")
    commands = function_body(source, "static void exec_command", "static void process_pending")
    stream = function_body(
        source,
        "static void process_read_stream",
        "static void present_pending_dataready",
    )

    require_in_order(
        start,
        "int implicit_seek = setloc_pending;",
        "setloc_pending = 0;",
        "seek_complete_delay_cycles() + sector_delay_cycles()",
        "stat_reg |= CDSTAT_SEEK;",
    )
    require_in_order(
        stream,
        "if (read_delay <= 0)",
        "if (stat_reg & CDSTAT_SEEK)",
        "stat_reg &= (uint8_t)~CDSTAT_SEEK;",
        "stat_reg |= CDSTAT_READ;",
        "cd_timing_begin_sector",
    )
    require_in_order(
        commands,
        "case 0x15:",
        "case 0x16:",
        "stop_read_stream();",
        "stat_reg &= (uint8_t)~(CDSTAT_READ | CDSTAT_PLAY);",
        "stat_reg |= CDSTAT_SEEK;",
    )

    model_contract()
    model_explicit_seek_ownership()
    print("CD-ROM seek semantics contract: PASS")


if __name__ == "__main__":
    main()
