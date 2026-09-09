"""Asset-free disc-topology admission tests for the second real TAS consumer."""
import unittest
import pepsiman

def cue():
    parts=[]
    for i in range(1,9):
        parts += [f'FILE "track{i}.bin" BINARY',f'  TRACK {i:02d} '+('MODE2/2352' if i==1 else 'AUDIO')]
        parts += ['    INDEX 01 00:00:00'] if i==1 else ['    INDEX 00 00:00:00','    INDEX 01 00:02:00']
    return '\n'.join(parts)+'\n'

class PepsimanAdmission(unittest.TestCase):
    def test_exact_topology(self):
        self.assertEqual(pepsiman.cue_files(cue()),[f'track{i}.bin' for i in range(1,9)])
    def test_companion_and_topology_negatives(self):
        original=cue()
        bad=[original.replace('TRACK 02 AUDIO','TRACK 02 MODE2/2352'),
             original.replace('INDEX 01 00:02:00','INDEX 01 00:01:00',1),
             original.replace('INDEX 00 00:00:00\n','',1),
             original.replace('track8.bin','track7.bin'),
             original.replace('track1.bin','../outside.bin'),
             original.replace('track1.bin','/outside.bin'),
             original.replace('track1.bin','C:outside.bin'),
             original+'REM undeclared\n',original[:original.index('FILE "track8.bin"')]]
        for text in bad:
            with self.subTest(text=text):
                with self.assertRaises(ValueError): pepsiman.cue_files(text)

if __name__=='__main__': unittest.main()
