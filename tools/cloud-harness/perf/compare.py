#!/usr/bin/env python3
import sys; sys.path.insert(0,'/home/user/tools/perf')
from summary import means
a,ha,_=means(sys.argv[1]); b,hb,_=means(sys.argv[2]); a.pop('_wall'); b.pop('_wall')
print(f"{'zone':16s}{'A ms/f':>10s}{'B ms/f':>10s}{'delta':>10s}{'%':>8s}")
for k in sorted(set(a)|set(b), key=lambda k:-abs(b.get(k,0)-a.get(k,0))):
    x,y=a.get(k,0),b.get(k,0)
    if x==0 and y==0: continue
    print(f"{k:16s}{x:10.3f}{y:10.3f}{y-x:+10.3f}{(100*(y-x)/x if x else 0):+8.1f}")
sa,sb=sum(a.values()),sum(b.values())
print(f"{'TOTAL':16s}{sa:10.3f}{sb:10.3f}{sb-sa:+10.3f}{100*(sb-sa)/sa:+8.1f}")
