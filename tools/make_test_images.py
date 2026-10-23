#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Deterministic image fixtures, standard library only; outputs under --out."""
import argparse
import binascii
from pathlib import Path
import shutil
import struct
import subprocess
import zlib

COLORS = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255)]

def png(path, w=16, h=16, alpha=False, checker=False):
    def chunk(tag, data):
        return struct.pack('>I', len(data))+tag+data+struct.pack('>I', binascii.crc32(tag+data)&0xffffffff)
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            rgb = ((255,255,255) if (x+y)%2 else (0,0,0)) if checker else COLORS[(y>=h//2)*2+(x>=w//2)]
            raw.extend(rgb)
            if alpha: raw.append(128 if x<w//2 else 0)
    path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR', struct.pack('>IIBBBBB', w,h,8,6 if alpha else 2,0,0,0))+chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b''))

def bmp(path, bits=24, top=False, rle=False):
    w=h=16
    palette=b''.join(bytes((b,g,r,0)) for r,g,b in COLORS)
    if bits==1: palette=b'\x00\x00\xff\x00\x00\xff\x00\x00'
    if bits==4: palette+=bytes(12*4)
    if bits==8: palette+=bytes(252*4)
    if bits>=24: palette=b''
    rows=[]
    for yy in range(h):
        y=yy if top else h-1-yy
        indices=[(y>=8)*2+(x>=8) for x in range(w)]
        if bits==1: indices=[int(x>=8) for x in range(w)]
        if rle:
            if bits==8: row=bytes((8,indices[0],8,indices[8],0,0))
            else: row=bytes((8,indices[0]*17,8,indices[8]*17,0,0))
        elif bits==1: row=bytes((0,255))
        elif bits==4: row=bytes(indices[i]*16+indices[i+1] for i in range(0,w,2))
        elif bits==8: row=bytes(indices)
        else: row=b''.join(bytes((COLORS[i][2],COLORS[i][1],COLORS[i][0]))+(b'\x00' if bits==32 else b'') for i in indices)
        rows.append(row if rle else row+bytes((-len(row))%4))
    data=b''.join(rows)+(b'\x00\x01' if rle else b'')
    off=54+len(palette)
    dib=struct.pack('<IiiHHIIiiII',40,w,-h if top else h,1,bits,(1 if bits==8 else 2) if rle else 0,len(data),0,0,0,0)
    path.write_bytes(b'BM'+struct.pack('<IHHI',off+len(data),0,0,off)+dib+palette+data)

def gif(path, animated=False):
    # A clear before every pixel keeps a fixed 3-bit code width, valid GIF LZW.
    data=bytearray(b'GIF89a'+struct.pack('<HHBBB',16,16,0x81,0,0)+b''.join(bytes(c) for c in COLORS))
    if animated: data.extend(b'\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00')
    for frame in range(2 if animated else 1):
        data.extend(b'\x21\xf9\x04\x04\x0a\x00\x00\x00')
        data.extend(b','+struct.pack('<HHHHB',0,0,16,16,0)+b'\x02')
        codes=[]
        for y in range(16):
            for x in range(16): codes.extend((4, ((y>=8)*2+(x>=8)+frame)%4))
        codes.append(5)
        acc=nb=0; compressed=bytearray()
        for code in codes:
            acc|=code<<nb; nb+=3
            while nb>=8: compressed.append(acc&255); acc>>=8; nb-=8
        if nb: compressed.append(acc&255)
        for i in range(0,len(compressed),255):
            part=compressed[i:i+255]; data.append(len(part)); data.extend(part)
        data.append(0)
    data.append(0x3b); path.write_bytes(data)

def quadrant_indices(w=16, h=16):
    return [[(y>=h//2)*2+(x>=w//2) for x in range(w)] for y in range(h)]

def pattern_indices(w=32, h=44):
    # Runs, stripes, a diagonal and a flat area at the bottom: in the flat
    # area every MAG/Pi copy position matches, and the writers rotate among
    # the matching ones, so each position code is used.
    rows=[]
    for y in range(h):
        row=[]
        for x in range(w):
            if y<4: c=(x//4)%4
            elif y<8: c=(x+y)%5+4
            elif y<16: c=9 if (x//2+y//2)%2 else 10
            elif y<24: c=11+((x*7+y*3)%5)
            else: c=2
            row.append(c)
        rows.append(row)
    return rows

class Bits:
    def __init__(self): self.out=bytearray(); self.acc=0; self.n=0
    def put(self, value, count):
        for i in range(count-1, -1, -1):
            self.acc=self.acc<<1|((value>>i)&1); self.n+=1
            if self.n==8: self.out.append(self.acc); self.acc=self.n=0
    def data(self):
        return bytes(self.out)+(bytes((self.acc<<(8-self.n),)) if self.n else b'')

MAG_DX=[0,1,2,4,0,1,0,1,2,0,1,2,0,1,2,0]
MAG_DY=[0,0,0,0,1,1,2,2,2,4,4,4,8,8,8,16]

def mag(path, colors=16, double=False, indices=None, palette=None, left=0):
    """MAKI02 writer with real flag A/B and copy codes (offsets from header)."""
    indices=indices or quadrant_indices(); h=len(indices); w=len(indices[0])
    palette=(palette or COLORS)+[(0,0,0)]*(colors-len(palette or COLORS))
    align=4 if colors==256 else 8
    x0=left; x1=left+w-1; base=x0//align*align; padded=(x1//align*align+align)-base
    ppw=2 if colors==256 else 4; words=padded//ppw; nflags=words//2
    units=[]
    for row in indices:
        full=[0]*(x0-base)+row+[0]*(padded-(x0-base)-w)
        packed=bytes(full) if colors==256 else bytes(full[x]*16+full[x+1] for x in range(0,padded,2))
        units.extend(packed[i:i+2] for i in range(0,len(packed),2))
    flag_a=Bits(); flag_b=bytearray(); pixels=bytearray(); previous=[0]*nflags
    for y in range(h):
        ops=[]
        for x in range(words):
            cur=y*words+x
            match=[k for k in range(1,16) if cur-MAG_DY[k]*words-MAG_DX[k]>=0 and units[cur-MAG_DY[k]*words-MAG_DX[k]]==units[cur]]
            op=match[cur%len(match)] if match else 0
            if not op: pixels.extend(units[cur])
            ops.append(op)
        for j in range(nflags):
            flag=ops[2*j]<<4|ops[2*j+1]; diff=flag^previous[j]; previous[j]=flag
            flag_a.put(1 if diff else 0, 1)
            if diff: flag_b.append(diff)
    fa=flag_a.data(); aoff=32+colors*3; boff=aoff+len(fa); poff=boff+len(flag_b)
    mode=(128 if colors==256 else 0)+(1 if double else 0)
    header=struct.pack('<4B4H5I',0,0,0,mode,x0,0,x1,h-1,aoff,boff,len(flag_b),poff,len(pixels))
    path.write_bytes(b'MAKI02  FDTP    fixture\x1a'+header+b''.join(bytes((g,r,b)) for r,g,b in palette)+fa+bytes(flag_b)+bytes(pixels))

def pi(path, indices=None, palette=None, aspect=(0,0), default_palette=False):
    """Pi writer: position codes with copy lengths, else colors from the
    move-to-front tables (decoder: src/img_legacy.c)."""
    indices=indices or quadrant_indices(); h=len(indices); w=len(indices[0])
    palette=(palette or COLORS)+[(0,0,0)]*(16-len(palette or COLORS))
    table=[[(i-j)&15 for j in range(16)] for i in range(16)]
    out=Bits()
    def color(left, c):
        i=table[left].index(c); table[left].insert(0, table[left].pop(i))
        if i<2: out.put(2|i,2)
        elif i<4: out.put(i-2,3)
        elif i<8: out.put(0b01000|(i-4),5)
        else: out.put(0b011000|(i-8),6)
    image=[c for row in indices for c in row]
    c1,c2=image[0],image[1]; color(0,c1); color(c1,c2)
    buf=[c1 if p%2==0 else c2 for p in range(2*w)]+image
    end=len(buf)
    def distance(pos,p):
        if pos==0: return 4 if buf[p-1]==buf[p-2] else 2
        return (w, 2*w, w-1, w+1)[pos-1]
    def best(p, prev):
        found=[]
        for pos in range(5):
            if pos==prev: continue
            d=distance(pos,p); n=0
            while p+2*n<end and all(buf[q]==buf[q-d] for q in range(p+2*n,min(p+2*n+2,end))): n+=1
            if n: found.append((n,pos))
        if not found: return None
        longest=max(n for n,_ in found); ties=[pos for n,pos in found if n==longest]
        # Equal runs: rotate so every position code is written somewhere.
        return ties[(p//2)%len(ties)],longest
    p=2*w; prev=-1
    while p<end:
        found=best(p,prev)
        if found:
            pos,n=found
            out.put((0,1,2,6,7)[pos],2 if pos<3 else 3)
            bits=n.bit_length()-1; out.put((1<<(bits+1))-2,bits+1); out.put(n-(1<<bits),bits)
            p+=2*n; prev=pos; continue
        out.put((0,1,2,6,7)[prev],2 if prev<3 else 3)
        while True:
            color(buf[p-1],buf[p]); color(buf[p],buf[p+1]); p+=2
            if p>=end: break
            more=best(p,-1) is None
            out.put(1 if more else 0,1)
            if not more: break
        prev=-1
    mode=0x80 if default_palette else 0
    head=b'Pi'+b'FDTP fixture\x1a'+b'\x00'+bytes((mode,aspect[0],aspect[1],4))+b'FDTP'+struct.pack('>H',2)+b'XX'+struct.pack('>HH',w,h)
    path.write_bytes(head+(b'' if default_palette else b''.join(bytes(c) for c in palette))+out.data()+b'\x00\x00')

def generate(out):
    out.mkdir(parents=True,exist_ok=True)
    mag(out/'mag16.mag'); mag(out/'mag256.mag',256); mag(out/'double.mag',double=True)
    pi(out/'quadrants.pi'); pi(out/'double.pi',aspect=(1,2))
    pattern=pattern_indices(); colors16=[((i*37)%256,(i*91)%256,(i*53)%256) for i in range(16)]
    mag(out/'pattern16.mag',indices=pattern,palette=colors16,left=4)
    mag(out/'pattern256.mag',256,indices=pattern,palette=colors16,left=2)
    pi(out/'pattern.pi',indices=pattern,palette=colors16)
    png(out/'quadrants.png'); png(out/'alpha.png',alpha=True)
    png(out/'checker.png',checker=True); png(out/'large.png',2048,1024)
    for bits in (1,4,8,24,32): bmp(out/f'bmp{bits}.bmp',bits)
    bmp(out/'topdown.bmp',24,True)
    for bits in (4,8): bmp(out/f'rle{bits}.bmp',bits,rle=True)
    gif(out/'quadrants.gif'); gif(out/'animated.gif',True)
    sips=shutil.which('sips')
    if sips:
        for kind in ('jpeg','webp'):
            dest=out/('quadrants.'+('jpg' if kind=='jpeg' else 'webp'))
            result=subprocess.run([sips,'-s','format',kind,str(out/'quadrants.png'),'--out',str(dest)],capture_output=True)
            if result.returncode: dest.unlink(missing_ok=True); print(f'note: sips cannot write {kind}; tests/imageview_harness.c --make-modern writes it')
    else: print('SKIP JPEG/WebP: sips unavailable')
    if (out/'quadrants.jpg').exists():
        original=(out/'quadrants.jpg').read_bytes()
        for orientation in range(1,9):
            tiff=b'II\x2a\x00\x08\x00\x00\x00'+struct.pack('<H',1)+struct.pack('<HHI',0x112,3,1)+struct.pack('<H',orientation)+b'\x00\x00'+b'\x00'*4
            exif=b'Exif\x00\x00'+tiff
            (out/f'exif{orientation}.jpg').write_bytes(original[:2]+b'\xff\xe1'+struct.pack('>H',len(exif)+2)+exif+original[2:])
    (out/'random.png').write_bytes(bytes(range(256))*4)

if __name__=='__main__':
    parser=argparse.ArgumentParser(); parser.add_argument('--out',type=Path,required=True)
    generate(parser.parse_args().out)
