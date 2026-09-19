"""Generate input-only SPU black-box experiments for T172.

No emulator code or old expected outputs supplied this matrix. Register fields
come from PSX-SPX a253f078 docs/soundprocessingunitspu.md, lines 85-160,
330-507 and 549-655. Each case cold-resets a separately licensed reference.
Writes occur between sample boundaries. Observe at reset, immediately after
each write, and after every completed sample; never expose private core state.
Usage: python runtime/tests/spu_envelope_experiments.py > experiments.json
"""

import json


BASE = 0x1F801C00
CTRL = 0x1F801DAA
KON, KOFF = 0x1F801D88, 0x1F801D8C
LOW, HIGH, LEVEL = BASE + 8, BASE + 10, BASE + 12


def write(address, value):
    return {"op": "write16", "address": address, "value": value}


def tick(samples):
    return {"op": "tick", "samples": samples}


def matrix():
    cases = []

    def add(name, question, ops):
        cases.append({"id": name, "question": question,
                      "operations": [write(CTRL, 0xC000)] + ops})

    def voice(lo, hi):
        return [write(BASE + 6, 0x200), write(BASE + 14, 0x200),
                write(BASE + 4, 0x1000), write(LOW, lo), write(HIGH, hi)]

    # Fixed-to-sweep handoff probes immediate versus sample-latched writes.
    for address, channel in [(BASE, "voice_left"), (BASE + 2, "voice_right"),
                             (0x1F801D80, "main_left"), (0x1F801D82, "main_right")]:
        for delay in [0, 1, 2, 4]:
            add(f"fixed_latch_{channel}_{delay}", "When does fixed volume take effect?",
                [write(address, 0x1234)] + ([tick(delay)] if delay else []) +
                [write(address, 0x8030), tick(24)])

    # Every mode/polarity combination at positive, zero and negative levels.
    for initial in [0, 1, 0x1000, 0x3000, 0x3FFF, 0x4000, 0x7000, 0x7FFF]:
        for mode in range(8):
            for rate in [0, 0x28, 0x30, 0x7F]:
                raw = 0x8000 | (mode << 12) | rate
                add(f"sweep_{initial:04x}_{raw:04x}",
                    "How do polarity, direction, threshold and halt interact?",
                    [write(BASE, initial), tick(4), write(BASE, raw), tick(48)])

    # Perturb different sub-period offsets. Include untouched controls.
    for change, raw in [("same", 0x8038), ("faster", 0x8034),
                        ("slower", 0x803C), ("direction", 0xA038),
                        ("halt", 0x807F)]:
        for offset in range(1, 9):
            add(f"sweep_counter_{change}_{offset}",
                "Does a sweep write preserve fractional counter progress?",
                [write(BASE, 0x1000), tick(4), write(BASE, 0x8038), tick(offset),
                 write(BASE, raw), tick(40)])
    add("sweep_counter_control", "Uninterrupted eight-sample period control",
        [write(BASE, 0x1000), tick(4), write(BASE, 0x8038), tick(48)])
    for offset in range(1, 9):
        add(f"sweep_fixed_interposition_{offset}",
            "Does fixed mode reset the sweep counter?",
            [write(BASE, 0x1000), tick(4), write(BASE, 0x8038), tick(offset),
             write(BASE, 0x1000), tick(1), write(BASE, 0x8038), tick(40)])

    # Observe complete short envelopes and their boundary neighborhoods.
    for sustain_level in [0, 7, 15]:
        for decay_shift in [0, 6, 15]:
            lo = (decay_shift << 4) | sustain_level
            add(f"adsr_phases_{sustain_level}_{decay_shift}",
                "At which sample do attack and decay transitions take effect?",
                voice(lo, 0x1FC0) + [write(KON, 1), tick(768),
                                     write(KOFF, 1), tick(24)])

    # Key events and rate changes at multiple fractions of a slow attack.
    for offset in range(1, 25):
        setup = voice(0x3860, 0x1FCE) + [write(KON, 1), tick(offset)]
        add(f"adsr_rekey_{offset}", "Does key-on reset level and counter?",
            setup + [write(KON, 1), tick(64)])
        add(f"adsr_keyoff_{offset}", "Does key-off retain counter progress?",
            setup + [write(KOFF, 1), tick(64)])
        add(f"adsr_ratewrite_{offset}", "Does an ADSR rate write retain counter progress?",
            setup + [write(LOW, 0x3460), tick(64)])
    add("adsr_counter_control", "Uninterrupted slow-attack control",
        voice(0x3860, 0x1FCE) + [write(KON, 1), tick(96)])

    # Register pokes cover signs, thresholds and extrema, including halted attack.
    for lo in [0x3060, 0x7F60]:
        for current in [0, 1, 0x07FF, 0x0800, 0x6000, 0x6001,
                        0x7FFE, 0x7FFF, 0x8000, 0xFFFF]:
            for offset in [4, 5, 6]:
                add(f"adsr_write_{lo:04x}_{current:04x}_{offset}",
                    "Does direct ADSR volume write alter internal progression?",
                    voice(lo, 0x1FC0) + [write(KON, 1), tick(offset),
                                        write(LEVEL, current), tick(32)])

    # Release and sustain halt encodings are observed without assuming a result.
    for release in [0, 15, 30, 31, 0x20, 0x2F, 0x3E, 0x3F]:
        add(f"release_{release:02x}", "Does the final release rate halt?",
            voice(0x000F, 0x1FC0 | release) +
            [write(KON, 1), tick(16), write(KOFF, 1), tick(96)])
    for rate in [0x78, 0x7B, 0x7C, 0x7E, 0x7F]:
        add(f"sustain_{rate:02x}", "Which sustain rates continue to advance?",
            voice(0x000F, rate << 6) + [write(KON, 1), tick(16),
                                      write(LEVEL, 0x4000), tick(96)])

    # Voice isolation: no voice-1 key or volume write is sent.
    add("voice_isolation", "Do voice-0 operations affect voice-1 registers?",
        voice(0x3060, 0x1FC0) + [write(KON, 1), tick(32),
                                write(KOFF, 1), tick(32)])

    assert len({case["id"] for case in cases}) == len(cases)
    for case in cases:
        for op in case["operations"]:
            if op["op"] == "write16":
                assert 0x1F801C00 <= op["address"] <= 0x1F801FFE
                assert op["address"] % 2 == 0 and 0 <= op["value"] <= 65535
            else:
                assert op["op"] == "tick" and 0 < op["samples"] <= 768
    return {
        "schema": "t172-spu-experiment-v1",
        "reset": "cold before every case; fresh core state and zero SPU RAM",
        "preload": [{"address": 0x1000, "bytes": [0x0C, 0x07] + [0] * 14}],
        "observe": [LEVEL, BASE + 28, 0x1F801E00, 0x1F801E02,
                    0x1F801E04, 0x1F801E06, 0x1F801DB8, 0x1F801DBA,
                    0x1F801D9C, 0x1F801DAE],
        "observation_timing": "reset, each write immediately, each completed sample",
        "write_timing": "ordered writes between ticks; no implicit time advance",
        "expected_outputs": None,
        "cases": cases,
    }


if __name__ == "__main__":
    print(json.dumps(matrix(), indent=2))
