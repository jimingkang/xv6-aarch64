import json,re,bisect
D=json.load(open('recs8.json')); R=D['recs']; BY={r['seq']:r for r in R if r['ev']!='wake'}
FORKS=D['forks']
# symbols
SYM=[]
for l in open('k8.sym'):
    p=l.split()
    if len(p)>=2:
        try: SYM.append((int(p[0],16),p[-1]))
        except: pass
SYM.sort(); SA=[s[0] for s in SYM]
def sym(v):
    i=bisect.bisect_right(SA,v)-1
    if i<0: return None
    a,n=SYM[i]
    if n.startswith('.'): return None
    return n if v==a else f'{n}+0x{v-a:x}'
FONT="'WenQuanYi Zen Hei','PingFang SC','Microsoft YaHei',sans-serif"; MONO="'DejaVu Sans Mono',Menlo,monospace"
o=[]
def a(s): o.append(s)
def esc(s): return str(s).replace('&','&amp;').replace('<','&lt;').replace('>','&gt;')
def T(x,y,s,sz=12,fill="#222",anc="start",b=False,mono=False,halo=False):
    st=' stroke="#fff" stroke-width="3" paint-order="stroke"' if halo else ''
    a(f'<text x="{x:.1f}" y="{y:.1f}" font-size="{sz}" fill="{fill}" text-anchor="{anc}"'+(' font-weight="bold"' if b else '')+(f' font-family="{MONO}"' if mono else '')+st+f'>{esc(s)}</text>')
def tw(s,sz): return sum(sz if ord(c)>0x2000 else sz*0.6 for c in s)
W=2400
# ---------------- timeline ----------------
SEGS=[(26,80),(330,347)]
def shown(r):
    if not any(a_<=r['seq']<=b_ for a_,b_ in SEGS): return False
    if r['ev']=='wake': return r['pid']>=14
    return not r.get('kthread') and r['ev']!='runnable'
items=[r for r in R if shown(r)]
LX=170; U=27; BRK=150
pos={}; x=LX+20; prev=None
for r in items:
    if prev is not None and prev<=80 and r['seq']>=330: x+=BRK
    pos[r['seq']]=x; x+=U; prev=r['seq']
TL_END=x
BREAK_X=pos[min(s for s in pos if s>=330)]-U/2-BRK+U/2
def X(s): return pos[s]
def kind(r):
    p=r['pid']; n=r['name']
    if p==14: return 'init'
    if p==15: return 'wifi'
    if p==16: return 'sh' if n=='sh' else 'login'
    if p==17: return 'ft'
    return 'child'
COL={'init':'#3b6fd6','wifi':'#e08a1e','login':'#2e9d5b','sh':'#1f7a46','ft':'#8e44ad','child':'#7f8c8d'}
TOP=185; RH=34; GAP=58
def Y(c): return TOP+c*(RH+GAP)+RH/2
a('<defs><marker id="ar" markerWidth="8" markerHeight="8" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#c0392b"/></marker>'
  '<marker id="am" markerWidth="8" markerHeight="8" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#888"/></marker>'
  '<marker id="spa" markerWidth="11" markerHeight="11" refX="9" refY="5.5" orient="auto"><path d="M0,0 L11,5.5 L0,11 z" fill="#c0392b"/></marker></defs>')
T(W/2,40,"图 AO　log8（仅用户进程）：init → wifi → login → sh → forktest（128 个子进程）的调度、栈和换出时的 context 对比",26,"#111","middle",b=True)
T(W/2,68,"横轴 = 全局编号（内核线程记录已隐藏，[81]–[329] 中间 ~250 条 forktest 重复记录折叠）；色条 = 该 CPU 上两条记录之间运行的任务，条下小字 = 这段时间 sp 所在的内核栈（kstack VA 低位）",14,"#555","middle")
# phases
PH=[(26,41,"① init 读 SD（sdsector 忙等，被定时器 yield，CPU 间漂移）","#eef3fc"),(42,55,"② wifi exit → login → sh（键盘中断唤醒）","#fdf3e6"),(56,80,"③ forktest：父进程 17 连续 fork，子进程各跑一次就 exit","#f3ecf8"),(330,347,"④ 最后几个子进程 → 父 exit 唤醒 sh → sh 回到读命令","#ecf6ef")]
for s0,s1,t,c in PH:
    xs=[pos[s] for s in pos if s0<=s<=s1]
    x0=min(xs)-U/2; x1=max(xs)+U/2
    a(f'<rect x="{x0:.1f}" y="{TOP-82}" width="{x1-x0:.1f}" height="{4*(RH+GAP)+44}" fill="{c}"/>')
    T(x0+6,TOP-66,t,13,"#444")
