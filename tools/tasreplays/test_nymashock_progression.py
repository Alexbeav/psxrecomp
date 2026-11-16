from pathlib import Path
import json,tempfile,unittest
import dualshock_route
import nymashock_progression as route

class ProgressionContract(unittest.TestCase):
    def test_controller_boundaries(self):
        self.assertEqual(route.controller_command(12,['start','cross'],[0,128,129,255]),'12 49143 0 128 129 255\n')
        for frames,buttons,axes in [(0,[],[128]*4),(12001,[],[128]*4),(True,[],[128]*4),
                (1,['analog'],[128]*4),(1,['cross','cross'],[128]*4),(1,[],[-1,0,0,0]),
                (1,[],[0,0,0,256]),(1,[],[True,0,0,0]),(1,[],[128]*3)]:
            with self.subTest(frames=frames,buttons=buttons,axes=axes),self.assertRaises(ValueError):
                route.controller_command(frames,buttons,axes)

    def test_queue_cannot_replace_pending_or_completed_input(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);(root/'commands').mkdir()
            (root/'ready.json').write_text(json.dumps({'completed_step':0,'frame':300}))
            route.queue(root,60,[],[128]*4)
            original=(root/'commands/000001.txt').read_bytes()
            with self.assertRaises(ValueError):route.queue(root,20,['cross'],[128]*4)
            self.assertEqual((root/'commands/000001.txt').read_bytes(),original)
            (root/'ready.json').write_text(json.dumps({'completed_step':1,'frame':360}))
            route.queue(root,0,[],[],finish=True)
            self.assertEqual((root/'commands/000002.txt').read_bytes(),b'finish\n')
            (root/'complete.json').write_text('{}')
            with self.assertRaises(ValueError):route.queue(root,1,[],[128]*4)

    def test_observed_route_continuity_and_export(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);p=root/'controller.tsv'
            header='frame\tbuttons\tly\tlx\try\trx\tanalog\n'
            good=header+'1\t65535\t0\t128\t129\t255\t0\n2\t49143\t128\t128\t128\t128\t0\n'
            p.write_text(good);rows=route.read_controller(p,2)
            self.assertEqual(rows,[(65535,0,128,129,255,0),(49143,128,128,128,128,0)])
            identity=dualshock_route.write_route(rows,root/'input.psxrti2')
            self.assertEqual(identity['frames'],2)
            for bad in [good.replace('\n2\t','\n3\t'),good.replace('\t255\t0','\t255\t1'),header,good+'3 1\n']:
                p.write_text(bad)
                with self.assertRaises(ValueError):route.read_controller(p,2)

    def test_raw_card_shape_and_read_only_identity(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'card.mcd';data=b'MC'+bytes(131070);p.write_bytes(data)
            identity=route.card(p);self.assertEqual(len(identity['sha256']),64)
            self.assertEqual(p.read_bytes(),data)
            for bad in [data[:-1],bytes(131072),data+bytes(1)]:
                p.write_bytes(bad)
                with self.assertRaises(ValueError):route.card(p)

if __name__=='__main__':unittest.main()
