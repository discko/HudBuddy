#!/usr/bin/env python3
"""Convert approved six-frame art to embedded RGB565 at display size (Pillow)."""
from pathlib import Path
from PIL import Image, ImageOps
root=Path(__file__).resolve().parents[1]
out=root/'firmware/libraries/HudBuddyScene/src'
(out/'DogFrames.h').write_text('#pragma once\n#include <stdint.h>\nnamespace DogFrames {\nconstexpr int width=200,height=170,count=6;\nextern const uint16_t pixels[2][6][200*170];\n}\n')
with (out/'DogFrames.cpp').open('w') as f:
 f.write('// Generated from assets/puppy-v2 by scripts/build-dog-frames.py\n#include "DogFrames.h"\nnamespace DogFrames {\nconst uint16_t pixels[2][6][200*170]={\n')
 for mirrored in (False,True):
  f.write('{\n')
  for i in range(6):
   im=Image.open(root/f'assets/puppy-v2/run-{i}.png').convert('RGB').resize((200,170),Image.Resampling.LANCZOS)
   if mirrored: im=ImageOps.mirror(im)
   values=[]
   for r,g,b in im.getdata():
    if max(r,g,b)<12:r=g=b=0
    values.append(((r>>3)<<11)|((g>>2)<<5)|(b>>3))
   f.write('{\n')
   for start in range(0,len(values),24):f.write(','.join(map(str,values[start:start+24]))+',\n')
   f.write('},\n')
  f.write('},\n')
 f.write('};\n}\n')
print('12 RGB565 frames: 816000 bytes, including mirrored run')
