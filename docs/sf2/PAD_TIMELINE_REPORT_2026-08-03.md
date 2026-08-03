# Final-SIO PAD timeline report — 2026-08-03

## Purpose

Extend the deterministic Mission 1 gate from scripted menu input to a human
connected slice without capturing guest RAM, media, video, or audio. The
title-neutral `PSXPAD1` stream stores only the final SIO-visible state of both
ports once per guest VBlank: buttons, sticks, reported pad type, and connection
state. Record and replay are mutually exclusive, recording refuses overwrite,
and replay faults remain sticky rather than falling through to live input.

## Human acceptance retained

The user completed an authentic cold-boot OpenGL run through Mission 1,
including damage, one death and checkpoint reload, mission completion, retail
save, and entry into Mission 2 before closing normally. The run produced 43,967
sequential PAD records (frames 1--43,967), exact length 1,406,976 bytes, and a
valid 128 KiB card-1 image. This remains valid human functional evidence.

It is not accepted as deterministic replay evidence. The launcher lost the
child exit-code property after Alt+F4 and therefore omitted its receipt; the
timeline nevertheless finalized exactly. The wrapper now treats an unavailable
PowerShell exit-code property separately from a nonzero child exit.

## First replay divergence

Three bounded attempts isolated the failure:

1. The first combined PowerShell process/observer command detached the observer
   after 30 seconds. The owned replay itself continued. This was an
   orchestration failure.
2. A separated software/headless replay reached the final timeline frame and
   executed recorded input, but its application timeline and saved card did not
   match the OpenGL recording. Software was therefore a narrowed, non-equivalent
   backend control rather than accepted replay parity.
3. A hidden-OpenGL replay remained at retail TITLE. At recorded frame 20,552,
   the timeline required `0xFFEF`, while `pad_status` observed `0xFFFF`.

The owner was the generic low-latency input path. The first VBlank sample was
recorded, then an after-pacer host sample overwrote SIO immediately before the
guest resumed. Replay suffered the symmetric defect: its recorded sample was
overwritten by neutral live input. The first file was therefore internally
valid but described the wrong sampling boundary.

## Generic correction and regression

- Replay exclusively owns SIO across the late low-latency sample.
- Recording occurs after the late sample and frame mod hooks.
- Headless, debug-turbo, manual-turbo, load-turbo, FMV-skip, and netplay
  depth-24 early returns finalize their authoritative sample before returning.
- Netplay is rejected for record/replay because lockstep applies its final pad
  in a later RAII tail; that boundary needs a separate contract before support.
- Exhaustion or frame mismatch enters a sticky replay-fault mode and never
  resumes host input.
- The binary-format unit test covers exact two-port round trip, no-overwrite,
  strict frame order, and sticky exhaustion.
- A source-order regression protects late-sample ownership and all pre-tail
  finalization branches.

The rebuilt runtime passed a real two-process preflight: 320 sequential samples,
an exact 20-frame `0xFFEF` pulse at frames 240--259, and replay observation at
frame 242.

## Corpus classification

The private corpus's deterministic-input and validation contracts were
confirmed at the policy level: capture the state the guest consumes, keep
receipts payload-free, validate from clean processes, and distinguish a valid
artifact from a semantically faithful replay. No title address or behavior was
copied. Existing fixed-frame scheduled-input work was narrowed: it proved guest
frame scheduling but did not cover a second host sample after the recorder.
The hypothesis that software and OpenGL were interchangeable replay hosts was
contradicted for this route. The prior human recording remains relevant only as
human acceptance; using it as deterministic evidence is contradicted.

Tenchu is the first independent consumer proposed for the title-neutral final-
sample ownership contract after SF2 produces a clean connected pair.

## Remaining gate

Create one new human recording with the corrected runtime, then replay it twice
from clean hidden-OpenGL processes with dummy audio. Compare stable guest state,
active overlay identities/generations, GPU/display, CD/SPU/XA, PAD, device
clocks, normalized fingerprints, memory-card writes, and final card identity.
The user's explicit death/reload/completion/save/Mission-2 observation supplies
the human semantic annotation; automation must not invent those labels from a
rendered frame alone.
