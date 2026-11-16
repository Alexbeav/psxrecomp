"""Check the builder's shipped support files before source/toolchain setup."""
import hashlib
from pathlib import Path

LICENSE_SHA256 = '20e50fe7aae3e56378ebf0417d9de904f55a0e61e4df315333e632a4d3555d95'


def check(app=None):
    app = Path(app) if app is not None else Path(__file__).parent
    required = ('compiler_options.py', 'delivery.py', 'batch_state.py', 'native_bridge.py', 'toolchain-pin.toml', 'pin_entry.py', 'asset_preflight.py',
        'pin_builder/workbench_child.py', 'pin_builder/workbench_progress.py',
        'pin_builder/source_snapshot.py',
        'pin_builder/lean_boot_gate.py', 'pin_builder/overlay_compile_check.py',
        'pin_builder/product_residue.py', 'pin_builder/licenses/LGPL-2.1.txt')
    missing = [name for name in required if not (app/name).is_file()]
    if missing:
        raise RuntimeError('Studio is missing required build support files: '+', '.join(missing)+'. Restore the application package before building.')
    license_path=app/'pin_builder/licenses/LGPL-2.1.txt'
    if hashlib.sha256(license_path.read_bytes()).hexdigest()!=LICENSE_SHA256:
        raise RuntimeError('Studio bundled LGPL-2.1.txt differs from its required bytes. Restore the application package before building.')
    return {'required_files':len(required),'lgpl_sha256':LICENSE_SHA256}
