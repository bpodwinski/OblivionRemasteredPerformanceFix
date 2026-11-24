import pathlib
import tempfile
import unittest
import ot_wait_profile as p


class WaitProfileTests(unittest.TestCase):
    def test_frame_work_windows_and_cohorts(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            ledger = root/'ledger.csv'
            ledger.write_text('# OblivionPerfFix-ledger v1, threshold_ms=5.0, signals=tg:1,pacing:1,gc:0\n'
                              '#F,frame,end_ms,period_ms,sleep_ms,pair_ms,tg3_ms,grid_ms,state\n'
                              'F,1,110,10,2,0.4,0.6,0,play\n'
                              'F,2,120,10,0,3.2,1.5,0,play\n'
                              'F,3,130,10,0,0,0,0,menu\n', encoding='utf-8')
            prefix = root/'scene'
            (root/'scene-stacks.txt').write_text('OblivionThread;exe+0x10\nOblivionThread;exe+0x20\n')
            (root/'scene-timeline.csv').write_text('QpcMs,Thread,Stack\n102,OblivionThread,0\n'
                                                 '109,OblivionThread,1\n110,OblivionThread,1\n'
                                                 '119,OblivionThread,1\n120,OblivionThread,0\n')
            frames = p.load_frames(ledger,100,130)
            samples = p.load_samples(prefix)
            report = p.compare(frames,samples,0.5,2,8)
            low, high = [report[k]['threads']['OblivionThread'] for k in ('low','high')]
            self.assertEqual(low['stacks'], {'exe+0x10':1})
            self.assertEqual(high['stacks'], {'exe+0x20':2})
            self.assertEqual(report['low']['game_ms'],8)
            self.assertEqual(report['high']['physics_ms'],1.5)
            self.assertIn('+100.0',p.render(report,frames,0.5,2,20))
            self.assertIn('Insufficient',p.render(report,frames,0.5,2,20))
            with self.assertRaises(ValueError):
                p.load_frames(ledger,200,300)
            ledger.write_text(ledger.read_text().replace('threshold_ms=5.0','threshold_ms=17.0'))
            with self.assertRaises(ValueError):
                p.load_frames(ledger,100,130)
            (root/'scene-timeline.csv').write_text('QpcMs,Thread,Stack\n101,OblivionThread,99\n')
            with self.assertRaises(ValueError):
                p.load_samples(prefix)

    def test_missing_cohort(self):
        frames = [(100,110,110,3,1,0,1)]
        report = p.compare(frames,{'OblivionThread':[(101,'OblivionThread;exe+0x20')]},0.5,2,8)
        text = p.render(report,frames,0.5,2,20)
        self.assertIn('not comparable',text)
        self.assertIn('high: 1 samples',text)
        self.assertNotIn('+100.0',text)

    def test_mixed_sessions_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            ledger = pathlib.Path(tmp)/'ledger.csv'
            header = '# OblivionPerfFix-ledger v1, pid={}, threshold_ms=5.0, signals=tg:1,pacing:1,gc:0\n#F,frame,end_ms,period_ms,sleep_ms,pair_ms,tg3_ms,grid_ms,state\n'
            ledger.write_text(header.format(1)+'F,1,110,10,0,3,1,0,play\n'+header.format(2)+'F,1,120,10,0,3,1,0,play\n')
            with self.assertRaises(ValueError):
                p.load_frames(ledger,100,120)


if __name__ == '__main__':
    unittest.main()