for c in range(4):
    yc=Y(c)
    a(f'<rect x="{LX}" y="{yc-RH/2}" width="{TL_END-LX}" height="{RH}" fill="#fff" fill-opacity="0.5" stroke="#ddd"/>')
    T(30,yc+6,f"CPU {c}",17,"#222",b=True)
a(f'<rect x="{BREAK_X:.1f}" y="{TOP-82}" width="{BRK-U/2:.1f}" height="{4*(RH+GAP)+44}" fill="#fff" stroke="#bbb" stroke-dasharray="5,4"/>')
for k,t in enumerate(["[81]–[329] 省略","同样的模式重复：","父 17 fork → 子在空闲","CPU 上换入 → exit →","下一个子进程用新的","kstack 槽（VA 每次","−0x2000，…de000 →","…ee0000）"]):
    T(BREAK_X+(BRK-U/2)/2,TOP+20+k*19,t,12,"#666","middle")
# bars
ins=[r for r in items if r['ev'] in('switch-in','again-in','first-dispatch')]
outs=[r for r in items if r['ev']=='switch-out']
def endmark(x,y,r):
    if r['state'].startswith('ZOMBIE'): T(x+3,y-RH/2+2,"×",15,"#c0392b",b=True)
    elif r['state'].startswith('SLEEP'): T(x+3,y-RH/2+2,"z",14,"#c0392b",b=True)
    else: T(x+2,y-RH/2+1,"↻",14,"#c0392b",b=True)
def slot(r): return f"…{r['kbase']&0xfffff:05x}"
used_out=set()
for si in ins:
    p=si['pid']
    so=next((r for r in outs if r['pid']==p and r['seq']>si['seq']),None)
    x0=X(si['seq']); yc=Y(si['cpu'])
    if so is None or (si['seq']<=80 and so['seq']>=330):
        x1=BREAK_X-4 if si['seq']<=80 else TL_END; cut=True
    else: x1=X(so['seq']); cut=False; used_out.add(so['seq'])
    col=COL[kind(si)]
    a(f'<rect x="{x0:.1f}" y="{yc-11}" width="{max(x1-x0,4):.1f}" height="22" rx="4" fill="{col}"' + (' fill-opacity="0.55"' if cut else '')+'/>')
    a(f'<path d="M{x0-6:.1f},{yc} l6,-6 l6,6 l-6,6 z" fill="#222"/>' if si['ev']!='again-in' else f'<circle cx="{x0:.1f}" cy="{yc}" r="5" fill="#fff" stroke="#222" stroke-width="2"/>')
    lab=f"{p}" if x1-x0<70 else f"{p} {si['name']}"+{54:" → exec forktest",36:" → exec wifi"}.get(si["seq"],"")
    if kind(si)=='child' and x1-x0<40: lab=f"{p}"
    T((x0+x1)/2,yc+5,lab,11 if x1-x0<60 else 12,"#fff","middle",b=True)
    T((x0+x1)/2,yc+RH/2+13,slot(si),9.5,col,"middle",mono=True)
    T(x0,yc-RH/2-5,f"[{si['seq']}]",10,"#555","middle")
    if not cut:
        T(x1,yc-RH/2-5,f"[{so['seq']}]",10,"#c0392b","middle"); endmark(x1,yc,so)
# outs whose in is hidden (started in break)
for so in outs:
    if so['seq'] in used_out: continue
    x1=X(so['seq']); yc=Y(so['cpu']); col=COL[kind(so)]
    x0=BREAK_X+BRK-U/2+2 if so['seq']>=330 else LX
    a(f'<rect x="{x0:.1f}" y="{yc-11}" width="{x1-x0:.1f}" height="22" rx="4" fill="{col}" fill-opacity="0.55"/>')
    T((x0+x1)/2,yc+5,f"{so['pid']}",11,"#fff","middle",b=True)
    T((x0+x1)/2,yc+RH/2+13,slot(so),9.5,col,"middle",mono=True)
    T(x1,yc-RH/2-5,f"[{so['seq']}]",10,"#c0392b","middle"); endmark(x1,yc,so)
