import sys
# Resynchronising compare: TI's trace (one line a cycle, collapsed to the first line of each PC run) against
# vm's (one line an execute packet). At a common PC the registers are compared; when the PCs differ, the side
# whose PC the other reaches sooner (within a window) is advanced. Finds the first register disagreement at a
# PC both visit, which is what a stepping-granularity difference (NOP holds, the loop buffer) cannot produce.
def load(p):
    names=None; rows=[]
    for l in open(p):
        if l.startswith('# REGS'): names=l.split()[2:]
        elif l.startswith('S '):
            t=l.split(); rows.append((int(t[1]), [x[-8:].lower().zfill(8) for x in t[2:]]))
    return names, rows
main=sys.argv[3].lower(); limit=int(sys.argv[4]) if len(sys.argv)>4 else 10
IGN=set('CYC A13 B13 CSR IER DNUM TSR ITSR NTSR IFR ISTP ILC RILC PCE1'.split())
tn,tr=load(sys.argv[1]); vn,vr=load(sys.argv[2])
ct=[]
for k,r in tr:
    if not (ct and ct[-1][1][0]==r[0]): ct.append((k,r))
vs=vr
i=next(i for i,(k,r) in enumerate(vs) if r[0]==main); j=next(j for j,(k,r) in enumerate(ct) if r[0]==main)
cols=[c for c in tn if c in vn and c not in IGN]; ti={c:tn.index(c) for c in cols}; vi={c:vn.index(c) for c in cols}
agree=0; shown=0; first=None; W=400
while i<len(vs) and j<len(ct):
    (kv,b),(kt,a)=vs[i],ct[j]
    if a[0]==b[0]:
        d=[(c,a[ti[c]],b[vi[c]]) for c in cols if a[ti[c]]!=b[vi[c]]]
        if d:
            if first is None: first=(kt,kv)
            if shown<limit: print('TI step %d / vm step %d at %s: '%(kt,kv,a[0])+'  '.join('%s TI=%s vm=%s'%x for x in d[:10])); shown+=1
        else: agree+=1
        i+=1; j+=1; continue
    # resync
    dv=next((n for n in range(1,W) if i+n<len(vs) and vs[i+n][1][0]==a[0]), None)
    dt=next((n for n in range(1,W) if j+n<len(ct) and ct[j+n][1][0]==b[0]), None)
    if dv is None and dt is None:
        print('paths part: TI step %d at %s, vm step %d at %s'%(kt,a[0],kv,b[0])); break
    if dt is None or (dv is not None and dv<=dt): i+=dv
    else: j+=dt
print('%d packets agree at common PCs; first register disagreement at TI/vm step %s'%(agree,first))
