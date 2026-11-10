import copy
import tempfile
import unittest
from pathlib import Path
import run_native as native

class RelocationTests(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup);self.root=Path(self.tmp.name)
  self.discs=[]
  for i in (1,2):
   (self.root/f'disc{i}.bin').write_bytes(bytes([i])*2352)
   p=self.root/f'disc {i}.cue';p.write_text(f'FILE "disc{i}.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n');self.discs.append(p.resolve())
  self.game=self.root/'game.toml';self.game.write_text('[game]\ndiscs = ["missing/old1.cue", "missing/old2.cue"]\n')
  self.route={'sha256':'a'*64,'disc_set':[{'kind':1,'digest':native.checkpoint_asset_digest(p)} for p in self.discs]}
 def roster(self,**kw):
  return native.disc_roster(self.route,self.discs[0],self.discs,self.game,**kw)
 def test_default_rejects_relocated_paths(self):
  with self.assertRaises(ValueError):self.roster()
 def test_explicit_compatible_resume_accepts_content(self):
  before=self.game.read_bytes();entries=self.roster(relocate=True,resume=True,compatible=True)
  text=native.disc_relocation_text(native.digest(self.game),self.route['sha256'],entries)
  self.assertTrue(text.startswith('PSX-ROSTER-RELOCATION-1\n'));self.assertEqual(before,self.game.read_bytes());self.assertEqual(len(entries),2)
 def test_requires_both_resume_selectors(self):
  for resume,compatible in ((False,False),(True,False),(False,True)):
   with self.assertRaises(ValueError):self.roster(relocate=True,resume=resume,compatible=compatible)
 def test_changed_content_rejected(self):
  (self.root/'disc2.bin').write_bytes(b'changed')
  with self.assertRaises(ValueError):self.roster(relocate=True,resume=True,compatible=True)
 def test_wrong_order_rejected(self):
  with self.assertRaises(ValueError):native.disc_roster(self.route,self.discs[1],self.discs[::-1],self.game,True,True,True)
 def test_wrong_boot_rejected(self):
  with self.assertRaises(ValueError):native.disc_roster(self.route,self.discs[1],self.discs,self.game,True,True,True)
 def test_missing_disc_rejected(self):
  with self.assertRaises(ValueError):native.disc_roster(self.route,self.discs[0],self.discs[:1],self.game,True,True,True)
 def test_mapping_bounds_duplicates_and_identity(self):
  entries=self.roster(relocate=True,resume=True,compatible=True)
  for key,value in [('path',str(self.discs[0])),('original_path',entries[0]['original_path']),('index',1),('digest','bad')]:
   bad=copy.deepcopy(entries);bad[1][key]=value
   with self.assertRaises(ValueError):native.disc_relocation_text('a'*64,'b'*64,bad)
  for invalid in ('x'*4096,'bad\tpath','bad\0path','bad\npath'):
   bad=copy.deepcopy(entries);bad[0]['original_path']=invalid
   with self.assertRaises(ValueError):native.disc_relocation_text('a'*64,'b'*64,bad)
  with self.assertRaises(ValueError):native.disc_relocation_text('bad','b'*64,entries)
  with self.assertRaises(ValueError):native.disc_relocation_text('a'*64,'b'*64,entries*5)
if __name__=='__main__':unittest.main()