# migrations for init / ft / sh: out -> next in
def curve(x1,y1,x2,y2,col,dash=False,mk='am'):
    my=(y1+y2)/2
    a(f'<path d="M{x1:.1f},{y1:.1f} C{x1+25:.1f},{y1:.1f} {x2-25:.1f},{y2:.1f} {x2-7:.1f},{y2:.1f}" fill="none" stroke="{col}" stroke-width="1.6"'+(' stroke-dasharray="5,4"' if dash else '')+f' marker-end="url(#{mk})"/>')
for so in outs:
    if so['state'].startswith('ZOMBIE') or kind(so)=='child': continue
    nxt=next((r for r in ins if r['pid']==so['pid'] and r['seq']>so['seq']),None)
    if not nxt or (so['seq']<=80 and nxt['seq']>=330): continue
    wk=next((w for w in items if w['ev']=='wake' and w['pid']==so['pid'] and so['seq']<w['seq']<nxt['seq']),None)
    if wk: continue
    curve(X(so['seq'])+4,Y(so['cpu']),X(nxt['seq']),Y(nxt['cpu']),COL[kind(so)])
# wakes
for w in [r for r in items if r['ev']=='wake']:
    xw=X(w['seq']); yw=Y(w['cpu'])-RH/2-24
    T(xw,yw,"⚡",16,"#d35400","middle")
    T(xw,yw-14,f"[{w['seq']}]",10,"#d35400","middle")
    who="by IRQ" if w['by']<0 else f"by {w['by']}"
    T(xw,Y(w['cpu'])+RH/2+27,f"{w['frm'].split('+')[0]}",9.5,"#d35400","middle")
    T(xw,Y(w['cpu'])+RH/2+38,who,9.5,"#d35400","middle")
    nxt=next((r for r in ins if r['pid']==w['pid'] and r['seq']>w['seq']),None)
    if nxt: curve(xw,yw+4,X(nxt['seq']),Y(nxt['cpu']),"#d35400",mk='ar')
# fork marks
for f in FORKS:
    if not any(a_<=f['after']<=b_ for a_,b_ in SEGS): continue
    s=f['after']
    if s not in pos: continue
    xf=pos[s]+U/2; yc=Y(f['cpu'])
    a(f'<line x1="{xf:.1f}" y1="{yc-RH/2:.1f}" x2="{xf:.1f}" y2="{yc+RH/2:.1f}" stroke="#8e44ad" stroke-width="1.2" stroke-dasharray="2,2"/>')
    T(xf,yc+RH/2+24,f"f{f['child']}",9,"#8e44ad","middle")
ly=Y(3)+RH/2+60
T(40,ly,"◆ 换入（新进程或换了 CPU）　○ again-in（同一 CPU 再选中同一任务）　红色 [n] = 换出：↻ 被抢占（slice 用完，从定时器 IRQ yield）　z 睡眠　× exit　⚡ 唤醒（橙箭头指向被唤醒任务的下一次换入）　灰箭头 = 被抢占后在哪个 CPU 继续　紫虚线 fN = 父进程 fork 出 pid N",13,"#333")
T(40,ly+22,"颜色：蓝 init(14)　橙 wifi(15)　绿 login/sh(16)　紫 forktest 父(17)　灰 forktest 子(18…145)。注意 pid 15 和 16 用同一个 kstack VA …e2000（wifi 退出后槽位复用），每个 forktest 子进程则各占一个新槽。",13,"#333")
# ---------------- panels ----------------
PY=ly+60
SEL=[(28,"① [28] init 被抢占（CPU2 换出）","sdsector 忙等时定时器 IRQ 打在 init 自己的内核栈上，el1irq 存现场后 yield → sched → swtch。"),
     (42,"② [42] wifi exit（CPU0 换出）","exit → sched，永不再换入。栈很浅：只有系统调用链。"),
     (45,"③ [45] login 睡眠（CPU3 换出）","read(0) → consoleread → sleep(&cons.r)。[46] 键盘 IRQ 唤醒，[47] 同 CPU again-in。"),
     (55,"④ [55] sh 在 wait 里睡（CPU0 换出）","sh fork 了 17（forktest），wait → sleep(chan = sh 自己的 proc)。[342] 由 17 的 exit 唤醒。"),
     (56,"⑤ [56] forktest 子 18 首次换入（CPU3）","新 context：fork 时只设 sp 和 x30，其余全 0。swtch 装入后 ret 到 forkret → 回用户态。"),
     (58,"⑥ [58] 子 18 exit（CPU3 换出）","fork 返回 0 → 立刻 exit → sched。和②wifi 的 exit 栈形状完全一样，只是换了一块栈。"),
     (68,"⑦ [68] forktest 父 17 在 fork 中被抢占（CPU2）","do_clone 复制到一半来了定时器 IRQ，yield 换出；[70] 在 CPU3 换入接着复制。"),
     (345,"⑧ [345] sh 在 sleep 里被抢占（CPU0）","[342] 被唤醒、[343] 换入后，刚从 sleep 出来还没返回就被 IRQ yield；[346] 换到 CPU2。")]
