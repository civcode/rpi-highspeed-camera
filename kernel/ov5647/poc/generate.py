#!/usr/bin/env python3
"""Offline legacy-profile reconstruction; no installation or hardware access."""
import hashlib, importlib.util, json, pathlib, re
ROOT = pathlib.Path(__file__).resolve().parents[3]
OUT = ROOT/'kernel/work/ov5647-poc-module'
PROFILE = 'ov5647-poc-128-64-32-rev1-unvalidated'
# Linux's existing VBLANK_MIN=24 is preserved. Historical 135/71-line
# frame lengths are incompatible with it; we do NOT lower that safety bound.
PROFILES = [(128,349,152),(64,659,88),(32,749,64)]

def generate(stock):
    spec=importlib.util.spec_from_file_location('base',ROOT/'kernel/ov5647/prepare-experimental-module.py')
    base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)
    if hashlib.sha256(stock.encode()).hexdigest()!=base.EXPECTED_STOCK_SHA256:
        raise ValueError('stock source hash mismatch')
    result=base.patch_driver(stock) # preserve working 240 and all stock modes
    body=base.REG_LIST.search(stock).group('body')
    entries=[]; tables=[]; records=[]
    for height,legacy_fps,vts in PROFILES:
        updates={0x3036:0x69,0x3814:0x71,0x3815:0x17,
                 0x380a:height>>8,0x380b:height&255}
        extra={0x370c:0x0f,0x380c:0x07,0x380d:0x3c,
               0x380e:vts>>8,0x380f:vts&255,0x4837:0x17}
        b=body
        for addr,val in updates.items():
            pat=rf'(?mi)^(\s*\{{0x{addr:04x},\s*0x)[0-9a-f]{{2}}(\}},\s*)$'
            assert len(re.findall(pat,b))==1
            b=re.sub(pat,lambda m:m[1]+f'{val:02x}'+m[2],b)
        b=re.sub(r'(?m)^\s*\{0x0100,\s*0x01\},\s*$',
                 lambda m:''.join(f'\t{{0x{a:04x}, 0x{v:02x}}},\n' for a,v in extra.items())+m[0],b)
        name=f'ov5647_poc_640x{height}'
        tables.append(f'/* Legacy-derived POC; physical ROI NOT validated. */\nstatic struct regval_list {name}[] = {{\n{b}}};\n')
        # Retain stock full source-window metadata, not an invented height*4 ROI.
        # Legacy --top 0 is a no-op; legacy scripts leave Y_END=0x079f.
        entries.append(f'''\n\t/* POC output 640x{height}; source FOV/sampling need visual validation. */
\t{{
\t\t.format = {{ .code = MEDIA_BUS_FMT_SBGGR10_1X10,
\t\t\t.colorspace = V4L2_COLORSPACE_RAW, .field = V4L2_FIELD_NONE,
\t\t\t.width = 640, .height = {height} }},
\t\t.crop = {{ .left = 16 + OV5647_PIXEL_ARRAY_LEFT,
\t\t\t.top = OV5647_PIXEL_ARRAY_TOP, .width = 2560, .height = 1920 }},
\t\t.pixel_rate = 87500000, .link_freq_index = FREQ_INDEX_FULL,
\t\t.hts = 1852, .vts = {vts},
\t\t.reg_list = {name}, .num_regs = ARRAY_SIZE({name})
\t}},''')
        records.append(dict(width=640,height=height,legacy_fps=legacy_fps,
            legacy_vts=int(1e9/(21165*legacy_fps)),vts=vts,hts=1852,pixel_rate=87500000,
            nominal_fps=87500000/(1852*vts),status='unvalidated_candidate',
            updates={f'0x{k:04x}':f'0x{v:02x}' for k,v in (updates|extra).items()},
            source_window='unchanged stock/legacy 0x0010,0x0000..0x0a2f,0x079f; effective ROI unknown'))
    start=result.index('static const struct ov5647_mode ov5647_modes[]')
    result=result[:start]+''.join(tables)+'\n'+result[start:]
    start=result.index('static const struct ov5647_mode ov5647_modes[]')
    end=result.index('\n};',start)
    result=result[:end]+''.join(entries)+result[end:]
    result=result.replace('ov5647-640x240-rev1-unvalidated',PROFILE)
    assert result.count('.reg_list')==8
    return result,records

if __name__=='__main__':
    for record in json.loads((ROOT/'kernel/ov5647/poc/sources.json').read_text()):
        assert hashlib.sha256((ROOT/record['file']).read_bytes()).hexdigest()==record['sha256'], 'pinned research source changed'
    stock=(ROOT/'kernel/work/ov5647-6.18.50/linux-source-6.18/drivers/media/i2c/ov5647.c').read_text()
    source,records=generate(stock)
    OUT.mkdir(exist_ok=False)
    (OUT/'ov5647.c').write_text(source)
    (OUT/'Makefile').write_text('obj-m += ov5647.o\n')
    (OUT/'profiles.json').write_text(json.dumps(dict(profile=PROFILE,profiles=records),indent=2)+'\n')
    print(OUT)
