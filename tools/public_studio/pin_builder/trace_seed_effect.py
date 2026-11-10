#!/usr/bin/env python3
"""Measure trace-seeded fallback change without weakening state/render gates."""

from __future__ import annotations

from typing import Any


class TraceSeedError(ValueError):
    """The before-and-after evidence is not comparable."""


def _count(value: Any, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise TraceSeedError(f"{label} must be a nonnegative integer")
    return value


def compare(value: dict[str, Any]) -> dict[str, Any]:
    if value.get("schema_version") != 1:
        raise TraceSeedError("schema_version must be 1")
    before, after = value.get("before"), value.get("after")
    if not isinstance(before, dict) or not isinstance(after, dict):
        raise TraceSeedError("before and after evidence are required")
    identity_fields = ("route_id", "content_sha256", "frame_count")
    for field in identity_fields:
        if before.get(field) != after.get(field):
            raise TraceSeedError(f"before and after {field} do not match")
    before_fallback = _count(before.get("runtime_fallback"), "before.runtime_fallback")
    after_fallback = _count(after.get("runtime_fallback"), "after.runtime_fallback")
    state_equal = before.get("state_sha256") == after.get("state_sha256")
    before_render = before.get("render")
    after_render = after.get("render")
    if not isinstance(before_render, dict) or not isinstance(after_render, dict):
        raise TraceSeedError("render evidence is required")
    if before_render.get("status") == "abstain" or after_render.get("status") == "abstain":
        render_status = "abstain"
        render_equal = None
    else:
        render_equal = before_render.get("sha256") == after_render.get("sha256")
        render_status = "pass" if render_equal else "fail"
    reduction = before_fallback - after_fallback
    no_regression = state_equal and render_equal is not False and after_fallback <= before_fallback
    if before_fallback == 0:
        outcome = "abstain_no_reducible_fallback"
    elif reduction > 0 and no_regression:
        outcome = "pass"
    else:
        outcome = "fail"
    return {
        "schema_version": 1,
        "route_id": before["route_id"],
        "content_sha256": before["content_sha256"],
        "frame_count": before["frame_count"],
        "runtime_fallback": {
            "before": before_fallback,
            "after": after_fallback,
            "reduction": reduction,
        },
        "state_equal": state_equal,
        "render": {"status": render_status, "equal": render_equal},
        "no_regression": no_regression,
        "outcome": outcome,
    }

