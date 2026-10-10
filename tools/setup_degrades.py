"""Last-recorded setup fallbacks; these records are not a current health check."""
from __future__ import annotations

from datetime import datetime, timezone
import os
from pathlib import Path
import tempfile


def _reason_text(reason, code):
    # The C reader rejects ASCII controls, including ESC and DEL.
    text = ''.join(' ' if ord(char) < 32 or ord(char) == 127 else char for char in str(reason))
    return ' '.join(text.split()).encode('utf-8', errors='replace')[:1023].decode('utf-8', errors='ignore') or code


class SetupDegrades:
    def __init__(self, root: Path, operation: str, progress):
        self.path = root / '.cache' / f'setup-degrades-{operation}.txt'
        self.operation, self.progress = operation, progress
        self.rows = []
        self.state, self.recorded_at = 'incomplete', ''
        self.save('incomplete')

    def add(self, code, reason):
        # One line per row in the bounded C reader. Keep valid UTF-8 boundaries.
        row = {'code': code, 'reason': _reason_text(reason, code)}
        if row not in self.rows:
            if code in ('report.persistence', 'report.truncated'):
                for index, saved in enumerate(self.rows):
                    if saved['code'] == code:
                        self.rows[index] = row
                        break
                else:
                    self.rows.append(row)
            elif sum(saved['code'] not in ('report.persistence', 'report.truncated') for saved in self.rows) < 126:
                self.rows.append(row)
            else:
                self.add('report.truncated', 'More than126 ordinary fallback rows; this bounded record is partial')
            event = getattr(self.progress, 'event', None)
            if callable(event):
                event('degrade', **row)

    def save(self, state):
        self.state = state
        self.recorded_at = datetime.now(timezone.utc).isoformat()
        pending = None
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', newline='\n',
                                             dir=self.path.parent, delete=False) as stream:
                pending = Path(stream.name)
                stream.write('psxrecomp-setup-degrades-v1\n' + self.operation + '\n'
                             + state + '\n' + self.recorded_at + '\n' + str(len(self.rows)) + '\n')
                for row in self.rows:
                    stream.write(row['code'] + '\t' + row['reason'] + '\n')
            os.replace(pending, self.path)
        except OSError as exc:
            self.add('report.persistence', f'Could not save last-recorded {self.operation} fallbacks: {exc}')
            self.progress.log(f'WARNING: setup fallback report was not saved: {exc}')
            self.state = 'not_saved'
        finally:
            if pending is not None:
                try:
                    pending.unlink(missing_ok=True)
                except OSError:
                    pass


def begin_degrades(progress, root, operation):
    progress._setup_degrades = SetupDegrades(root, operation, progress)


def record_degrade(progress, code, reason):
    report = getattr(progress, '_setup_degrades', None)
    if isinstance(report, SetupDegrades):
        report.add(code, reason)
        report.save('incomplete')
    else:
        # Callers outside generate/rebuild still receive the explicit event.
        event = getattr(progress, 'event', None)
        if callable(event):
            event('degrade', code=code, reason=_reason_text(reason, code))


def finish_degrades(progress):
    report = progress._setup_degrades
    report.save('complete')
    return {'degrades': report.rows, 'degrade_report': {
        'scope': 'last_recorded', 'path': str(report.path),
        'state': report.state, 'recorded_at': report.recorded_at}}
