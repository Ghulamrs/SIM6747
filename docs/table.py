import re,os,glob,statistics,subprocess
# One row per image: TI's output vs vm's (patched), cycles (TI cycle.Total, cycle.CPU; vm from entry and from main), wall times.
boot={l.split()[0]:int(l.split()[2].split('=')[1]) for l in open('boot2.txt') if 'bootcycles=' in l and not l.strip().endswith('?')}
vmwall={l.split()[0]:int(l.split()[1]) for l in open('vmtime.txt')}
def field(txt,k):
    m=re.search(r'%s=(\S+)'%k,txt); return m.group(1) if m else ''
rows=[]
for r in sorted(glob.glob('res/*.total.result')):
    n=os.path.basename(r)[:-13]
    tot=open(r).read(); cpu=open('res/%s.cpu.result'%n).read() if os.path.exists('res/%s.cpu.result'%n) else ''
    vmres=open('out/%s.vm.result'%n).read() if os.path.exists('out/%s.vm.result'%n) else ''
    ti_out='res/%s.total.stdout'%n; vm_out='out/%s.vm.stdout'%n
    same = os.path.exists(ti_out) and os.path.exists(vm_out) and open(ti_out,'rb').read().replace(b'\r\n',b'\n')==open(vm_out,'rb').read()
    vmexit='C$$EXIT' in vmres
    tcount=field(tot,'count'); ccount=field(cpu,'count'); tpc=field(tot,'pc'); texit=field(tot,'exit'); twall=field(tot,'wall_ms')
    vcount=field(vmres,'count'); vmain=int(vcount)-boot[n] if vcount and n in boot else None
    rows.append(dict(n=n,same=same and vmexit,vmexit=vmexit,ti=tcount,cpu=ccount,tiok=(tpc==texit),twall=twall,vm=vcount,vmain=vmain,vwall=vmwall.get(n),vmerr=vmres.strip().split('status=')[-1] if vmres else ''))
print('| program | output | TI cycle.Total | TI cycle.CPU | vm cycles (from main) | vm/Total | vm/CPU | TI wall ms | vm wall ms | wall ratio |')
print('|---|---|---:|---:|---:|---:|---:|---:|---:|---:|')
rt=[];rc=[];rw=[]
for r in rows:
    out='same' if r['same'] else ('vm faulted' if not r['vmexit'] else 'DIFFERS')
    if not r['tiok']: out+=' (TI not at C$$EXIT)'
    def ratio(a,b):
        try: return '%.3f'%(a/float(b))
        except: return '-'
    vt=ratio(r['vmain'],r['ti']) if r['vmain'] else '-'; vc=ratio(r['vmain'],r['cpu']) if r['vmain'] else '-'
    w=ratio(r['vwall'],r['twall']) if r['vwall'] and r['twall'] else '-'
    if vt!='-': rt.append(float(vt))
    if vc!='-': rc.append(float(vc))
    if w!='-': rw.append(float(w))
    print('| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |'%(r['n'],out,r['ti'],r['cpu'],r['vmain'] if r['vmain'] else '-',vt,vc,r['twall'],r['vwall'],w))
print()
print('programs:',len(rows),' output same:',sum(1 for r in rows if r['same']),' vm faulted:',sum(1 for r in rows if not r['vmexit']))
for name,l in [('vm/cycle.Total',rt),('vm/cycle.CPU',rc),('wall vm/TI',rw)]:
    if l: print('%s: n=%d median %.3f range %.3f..%.3f'%(name,len(l),statistics.median(l),min(l),max(l)))
