#!/usr/bin/env python3
"""Build native LVGL and test pager/slider touch behavior; requires clang and ar."""
from pathlib import Path
import subprocess,concurrent.futures
root=Path(__file__).resolve().parents[1]
src=root/'vendor/waveshare-2.16/examples/arduino/libraries/lvgl/src'
import tempfile
out=Path(tempfile.mkdtemp(prefix='hud-lvgl-test-'))
def build(p):
 o=out/(str(p.relative_to(src)).replace('/','_')+'.o')
 subprocess.run(['clang','-w','-O0','-DLV_CONF_PATH='+str(root/'firmware/hud/lv_conf.h'),'-c',str(p),'-o',str(o)],check=True,capture_output=True)
 return str(o)
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool: objs=list(pool.map(build,src.rglob('*.c')))
subprocess.run(['ar','rcs',str(out/'liblvgl.a'),*objs],check=True)
print('LVGL native test library ready')

subprocess.run(['clang','-DLV_CONF_PATH='+str(root/'firmware/hud/lv_conf.h'),'-I'+str(src.parent),str(root/'tests/pager-gestures.c'),str(out/'misc_lv_gc.c.o'),str(out/'liblvgl.a'),'-o',str(out/'pager-test')],check=True)
subprocess.run([str(out/'pager-test')],check=True)
import shutil
shutil.rmtree(out)
