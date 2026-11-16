import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import backend
from batch_state import BatchState
from selection import GameJob
from settings import Settings
from test_backend import synthetic_disc


class BatchTests(unittest.TestCase):
    def test_monotonic_game_and_total_freeze(self):
        tick=[10.0];events=[]
        jobs=[GameJob('First',[]),GameJob('Second',[])]
        state=BatchState(jobs,events.append,lambda:tick[0])
        jobs.clear()  # Build owns its snapshot.
        tick[0]=14;state.start(0)
        tick[0]=19;state.end('completed')
        tick[0]=22;state.start(1)
        tick[0]=29;state.end('failed',error='test failure')
        tick[0]=30;record=state.finish('failed')
        tick[0]=999
        self.assertEqual(state.total(),20)
        self.assertEqual([g['elapsed_seconds'] for g in record['games']],[5,7])
        self.assertEqual([g['state'] for g in record['games']],['completed','failed'])
        self.assertEqual(events[-1]['total_seconds'],20)

    def test_failure_continues_and_keeps_stable_rows(self):
        self.run_batch(False)

    def test_cancel_prevents_later_jobs_and_preserves_queued_rows(self):
        self.run_batch(True)

    def run_batch(self,cancel):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);cue=synthetic_disc(root)
            jobs=[GameJob(str(n),[cue]) for n in range(3)]
            prefs=Settings(output_folder=str(root/'output'),skip_bios=True)
            events=[];runner=backend.Runner(lambda line:None,events.append);calls=[]
            def snapshot(kind,ref,custom,cache,destination,runner):
                destination.mkdir();return {'kind':kind}
            def build(options,data,active,**kwargs):
                calls.append(options.output)
                if len(calls)==2:
                    if cancel:
                        active.cancel();raise backend.Cancelled('cancel')
                    raise ValueError('one game failed')
                payload=Path(options.output)/'build-release';payload.mkdir(parents=True);(payload/'Game.exe').write_bytes(b'fixture program')
                return {'output':str(payload)}
            with mock.patch.object(backend,'snapshot_source',side_effect=snapshot),mock.patch('buildtools.prepare',return_value={'receipt':{}}),mock.patch.object(backend,'build',side_effect=build):
                if cancel:
                    with self.assertRaises(backend.Cancelled):backend.build_batch(jobs,prefs,root/'data',runner)
                else:backend.build_batch(jobs,prefs,root/'data',runner)
            receipt=json.loads(next((root/'data/history').glob('batch-*/batch-record.json')).read_text())
            self.assertEqual([g['state'] for g in receipt['games']],['completed','cancelled','queued'] if cancel else ['completed','failed','completed'])
            self.assertEqual(len(calls),2 if cancel else 3)
            self.assertIn('elapsed_seconds',receipt)
            self.assertEqual([g['id'] for g in receipt['games']],[0,1,2])
            self.assertEqual(events[-1]['event'],'batch_finished')
            self.assertTrue(all(g['elapsed_seconds']>=0 for g in receipt['games']))

if __name__=='__main__':unittest.main(verbosity=2)
