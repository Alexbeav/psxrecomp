"""The admitted BIOS must be the one the movie itself declares.

A .bk2 header records the firmware its author ran, as `PSX_Firmware_<region> <sha1>`.
Admission verified the core, waterbox, disc, cue, movie, observer DLL, host closure,
settings, cards and run policy -- but never that line. So a source could be admitted on
a BIOS the movie never used, and both US titles were: Tekken 3 and Mega Man X5 ran
SCPH1001 (10155D8D) for months while every US movie declares SCPH-5501 (0555C6FA).
Replaying could never have caught it, because our runtime and BizHawk were compared
against each other on the same wrong BIOS and agreed perfectly.

Pinning the right BIOS is not enough on its own: a pin cannot catch a pin that was
wrong to begin with. These checks hold the pin and the movie's own declaration
together, so repinning one without the other fails here rather than in an archive.

Synthetic headers only; no BIOS, disc or movie data.
"""
import io
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import megamanx5_admission as admission


def movie(header_lines, tmp):
    """A minimal .bk2 carrying just the header lines under test."""
    tmp.parent.mkdir(parents=True, exist_ok=True)
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, 'w') as archive:
        archive.writestr('Header.txt', '\r\n'.join(header_lines) + '\r\n')
    tmp.write_bytes(buffer.getvalue())
    return tmp


def rejects(call, because):
    try:
        call()
    except ValueError:
        return
    raise AssertionError('accepted what it must reject: ' + because)


def main():
    import tempfile
    checks = 0
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        base = ['MovieVersion BizHawk v2.0', 'Platform PSX', 'Core Nymashock',
                'emuVersion Version 2.9.1', 'GameName Mega Man X5 (USA)']

        # The real US declaration, read back exactly and case-folded.
        us = movie(base + ['PSX_Firmware_U 0555C6FAE8906F3F09BAF5988F00E55F88E9F30B'], root / 'us.bk2')
        assert admission.declared_firmware(us) == (
            'PSX_Firmware_U', '0555c6fae8906f3f09baf5988f00e55f88e9f30b')
        checks += 1

        # A Japanese title declares its own, under a different region key.
        jp = movie(base + ['PSX_Firmware_J B05DEF971D8EC59F346F2D9AC21FB742E3EB6917'], root / 'jp.bk2')
        assert admission.declared_firmware(jp) == (
            'PSX_Firmware_J', 'b05def971d8ec59f346f2d9ac21fb742e3eb6917')
        checks += 1

        # A header that declares nothing, or declares twice, is not a usable claim.
        rejects(lambda: admission.declared_firmware(movie(base, root / 'none.bk2')),
                'a movie declaring no firmware')
        checks += 1
        rejects(lambda: admission.declared_firmware(movie(
            base + ['PSX_Firmware_U 0555C6FAE8906F3F09BAF5988F00E55F88E9F30B',
                    'PSX_Firmware_J B05DEF971D8EC59F346F2D9AC21FB742E3EB6917'], root / 'two.bk2')),
                'a movie declaring two firmwares')
        checks += 1

    # The module's own pins must agree with each other and with the region it admits.
    assert admission.FIRMWARE_NAME in admission.FIXED, 'the pinned BIOS is not in the binding closure'
    checks += 1
    assert len(admission.FIRMWARE_SHA1) == 40 and admission.FIRMWARE_SHA1 == admission.FIRMWARE_SHA1.lower()
    checks += 1
    assert int(admission.FIRMWARE_SHA1, 16) >= 0, 'the pinned SHA-1 is not hexadecimal'
    checks += 1
    region = 'PSX_Firmware_' + admission.FIRMWARE_KEY.rsplit('+', 1)[1]
    assert region == 'PSX_Firmware_U', f'this title admits {admission.FIRMWARE_KEY}, region key {region}'
    checks += 1
    # The pin is SCPH-5501, the firmware every US movie in the corpus declares. If this
    # ever has to change, the movie header changed too, and both must move together.
    assert admission.FIRMWARE_SHA1 == '0555c6fae8906f3f09baf5988f00e55f88e9f30b'
    assert admission.FIRMWARE_NAME == 'SCPH5501.BIN'
    checks += 1

    print(f'Mega Man X5 admission: BIOS is checked against the movie header, {checks} checks')


if __name__ == '__main__':
    main()
