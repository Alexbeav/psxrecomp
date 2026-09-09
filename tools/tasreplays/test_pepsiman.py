"""Asset-free disc-topology admission tests for the second real TAS consumer."""
import unittest
import hashlib
import json
from pathlib import Path
import tempfile
from unittest.mock import patch
import pepsiman
from observation_evidence import compare_returns, terminal_consistency
from compare_ram_pages import MAGIC, page_hash

def cue():
    parts=[]
    for i in range(1,9):
        parts += [f'FILE "track{i}.bin" BINARY',f'  TRACK {i:02d} '+('MODE2/2352' if i==1 else 'AUDIO')]
        parts += ['    INDEX 01 00:00:00'] if i==1 else ['    INDEX 00 00:00:00','    INDEX 01 00:02:00']
    return '\n'.join(parts)+'\n'

class PepsimanAdmission(unittest.TestCase):
    def test_input_identity_shape(self):
        for size,sha in pepsiman.TRACKS:
            self.assertEqual(size%2352,0)
            self.assertRegex(sha,r'^[0-9a-f]{64}$')
        for sha in [pepsiman.MOVIE_SHA,pepsiman.WORDS_SHA,pepsiman.BIOS_SHA,pepsiman.EXE_SHA]:
            self.assertRegex(sha,r'^[0-9a-f]{64}$')
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

    def test_complete_control_boundaries(self):
        with tempfile.TemporaryDirectory() as temp, patch.object(pepsiman,'FRAMES',3), patch.object(pepsiman,'verify_control_identity'):
            root=Path(temp)
            image=root/'frame-000004.png'
            image.write_bytes(b'fixture image identity')
            records={'complete.json':{'frame':4,'original_inputs':3,'full_movie':True,'neutral_tail':1},
                     'exit.json':{'exit_code':0,'stop_reason':None},
                     'semantic-review.json':{'completion_observed':True,'frame':4,'image':image.name,
                                             'image_sha256':pepsiman.digest(image)},
                     'manifest.json':{},'loaded-bios.json':{},'effective-sync.json':{},'effective-settings.json':{}}
            def restore():
                for name,data in records.items(): (root/name).write_text(json.dumps(data))
                (root/'ram-frames.tsv').write_text('frame\tlag_count\tpc\tram_sha256\n'+''.join(f'{i}\t0\t00000000\t'+ 'A'*64+'\n' for i in range(5)))
            restore()
            pepsiman.admit_source_control(root)
            cases=[('complete.json','neutral_tail',True),('complete.json','neutral_tail',6001),
                   ('complete.json','frame',3),('complete.json','full_movie',False),
                   ('exit.json','stop_reason','host_timeout'),('exit.json','exit_code',1),
                   ('semantic-review.json','completion_observed',False),('semantic-review.json','frame',2),
                   ('semantic-review.json','image','../outside.png'),('semantic-review.json','image_sha256','0'*64)]
            for name,key,value in cases:
                with self.subTest(name=name,key=key,value=value):
                    restore()
                    data=dict(records[name]);data[key]=value
                    (root/name).write_text(json.dumps(data))
                    with self.assertRaises((ValueError,OSError)): pepsiman.admit_source_control(root)
            for bad in ['frame\tlag_count\tpc\tram_sha256\n',
                        'frame\tlag_count\tpc\tram_sha256\n1\t0\t00000000\t'+'A'*64+'\n',
                        'frame\tlag_count\tpc\tram_sha256\n0\t0\t00000000\twrong\n']:
                restore();(root/'ram-frames.tsv').write_text(bad)
                with self.assertRaises(ValueError): pepsiman.admit_source_control(root)

    def test_source_binding_mutation_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); asset=root/'core.dll'; asset.write_bytes(b'original')
            manifest={'schema':'pepsiman-stock-control-v1','source_tag':'2.3','original_inputs':pepsiman.FRAMES,
                      'cutoff':pepsiman.FRAMES,'neutral_tail':0,
                      'bindings':[{'path':str(asset),'sha256':pepsiman.digest(asset)}]}
            asset.write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError,'Wrong or missing file'):
                pepsiman.verify_control_identity(root,manifest,0)


class Observations(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)
        self.zero=page_hash(bytes(4096))

    def capture(self,name,frames=3,cycle_change=None,page_change=None,frame_skew=0):
        path=self.root/name
        with path.open('w') as f:
            f.write(MAGIC+'\nframe\tcycle'+''.join(f'\t{i*4096:06X}' for i in range(512))+'\n')
            for frame in range(1,frames+1):
                pages=[self.zero]*512
                if page_change and frame==page_change[0]: pages[page_change[1]]='0000000000000000'
                cycle=frame*564480+(1 if cycle_change==frame else 0)
                f.write(f'{frame+frame_skew}\t{cycle}\t'+'\t'.join(pages)+'\n')
        return path

    def test_full_return_differences_and_coverage(self):
        source=self.capture('source.tsv')
        same=self.capture('native.tsv')
        self.assertTrue(compare_returns(source,same,3)['match'])
        for label,kwargs,kind,frame in [('cycle',{'cycle_change':2},'state_or_clock',2),
            ('last_page',{'page_change':(2,511)},'state_or_clock',2),
            ('short',{'frames':2},'missing_return',3),('long',{'frames':4},'missing_return',4)]:
            with self.subTest(label=label):
                result=compare_returns(source,self.capture(label+'.tsv',**kwargs),3)
                self.assertFalse(result['match'])
                self.assertEqual((result['first_divergence']['kind'],result['first_divergence']['frame']),(kind,frame))
                if label=='last_page': self.assertEqual(result['first_divergence']['changed_pages'],['1FF000'])
        with self.assertRaises(ValueError): compare_returns(source,self.capture('skew.tsv',frame_skew=1),3)
        with self.assertRaises(ValueError):
            (self.root/'empty.tsv').write_text(MAGIC+'\n')
            compare_returns(source,self.root/'empty.tsv',3)

    def test_terminal_bytes_must_match_both_indices(self):
        pages=self.capture('pages.tsv')
        raw=self.root/'ram.bin'; data=bytes(2097152);raw.write_bytes(data)
        terminal_consistency(pages,raw,3,hashlib.sha256(data).hexdigest())
        with self.assertRaises(ValueError): terminal_consistency(pages,raw,3,'0'*64)
        raw.write_bytes(data[:-1]+b'\1')
        with self.assertRaises(ValueError): terminal_consistency(pages,raw,3)

if __name__=='__main__': unittest.main()
