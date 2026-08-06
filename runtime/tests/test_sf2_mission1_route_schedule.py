#!/usr/bin/env python3
"""Regression for guest-event-anchored SF2 route input scheduling."""

import importlib.util
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ROUTE = ROOT / "tools" / "sf2_mission1_route.py"


def load_route_module():
    spec = importlib.util.spec_from_file_location("sf2_mission1_route", ROUTE)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    route = load_route_module()
    expected = {
        "new_game": 19200,
        "one_player": 19320,
        "leave_briefing": 24000,
        "move": 25800,
    }
    assert route.deterministic_route_schedule(18492) == expected

    # These were the two host-polled stable-TITLE observations that formerly
    # selected different schedules despite an identical retail event frame.
    for _host_poll_frame in (18596, 18612):
        assert route.deterministic_route_schedule(18492) == expected

    # The ordinary native route retains its original exact frame. A slower
    # interpreter/diagnostic route re-anchors only the pending semantic edge,
    # and nearby host polling observations choose the same guest boundary.
    assert route.semantic_future_frame(24000, 23999) == 24000
    assert route.semantic_future_frame(24000, 24417) == 25200
    assert route.semantic_future_frame(24000, 24435) == 25200

    # Sector-history replies are newest-first.  Startup identity evidence must
    # retain the first guest frame rather than whichever repeat a host poll saw.
    entries = [
        {"seq": 102, "frame": 1124},
        {"seq": 101, "frame": 1106},
        {"seq": 100, "frame": 1105},
    ]
    assert route.oldest_sector_entry(entries) == entries[2]

    print("sf2 mission1 route schedule regression: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
