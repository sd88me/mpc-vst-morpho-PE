#!/usr/bin/env python3
"""Generates vst/images/wordmark.svg:  tools/wordmark.py OUT.svg soft "#1f6bff" 1.4   (development tool; the skin uses the SVG it wrote)."""
import sys
MODE=sys.argv[2] if len(sys.argv)>2 else 'soft'
BLUE=sys.argv[3] if len(sys.argv)>3 else None
OUTW=float(sys.argv[4]) if len(sys.argv)>4 else 3.0
# letters in a unit box (x-height 1, baseline y=0, up is negative), monoline; each returns (svg path/shape strings, width)
def m(): return ['M0,0 V-1', 'M0,-.62 A.3,.3 0 0 1 .6,-.62 V0', 'M.6,-.62 A.3,.3 0 0 1 1.2,-.62 V0'], 1.2
def o(): return ['<c>.5,-.5,.5'], 1.0
def o2(): return ['<c>.5,-.5,.5','<c>.72,-.5,.5'], 1.24
def r(): return (['M0,0 V-1', 'M0,-.5 A.5,.5 0 0 1 .5,-1'], .8) if MODE=='geo' else (['M0,0 V-1', 'M0,-.6 C0,-.95 .45,-1.05 .75,-.9'], .8)
def p(): return ['M0,.42 V-1', '<c>.55,-.5,.5'], 1.1
def h(): return ['M0,.0 V-1.3', 'M0,-.62 A.3,.3 0 0 1 .6,-.62 V0'], .7
def load(name):
    return {'m':m,'o':o,'r':r,'p':p,'h':h,'O':o2}[name]()
word=[('m',.78),('o',.86),('r',.95),('p',1.04),('h',1.12),('O',1.34)]
x=12; out=[]; sw0=.2
base0=100; 
for i,(ch,sc) in enumerate(word):
    parts,w=load(ch); U=50*sc; sw=max(8.0, 0.24*U*0.85) if MODE=='soft' else max(7.5, 0.19*U*0.9)
    base=base0-i*2.5
    g=[]
    for pt in parts:
        if pt.startswith('<c>'):
            cx,cy,rr=map(float,pt[3:].split(','))
            g.append('<circle cx="%.1f" cy="%.1f" r="%.1f"/>'%(x+cx*U,base+cy*U,rr*U*0.92))
        else:
            # scale path coordinates
            import re
            def sc_(mo):
                nums=mo.group(0)
                return nums
            toks=re.findall(r'[MVCA]|-?\.?\d+\.?\d*|[ ,]',pt)
            res=[];cmd=None;args=[]
            tokens=[t for t in re.findall(r'[MVCA]|-?\d*\.?\d+',pt)]
            i2=0;s=''
            while i2<len(tokens):
                t=tokens[i2]
                if t in 'MVCA':
                    cmd=t;s+=t;i2+=1;continue
                if cmd=='M' or cmd=='C':
                    s+='%.1f,%.1f '%(x+float(t)*U,base+float(tokens[i2+1])*U);i2+=2
                elif cmd=='V':
                    s+='%.1f '%(base+float(t)*U);i2+=1
                elif cmd=='A':
                    a=[float(v) for v in tokens[i2:i2+7]]
                    s+='%.1f,%.1f %d %d %d %.1f,%.1f '%(a[0]*U,a[1]*U,a[2],a[3],a[4],x+a[5]*U,base+a[6]*U);i2+=7
            g.append('<path d="%s"/>'%s.strip())
    cap='round' if MODE=='soft' else 'butt'; jn='round' if MODE=='soft' else 'miter'
    if BLUE:
        out.append('<g stroke-width="%.1f" fill="none" stroke="#fff" stroke-linecap="%s" stroke-linejoin="%s">%s</g>'%(sw+2*OUTW,cap,jn,''.join(g)))
        out.append('<g stroke-width="%.1f" fill="none" stroke="%s" stroke-linecap="%s" stroke-linejoin="%s">%s</g>'%(sw,BLUE,cap,jn,''.join(g)))
    else:
        out.append('<g stroke-width="%.1f" fill="none" stroke="#fff" stroke-opacity=".78" stroke-linecap="%s" stroke-linejoin="%s">%s</g>'%(sw,cap,jn,''.join(g)))
    last=(x,U,base)
    x+=w*U*0.97+(4 if i==0 else 0)
lx,lU,lb=last
# POLY above the m, o and r, straight, in the same round monoline as the letters, orange with a thin white outline
px,py,ph=22,20,24      # left edge, top, height of the letters
pw=ph*.62
def Pl(x): return 'M%.1f,%.1f V%.1f M%.1f,%.1f H%.1f A%.1f,%.1f 0 0 1 %.1f,%.1f H%.1f'%(x,py,py+ph,x,py,x+pw*.55,ph*.26,ph*.26,x+pw*.55,py+ph*.52,x), pw
def Ol(x):
    r=ph*.46; return 'M%.1f,%.1f a%.1f,%.1f 0 1 0 %.1f,0 a%.1f,%.1f 0 1 0 %.1f,0'%(x,py+ph/2,r,r,2*r,r,r,-2*r), 2*r
def Ll(x): return 'M%.1f,%.1f V%.1f H%.1f'%(x,py,py+ph,x+pw*.85), pw*.85
def Yl(x): return 'M%.1f,%.1f L%.1f,%.1f L%.1f,%.1f M%.1f,%.1f V%.1f'%(x,py,x+pw*.5,py+ph*.52,x+pw,py,x+pw*.5,py+ph*.52,py+ph), pw
d=[];xx=px
for f in (Pl,Ol,Ll,Yl):
    dd,wd=f(xx); d.append(dd); xx+=wd+10
D=' '.join(d); swp=5.4
pe=('<g fill="none" stroke-linecap="round" stroke-linejoin="round"><path d="%s" stroke="#fff" stroke-width="%.1f"/><path d="%s" stroke="#ff9a00" stroke-width="%.1f"/></g>'
    %(D,swp+2*OUTW,D,swp))
svg='<svg xmlns="http://www.w3.org/2000/svg" width="360" height="130" viewBox="0 0 360 130">%s%s</svg>'%(''.join(out),pe)
open(sys.argv[1],'w').write(svg)
print('end x',x)
