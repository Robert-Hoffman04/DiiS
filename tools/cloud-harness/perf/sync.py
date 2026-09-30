#!/usr/bin/env python3
# sync.py src dst : mirror src into dst copying only files whose content differs (keeps dst mtimes -> incremental make)
import os,sys,shutil,filecmp
src,dst=sys.argv[1:3]
EXD={'.git','build'}; EXS=('.dol','.elf','.map','.nds')  # top-level outputs / ROMs only
seen=set()
for r,ds,fs in os.walk(src):
    ds[:]=[d for d in ds if d not in EXD]
    rel=os.path.relpath(r,src); os.makedirs(os.path.join(dst,rel),exist_ok=True)
    for f in fs:
        if rel=='.' and f.endswith(EXS): continue
        s=os.path.join(r,f); d=os.path.normpath(os.path.join(dst,rel,f)); seen.add(d)
        if not os.path.exists(d) or not filecmp.cmp(s,d,shallow=False): shutil.copy2(s,d)
for r,ds,fs in os.walk(dst):
    ds[:]=[d for d in ds if d not in EXD]
    for f in fs:
        p=os.path.join(r,f)
        if p not in seen and not (os.path.dirname(p)==dst and (f.endswith(EXS) or f=='.defs')): os.remove(p)
