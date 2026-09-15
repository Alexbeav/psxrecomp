"""Every admission module must require the BIOS its movie declares.

A .bk2 header records the firmware its author ran, as `PSX_Firmware_<region> <sha1>`.
Admission verified the core, waterbox, disc, cue, movie, observer DLL, host closure,
settings, cards and run policy -- and never that line, though both values were already
in files it reads. So a source could be admitted on a BIOS the movie never used, and
both US titles were: Tekken 3 and Mega Man X5 ran SCPH1001 (10155D8D) while every US
movie declares SCPH-5501 (0555C6FA). Replaying could not have caught it, because our
runtime and BizHawk were compared against each other on the same wrong BIOS and agreed.

Pinning the right BIOS is not enough on its own: a pin cannot catch a pin that was wrong
to begin with. This holds each module's pin and the header it claims to match together,
and fails when a new module forgets the check entirely.

Synthetic headers only; no BIOS, disc or movie data.
"""
import io
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

# Modules expected to carry the cross-check.
CHECKED = ('megamanx5_admission', 'megamanx4_admission', 'redc_admission')

# Deliberately exempt, and asserted below so the exemption cannot rot into a silent
# gap. nymashock_admission.py admits the qualified Bio Hazard 5920M source, whose
# firmware ALREADY matches its movie (PSX_Firmware_J B05DEF97, SCPH-5500). Editing it
# would change its bytes, and biohazard.py verifies the admitted reference's recorded
# admission_tool_sha256 against exactly those bytes -- so touching it would invalidate
# a qualified reference to add a check that would pass anyway.
EXEMPT = {'nymashock_admission': '9c0421858e217805f4abe18698afea8d5aa36ff0727eb8484944e00eb5e7eadb'}

# Region key -> the SHA-1 of the only firmware the corpus uses for it.
REGION_FIRMWARE = {'PSX_Firmware_U': '0555c6fae8906f3f09baf5988f00e55f88e9f30b',
                   'PSX_Firmware_J': 'b05def971d8ec59f346f2d9ac21fb742e3eb6917'}


def movie(header_lines, path):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, 'w') as archive:
        archive.writestr('Header.txt', '\r\n'.join(header_lines) + '\r\n')
    path.write_bytes(buffer.getvalue())
    return path


def rejects(call, because):
    try:
        call()
    except ValueError:
        return
    raise AssertionError('accepted what it must reject: ' + because)


def main():
    import tempfile
    checks = 0
    modules = {name: __import__(name) for name in CHECKED}

    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        base = ['MovieVersion BizHawk v2.0', 'Platform PSX', 'Core Nymashock']

        for name, module in modules.items():
            assert hasattr(module, 'declared_firmware'), f'{name} has no movie-header check'
            assert hasattr(module, 'FIRMWARE_NAME') and hasattr(module, 'FIRMWARE_SHA1'), \
                f'{name} does not name its firmware'
            checks += 1

            # Reads a real declaration back, case-folded, for the region it admits.
            region = 'PSX_Firmware_' + module.FIRMWARE_KEY.rsplit('+', 1)[1]
            assert region in REGION_FIRMWARE, f'{name} admits unknown region {region}'
            sha1 = REGION_FIRMWARE[region]
            good = movie(base + [f'{region} {sha1.upper()}'], root / f'{name}-ok.bk2')
            assert module.declared_firmware(good) == (region, sha1)
            checks += 1

            # The module's own pins must agree with each other.
            assert module.FIRMWARE_NAME in module.FIXED, f'{name} pins a BIOS outside its closure'
            assert module.FIRMWARE_SHA1 == module.FIRMWARE_SHA1.lower() and len(module.FIRMWARE_SHA1) == 40
            int(module.FIRMWARE_SHA1, 16)
            assert module.FIRMWARE_SHA1 == sha1, \
                f'{name} pins {module.FIRMWARE_SHA1} but admits {region}, which is {sha1}'
            checks += 1

            # A header that declares nothing, or declares twice, is not a usable claim.
            rejects(lambda m=module: m.declared_firmware(movie(base, root / f'{m.__name__}-none.bk2')),
                    f'{name}: a movie declaring no firmware')
            rejects(lambda m=module: m.declared_firmware(movie(
                base + [f'PSX_Firmware_U {REGION_FIRMWARE["PSX_Firmware_U"].upper()}',
                        f'PSX_Firmware_J {REGION_FIRMWARE["PSX_Firmware_J"].upper()}'],
                root / f'{m.__name__}-two.bk2')), f'{name}: a movie declaring two firmwares')
            checks += 2

    # The exemption stays an exemption: still no cross-check, still the right BIOS.
    for name, expected_sha256 in EXEMPT.items():
        module = __import__(name)
        assert not hasattr(module, 'declared_firmware'), \
            f'{name} gained the check; move it out of EXEMPT and re-admit its reference'
        bios = [k for k in module.FIXED if 'SCPH' in k.upper()]
        assert len(bios) == 1 and module.FIXED[bios[0]] == expected_sha256, \
            f'{name} changed BIOS while exempt from the cross-check'
        checks += 1

    print(f'Admission firmware cross-check: {len(modules)} module(s) covered, '
          f'{len(EXEMPT)} documented exemption, {checks} checks')


if __name__ == '__main__':
    main()
