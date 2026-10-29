# Completed presentation images

`PSX_SHARED_PRESENT_IMAGES=ON` selects shared completed-image ownership in
ENHANCED builds. REFERENCE/oracle targets and explicit OFF retain the original
native hold capture. This is a presentation-only choice; it changes no simulated
state or common netplay profile and requires no new host capability.

The temporal renderer already captures a stable native display band. Its freshly
completed image can also serve hold-last, avoiding a second capture of the same
band. Hold stores a read descriptor containing the texture and its own dimensions.
Its private drawable capture texture remains separate, so composing a native
hold with gamma, filtering, aspect and letterboxing cannot overwrite history.
Ordinary composed/pre-OSD drawable captures retain their existing path.

`interp_capture` is the sole history writer. Its two callers immediately publish
the completed hold descriptor on the same GL context/thread. A subsequent
rotation/reallocation is followed by the same publication before any hold
consumer; metadata-only reset retains the old completed image until then.
Context teardown invalidates the descriptor. No extra fence, worker or live
implementation switch is introduced.

Reused FLIP history is not proof that the current source image is identical.
Those intervals retain the original native hold copy. Fresh captures alone are
shared. The existing letterbox-size capture optimization is unchanged.

The real hidden OpenGL fixture compares shared and original copied pre-OSD
compositions exactly across resize, full/native versus downscaled captures,
filtering and forced 4:3. It checks reset lifetime, image dimensions, private
drawable writes, unchanged history, duplicate-FLIP fallback and context teardown.
Real CMake compile/run checks cover fresh default, explicit ON/OFF and reference
target selection. Missing real GL is a skip, not a pass.

Performance qualification is tracked under `beads-wuhs`, campaign
`beads-2wy8.1`, burndown G4. Component copy counts alone are not a built-game
gain; record actual matched CPU/GPU results before promotion. Owner adoption
checks remain bundled at the end of the campaign.

Diagnostic builds can opt into `PSX_GL_CAPTURE_PERF=1`. Four buffered timestamp
pairs bracket capture plus hold publication, ending before interval pacing.
Results are harvested only when available; busy slots are retained and new
scopes skipped. Missing entrypoints provide no passing result. The private
diagnostic counters retain completed samples, total GPU nanoseconds and skipped
or unavailable scopes for external A/B collection. This scope avoids treating
the paced whole-present timer as GPU busy work. Release builds omit it.
