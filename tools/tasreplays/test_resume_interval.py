"""Resume accepts the original route and its neutral tail, never the endpoint."""
from run_native import checkpoint_interval, checkpoint_asset_digest, checkpoint_cards_valid
from pathlib import Path
import hashlib
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
print('PASS: checkpoint media identity detects changed track under unchanged CUE; card images are bound')
