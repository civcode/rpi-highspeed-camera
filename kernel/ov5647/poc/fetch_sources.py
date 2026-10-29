"""Fetch only checksum-pinned research files; no installs or overwrites."""
import hashlib,json,pathlib,urllib.request
root=pathlib.Path(__file__).resolve().parents[3]
for r in json.loads((root/'kernel/ov5647/poc/sources.json').read_text()):
 p=root/r['file']
 data=p.read_bytes() if p.exists() else urllib.request.urlopen(r['url'],timeout=30).read()
 assert hashlib.sha256(data).hexdigest()==r['sha256'],r['url']
 if not p.exists():
  p.parent.mkdir(parents=True,exist_ok=True)
  with p.open('xb') as f: f.write(data)
 print(p)
