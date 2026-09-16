"""Resume accepts the original route and its neutral tail, never the endpoint."""
from run_native import checkpoint_interval, checkpoint_asset_digest, checkpoint_cards_valid, resolve_saved_states
from pathlib import Path
import hashlib
import json
import tempfile

# A pre-card (v2) manifest cannot resume: it never recorded the guest's card images.
try:
    checkpoint_interval(dict(schema='psx-tas-stateio-v2', frame=300, input_consumed=301), 227202, 12001)
except ValueError:
    pass
else:
    raise AssertionError('v2 manifest admitted')

for frame, consumed in ((300, 301), (227201, 227202), (233567, 233568)):
    manifest = dict(schema='psx-tas-stateio-v3', frame=frame, input_consumed=consumed)
    assert checkpoint_interval(manifest, 227202, 12001) == (frame, consumed)
for frame, consumed in ((0, 1), (239202, 239202), (300, 239203), (True, 2), (300, '301')):
    try:
        checkpoint_interval(dict(schema='psx-tas-stateio-v3', frame=frame,
                                 input_consumed=consumed), 227202, 12001)
    except ValueError:
        pass
    else:
        raise AssertionError((frame, consumed))
print('PASS: resume interval including neutral tail and endpoint rejection')
with tempfile.TemporaryDirectory() as folder:
    root = Path(folder)
    cue = root/'game.cue'; track = root/'track.bin'
    cue.write_text('FILE "track.bin" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\n')
    track.write_bytes(b'first disc')
    before = checkpoint_asset_digest(cue)
    track.write_bytes(b'other disc')
    assert checkpoint_asset_digest(cue) != before
    # Card images beside a state: named ones must exist with their exact digest; 'none' needs no file.
    state = root/'tas-state-000300.pst'; state.write_bytes(b'state')
    image = bytes(range(256)) * 512
    Path(f'{state}.card1.mcd').write_bytes(image)
    manifest = {'card1_sha256': hashlib.sha256(image).hexdigest(), 'card2_sha256': 'none'}
    assert checkpoint_cards_valid(state, manifest)
    assert not checkpoint_cards_valid(state, dict(manifest, card2_sha256=manifest['card1_sha256']))
    Path(f'{state}.card1.mcd').write_bytes(image[:-1] + b'\1')
    assert not checkpoint_cards_valid(state, manifest)
    assert not checkpoint_cards_valid(state, dict(manifest, card1_sha256=None))
    # Requests resolve to the one state that covers them; a deferred capture covers several.
    run = root/'run'; run.mkdir()
    def capture(frame, requested, payload=b'state', cards=None):
        path = run/f'tas-state-{frame:06d}.pst'; path.write_bytes(payload)
        Path(f'{path}.json').write_text(json.dumps({
            'frame': frame, 'requested_frame': requested, 'state_bytes': len(payload),
            'state_sha256': hashlib.sha256(payload).hexdigest(), 'card1_sha256': 'none', 'card2_sha256': 'none'}))
        return path
    capture(500, 500); capture(1501, 1500); capture(2600, 2500, b'other')
    resolved = {item['frame']: item for item in resolve_saved_states(run, [500, 1500, 1501, 2500, 2560, 3000])}
    assert [resolved[n]['state_frame'] for n in (500, 1500, 1501, 2500, 2560)] == [500, 1501, 1501, 2600, 2600]
    assert all(resolved[n]['valid'] for n in (500, 1500, 1501, 2500, 2560))
    assert resolved[3000] == {'frame': 3000, 'state_frame': None, 'state': None, 'valid': False}
    (run/'tas-state-002600.pst').write_bytes(b'changed')
    assert not resolve_saved_states(run, [2500])[0]['valid']
    capture(2601, 2550)  # overlapping coverage is ambiguous, never silently picked
    assert resolve_saved_states(run, [2560])[0]['state'] is None
print('PASS: checkpoint media identity detects changed track under unchanged CUE; card images are bound')