NP=len(SEL); PW=(W-80)/4
SC=0.27
REFS={}
for l in open('log8.sym'):
    m=re.search(r'proc=\S+ pid=(\d+)',l)
    if m: pid=m[1]
    if l.startswith(' refs:'):
        for k,v in re.findall(r'(vm|files|fs)=(0x[0-9a-f]+)',l): REFS.setdefault((k,int(v,16)),set()).add(int(pid))
def H(x): return int(x,16)
def meaning(r,reg):
    c=r['ctx']; v=H(c[reg]); tf=r['tfr']; new=(H(c['x30'])==0xffffff8000083078)
    if reg=='sp': return ("= trapframe 地址：首次换入后栈从这里开始","stack") if new else ("→ 栈上 sched 的帧（swtch 换回时 mov sp 到这里）","stack")
    if reg=='x29': return ("0：帧链从空开始","zero") if new else ("= sp：sched 帧记录 {调用者 fp, lr}，回溯起点","stack")
    if reg=='x30': return ("forkret：ret 到 forkret → usertrapret → eret","code") if new else ("sched+0xb4：bl swtch 的下一条，换入后 ret 到这里","same")
    if new: return ("0（新 context 不需要）","zero")
    if reg=='x19': return ("&cpus[0]：sched 里 mycpu() 的基址","same")
    if reg=='x20': return ("0xa0 = sizeof(struct cpu)","same")
    if reg=='x21': return (f"p 自己（pid {r['pid']} 的 struct proc）","self")
    if reg=='x22': return ("intena=0：从定时器 IRQ 里 yield（中断已关）","path") if v==0 else ("intena=1：从 sleep/exit 系统调用路径进来","path")
    if reg=='x18':
        if v==0: return ("0","zero")
        if tf.get('x18') and H(tf['x18'])==v: return ("平台寄存器，内核不用：= 用户态 x18","user")
    # x23..x28
    if r['bt'] and r['bt'][1].startswith('consoleread') and reg=='x23': return (f"consoleread 的 n = {v}（read 请求字节数）","path")
    if r['bt'] and r['bt'][1].startswith('consoleread') and reg=='x24': return ("user_dst = 1（拷到用户地址）","path")
    if tf.get(reg) and H(tf[reg])==v and v!=0 and v not in (1,): return (f"= trapframe.{reg}：用户态的值，这条路径没用到","user")
    s=sym(v) if v>=0xffffff8000080000 and v<0xffffff8000200000 else None
    if s:
        nm={'sd_rca':'sdsector 用的 SD 卡相对地址变量','wait_lock':'exit/wait 用的锁','cons+0x98':'= sleep 的 chan（cons.r）'}.get(s,'')
        return (f"&{s}"+(f"：{nm}" if nm else ''),"data")
    if 0xffffff803f000000<=v<0xffffff8040000000: return (f"SD host MMIO 0x{v&0xffffffff:x}：正在轮询的寄存器","data")
    for (k,base),pids in REFS.items():
        if base and v>=0xffffff8000000000 and 0<=v-base<0x100:
            ps=sorted(pids); off=v-base
            if k=='vm' and len(ps)==1: return (f"pid {ps[0]} 的 vm 结构"+(f" +0x{off:x}" if off else ''),"data")
            if off==0 and k in('files','fs'): return (f"{k} 结构：do_clone 正在给新子进程（pid 25）复制","data")
    if v<0x10000: return (f"调用链局部变量（= {v}）","path")
    return ("?","path")
