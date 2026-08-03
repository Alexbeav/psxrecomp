from pathlib import Path


root = Path(__file__).resolve().parents[2]
main = (root / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")

low_latency = main.index("if (g_low_latency_input)")
mod_hooks = main.index("mod_call_frame_hooks();", low_latency)
capture = main.index("finalize_host_input_frame();", mod_hooks)
assert low_latency < mod_hooks < capture

block = main[low_latency:mod_hooks]
assert "if (!pad_timeline_is_replay())" in block
assert block.index("if (!pad_timeline_is_replay())") < block.index("sample_pad_into_sio(override);")

early = main[main.index("if (g_headless)", mod_hooks - 12000):low_latency]
assert early.count("finalize_host_input_frame();") >= 6
finalizer = main[main.index("static void finalize_host_input_frame"):low_latency]
assert "pad_timeline_capture(s_frame_count);" in finalizer
assert "mouse_pad_commit_frame();" in finalizer

timeline = (root / "runtime" / "src" / "pad_timeline.cpp").read_text(encoding="utf-8")
assert "ReplayFault" in timeline
assert "g_mode = Mode::ReplayFault" in timeline
assert "PAD timelines are not supported with netplay" in main

print("pad timeline final-sample ownership guards: PASS")
