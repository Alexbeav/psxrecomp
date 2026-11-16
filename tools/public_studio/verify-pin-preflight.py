"""Preflight the real existing pin/artifacts and isolated old-core activation."""
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
from unittest import mock
from pin_builder import pin_build as pb, ui_selection

if '--no-op' in sys.argv:
    print('noop: real pin preflight')
    raise SystemExit(0)
root=Path(__file__).resolve().parents[3]
cache=root/'pin-cache'
pin=pb.load_pin(cache/'records/PIN.json')
selection=ui_selection.select(pin,cache/'ui-input/UI.json')
artifacts=ui_selection.module_artifacts(pin,selection)
assert selection['module']['abi']=='recomp_launcher_query_v1'
assert artifacts['checked'][0]['sha256']=='5349989c011c1caadc8740cdb66a712510b659386fdb6d6367a5d6c1e0954731'
assert artifacts['checked'][1]['sha256']=='858008a3756446937c8edc027aa8ac9347b404397c3f61274aa5569d88aef98a'
original=cache/'c/tools/toolchain_pack.py'
original_hash=pb.sha(original)
with tempfile.TemporaryDirectory(dir=root/'tmp') as folder:
    fw=Path(folder)
    (fw/'tools').mkdir()
    copied=fw/'tools/toolchain_pack.py'
    copied.write_bytes(original.read_bytes())
    receipt={}
    pb.isolate_toolchain_environment(fw,receipt)
    spec=importlib.util.spec_from_file_location('pin_toolchain_spy',copied)
    module=importlib.util.module_from_spec(spec)
    sys.modules[spec.name]=module
    spec.loader.exec_module(module)
    def forbidden(*args,**kwargs):
        raise AssertionError('global publication attempted')
    with mock.patch.dict(os.environ,{'PSX_STUDIO_PROJECT_ONLY':'1'}), \
         mock.patch.object(module,'_set_latest_pointer',side_effect=forbidden), \
         mock.patch.object(module,'_register_user_path_windows',side_effect=forbidden), \
         mock.patch.object(module,'activate_toolchain_bin') as activate:
        result=module.register_toolchain_user_env(root/'inputs/toolchain')
        assert result==root/'inputs/toolchain'
        activate.assert_called_once()
    assert pb.sha(original)==original_hash
record={'passed':6,'module_artifacts':artifacts,'activation':receipt,'original_source_unchanged':True,
        'scope':'PIN/UI contract and real old-core process-only activation; no game build yet'}
(root/'reports/pin-preflight-1.json').write_text(json.dumps(record,indent=2),encoding='utf-8')
print(json.dumps(record))
