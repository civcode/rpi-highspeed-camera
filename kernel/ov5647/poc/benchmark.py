#!/usr/bin/env python3
"""Bounded fixed-mode POC. Never changes modules, clocks or registers directly."""
import argparse, hashlib, json, math, os, pathlib, statistics, subprocess, time
ROOT=pathlib.Path(__file__).resolve().parents[3]
RATES={128:[30,90,170,250,300],64:[30,90,170,300,400,500],32:[30,90,170,300,500,650,700]}
MODULE_SHA='bc88ac19568d23a54785206b5d0aa808e4198b3be24e7b4e797ce7437b35af93'

def command(args,timeout=40):
 p=subprocess.run([str(x) for x in args],capture_output=True,text=True,timeout=timeout)
 if p.returncode: raise RuntimeError(f'{args}: rc={p.returncode}\n{p.stdout}\n{p.stderr[-3000:]}')
 return p

def assess(meta,requested):
 if len(meta)<4: raise ValueError('too few frames')
 if any(m['status']!='complete' for m in meta): raise ValueError('incomplete frame')
 if any(b['sequence']!=a['sequence']+1 for a,b in zip(meta,meta[1:])): raise ValueError('sequence gap')
 # Exclude first half second (controls/startup) explicitly, retaining all metadata.
 skip=min(math.ceil(requested*.5),len(meta)//2)
 steady=meta[skip:]
 delta=[(b['sensor_timestamp_ns']-a['sensor_timestamp_ns'])/1e3 for a,b in zip(steady,steady[1:])]
 if min(delta)<=0: raise ValueError('nonmonotonic timestamps')
 fps=1e6/statistics.mean(delta)
 return dict(warmup_frames=skip,measured_frames=len(steady),fps=fps,
  min_interval_us=min(delta),max_interval_us=max(delta),median_interval_us=statistics.median(delta),
  exposure_us=steady[-1]['exposure_us'],gain=steady[-1]['analogue_gain'],
  frame_duration_us=steady[-1]['frame_duration_us'],
  timing_pass=fps>=requested*.98 and max(delta)<=1.5e6/requested)

def main():
 ap=argparse.ArgumentParser();ap.add_argument('--output',type=pathlib.Path,required=True);a=ap.parse_args()
 module=pathlib.Path('/lib/modules')/os.uname().release/'updates/hscam/ov5647.ko'
 assert hashlib.sha256(module.read_bytes()).hexdigest()==MODULE_SHA,'wrong module file'
 assert pathlib.Path('/sys/module/ov5647/srcversion').read_text().strip()==command(['modinfo','-F','srcversion',module]).stdout.strip(),'loaded module identity mismatch'
 inv=json.loads(command([ROOT/'build/hscam-inspect','--json']).stdout)
 assert len(inv['cameras'])==1
 cam=inv['cameras'][0];modes={}
 for h in RATES:
  matches=[m for m in cam['modes'] if (m['width'],m['height'],m['bit_depth'])==(640,h,10)]
  assert len(matches)==1,f'640x{h} not uniquely advertised'
  modes[h]=matches[0]['id']
 a.output.mkdir(parents=True,exist_ok=False)
 (a.output/'inventory.json').write_text(json.dumps(inv,indent=2)+'\n')
 results=[]
 def publish():
  tmp=a.output/'results.json.tmp';tmp.write_text(json.dumps(results,indent=2)+'\n');tmp.replace(a.output/'results.json')
 def capture(h,fps,seconds,label):
  prefix=a.output/f'{h}-{label}-{fps}fps';bundle=prefix.with_suffix('.hscap')
  period=round(1e6/fps);exposure=min(4000,period-300)
  args=[ROOT/'build/hscam-capture','--camera',cam['id'],'--mode',modes[h],
    '--raw','--size',f'640x{h}','--representation','uncompressed',
    '--frame-duration-us',period,'--exposure-us',exposure,'--gain',4,
    '--frames',math.ceil(fps*seconds),'--output',bundle]
  p=command(args,timeout=seconds*3+30)
  prefix.with_suffix('.log').write_text(p.stdout+'\n'+p.stderr)
  assert f'configured 640x{h} ' in p.stdout,'negotiated wrong size'
  assert 'overruns=0' in p.stdout and 'gaps=0' in p.stdout,'capture drops'
  meta=[json.loads(x) for x in (bundle/'metadata.jsonl').read_text().splitlines()]
  r=dict(height=h,requested_fps=fps,label=label,bundle=str(bundle),**assess(meta,fps))
  (prefix.with_suffix('.timing.json')).write_text(json.dumps(r,indent=2)+'\n')
  print(json.dumps(r),flush=True);results.append(r);publish()
  frame=len(meta)//2
  command([ROOT/'build/hscam-export','image',bundle,'--frame',frame,'--output',prefix.with_suffix('.png')])
  stats=command([ROOT/'build/hscam-export','stats',bundle,'--frame',frame]).stdout
  prefix.with_suffix('.stats.txt').write_text(stats)
  # Check independent payloads without changing RAW values or enhancing previews.
  idxframe=640*h*2
  with (bundle/'frames.bin').open('rb') as f:
   hashes=set()
   for n in (frame-1,frame,frame+1):
    f.seek(n*idxframe);hashes.add(hashlib.sha256(f.read(idxframe)).hexdigest())
  r['distinct_sample_frames']=len(hashes);r['image_review']='pending physical FOV/row/Bayer validation'
  publish()
  return r
 for h,rates in RATES.items():
  best=None
  for fps in rates:
   r=capture(h,fps,2,'probe')
   if not r['timing_pass']: break
   best=fps
  if best: capture(h,best,10,'soak')
  # Verify unchanged VGA mode recovers before attempting another experimental mode.
  p=command([ROOT/'build/hscam-capture','--mode','146a3eee8add7422','--raw',
     '--representation','uncompressed','--frame-duration-us',33333,'--frames',12,'--discard-payload'])
  (a.output/f'{h}-recovery.log').write_text(p.stdout+'\n'+p.stderr)
 print('POC complete: timings only; inspect exported PNGs for geometry.',flush=True)
if __name__=='__main__': main()
