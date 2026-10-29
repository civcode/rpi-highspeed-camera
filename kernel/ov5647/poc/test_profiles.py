import importlib.util, pathlib, re, unittest
p=pathlib.Path(__file__).with_name('generate.py')
spec=importlib.util.spec_from_file_location('poc',p);poc=importlib.util.module_from_spec(spec);spec.loader.exec_module(poc)
class Profiles(unittest.TestCase):
 def test_reconstruction(self):
  stock=(poc.ROOT/'kernel/work/ov5647-6.18.50/linux-source-6.18/drivers/media/i2c/ov5647.c').read_text()
  result,records=poc.generate(stock)
  for r in records:
   h=r['height']; name=f'ov5647_poc_640x{h}'
   body=result.split(f'{name}[] = {{',1)[1].split('};',1)[0]
   regs={int(a,16):int(b,16) for a,b in re.findall(r'\{0x([0-9a-f]+), 0x([0-9a-f]+)\}',body)}
   self.assertEqual((regs[0x380a]<<8)|regs[0x380b],h)
   self.assertEqual((regs[0x380e]<<8)|regs[0x380f],r['vts'])
   self.assertGreaterEqual(r['vts']-h,24)
   self.assertEqual(regs[0x3815],0x17)
   self.assertEqual((regs[0x3806]<<8)|regs[0x3807],0x079f)
   self.assertEqual(regs[0x3036],0x69)
  # Four original stock mode entries and register tables survive verbatim.
  for name in ('2592x1944','1080p30','2x2binned','640x480'):
   pat=rf'static struct regval_list ov5647_{name}_10bpp\[\] = \{{.*?\n\}};'
   original=re.search(pat,stock,re.S).group()
   self.assertIn(original,result)
  self.assertEqual(result.count('.reg_list'),8)
 def test_bad_source(self):
  with self.assertRaisesRegex(ValueError,'hash'): poc.generate('wrong')
if __name__=='__main__': unittest.main()
