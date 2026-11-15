#!/usr/bin/env python3
"""Render firmware-generated PPM images as a contact sheet/GIF (requires Pillow).
First: bash tests/host/run_display.sh /tmp/cmcar-display-preview
Then:  python3 tools/display_preview.py /tmp/cmcar-display-preview
"""
from pathlib import Path
import argparse
from PIL import Image, ImageDraw

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('directory',type=Path)
p=parser.parse_args().directory
names=['calm','happy','curious','comforting','sad']
out=Image.new('RGB',(1200,290),'#10151f')
draw=ImageDraw.Draw(out)
for i,name in enumerate(names):
    im=Image.open(p/f'{name}.ppm').convert('RGB')
    mask=Image.new('L',(240,240))
    ImageDraw.Draw(mask).ellipse((1,1,239,239),fill=255)
    out.paste(im,(i*240,0),mask)
    draw.text((i*240+85,258),name,fill='#c1d3e3')
out.save(p/'moods.png')
frames=[Image.open(f).convert('RGB') for f in sorted(p.glob('frame-*.ppm'))]
if frames:
    frames[0].save(p/'animation.gif',save_all=True,append_images=frames[1:],duration=50,loop=0)
print(p/'moods.png')
print(p/'animation.gif')
