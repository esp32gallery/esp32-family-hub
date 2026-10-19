#!/usr/bin/env python3
"""Rasterize native 32/40 px Noto CJK glyphs as 4-bit coverage, with no artificial bolding."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
import re
ROOT = Path(__file__).parent
SOURCE = (ROOT / 'family-hub.ino').read_text()
LITERALS = ''.join(re.findall(r'"([^"\\]*(?:\\.[^"\\]*)*)"', SOURCE))
CHARS = ''.join(sorted({c for c in LITERALS if '\u4e00' <= c <= '\u9fff'} | {chr(i) for i in range(32,127)}))
FONT = Path.home() / 'Library/Fonts/NotoSansCJK.ttc'
lines = ['// Generated native-size Noto Sans CJK, 4-bit anti-aliased coverage.',
         '#pragma once', '#include <Arduino.h>',
         'static const uint32_t ZH_CODES[] = {' + ','.join(hex(ord(c)) for c in CHARS) + '};']
samples=[]
for size in (32,40):
    font=ImageFont.truetype(str(FONT),size-2,index=17)
    assert font.getname() == ('Noto Sans CJK SC', 'Medium'), font.getname()
    lines.append(f'static const uint8_t ASCII_ADV_{size}[95] = {{' + ','.join(str(round(font.getlength(chr(i)))+2) for i in range(32,127)) + '};')
    lines.append(f'static const uint8_t ZH_ALPHA_{size}[][{size*size//2}] PROGMEM = {{')
    glyphs={}
    for c in CHARS:
        im=Image.new('L',(size,size)); d=ImageDraw.Draw(im)
        box=d.textbbox((0,0),c,font=font)
        d.text(((size-box[2]+box[0])//2-box[0] if ord(c)>=128 else 1-box[0],(size-box[3]+box[1])//2-box[1]),c,font=font,fill=255)
        vals=[(p*15+127)//255 for p in im.getdata()]
        lines.append(' {'+','.join(str((vals[i]<<4)|vals[i+1]) for i in range(0,len(vals),2))+f'}}, // U+{ord(c):04X}')
        glyphs[c]=im
    lines.append('};')
    lines.append(f'static_assert(sizeof(ZH_ALPHA_{size}) / sizeof(ZH_ALPHA_{size}[0]) == sizeof(ZH_CODES) / sizeof(ZH_CODES[0]), "Glyph table count mismatch");')
    for text in ('Family Hub','Hold to talk','Recording','Tap to replay','Brightness Volume 68%' ):
        sample=Image.new('RGB',(480,64),'#fff4d8')
        for i,c in enumerate(text):
            if c in glyphs: sample.paste('#243028',(20+i*size,12),glyphs[c])
        samples.append(sample)
(ROOT/'zh_font.h').write_text('\n'.join(lines)+'\n')
preview=Image.new('RGB',(480,len(samples)*64),'white')
for i,im in enumerate(samples): preview.paste(im,(0,i*64))
preview.save(ROOT.parent/'preview/font-native.png')
print(f'{len(CHARS)} glyphs, native 32/40 px, {len(CHARS)*(32*32+40*40)//2} bytes')
