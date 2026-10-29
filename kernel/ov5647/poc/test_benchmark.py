import importlib.util,pathlib,unittest
s=importlib.util.spec_from_file_location('bench',pathlib.Path(__file__).with_name('benchmark.py'))
b=importlib.util.module_from_spec(s);s.loader.exec_module(b)
class Benchmark(unittest.TestCase):
 def frames(self):
  return [dict(sequence=i,status='complete',sensor_timestamp_ns=i*2000000,
    exposure_us=1500,analogue_gain=4,frame_duration_us=2000) for i in range(1000)]
 def test_stable(self):
  r=b.assess(self.frames(),500)
  self.assertEqual(r['fps'],500);self.assertTrue(r['timing_pass'])
 def test_gap(self):
  m=self.frames();m[300]['sequence']+=1
  with self.assertRaisesRegex(ValueError,'sequence'): b.assess(m,500)
 def test_nonmonotonic(self):
  m=self.frames();m[300]['sensor_timestamp_ns']=m[299]['sensor_timestamp_ns']
  with self.assertRaisesRegex(ValueError,'nonmonotonic'): b.assess(m,500)
 def test_stall(self):
  m=self.frames()
  for x in m[300:]: x['sensor_timestamp_ns']+=40000000
  self.assertFalse(b.assess(m,500)['timing_pass'])
 def test_startup_is_retained_but_not_measured(self):
  m=self.frames();m[0]['sensor_timestamp_ns']-=40000000
  r=b.assess(m,500);self.assertTrue(r['timing_pass']);self.assertEqual(r['warmup_frames'],250)
if __name__=='__main__':unittest.main()
