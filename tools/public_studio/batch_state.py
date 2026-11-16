"""Monotonic batch state shared by the builder and native frontend."""
import time


class BatchState:
    def __init__(self, jobs, emit=lambda event: None, clock=time.monotonic):
        self.clock, self.emit = clock, emit
        self.started = clock()
        self.finished = None
        self.current = None
        self.games = [dict(id=index, name=job.name, state="queued", elapsed_seconds=0.0)
                      for index, job in enumerate(jobs)]
        self.publish("batch_started")

    def publish(self, kind, **extra):
        self.emit(dict(event=kind, total_seconds=self.total(), games=self.snapshot(), **extra))

    def total(self):
        return max(0.0, (self.finished if self.finished is not None else self.clock()) - self.started)

    def snapshot(self):
        result = [dict(row) for row in self.games]
        if self.current is not None:
            row = result[self.current]
            row["elapsed_seconds"] = max(0.0, self.clock() - self.game_started)
        return result

    def start(self, index):
        self.current = index
        self.game_started = self.clock()
        self.games[index]["state"] = "active"
        self.publish("game_started", index=index)

    def end(self, state, **extra):
        index = self.current
        if index is None:
            return
        self.games[index].update(state=state, elapsed_seconds=max(0.0, self.clock() - self.game_started), **extra)
        self.current = None
        self.publish("game_finished", index=index)

    def finish(self, state):
        if self.current is not None:
            self.end("cancelled" if state == "cancelled" else "failed")
        self.finished = self.clock()
        self.publish("batch_finished", state=state)
        return dict(games=self.snapshot(), elapsed_seconds=self.total())
