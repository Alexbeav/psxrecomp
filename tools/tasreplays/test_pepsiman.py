"""Asset-free disc-topology admission tests for the second real TAS consumer."""
import unittest
import hashlib
import json
from pathlib import Path
import tempfile
import os
import subprocess
import sys
from unittest.mock import patch
import pepsiman
from observation_evidence import compare_returns, terminal_consistency, compare_stock_observations, captured_frames, validate_cpu_capture
from verify_scph5500_seeds import derive
from compare_ram_pages import MAGIC, page_hash
from process_budget import wait_budgeted

def cue():
    parts=[]
    for i in range(1,9):
        parts += [f'FILE "track{i}.bin" BINARY',f'  TRACK {i:02d} '+('MODE2/2352' if i==1 else 'AUDIO')]
        parts += ['    INDEX 01 00:00:00'] if i==1 else ['    INDEX 00 00:00:00','    INDEX 01 00:02:00']
    return '\n'.join(parts)+'\n'

class PepsimanAdmission(unittest.TestCase):
    def test_legacy_control_reference_keeps_campaign_base(self):
        with tempfile.TemporaryDirectory() as temp:
            campaign=Path(temp);stock=campaign/'stock';observed=campaign/'observed'
            stock.mkdir();observed.mkdir();file=stock/'ram-frames.tsv';file.write_text('bound capture')
            binding={'path':'stock/ram-frames.tsv','sha256':pepsiman.digest(file)}
            resolved=pepsiman.control_binding_path(observed,binding)
            self.assertTrue(resolved.is_absolute());self.assertEqual(resolved,file.resolve())
    def test_seed_window_keeps_only_exact_bytes(self):
        original=bytes(256);target=bytearray(original);target[63]=1
        seeds=[{'address':f'0x{0xBFC00000+i:08X}'} for i in [0,64,192,193]]
        kept,dropped=derive(seeds,original,target)
        self.assertEqual(kept,[seeds[1],seeds[2]])
        self.assertEqual(dropped,[seeds[0],seeds[3]])
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

    def test_cpu_return_coverage_is_mandatory(self):
        path=self.root/'cpu-return.tsv'
        header='\t'.join(['frame','pc','cycle','sr','cause','epc']+[f'r{i}' for i in range(32)])+'\n'
        row=lambda frame:'\t'.join([str(frame),'80000000',str(frame*564480)]+['00000000']*35)+'\n'
        path.write_text(header+row(1)+row(2))
        self.assertTrue(validate_cpu_capture(self.root,2)['valid'])
        path.write_text(header+row(301)+row(302))
        self.assertTrue(validate_cpu_capture(self.root,302,first_frame=301)['valid'])
        with self.assertRaises(ValueError):validate_cpu_capture(self.root,302,first_frame=300)
        path.write_text(header+row(1)+row(2))
        with self.assertRaises(ValueError):validate_cpu_capture(self.root,3)
        path.write_text(header+row(1)+row(3))
        with self.assertRaises(ValueError):validate_cpu_capture(self.root,2)
        path.write_text(header+row(1).replace('80000000','bad'))
        with self.assertRaises(ValueError):validate_cpu_capture(self.root,1)

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

    def test_passivity_compares_complete_image_inventory(self):
        stock=self.root/'stock';observed=self.root/'observed';stock.mkdir();observed.mkdir()
        for root in [stock,observed]:
            (root/'ram-frames.tsv').write_text('header\n'+'same\n'*7)
            for name in ['loaded-bios.json','effective-sync.json','effective-settings.json']:(root/name).write_text('{}')
            for frame in captured_frames(6,3,2,2):(root/f'frame-{frame:06d}.png').write_bytes(b'pixels')
        compare=lambda:compare_stock_observations(stock,observed,6,3,2,2)
        compare()
        (observed/'frame-000003.png').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError,'screenshot differs'):compare()
        (observed/'frame-000003.png').write_bytes(b'pixels')
        (observed/'frame-000005.png').write_bytes(b'extra')
        with self.assertRaisesRegex(ValueError,'inventory'):compare()
        (stock/'frame-000005.png').write_bytes(b'extra')
        compare()
        (observed/'frame-000002.png').unlink()
        with self.assertRaisesRegex(ValueError,'inventory'):compare()


class HostBudgets(unittest.TestCase):
    def start(self,root,program):
        return subprocess.Popen([sys.executable,'-c',program,str(root)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,
                                creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)

    def test_storage_budget_stops_active_process(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            p=self.start(root,"import pathlib,sys,time;pathlib.Path(sys.argv[1],'capture').write_bytes(b'x'*4096);time.sleep(10)")
            result=wait_budgeted(p,root,3,max_bytes=1024,interval=0.02)
            self.assertEqual(result['stop_reason'],'host_storage_budget')
            self.assertIsNotNone(p.poll())

    def test_terminal_storage_budget_and_timeout(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            p=self.start(root,"import pathlib,sys;pathlib.Path(sys.argv[1],'capture').write_bytes(b'x'*4096)")
            p.wait(timeout=5)
            self.assertEqual(wait_budgeted(p,root,3,max_bytes=1024)['stop_reason'],'host_storage_budget')
            p=self.start(root,'import time;time.sleep(10)')
            self.assertEqual(wait_budgeted(p,root,0.1,interval=0.02)['stop_reason'],'host_timeout')

    def test_complete_process_has_no_guest_verdict(self):
        with tempfile.TemporaryDirectory() as temp:
            p=self.start(Path(temp),'pass')
            result=wait_budgeted(p,Path(temp),3,max_bytes=1024,interval=0.02)
            self.assertEqual(result['exit_code'],0)
            self.assertIsNone(result['stop_reason'])

if __name__=='__main__': unittest.main()