REGS=['sp','x18','x19','x20','x21','x22','x23','x24','x25','x26','x27','x28','x29','x30']
KC={'stack':'#c0392b','code':'#1d4ed8','data':'#7c3aed','user':'#047857','same':'#555','self':'#7c3aed','path':'#b45309','zero':'#999'}
def fname(x): return x.split('+')[0]
def panel(x0,y0,seq,title,note):
    r=BY[seq]; top=r['ktop']; base=r['kbase']; tf=r['tf']; csp=H(r['ctx']['sp']); fps=r['fps']; lrs=[fname(b) for b in r['bt']]
    col=COL[kind(r)]
    T(x0,y0,title,15,col,b=True)
    T(x0,y0+19,f"kstack VA 0x…{base&0xfffffff:07x}–{top&0xfffff:05x}  PA {r['kpa'][-8:]}  proc 0x…{r['proc']&0xffffff:06x}",10.5,"#555",mono=True)
    # wrap note
    line='';ln=0
    for ch in note:
        line+=ch
        if tw(line,11.5)>PW-40: T(x0,y0+38+ln*16,line,11.5,"#333"); line='';ln+=1
    if line: T(x0,y0+38+ln*16,line,11.5,"#333"); ln+=1
    bx=x0+90; bw=230; yt=y0+40+ln*16+12
    def yy(ad): return yt+(top-ad)*SC
    a(f'<rect x="{bx}" y="{yy(top):.1f}" width="{bw}" height="{(top-tf)*SC:.1f}" fill="#fde2e2" stroke="#c0392b"/>')
    T(bx+bw/2,(yy(top)+yy(tf))/2+4,"trapframe（用户寄存器 0x110）",10.5,"#c0392b","middle")
    T(bx-6,yy(top)+4,f"…{top&0xffff:04x}",10,"#555","end",mono=True)
    T(bx-6,yy(tf)+4,f"…{tf&0xffff:04x}",10,"#555","end",mono=True)
    bounds=fps+[tf]; owners=['sched']+lrs[:max(len(fps)-1,0)]
    for k in range(len(fps)):
        lo=fps[k]; hi=bounds[k+1]; ow=owners[k]; h=(hi-lo)*SC
        fill="#eef2fb" if k%2 else "#f7f9fd"
        if k+1<len(owners) and owners[k+1]=='el1irq': fill="#fff1e0"; ow=f'yield 帧 + el1irq 存的中断现场（0x{hi-lo:x}）'
        elif ow=='el1irq': fill="#fff1e0"; ow='el1irq 帧'
        if k==len(fps)-1 and len(fps)>=12: ow=f"{ow} … ← usertrap（回溯上限 12 帧）"
        a(f'<rect x="{bx}" y="{yy(hi):.1f}" width="{bw}" height="{h:.1f}" fill="{fill}" stroke="#9fb0d0"/>')
        if h>=9: T(bx+bw/2,yy(hi)+h/2+4,ow,10 if h<15 else 11,"#223","middle")
        if h>=7 or k==0: T(bx-6,yy(lo)+4,f"…{lo&0xffff:04x}",9.5,"#888","end",mono=True)
    ysp=yy(csp)
    a(f'<line x1="{bx-50}" y1="{ysp:.1f}" x2="{bx+bw+25}" y2="{ysp:.1f}" stroke="#111" stroke-width="2"/>')
    T(bx+bw+29,ysp+4,"← p->context.sp",11,"#111",b=True)
    T(bx+bw+29,ysp+18,"首次换入时 sp 从这里开始" if not fps else "swtch 在这里离开 / 回来",10,"#555")
    yf=ysp+6
    a(f'<rect x="{bx}" y="{yf:.1f}" width="{bw}" height="30" fill="#fff" stroke="#ccc" stroke-dasharray="4,3"/>')
    T(bx+bw/2,yf+19,f"空闲 {csp-base} 字节（未画）  base …{base&0xffff:04x}",10,"#999","middle")
    # ctx box
    by=yf+48; rh=18; bw_=PW-30
    a(f'<rect x="{x0}" y="{by}" width="{bw_}" height="{40+rh*14+6}" rx="6" fill="#fffdf6" stroke="#c9a227" stroke-width="1.4"/>')
    T(x0+10,by+18,f"p->context（struct proc +0xb8 = 0x…{(r['proc']+0xb8)&0xffffff:06x}）",12.5,"#7a5b14",b=True)
    T(x0+10,by+33,("[%d] 换入前，swtch 要装入的 13 个值 + sp"%seq) if r['ev']!='switch-out' else ("[%d] 换出时 swtch 存进去的 13 个值 + sp"%seq),10.5,"#8a7a50")
    yr=by+40
    for reg in REGS:
        m,kd=meaning(r,reg); c=KC[kd]
        if kd=='stack': a(f'<rect x="{x0+3}" y="{yr}" width="{bw_-6}" height="{rh}" fill="#fde8e8"/>')
        T(x0+10,yr+13,{'x29':'x29 fp','x30':'x30 lr'}.get(reg,reg),11,"#111",b=True,mono=True)
        T(x0+58,yr+13,r['ctx'][reg],10,c,mono=True)
        mm=m
        while tw(mm,10.5)>bw_-195: mm=mm[:-1]
        if mm!=m: mm=mm[:-1]+'…'
        T(x0+188,yr+13,mm,10.5,c)
        if kd=='stack' and H(r['ctx'][reg])!=0:
            off=0 if reg=='sp' else 9
            a(f'<path d="M{x0+3},{yr+rh/2:.1f} C{x0-14-off},{yr+rh/2:.1f} {x0-14-off},{ysp:.1f} {bx-50-8:.1f},{ysp+(0 if reg=="sp" else 3):.1f}" fill="none" stroke="#c0392b" stroke-width="1.8"'+(' stroke-dasharray="6,4"' if reg=='x29' else '')+' marker-end="url(#spa)"/>')
        yr+=rh
    return yr+10
