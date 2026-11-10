import re,json
L=open('log8.sym',errors='replace').read().split('\n')
recs=[];forks=[];cur=None;last=-1
def H(x): return int(x,16)
for l in L:
    m=re.match(r'--- sched-trace\[(\d+)\] cpu=(\d+) (\S+) \((.*)\) ---',l)
    if m:
        cur=dict(seq=int(m[1]),cpu=int(m[2]),ev=m[3].split(':')[-1],role=m[4],bt=[],fps=[],ctx={},tfr={});recs.append(cur);last=cur['seq'];continue
    m=re.match(r'sched-wake\[(\d+)\] cpu=(\d+) wake pid=(\d+) name=(\S+) chan=(\S+)(?: <([^>]*)>)? by pid=(-?\d+) name=(\S+) caller.lr=\S+ <([^>]*)> from.lr=\S+(?: <([^>]*)>)?',l)
    if m:
        recs.append(dict(seq=int(m[1]),cpu=int(m[2]),ev='wake',pid=int(m[3]),name=m[4],chan=m[6] or m[5],by=int(m[7]),byname=m[8],frm=m[10] or '?'));last=int(m[1]);cur=None;continue
    m=re.search(r'sched-q#\d+ cpu=(\d+) fork parent pid=(\d+) name=(\S+) -> child pid=(\d+)',l)
    if m: forks.append(dict(after=last,cpu=int(m[1]),ppid=int(m[2]),child=int(m[4])));continue
    if cur is None: continue
    if l.startswith('--- end'): cur=None; continue
    m=re.match(r'proc=(\S+) pid=(\d+) \S+ name=(\S+) state=(\w+)',l)
    if m: cur.update(proc=H(m[1]),pid=int(m[2]),name=m[3],state=m[4])
    m=re.search(r' task: .*chan=(\S+)( <[^>]*>)?',l)
    if m: cur['chan']=(m[2] or m[1]).strip(' <>')
    m=re.search(r'slice=(-?\d+).*last_cpu=(\d+) run_ticks=(\S+)',l)
    if m: cur['slice']=int(m[1]); cur['rt']=H(m[3])
    m=re.search(r'kthread_fn=(\S+)',l)
    if m: cur['kthread']=H(m[1])!=0
    m=re.search(r'kstack: base=(\S+) top=(\S+) pa=(\S+)',l)
    if m: cur.update(kbase=H(m[1]),ktop=H(m[2]),kpa=m[3])
    if l.startswith(' context=') or l.startswith('              x24='):
        for k,v in re.findall(r'(sp|x\d+)(?:\([a-z]+\))?=(0x[0-9a-f]+)',l): cur['ctx'][k]=v
    m=re.match(r'\s+bt#\d+ fp=(\S+) lr=\S+ <([^>]*)>',l)
    if m: cur['fps'].append(H(m[1])); cur['bt'].append(m[2])
    m=re.match(r' trapframe=(0x[0-9a-f]+)',l)
    if m: cur['tf']=H(m[1]); cur['tfmode']=1
    if l.startswith('  x0=') or l.startswith('  x8=') or l.startswith('  x16=') or l.startswith('  x24='):
        for k,v in re.findall(r'(x\d+)=(0x[0-9a-f]+)',l): cur['tfr'][k]=v
recs.sort(key=lambda r:r['seq'])
json.dump(dict(recs=recs,forks=forks),open('recs8.json','w'))
U=[r for r in recs if r['ev']!='wake' and not r.get('kthread')]
print(len(recs),len(U))
for r in recs:
    if r['ev']=='wake': print(f"[{r['seq']}] wake {r['pid']} {r['name']} chan={r['chan']} by {r['by']} {r['byname']} from {r['frm']}"); continue
    if r.get('kthread'): continue
    if r['seq']>80 and r['seq']<330 : continue
    print(f"[{r['seq']}] cpu{r['cpu']} {r['ev']:14s} {r['pid']:3d} {r['name']:9s} {r['state']:9s} {r['role'][:30]:30s} sp={r['ctx'].get('sp','?')[-6:]} chan={r.get('chan','')} bt={'/'.join(b.split('+')[0] for b in r['bt'][:4])}")
