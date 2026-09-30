import sys, csv
def load(p):
    rows=[l for l in open(p) if not l.startswith('#')]
    r=list(csv.DictReader(rows)); return [x for x in r if x.get('frame','').isdigit()]
def means(p, start=180):
    """per-zone mean ms/frame over frames >= start; hits per frame. Rows are 60-frame blocks (frame = end frame)."""
    r=[x for x in load(p) if int(x['frame'])>start]   # block ending at 240 covers 181..240
    nf=60.0*len(r); ms={}; hits={}
    ms['_wall']=sum(float(x['wall_us']) for x in r)/nf/1000
    for k in r[0]:
        if k.endswith('_us') and k!='wall_us': ms[k[:-3]]=sum(float(x[k]) for x in r)/nf/1000
        elif k.endswith('_hits'): hits[k[:-5]]=sum(float(x[k]) for x in r)/nf
    return ms,hits,len(r)
if __name__=='__main__':
    ms,hits,n=means(sys.argv[1])
    print(f"== {sys.argv[1]}  ({n} blocks, frames>=180; emulated ms/frame)")
    tot=0
    wall=ms.pop('_wall')
    for k,v in sorted(ms.items(), key=lambda kv:-kv[1]):
        if v>0: print(f"  {k:16s}{v:9.3f}")
    print(f"  {'(sum zones)':16s}{sum(ms.values()):9.3f}   emulated wall/frame {wall:.3f}")
    print(f"  hits/frame: arm9_jit={hits.get('arm9_jit',0):.1f} arm7_jit={hits.get('arm7_jit',0):.1f}")