a(f'<rect x="20" y="{PY}" width="{W-40}" height="PANELH" rx="8" fill="#fbfbfd" stroke="#d8dbe3"/>')
T(40,PY+30,"8 次切换的栈与 context：每次换到不同进程时，swtch 两侧的栈长什么样、p->context 里每个寄存器指向哪里（全部来自 log8 的实时记录；红线 = context.sp / x29 指向栈上的位置）",17,"#222",b=True)
y=PY+60; ends=[]
for i,(s,t,n) in enumerate(SEL):
    if i==4: y=max(ends)+30; ends=[]
    ends.append(panel(50+(i%4)*PW,y+20,s,t,n))
PEND=max(ends)+10
o[:]=[q.replace('height="PANELH"',f'height="{PEND-PY}"') for q in o]
# ---------------- matrix ----------------
MY=PEND+40
T(40,MY,"context 横向对比：同一个寄存器在不同进程/不同换出原因下的值",19,"#111",b=True)
T(40,MY+22,"颜色 = 含义类别：灰 = 所有 live context 都一样　红 = 指向自己的内核栈　紫 = 自己的 proc / 内核数据　橙 = 由换出路径决定　绿 = 原样带着的用户态值　浅灰 = 新 context 里的 0",13,"#555")
cols=[s for s,_,_ in SEL]
cx0=40; rw=105; cwid=(W-80-rw-330)/len(cols); ry=MY+40; rhh=26
a(f'<rect x="{cx0}" y="{ry}" width="{W-80}" height="{rhh*1.6}" fill="#f1f2f6"/>')
T(cx0+8,ry+26,"寄存器",12.5,"#111",b=True)
for j,s in enumerate(cols):
    r=BY[s]; xx=cx0+rw+j*cwid
    T(xx+cwid/2,ry+17,f"[{s}] {r['pid']} {r['name']}",12,COL[kind(r)],"middle",b=True)
    why={'ZOMBIE':'exit','SLEEPING':'sleep','RUNNABLE':'被抢占','RUNNING':'首次换入'}[r['state'].split('(')[0]]
    T(xx+cwid/2,ry+34,f"{why} · {fname(r['bt'][1]) if len(r['bt'])>1 else ('forkret' if not r['bt'] else r['bt'][0])}" if r['bt'] else f"{why} · 新 context",10.5,"#666","middle")
