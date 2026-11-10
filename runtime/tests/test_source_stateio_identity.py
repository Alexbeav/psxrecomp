"""v7 identity dimensions, compiled at O0/O2 with -Werror."""
import argparse
import hashlib
import os
import subprocess
import tempfile
from pathlib import Path

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--cc', default='gcc')
    a = p.parse_args()
    here = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        for opt in ['-O0', '-O2']:
            exe = root / ('stateio_identity' + opt + ('.exe' if os.name == 'nt' else ''))
            subprocess.run([a.cc, '-std=c11', '-D_XOPEN_SOURCE=700', opt, '-Wall', '-Wextra', '-Werror',
                            '-I' + str(here.parent / 'include'),
                            str(here / 'test_source_stateio_identity.c'),
                            str(here.parent / 'src' / 'source_stateio_identity.c'),
                            str(here.parent / 'src' / 'psx_sha256.c'),
                            '-o', str(exe)], check=True)
            probe = root / ('probe' + opt + '.bin')
            probe.write_bytes(b'identity probe')
            # Exercise the config digest against two distinct environments so a
            # constant-returning implementation cannot pass.
            env_a = {k: v for k, v in os.environ.items() if not k.startswith('PSX_')}
            env_b = dict(env_a, PSX_TIMER1_MODEL='octoshock-2.2.2')
            out_a = subprocess.run([str(exe), str(probe)], env=env_a,
                                   capture_output=True, timeout=30)
            assert out_a.returncode == 0, (opt, out_a.returncode, out_a.stdout, out_a.stderr)
            out_b = subprocess.run([str(exe), str(probe)], env=env_b,
                                   capture_output=True, timeout=30)
            assert out_b.returncode == 0, (opt, out_b.returncode, out_b.stdout, out_b.stderr)
            def config_of(out):
                for line in out.stdout.decode(errors='replace').splitlines():
                    if line.startswith('config='):
                        return line.split('=', 1)[1].strip()
                raise AssertionError((opt, out))
            # The digest must move with the resolved configuration, not be a constant.
            cfg_a, cfg_b = config_of(out_a), config_of(out_b)
            assert len(cfg_a) == 64 and len(cfg_b) == 64, (opt, cfg_a, cfg_b)
            assert cfg_a != cfg_b, (opt, 'config digest did not react to PSX_TIMER1_MODEL')
            # The CD source clock tape is bound by content, not by path: the same
            # bytes in another folder (another host) give the same digest, and
            # other bytes of the same length do not.
            tape_bytes = bytes(range(256)) * 16
            here_dir, there_dir = root / ('host-a' + opt), root / ('host-b' + opt)
            here_dir.mkdir()
            there_dir.mkdir()
            (here_dir / 'random.psxrng').write_bytes(tape_bytes)
            (there_dir / 'random.psxrng').write_bytes(tape_bytes)
            other = root / ('other' + opt + '.psxrng')
            other.write_bytes(bytes(reversed(tape_bytes)))
            def run_with_tape(tape, stored=None, extra=None):
                env = dict(env_b, PSX_CD_SOURCE_CLOCK_TAPE=str(tape))
                env.update(extra or {})
                argv = [str(exe), str(probe)] + ([stored] if stored else [])
                out = subprocess.run(argv, env=env, capture_output=True, timeout=30)
                assert out.returncode == 0, (opt, out.returncode, out.stdout, out.stderr)
                fields = {}
                for line in out.stdout.decode('utf-8', errors='replace').splitlines():
                    if '=' in line:
                        k, v = line.split('=', 1)
                        fields[k.strip()] = v.strip()
                return fields
            def config_with_tape(tape):
                return run_with_tape(tape)['config']
            cfg_here = config_with_tape(here_dir / 'random.psxrng')
            cfg_there = config_with_tape(there_dir / 'random.psxrng')
            cfg_other = config_with_tape(other)
            assert cfg_here == cfg_there, (opt, 'same clock tape in another folder changed the config digest')
            assert cfg_here != cfg_other, (opt, 'a different clock tape kept the config digest')
            assert cfg_here != cfg_b, (opt, 'the clock tape did not enter the config digest')
            # A state saved before the content binding stores the path-bound
            # (legacy) digest. With the same tape at the same path it still
            # resumes; with another tape it is refused; the content form still
            # resumes across folders.
            legacy_here = run_with_tape(here_dir / 'random.psxrng')['legacy']
            assert legacy_here != cfg_here, (opt, 'legacy and content forms coincide with a tape set')
            judged = run_with_tape(here_dir / 'random.psxrng', legacy_here)['accept']
            assert judged == 'legacy', (opt, 'legacy state with the same tape path was not accepted', judged)
            judged = run_with_tape(other, legacy_here)['accept']
            assert judged == 'refused', (opt, 'legacy state with a different tape was accepted', judged)
            judged = run_with_tape(there_dir / 'random.psxrng', cfg_here)['accept']
            assert judged == 'content', (opt, 'content-form state was not accepted in another folder', judged)
            judged = run_with_tape(other, cfg_here)['accept']
            assert judged == 'refused', (opt, 'content-form state with a different tape was accepted', judged)
            migration = dict(PSX_TAS_RESUME_LEGACY_CLOCK_PATH=str(here_dir / 'random.psxrng'),
                             PSX_TAS_RESUME_LEGACY_CLOCK_SHA256=hashlib.sha256(tape_bytes).hexdigest())
            judged = run_with_tape(there_dir / 'random.psxrng', legacy_here, migration)['accept']
            assert judged == 'legacy', (opt, 'explicit relocation of legacy tape identity refused', judged)
            for tape, extra in [
                (other, migration),
                (there_dir / 'missing.psxrng', migration),
                (there_dir / 'random.psxrng', dict(migration, PSX_TIMER1_MODEL='foreign-model')),
                (there_dir / 'random.psxrng', dict(migration, PSX_TAS_RESUME_LEGACY_CLOCK_PATH='wrong-old-path')),
                (there_dir / 'random.psxrng', dict(migration, PSX_TAS_RESUME_LEGACY_CLOCK_SHA256='')),
            ]:
                assert run_with_tape(tape, legacy_here, extra)['accept'] == 'refused', (opt, 'unsafe legacy relocation accepted', tape, extra)
            # One explicit native model transition: no generic digest override.
            old_tape = here_dir / 'random.psxrng'
            new_tape = there_dir / 'random.psxrng'
            model = dict(PSX_SPU_KEYOFF_MODEL='octoshock-1.11.9')
            transition = dict(model, PSX_TAS_RESUME_SPU_KEYOFF_FROM='unversioned',
                              PSX_TAS_RESUME_COMPATIBLE_BUILD='1', PSX_TAS_RESUME_STATE='native.pst')
            new_cfg = run_with_tape(new_tape, extra=model)['config']
            assert new_cfg != cfg_here, (opt, 'new model absent from identity')
            assert run_with_tape(new_tape, cfg_here, model)['accept'] == 'refused'
            assert run_with_tape(new_tape, cfg_here, transition)['accept'] == 'spu-content'
            assert run_with_tape(new_tape, legacy_here, dict(transition, **migration))['accept'] == 'spu-legacy'
            assert run_with_tape(new_tape, new_cfg, model)['accept'] == 'content'
            assert run_with_tape(new_tape, new_cfg, transition)['accept'] == 'content'
            for extra in [
                dict(transition, PSX_TAS_RESUME_COMPATIBLE_BUILD=''),
                dict(transition, PSX_TAS_RESUME_STATE=''),
                dict(transition, PSX_TAS_RESUME_SPU_KEYOFF_FROM='foreign'),
                dict(transition, PSX_SPU_KEYOFF_MODEL='foreign'),
                dict(transition, PSX_INPUT_ROUTE_CAPTURE_EVERY='9999'),
                dict(transition, PSX_TIMER1_MODEL='foreign'),
            ]:
                assert run_with_tape(new_tape, cfg_here, extra)['accept'] == 'refused', (opt, extra)
            assert run_with_tape(other, cfg_here, transition)['accept'] == 'refused'
            assert run_with_tape(other, legacy_here, dict(transition, **migration))['accept'] == 'refused'
    print('PASS: v7 identity dimensions and explicit SPU model transition positives/negatives at O0/O2')

if __name__ == '__main__':
    main()
