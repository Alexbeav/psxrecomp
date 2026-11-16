# ChangeThread inside an interrupt handler

An explicit SYS03 ChangeThread inside the outermost handler uses the selected
thread-table block immediately. The HLE scheduler saves the handler's syscall
context in the outgoing block, advances its saved EPC past the syscall, stores
saved v0=1, selects the target and returns from that target's context. The
existing context writer's kernel-register rules remain in force.

A game may keep its own copy of the interrupted context. Running more of the
old thread before switching would repeat that work when the game restores its
copy. The delivery therefore disables deferral for this explicit call. A valid
target is taken regardless of its state word; saving context does not reopen
a closed outgoing block. A handler that edits the current-thread pointer
directly retains the existing runnable-target and deferral checks.

The helper retains the same-thread, invalid-table-address, nested-delivery
and legacy-fiber boundaries. Speculative native execution refuses a switch
before committing state. A delivery-owned notification joins the syscall and
IRQ epilogue; initialization and restore reset it. It is not serialized guest
or timing state.

`changethread_in_handler_test` runs the production syscall, interrupt and BIOS
routing modules with three guest programs at O0 and O2: copy/restore, no copy
restored, and a selected closed block. It checks register context, stack,
EPC/SR/Cause, exact work counts, same-thread and invalid-target calls. Both
source CMake trees register it for GNU builds. The separate
`syscall_closed_thread_context` regression protects outgoing closed-block saves.

The run report writes `changethread_in_handler` with `switches`, `same_thread`,
`not_taken`, `first_epc`, `first_frame` and `first_target`. Thread event34 is
`syscall3_in_exc_switch`. These counters establish route coverage; they do not
prove a title's interrupt timing or gameplay. See the private source/consumer
[verification tools](UPSTREAM_CONSUMER_EVALUATION.md) for the bounded checks.