T(W-40-322,ry+26,"规律",12.5,"#111",b=True)
RULE={'sp':"每个进程指向自己 kstack 里 sched 的帧；深浅取决于调用链",
 'x18':"内核不用：live 时带着用户态 x18，新 context 为 0",
 'x19':"live 一律 &cpus[0]（sched 用来算 mycpu）",
 'x20':"live 一律 0xa0 = sizeof(struct cpu)",
 'x21':"live 一律 = p 自己：每列不同，等于各自 proc 地址",
 'x22':"intena：IRQ 抢占进来 = 0，sleep/exit 进来 = 1",
 'x23':"调用链里的局部变量（consoleread 的 n 等）",
 'x24':"路径决定：&sd_rca / &wait_lock / user_dst / 新子进程的 files…",
 'x25':"路径决定：SD MMIO / &cons.r / fs 结构；没用到时是用户值",
 'x26':"大多是用户态 x26 原样带下来（callee-saved 没人改）",
 'x27':"同上；init 读盘时是 SD MMIO，fork 时是父 vm 结构",
 'x28':"同上",
 'x29':"= sp：sched 帧记录的地址，回溯起点",
 'x30':"live 一律 sched+0xb4；新 context = forkret"}
for i,reg in enumerate(REGS):
    yy_=ry+rhh*1.6+i*rhh
    a(f'<rect x="{cx0}" y="{yy_}" width="{W-80}" height="{rhh}" fill="{"#fff" if i%2 else "#fafbfc"}" stroke="#eee"/>')
    T(cx0+8,yy_+17,{'x29':'x29 fp','x30':'x30 lr'}.get(reg,reg),12,"#111",b=True,mono=True)
    for j,s in enumerate(cols):
        r=BY[s]; m,kd=meaning(r,reg); v=H(r['ctx'][reg]); xx=cx0+rw+j*cwid
        bg={'stack':'#fde8e8','same':'#eceff3','self':'#efe6fa','data':'#efe6fa','path':'#fdf0dc','user':'#e1f4ea','zero':'#f7f7f7','code':'#e3ebfd'}[kd]
        a(f'<rect x="{xx+2}" y="{yy_+2}" width="{cwid-4}" height="{rhh-4}" rx="3" fill="{bg}"/>')
        if v==0: sv="0"
        elif v>=0xffffff8000000000: 
            ss=sym(v) if v<0xffffff8000200000 else None
            sv=ss if ss else f"…{v&0xffffffff:08x}"
        else: sv=f"0x{v:x}" if v>9 else str(v)
        T(xx+cwid/2,yy_+17,sv,10.5,KC[kd],"middle",mono=True)
    T(W-40-322,yy_+17,RULE[reg],11,"#333")
yb=ry+rhh*1.6+14*rhh+30
notes=["结论",
"1. 同一时刻每个 CPU 只有一个 context 在用：换出时 swtch 把 x19–x30、sp 存进 p->context，再从 c->context 装回调度器；换入反过来。所以 8 列里“灰色”那几行（x19/x20/x30）永远一样 —— 它们是 sched() 自己的寄存器，不属于进程。",
"2. 真正区分进程的是 sp/x29（指向各自 kstack 上 sched 的帧）和 x21（= 自己的 proc）。wifi 和 login/sh 的 sp 都在 …e2xxx，是同一个 VA 槽先后给了 pid 15 和 16；forktest 的子进程每个占一个新槽（…de000、…dc000 …）。",
"3. x22 告诉你“为什么换出”：0 = 从定时器 IRQ yield（init 读盘、父 17 fork 到一半、sh 刚出 sleep），1 = 自己走 sleep/exit。x23–x28 是换出路径上的 callee-saved 寄存器，路径没用到的就原样保留用户态的值（绿色）。",
"4. 新进程（⑤）的 context 只有 sp=trapframe 和 x30=forkret，其余全 0：第一次换入不经过 sched，直接 ret 到 forkret 回用户态。它 exit（⑥）时的栈形状与 wifi exit（②）完全相同。",
"5. 被抢占的三列（①⑦⑧）栈里都有一块 el1irq 中断现场：中断压在当时正在跑的进程自己的内核栈上，下次在别的 CPU 换入（[29]/[70]/[346]）时从这里 eret 回去，所以“换 CPU 继续”不需要搬栈。"]
for k,t in enumerate(notes): T(40,yb+k*22,t,14 if k==0 else 13,"#111" if k==0 else "#333",b=(k==0))
H_=int(yb+len(notes)*22+30)
out=f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H_}" viewBox="0 0 {W} {H_}" font-family="{FONT}"><rect width="{W}" height="{H_}" fill="#fff"/>'+'\n'.join(o)+'</svg>'
open('fig-ao-sched-log8-stack-ctx.svg','w').write(out); print(H_, TL_END)
