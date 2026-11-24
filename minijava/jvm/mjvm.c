// mjvm -- a small Java Virtual Machine for xv6-aarch64.
//
//   mjvm [-v | -stats] [-cp dir] MainClass [args...]
//
// It loads real .class files (from mjavac, or from `javac --release 8`),
// links them, and runs them with a bytecode interpreter.  The point of the
// exercise is the thread/monitor machinery:
//
//   * every java.lang.Thread is a kernel thread made with thread_clone()
//   * every object has a 64-bit lock word ("mark word") in its header
//       0                                 unlocked
//       owner_id<<32 | recursion<<2 | 01  thin lock: one CAS, no syscall
//       ObjectMonitor* | 10               inflated: entry list + wait set
//   * monitorenter / monitorexit / ACC_SYNCHRONIZED all go through
//     monitor_enter() / monitor_exit() below
//   * blocked threads sleep on a per-thread semaphore (HotSpot: ParkEvent)
//   * Object.wait/notify/notifyAll, Thread.join (= wait until !alive)
//   * exceptions are dispatched through each method's exception table,
//     which is also how javac makes `synchronized {}` release on throw
//   * volatile fields are read with load-acquire and written with
//     store-release (LDAR / STLR on AArch64)
//
// Not implemented: garbage collection (bump allocation only), float/double,
// interfaces beyond invokeinterface name lookup, reflection, class loaders.

#include "plat.h"

#define NULL ((void *)0)
#define VM_THREAD_STACK (512 * 1024)
#define MAX_THREADS 48

// ------------------------------------------------------------------ core types

typedef union Slot { int64_t j; int32_t i; struct Object *a; void *p; } Slot;

typedef struct Object {
  struct Class *cls;
  volatile int64_t lock;          // the mark word
} Object;
#define FIELDS(o) ((Slot *)((char *)(o) + sizeof(Object)))

typedef struct Array {
  Object h;
  int32_t length;
  int32_t esize;
  char data[];                    // 8-byte aligned (offset 24)
} Array;

typedef struct Thread Thread;
typedef void (*NativeFn)(Thread *t, Slot *args, Slot *ret);

typedef struct CPEntry {
  uint8_t tag;
  uint16_t a, b;
  union { int32_t i; int64_t j; char *s; } v;
  void *resolved;                 // Class*, Field*, Method*, String Object*
} CPEntry;

typedef struct ExcEntry { uint16_t start, end, handler, type; } ExcEntry;

typedef struct Method {
  char *name, *desc;              // interned
  uint16_t flags;
  struct Class *cls;
  uint16_t max_stack, max_locals;
  uint32_t code_len;
  uint8_t *code;
  int nexc;
  ExcEntry *exc;
  int argslots;                   // incl. `this`
  char rettype;                   // V I Z B C S J L [
  int vindex;                     // vtable slot, -1 if not virtual
  NativeFn native;
} Method;

typedef struct Field {
  char *name, *desc;              // interned
  uint16_t flags;
  struct Class *cls;
  int index;                      // instance slot or static slot
} Field;

enum { CLS_LOADED, CLS_LINKED, CLS_INITIALIZING, CLS_INITIALIZED, CLS_ERROR };

typedef struct Class {
  char *name;                     // interned, e.g. "java/lang/Thread"
  char *supername;
  struct Class *super;
  uint16_t flags;
  int ncp;
  CPEntry *cp;
  int nfields;
  Field *fields;
  int nmethods;
  Method *methods;
  int nifaces;
  char **ifnames;
  int ninst;                      // instance slots incl. superclasses
  int nstatic;
  Slot *statics;
  Method **vtable;
  int vtlen;
  Object *mirror;                 // the java.lang.Class object (lockable)
  volatile int state;
  Thread *init_thread;
  char *elemdesc;                 // arrays: "I", "Ljava/lang/String;" ...
  struct Class *next;
} Class;

#define ACC_PUBLIC       0x0001
#define ACC_PRIVATE      0x0002
#define ACC_STATIC       0x0008
#define ACC_FINAL        0x0010
#define ACC_SYNCHRONIZED 0x0020
#define ACC_VOLATILE     0x0040
#define ACC_NATIVE       0x0100
#define ACC_INTERFACE    0x0200

struct Thread {
  int id;
  int park;                       // semaphore handle (ParkEvent)
  long handle;                    // kernel thread
  Object *obj;                    // java.lang.Thread
  Object *exc;                    // pending exception
  Thread *next;                   // monitor queue link
  int saved_rec;
  volatile int alive, started;
  int nblock_traces;
  int depth;
  long bytecodes;
  Method *entry;                  // what to run: main() or run()
  Object *entry_arg;
  int outlen;
  char outbuf[256];
};

typedef struct ObjectMonitor {
  volatile int spin;
  Thread *owner;
  int64_t rec;
  Thread *entry_head, *entry_tail;
  Thread *wait_head, *wait_tail;
  Object *obj;
} ObjectMonitor;

static int verbose, show_stats;
static const char *classpath = ".";
static Thread *threads[MAX_THREADS + 1];
static volatile int nthreads;
static volatile long st_inflations, st_parks, st_waits, st_contended, st_bytecodes;
static int exit_status;

// ------------------------------------------------------------------ small libc

static int slen(const char *s) { int n = 0; while(s[n]) n++; return n; }
static int sneq(const char *a, const char *b, int n) { return memcmp(a, b, n) == 0; }

static void
spin_lock(volatile int *l)
{
  while(__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
    while(__atomic_load_n(l, __ATOMIC_RELAXED))
      plat_yield();
}
static void spin_unlock(volatile int *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }

static volatile int heap_lock;
static char *heap_cur, *heap_end;
static long heap_used;

static void *
vm_alloc(long size)
{
  size = (size + 15) & ~15L;
  spin_lock(&heap_lock);
  if(heap_cur + size > heap_end){
    long chunk = size > (1L << 20) ? size : (1L << 20);
    char *m = plat_chunk(chunk);
    if(!m){
      spin_unlock(&heap_lock);
      plat_write("mjvm: java.lang.OutOfMemoryError (no GC)\n", 41);
      plat_exit(1);
    }
    heap_cur = m;
    heap_end = m + chunk;
  }
  char *p = heap_cur;
  heap_cur += size;
  heap_used += size;
  spin_unlock(&heap_lock);
  memset(p, 0, size);           // chunks are zeroed, but keep it explicit
  return p;
}

static char *
sdup(const char *s, int n)
{
  char *p = vm_alloc(n + 1);
  memcpy(p, s, n);
  p[n] = 0;
  return p;
}

// formatted output: %s %d %p %c, one write per call
// one lock around console writes so lines from different threads never mix
static volatile int out_lock;
static void
vm_write(const char *s, int n)
{
  spin_lock(&out_lock);
  plat_write(s, n);
  spin_unlock(&out_lock);
}

static int
fmt_long(char *p, int64_t v)
{
  char tmp[24];
  int n = 0, len = 0;
  uint64_t u = v < 0 ? -(uint64_t)v : (uint64_t)v;
  do { tmp[n++] = '0' + u % 10; u /= 10; } while(u);
  if(v < 0) p[len++] = '-';
  while(n) p[len++] = tmp[--n];
  return len;
}

static int
fmt_hex(char *p, uint64_t v)
{
  static const char d[] = "0123456789abcdef";
  int len = 0, started = 0;
  for(int sh = 60; sh >= 0; sh -= 4){
    int x = (v >> sh) & 15;
    if(x || started || sh == 0){ p[len++] = d[x]; started = 1; }
  }
  return len;
}

static void
say(const char *fmt, ...)
{
  char buf[512];
  int n = 0;
  __builtin_va_list ap;
  __builtin_va_start(ap, fmt);
  for(const char *f = fmt; *f && n < 440; f++){
    if(*f != '%'){ buf[n++] = *f; continue; }
    f++;
    if(*f == 'd') n += fmt_long(buf + n, __builtin_va_arg(ap, int));
    else if(*f == 'l') n += fmt_long(buf + n, __builtin_va_arg(ap, long));
    else if(*f == 'p'){ buf[n++] = '0'; buf[n++] = 'x'; n += fmt_hex(buf + n, (uint64_t)__builtin_va_arg(ap, void *)); }
    else if(*f == 'c') buf[n++] = (char)__builtin_va_arg(ap, int);
    else if(*f == 's'){
      const char *s = __builtin_va_arg(ap, const char *);
      if(!s) s = "(null)";
      while(*s && n < 500) buf[n++] = *s++;
    }
  }
  __builtin_va_end(ap);
  vm_write(buf, n);
}

static void __attribute__((noreturn))
fatal(const char *msg, const char *arg)
{
  say("mjvm: %s%s\n", msg, arg ? arg : "");
  plat_exit(1);
}

// "java/lang/Thread" -> "java.lang.Thread" (into a static-ish buffer)
static const char *
dotted(const char *name, char *buf, int cap)
{
  int i = 0;
  for(; name[i] && i < cap - 1; i++) buf[i] = name[i] == '/' ? '.' : name[i];
  buf[i] = 0;
  return buf;
}

// ------------------------------------------------------------------ string interning (VM names)

typedef struct IStr { struct IStr *next; char s[]; } IStr;
static IStr *itab[512];
static volatile int itab_lock;

static unsigned
hash(const char *s, int n)
{
  unsigned h = 2166136261u;
  for(int i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
  return h;
}

static char *
internn(const char *s, int n)
{
  unsigned h = hash(s, n) & 511;
  spin_lock(&itab_lock);
  for(IStr *e = itab[h]; e; e = e->next)
    if(slen(e->s) == n && sneq(e->s, s, n)){ spin_unlock(&itab_lock); return e->s; }
  IStr *e = vm_alloc(sizeof(IStr) + n + 1);
  memcpy(e->s, s, n);
  e->s[n] = 0;
  e->next = itab[h];
  itab[h] = e;
  spin_unlock(&itab_lock);
  return e->s;
}
static char *intern(const char *s) { return internn(s, slen(s)); }

// ------------------------------------------------------------------ forward decls

static Class *load_class(Thread *t, const char *name);
static int init_class(Thread *t, Class *c);
static int invoke(Thread *t, Method *m, Slot *args, Slot *ret);
static Object *new_object(Class *c);
static Object *new_string(const char *s, int n);
static void throw_new(Thread *t, const char *cls, const char *msg);
static Method *find_method(Class *c, const char *name, const char *desc);
static int instance_of(Object *o, Class *c);
static Class *array_class(Thread *t, const char *elemdesc);

static Class *classes;
static volatile int class_lock;           // protects the class list
static Class *C_Object, *C_String, *C_Thread, *C_Class, *C_Throwable, *C_StringBuilder;

// ------------------------------------------------------------------ objects

static Object *
new_object(Class *c)
{
  Object *o = vm_alloc(sizeof(Object) + c->ninst * sizeof(Slot));
  o->cls = c;
  return o;
}

static Array *
new_array(Class *ac, int n, int esize)
{
  Array *a = vm_alloc(sizeof(Array) + (long)n * esize);
  a->h.cls = ac;
  a->length = n;
  a->esize = esize;
  return a;
}

// java.lang.String: FIELDS[0] = char* (UTF-8, NUL-terminated), FIELDS[1] = length
#define STR_CHARS(o) ((char *)FIELDS(o)[0].p)
#define STR_LEN(o)   ((int)FIELDS(o)[1].i)

static Object *
new_string(const char *s, int n)
{
  Object *o = new_object(C_String);
  FIELDS(o)[0].p = sdup(s, n);
  FIELDS(o)[1].i = n;
  return o;
}

typedef struct JStr { struct JStr *next; Object *o; } JStr;
static JStr *jstrtab[512];
static volatile int jstr_lock;

// String.intern() semantics for literals: equal literals are the same object
static Object *
intern_string(const char *s, int n)
{
  unsigned h = hash(s, n) & 511;
  spin_lock(&jstr_lock);
  for(JStr *e = jstrtab[h]; e; e = e->next)
    if(STR_LEN(e->o) == n && sneq(STR_CHARS(e->o), s, n)){ spin_unlock(&jstr_lock); return e->o; }
  JStr *e = vm_alloc(sizeof(JStr));
  e->o = new_string(s, n);
  e->next = jstrtab[h];
  jstrtab[h] = e;
  spin_unlock(&jstr_lock);
  return e->o;
}

static int
instance_of(Object *o, Class *c)
{
  if(!o) return 0;
  if(c == C_Object) return 1;
  for(Class *k = o->cls; k; k = k->super){
    if(k == c) return 1;
    for(int i = 0; i < k->nifaces; i++)
      if(k->ifnames[i] == c->name) return 1;
  }
  return 0;
}

// ------------------------------------------------------------------ output (System.out)

static void
out_append(Thread *t, const char *s, int n)
{
  for(int i = 0; i < n; i++){
    if(t->outlen >= (int)sizeof(t->outbuf)){ vm_write(t->outbuf, t->outlen); t->outlen = 0; }
    t->outbuf[t->outlen++] = s[i];
  }
}

static void
out_flush(Thread *t)
{
  if(t->outlen){ vm_write(t->outbuf, t->outlen); t->outlen = 0; }
}

// ------------------------------------------------------------------ threads & parking

static Thread *
new_thread(Object *obj)
{
  Thread *t = vm_alloc(sizeof(Thread));
  int id = __atomic_add_fetch(&nthreads, 1, __ATOMIC_SEQ_CST);
  if(id > MAX_THREADS) fatal("too many threads", 0);
  t->id = id;
  t->obj = obj;
  t->park = plat_sem_create();
  if(t->park < 0) fatal("out of kernel sync objects", 0);
  threads[id] = t;
  return t;
}

static void park(Thread *t)   { st_parks++; plat_sem_wait(t->park); }
static void unpark(Thread *t) { plat_sem_post(t->park); }

// Thread object layout (java/lang/Thread, 4 slots):
//   [0] Thread* (VM)  [1] Runnable target  [2] String name  [3] daemon
#define THR_VM(o)     (*(Thread **)&FIELDS(o)[0].p)
#define THR_TARGET(o) (FIELDS(o)[1].a)
#define THR_NAME(o)   (FIELDS(o)[2].a)

static const char *
thread_name(Thread *t)
{
  if(t->obj && THR_NAME(t->obj)) return STR_CHARS(THR_NAME(t->obj));
  return "?";
}

// ------------------------------------------------------------------ monitors

#define LW_THIN 1L
#define LW_INFLATED 2L
#define LW_TAG(w) ((w) & 3L)
#define THIN(id, rec) (((int64_t)(id) << 32) | ((int64_t)(rec) << 2) | LW_THIN)
#define THIN_OWNER(w) ((int)((uint64_t)(w) >> 32))
#define THIN_REC(w) ((int)(((w) >> 2) & 0x3fffffff))
#define THIN_REC_MAX 1000
#define SPIN_LIMIT 64

static void
q_append(Thread **h, Thread **tl, Thread *t)
{
  t->next = 0;
  if(*tl) (*tl)->next = t; else *h = t;
  *tl = t;
}

static Thread *
q_take(Thread **h, Thread **tl)
{
  Thread *t = *h;
  if(t){ *h = t->next; if(!*h) *tl = 0; t->next = 0; }
  return t;
}

static const char *
objdesc(Object *o, char *buf)
{
  // "ClassName@hex"
  char tmp[128];
  const char *n = dotted(o->cls->name, tmp, sizeof tmp);
  int i = 0;
  while(n[i] && i < 100){ buf[i] = n[i]; i++; }
  buf[i++] = '@';
  i += fmt_hex(buf + i, (uint64_t)o);
  buf[i] = 0;
  return buf;
}

static void
inflate(Thread *me, Object *o, int64_t w, const char *why)
{
  ObjectMonitor *m = vm_alloc(sizeof(ObjectMonitor));
  m->obj = o;
  if(LW_TAG(w) == LW_THIN){
    m->owner = threads[THIN_OWNER(w)];
    m->rec = THIN_REC(w);
  }
  int64_t expect = w;
  if(__atomic_compare_exchange_n(&o->lock, &expect, (int64_t)m | LW_INFLATED, 0,
                                 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)){
    st_inflations++;
    if(verbose){
      char b[160];
      say("[mjvm] %s: inflate %s (%s), thin owner=%s rec=%d -> ObjectMonitor %p\n",
          thread_name(me), objdesc(o, b), why, m->owner ? thread_name(m->owner) : "-", (int)m->rec, m);
    }
  }
}

static void
mon_enter(ObjectMonitor *m, Thread *me)
{
  spin_lock(&m->spin);
  if(!m->owner){ m->owner = me; m->rec = 1; spin_unlock(&m->spin); return; }
  if(m->owner == me){ m->rec++; spin_unlock(&m->spin); return; }
  me->saved_rec = 1;
  q_append(&m->entry_head, &m->entry_tail, me);
  if(verbose && ++me->nblock_traces <= 2){
    char b[160];
    say("[mjvm] %s: blocked on %s (owner %s), park%s\n", thread_name(me), objdesc(m->obj, b),
        thread_name(m->owner), me->nblock_traces == 2 ? "   (further blocks not shown)" : "");
  }
  spin_unlock(&m->spin);
  for(;;){
    park(me);
    spin_lock(&m->spin);
    int mine = m->owner == me;
    spin_unlock(&m->spin);
    if(mine) return;
  }
}

static Thread *
mon_handoff(ObjectMonitor *m)
{
  Thread *n = q_take(&m->entry_head, &m->entry_tail);
  m->owner = n;
  m->rec = n ? n->saved_rec : 0;
  return n;
}

// returns 0, or -1 with IllegalMonitorStateException pending
static int
monitor_enter(Thread *me, Object *o)
{
  int spins = 0;
  for(;;){
    int64_t w = __atomic_load_n(&o->lock, __ATOMIC_ACQUIRE);
    if(w == 0){
      // fast path: one CAS (LDAXR/STXR on ARMv8.0)
      if(__atomic_compare_exchange_n(&o->lock, &w, THIN(me->id, 1), 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return 0;
      continue;
    }
    if(LW_TAG(w) == LW_THIN){
      if(THIN_OWNER(w) == me->id){
        if(THIN_REC(w) < THIN_REC_MAX){
          if(__atomic_compare_exchange_n(&o->lock, &w, w + 4, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return 0;
        } else inflate(me, o, w, "recursion overflow");
        continue;
      }
      if(spins == 0) st_contended++;
      if(++spins < SPIN_LIMIT){ if(spins > 8) plat_yield(); continue; }
      inflate(me, o, w, "contention");
      continue;
    }
    mon_enter((ObjectMonitor *)(w & ~3L), me);
    return 0;
  }
}

static int
monitor_exit(Thread *me, Object *o)
{
  for(;;){
    int64_t w = __atomic_load_n(&o->lock, __ATOMIC_ACQUIRE);
    if(LW_TAG(w) == LW_THIN){
      if(THIN_OWNER(w) != me->id) break;
      int64_t nw = THIN_REC(w) > 1 ? w - 4 : 0;
      // release: everything written in the critical section becomes
      // visible before the lock word says "unlocked" (STLXR)
      if(__atomic_compare_exchange_n(&o->lock, &w, nw, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
        return 0;
      continue;
    }
    if(LW_TAG(w) == LW_INFLATED){
      ObjectMonitor *m = (ObjectMonitor *)(w & ~3L);
      spin_lock(&m->spin);
      if(m->owner != me){ spin_unlock(&m->spin); break; }
      if(--m->rec > 0){ spin_unlock(&m->spin); return 0; }
      Thread *n = mon_handoff(m);
      spin_unlock(&m->spin);
      if(n) unpark(n);
      return 0;
    }
    break;
  }
  throw_new(me, "java/lang/IllegalMonitorStateException", "current thread is not owner");
  return -1;
}

static ObjectMonitor *
owned_monitor(Thread *me, Object *o, const char *why)
{
  for(;;){
    int64_t w = __atomic_load_n(&o->lock, __ATOMIC_ACQUIRE);
    if(LW_TAG(w) == LW_INFLATED){
      ObjectMonitor *m = (ObjectMonitor *)(w & ~3L);
      if(m->owner == me) return m;
      break;
    }
    if(LW_TAG(w) != LW_THIN || THIN_OWNER(w) != me->id) break;
    inflate(me, o, w, why);
  }
  throw_new(me, "java/lang/IllegalMonitorStateException", "current thread is not owner");
  return 0;
}

static int
monitor_wait(Thread *me, Object *o)
{
  ObjectMonitor *m = owned_monitor(me, o, "wait");
  if(!m) return -1;
  st_waits++;
  spin_lock(&m->spin);
  me->saved_rec = m->rec;
  q_append(&m->wait_head, &m->wait_tail, me);
  Thread *n = mon_handoff(m);
  spin_unlock(&m->spin);
  if(n) unpark(n);
  for(;;){
    park(me);
    spin_lock(&m->spin);
    int mine = m->owner == me;
    spin_unlock(&m->spin);
    if(mine) return 0;
  }
}

static int
monitor_notify(Thread *me, Object *o, int all)
{
  ObjectMonitor *m = owned_monitor(me, o, all ? "notifyAll" : "notify");
  if(!m) return -1;
  spin_lock(&m->spin);
  Thread *w;
  while((w = q_take(&m->wait_head, &m->wait_tail)) != 0){
    q_append(&m->entry_head, &m->entry_tail, w);
    if(!all) break;
  }
  spin_unlock(&m->spin);
  return 0;
}

// ------------------------------------------------------------------ exceptions

// Throwable layout: [0] message (String)  [1] backtrace (Backtrace*)
typedef struct Backtrace { int n; Method *m[16]; int pc[16]; } Backtrace;

static void
throw_new(Thread *t, const char *cname, const char *msg)
{
  Class *c = load_class(t, cname);
  Object *e = new_object(c);
  if(msg) FIELDS(e)[0].a = new_string(msg, slen(msg));
  t->exc = e;
}

static void
record_frame(Object *e, Method *m, int pc)
{
  if(!e || !instance_of(e, C_Throwable)) return;
  Backtrace *bt = FIELDS(e)[1].p;
  if(!bt){ bt = vm_alloc(sizeof(Backtrace)); FIELDS(e)[1].p = bt; }
  if(bt->n < 16){ bt->m[bt->n] = m; bt->pc[bt->n] = pc; bt->n++; }
}

static void
print_exception(Thread *t, Object *e)
{
  char b1[128], b2[128];
  Object *msg = FIELDS(e)[0].a;
  out_flush(t);
  say("Exception in thread \"%s\" %s%s%s\n", thread_name(t), dotted(e->cls->name, b1, sizeof b1),
      msg ? ": " : "", msg ? STR_CHARS(msg) : "");
  Backtrace *bt = FIELDS(e)[1].p;
  for(int i = 0; bt && i < bt->n; i++)
    say("\tat %s.%s (bytecode pc %d)\n", dotted(bt->m[i]->cls->name, b2, sizeof b2), bt->m[i]->name, bt->pc[i]);
}

// ------------------------------------------------------------------ class file parsing

typedef struct Reader { uint8_t *p, *end; const char *name; } Reader;

static void
need(Reader *r, int n)
{
  if(r->p + n > r->end) fatal("truncated class file ", r->name);
}
static uint8_t u1(Reader *r) { need(r, 1); return *r->p++; }
static uint16_t u2(Reader *r) { need(r, 2); uint16_t v = (r->p[0] << 8) | r->p[1]; r->p += 2; return v; }
static uint32_t u4(Reader *r) { need(r, 4); uint32_t v = ((uint32_t)r->p[0] << 24) | (r->p[1] << 16) | (r->p[2] << 8) | r->p[3]; r->p += 4; return v; }

static char *
cp_utf8(Class *c, int idx)
{
  if(idx <= 0 || idx >= c->ncp || c->cp[idx].tag != 1) fatal("bad constant pool utf8 in ", c->name);
  return c->cp[idx].v.s;
}

static char *
cp_classname(Class *c, int idx)
{
  if(idx <= 0 || idx >= c->ncp || c->cp[idx].tag != 7) fatal("bad constant pool class in ", c->name);
  return cp_utf8(c, c->cp[idx].a);
}

// count argument slots of a method descriptor and find the return type
static void
parse_desc(const char *d, int *slots, char *ret)
{
  int n = 0;
  const char *p = d + 1;
  while(*p && *p != ')'){
    if(*p == 'J' || *p == 'D'){ n += 2; p++; continue; }
    n++;
    while(*p == '[') p++;
    if(*p == 'L'){ while(*p && *p != ';') p++; }
    p++;
  }
  *slots = n;
  *ret = p[1];
}

static Class *
parse_class(const char *cname, uint8_t *data, int len)
{
  Reader r = { data, data + len, cname };
  Class *c = vm_alloc(sizeof(Class));
  if(u4(&r) != 0xCAFEBABE) fatal("bad magic in ", cname);
  u2(&r);                                  // minor
  int major = u2(&r);
  if(major > 65) fatal("class file version too new: ", cname);
  c->ncp = u2(&r);
  c->cp = vm_alloc(sizeof(CPEntry) * c->ncp);
  for(int i = 1; i < c->ncp; i++){
    CPEntry *e = &c->cp[i];
    e->tag = u1(&r);
    switch(e->tag){
    case 1: { int n = u2(&r); need(&r, n); e->v.s = internn((char *)r.p, n); r.p += n; break; }
    case 3: e->v.i = (int32_t)u4(&r); break;                                 // Integer
    case 4: e->v.i = (int32_t)u4(&r); break;                                 // Float (unsupported use)
    case 5: case 6: { uint64_t hi = u4(&r), lo = u4(&r); e->v.j = (int64_t)(hi << 32 | lo); i++; break; }
    case 7: case 8: case 16: case 19: case 20: e->a = u2(&r); break;        // Class String MethodType Module Package
    case 9: case 10: case 11: case 12: case 17: case 18: e->a = u2(&r); e->b = u2(&r); break;
    case 15: u1(&r); e->a = u2(&r); break;                                    // MethodHandle
    default: fatal("unknown constant pool tag in ", cname);
    }
  }
  c->flags = u2(&r);
  c->name = intern(cp_classname(c, u2(&r)));
  int sup = u2(&r);
  c->supername = sup ? cp_classname(c, sup) : 0;
  c->nifaces = u2(&r);
  c->ifnames = vm_alloc(sizeof(char *) * (c->nifaces + 1));
  for(int i = 0; i < c->nifaces; i++) c->ifnames[i] = cp_classname(c, u2(&r));

  c->nfields = u2(&r);
  c->fields = vm_alloc(sizeof(Field) * (c->nfields + 1));
  for(int i = 0; i < c->nfields; i++){
    Field *f = &c->fields[i];
    f->flags = u2(&r);
    f->name = cp_utf8(c, u2(&r));
    f->desc = cp_utf8(c, u2(&r));
    f->cls = c;
    int na = u2(&r);
    for(int k = 0; k < na; k++){ u2(&r); uint32_t l = u4(&r); need(&r, l); r.p += l; }
  }
  c->nmethods = u2(&r);
  c->methods = vm_alloc(sizeof(Method) * (c->nmethods + 1));
  for(int i = 0; i < c->nmethods; i++){
    Method *m = &c->methods[i];
    m->flags = u2(&r);
    m->name = cp_utf8(c, u2(&r));
    m->desc = cp_utf8(c, u2(&r));
    m->cls = c;
    m->vindex = -1;
    parse_desc(m->desc, &m->argslots, &m->rettype);
    if(!(m->flags & ACC_STATIC)) m->argslots++;
    int na = u2(&r);
    for(int k = 0; k < na; k++){
      char *an = cp_utf8(c, u2(&r));
      uint32_t l = u4(&r);
      need(&r, l);
      uint8_t *next = r.p + l;
      if(strcmp(an, "Code") == 0){
        m->max_stack = u2(&r);
        m->max_locals = u2(&r);
        m->code_len = u4(&r);
        need(&r, m->code_len);
        m->code = r.p;
        r.p += m->code_len;
        m->nexc = u2(&r);
        m->exc = vm_alloc(sizeof(ExcEntry) * (m->nexc + 1));
        for(int x = 0; x < m->nexc; x++){
          m->exc[x].start = u2(&r); m->exc[x].end = u2(&r);
          m->exc[x].handler = u2(&r); m->exc[x].type = u2(&r);
        }
      }
      r.p = next;
    }
  }
  return c;
}

// ------------------------------------------------------------------ linking

static Field *
find_field(Class *c, const char *name, const char *desc)
{
  for(; c; c = c->super)
    for(int i = 0; i < c->nfields; i++)
      if(c->fields[i].name == name && (!desc || c->fields[i].desc == desc)) return &c->fields[i];
  return 0;
}

static Method *
find_method(Class *c, const char *name, const char *desc)
{
  for(Class *k = c; k; k = k->super)
    for(int i = 0; i < k->nmethods; i++)
      if(k->methods[i].name == name && k->methods[i].desc == desc) return &k->methods[i];
  return 0;
}

static void
link_class(Thread *t, Class *c)
{
  if(c->supername){
    c->super = load_class(t, c->supername);
    if(!c->super) fatal("cannot load superclass ", c->supername);
  }
  for(int i = 0; i < c->nifaces; i++){
    c->ifnames[i] = intern(c->ifnames[i]);
    load_class(t, c->ifnames[i]);
  }
  c->ninst = c->super ? c->super->ninst : 0;
  for(int i = 0; i < c->nfields; i++){
    Field *f = &c->fields[i];
    if(f->flags & ACC_STATIC) f->index = c->nstatic++;
    else f->index = c->ninst++;
  }
  c->statics = vm_alloc(sizeof(Slot) * (c->nstatic + 1));
  // vtable: inherit, then override by (name, descriptor)
  int cap = (c->super ? c->super->vtlen : 0) + c->nmethods;
  c->vtable = vm_alloc(sizeof(Method *) * (cap + 1));
  if(c->super){
    memcpy(c->vtable, c->super->vtable, sizeof(Method *) * c->super->vtlen);
    c->vtlen = c->super->vtlen;
  }
  for(int i = 0; i < c->nmethods; i++){
    Method *m = &c->methods[i];
    if((m->flags & ACC_STATIC) || m->name[0] == '<') continue;
    int slot = -1;
    for(int k = 0; k < c->vtlen; k++)
      if(c->vtable[k]->name == m->name && c->vtable[k]->desc == m->desc){ slot = k; break; }
    if(slot < 0) slot = c->vtlen++;
    c->vtable[slot] = m;
    m->vindex = slot;
  }
  if(C_Class){
    c->mirror = new_object(C_Class);
    FIELDS(c->mirror)[0].p = c;
  }
  c->state = CLS_LINKED;
}

// ------------------------------------------------------------------ built-in classes (natives)

typedef struct NativeDef { const char *name, *desc; int flags; NativeFn fn; } NativeDef;

static Class *
define_builtin(const char *name, const char *super, int ninst_own, const NativeDef *defs)
{
  Class *c = vm_alloc(sizeof(Class));
  c->name = intern(name);
  c->supername = super ? intern(super) : 0;
  c->flags = ACC_PUBLIC;
  int n = 0;
  while(defs && defs[n].name) n++;
  c->nmethods = n;
  c->methods = vm_alloc(sizeof(Method) * (n + 1));
  for(int i = 0; i < n; i++){
    Method *m = &c->methods[i];
    m->name = intern(defs[i].name);
    m->desc = intern(defs[i].desc);
    m->flags = defs[i].flags | ACC_NATIVE | ACC_PUBLIC;
    m->cls = c;
    m->native = defs[i].fn;
    m->vindex = -1;
    parse_desc(m->desc, &m->argslots, &m->rettype);
    if(!(m->flags & ACC_STATIC)) m->argslots++;
  }
  // hidden instance slots are modelled as anonymous fields
  c->nfields = ninst_own;
  c->fields = vm_alloc(sizeof(Field) * (ninst_own + 1));
  for(int i = 0; i < ninst_own; i++){
    c->fields[i].name = intern("$hidden");
    c->fields[i].desc = intern("J");
    c->fields[i].cls = c;
  }
  spin_lock(&class_lock);
  c->next = classes;
  classes = c;
  spin_unlock(&class_lock);
  link_class(0, c);
  c->state = CLS_INITIALIZED;
  return c;
}

static int
call_virtual(Thread *t, Object *recv, const char *name, const char *desc, Slot *ret)
{
  Method *m = find_method(recv->cls, intern(name), intern(desc));
  if(!m) fatal("no such method ", name);
  Slot a[1];
  a[0].a = recv;
  return invoke(t, m, a, ret);
}

// String.valueOf(Object): null -> "null", else o.toString()
static Object *
to_jstring(Thread *t, Object *o)
{
  if(!o) return intern_string("null", 4);
  if(o->cls == C_String) return o;
  Slot r;
  if(call_virtual(t, o, "toString", "()Ljava/lang/String;", &r) < 0) return 0;
  return r.a ? r.a : intern_string("null", 4);
}

// ---- java.lang.Object
static void n_obj_init(Thread *t, Slot *a, Slot *r) { }
static void n_obj_hash(Thread *t, Slot *a, Slot *r) { r->i = (int32_t)((uint64_t)a[0].a >> 4); }
static void n_obj_equals(Thread *t, Slot *a, Slot *r) { r->i = a[0].a == a[1].a; }
static void
n_obj_tostring(Thread *t, Slot *a, Slot *r)
{
  char b[160];
  objdesc(a[0].a, b);
  r->a = new_string(b, slen(b));
}
static void n_obj_wait(Thread *t, Slot *a, Slot *r) { monitor_wait(t, a[0].a); }
static void n_obj_waitj(Thread *t, Slot *a, Slot *r) { monitor_wait(t, a[0].a); }   // timeout ignored
static void n_obj_notify(Thread *t, Slot *a, Slot *r) { monitor_notify(t, a[0].a, 0); }
static void n_obj_notifyall(Thread *t, Slot *a, Slot *r) { monitor_notify(t, a[0].a, 1); }
static void
n_obj_getclass(Thread *t, Slot *a, Slot *r)
{
  r->a = a[0].a->cls->mirror;
}
static const NativeDef object_natives[] = {
  {"<init>", "()V", 0, n_obj_init},
  {"hashCode", "()I", 0, n_obj_hash},
  {"equals", "(Ljava/lang/Object;)Z", 0, n_obj_equals},
  {"toString", "()Ljava/lang/String;", 0, n_obj_tostring},
  {"wait", "()V", ACC_FINAL, n_obj_wait},
  {"wait", "(J)V", ACC_FINAL, n_obj_waitj},
  {"notify", "()V", ACC_FINAL, n_obj_notify},
  {"notifyAll", "()V", ACC_FINAL, n_obj_notifyall},
  {"getClass", "()Ljava/lang/Class;", ACC_FINAL, n_obj_getclass},
  {0}
};

// ---- java.lang.Class (only as a lockable mirror + getName)
static void
n_class_getname(Thread *t, Slot *a, Slot *r)
{
  char b[160];
  Class *c = FIELDS(a[0].a)[0].p;
  dotted(c->name, b, sizeof b);
  r->a = new_string(b, slen(b));
}
static const NativeDef class_natives[] = {
  {"getName", "()Ljava/lang/String;", 0, n_class_getname},
  {0}
};

// ---- java.lang.String
static void n_str_length(Thread *t, Slot *a, Slot *r) { r->i = STR_LEN(a[0].a); }
static void
n_str_charat(Thread *t, Slot *a, Slot *r)
{
  int i = a[1].i;
  if(i < 0 || i >= STR_LEN(a[0].a)){ throw_new(t, "java/lang/StringIndexOutOfBoundsException", 0); return; }
  r->i = (uint8_t)STR_CHARS(a[0].a)[i];
}
static void
n_str_equals(Thread *t, Slot *a, Slot *r)
{
  Object *x = a[0].a, *y = a[1].a;
  r->i = y && y->cls == C_String && STR_LEN(x) == STR_LEN(y) && sneq(STR_CHARS(x), STR_CHARS(y), STR_LEN(x));
}
static void n_str_tostring(Thread *t, Slot *a, Slot *r) { r->a = a[0].a; }
static void n_str_hash(Thread *t, Slot *a, Slot *r)
{
  int32_t h = 0;
  for(int i = 0; i < STR_LEN(a[0].a); i++) h = 31 * h + (uint8_t)STR_CHARS(a[0].a)[i];
  r->i = h;
}
static void n_str_intern(Thread *t, Slot *a, Slot *r) { r->a = intern_string(STR_CHARS(a[0].a), STR_LEN(a[0].a)); }
static void
n_str_valueof_i(Thread *t, Slot *a, Slot *r)
{
  char b[24];
  r->a = new_string(b, fmt_long(b, a[0].i));
}
static void n_str_valueof_o(Thread *t, Slot *a, Slot *r) { r->a = to_jstring(t, a[0].a); }
static const NativeDef string_natives[] = {
  {"length", "()I", 0, n_str_length},
  {"charAt", "(I)C", 0, n_str_charat},
  {"equals", "(Ljava/lang/Object;)Z", 0, n_str_equals},
  {"hashCode", "()I", 0, n_str_hash},
  {"toString", "()Ljava/lang/String;", 0, n_str_tostring},
  {"intern", "()Ljava/lang/String;", 0, n_str_intern},
  {"valueOf", "(I)Ljava/lang/String;", ACC_STATIC, n_str_valueof_i},
  {"valueOf", "(Ljava/lang/Object;)Ljava/lang/String;", ACC_STATIC, n_str_valueof_o},
  {0}
};

// ---- java.lang.StringBuilder: [0] char* buf  [1] len  [2] cap
static void
sb_add(Object *sb, const char *s, int n)
{
  Slot *f = FIELDS(sb);
  int len = f[1].i, cap = f[2].i;
  if(len + n + 1 > cap){
    int nc = cap ? cap * 2 : 32;
    while(nc < len + n + 1) nc *= 2;
    char *nb = vm_alloc(nc);
    if(len) memcpy(nb, f[0].p, len);
    f[0].p = nb;
    f[2].i = nc;
  }
  memcpy((char *)f[0].p + len, s, n);
  f[1].i = len + n;
}
static void n_sb_init(Thread *t, Slot *a, Slot *r) { }
static void
n_sb_init_s(Thread *t, Slot *a, Slot *r)
{
  Object *s = to_jstring(t, a[1].a);
  if(s) sb_add(a[0].a, STR_CHARS(s), STR_LEN(s));
}
static void
n_sb_append_s(Thread *t, Slot *a, Slot *r)
{
  Object *s = a[1].a ? a[1].a : intern_string("null", 4);
  sb_add(a[0].a, STR_CHARS(s), STR_LEN(s));
  r->a = a[0].a;
}
static void
n_sb_append_o(Thread *t, Slot *a, Slot *r)
{
  Object *s = to_jstring(t, a[1].a);
  if(!s) return;
  sb_add(a[0].a, STR_CHARS(s), STR_LEN(s));
  r->a = a[0].a;
}
static void n_sb_append_i(Thread *t, Slot *a, Slot *r) { char b[24]; sb_add(a[0].a, b, fmt_long(b, a[1].i)); r->a = a[0].a; }
static void n_sb_append_j(Thread *t, Slot *a, Slot *r) { char b[24]; sb_add(a[0].a, b, fmt_long(b, a[1].j)); r->a = a[0].a; }
static void n_sb_append_c(Thread *t, Slot *a, Slot *r) { char c = (char)a[1].i; sb_add(a[0].a, &c, 1); r->a = a[0].a; }
static void n_sb_append_z(Thread *t, Slot *a, Slot *r) { if(a[1].i) sb_add(a[0].a, "true", 4); else sb_add(a[0].a, "false", 5); r->a = a[0].a; }
static void n_sb_tostring(Thread *t, Slot *a, Slot *r) { Slot *f = FIELDS(a[0].a); r->a = new_string(f[0].p ? f[0].p : "", f[1].i); }
static void n_sb_length(Thread *t, Slot *a, Slot *r) { r->i = FIELDS(a[0].a)[1].i; }
static const NativeDef sb_natives[] = {
  {"<init>", "()V", 0, n_sb_init},
  {"<init>", "(Ljava/lang/String;)V", 0, n_sb_init_s},
  {"append", "(Ljava/lang/String;)Ljava/lang/StringBuilder;", 0, n_sb_append_s},
  {"append", "(Ljava/lang/Object;)Ljava/lang/StringBuilder;", 0, n_sb_append_o},
  {"append", "(I)Ljava/lang/StringBuilder;", 0, n_sb_append_i},
  {"append", "(J)Ljava/lang/StringBuilder;", 0, n_sb_append_j},
  {"append", "(C)Ljava/lang/StringBuilder;", 0, n_sb_append_c},
  {"append", "(Z)Ljava/lang/StringBuilder;", 0, n_sb_append_z},
  {"toString", "()Ljava/lang/String;", 0, n_sb_tostring},
  {"length", "()I", 0, n_sb_length},
  {0}
};

// ---- java.io.PrintStream (System.out); one write() per println
static void
ps_str(Thread *t, Object *s)
{
  if(!s) s = intern_string("null", 4);
  out_append(t, STR_CHARS(s), STR_LEN(s));
}
static void n_ps_nl(Thread *t) { out_append(t, "\n", 1); out_flush(t); }
static void n_ps_print_s(Thread *t, Slot *a, Slot *r) { ps_str(t, a[1].a); }
static void n_ps_print_o(Thread *t, Slot *a, Slot *r) { Object *s = to_jstring(t, a[1].a); if(s) ps_str(t, s); }
static void n_ps_print_i(Thread *t, Slot *a, Slot *r) { char b[24]; out_append(t, b, fmt_long(b, a[1].i)); }
static void n_ps_print_j(Thread *t, Slot *a, Slot *r) { char b[24]; out_append(t, b, fmt_long(b, a[1].j)); }
static void n_ps_print_c(Thread *t, Slot *a, Slot *r) { char c = (char)a[1].i; out_append(t, &c, 1); }
static void n_ps_print_z(Thread *t, Slot *a, Slot *r) { if(a[1].i) out_append(t, "true", 4); else out_append(t, "false", 5); }
static void n_ps_println(Thread *t, Slot *a, Slot *r) { n_ps_nl(t); }
static void n_ps_println_s(Thread *t, Slot *a, Slot *r) { n_ps_print_s(t, a, r); n_ps_nl(t); }
static void n_ps_println_o(Thread *t, Slot *a, Slot *r) { n_ps_print_o(t, a, r); if(!t->exc) n_ps_nl(t); }
static void n_ps_println_i(Thread *t, Slot *a, Slot *r) { n_ps_print_i(t, a, r); n_ps_nl(t); }
static void n_ps_println_j(Thread *t, Slot *a, Slot *r) { n_ps_print_j(t, a, r); n_ps_nl(t); }
static void n_ps_println_c(Thread *t, Slot *a, Slot *r) { n_ps_print_c(t, a, r); n_ps_nl(t); }
static void n_ps_println_z(Thread *t, Slot *a, Slot *r) { n_ps_print_z(t, a, r); n_ps_nl(t); }
static void n_ps_flush(Thread *t, Slot *a, Slot *r) { out_flush(t); }
static const NativeDef ps_natives[] = {
  {"print", "(Ljava/lang/String;)V", 0, n_ps_print_s},
  {"print", "(Ljava/lang/Object;)V", 0, n_ps_print_o},
  {"print", "(I)V", 0, n_ps_print_i},
  {"print", "(J)V", 0, n_ps_print_j},
  {"print", "(C)V", 0, n_ps_print_c},
  {"print", "(Z)V", 0, n_ps_print_z},
  {"println", "()V", 0, n_ps_println},
  {"println", "(Ljava/lang/String;)V", 0, n_ps_println_s},
  {"println", "(Ljava/lang/Object;)V", 0, n_ps_println_o},
  {"println", "(I)V", 0, n_ps_println_i},
  {"println", "(J)V", 0, n_ps_println_j},
  {"println", "(C)V", 0, n_ps_println_c},
  {"println", "(Z)V", 0, n_ps_println_z},
  {"flush", "()V", 0, n_ps_flush},
  {0}
};

// ---- java.lang.System
static void n_sys_millis(Thread *t, Slot *a, Slot *r) { r->j = plat_now_us() / 1000; }
static void n_sys_nanos(Thread *t, Slot *a, Slot *r) { r->j = plat_now_us() * 1000; }
static void n_sys_exit(Thread *t, Slot *a, Slot *r) { out_flush(t); plat_exit(a[0].i); }
static void
n_sys_identityhash(Thread *t, Slot *a, Slot *r)
{
  r->i = (int32_t)((uint64_t)a[0].a >> 4);
}
static const NativeDef system_natives[] = {
  {"currentTimeMillis", "()J", ACC_STATIC, n_sys_millis},
  {"nanoTime", "()J", ACC_STATIC, n_sys_nanos},
  {"exit", "(I)V", ACC_STATIC, n_sys_exit},
  {"identityHashCode", "(Ljava/lang/Object;)I", ACC_STATIC, n_sys_identityhash},
  {0}
};

// ---- java.lang.Math
static void n_math_abs(Thread *t, Slot *a, Slot *r) { r->i = a[0].i < 0 ? -a[0].i : a[0].i; }
static void n_math_max(Thread *t, Slot *a, Slot *r) { r->i = a[0].i > a[1].i ? a[0].i : a[1].i; }
static void n_math_min(Thread *t, Slot *a, Slot *r) { r->i = a[0].i < a[1].i ? a[0].i : a[1].i; }
static const NativeDef math_natives[] = {
  {"abs", "(I)I", ACC_STATIC, n_math_abs},
  {"max", "(II)I", ACC_STATIC, n_math_max},
  {"min", "(II)I", ACC_STATIC, n_math_min},
  {0}
};

// ---- java.lang.Throwable: [0] message [1] backtrace
static void n_thr_init(Thread *t, Slot *a, Slot *r) { }
static void n_thr_init_s(Thread *t, Slot *a, Slot *r) { FIELDS(a[0].a)[0].a = a[1].a; }
static void n_thr_getmessage(Thread *t, Slot *a, Slot *r) { r->a = FIELDS(a[0].a)[0].a; }
static void
n_thr_tostring(Thread *t, Slot *a, Slot *r)
{
  char b[400];
  dotted(a[0].a->cls->name, b, 200);
  int n = slen(b);
  Object *m = FIELDS(a[0].a)[0].a;
  if(m){
    b[n++] = ':'; b[n++] = ' ';
    for(int i = 0; i < STR_LEN(m) && n < 390; i++) b[n++] = STR_CHARS(m)[i];
  }
  r->a = new_string(b, n);
}
static void n_thr_print(Thread *t, Slot *a, Slot *r) { print_exception(t, a[0].a); }
static const NativeDef throwable_natives[] = {
  {"<init>", "()V", 0, n_thr_init},
  {"<init>", "(Ljava/lang/String;)V", 0, n_thr_init_s},
  {"getMessage", "()Ljava/lang/String;", 0, n_thr_getmessage},
  {"toString", "()Ljava/lang/String;", 0, n_thr_tostring},
  {"printStackTrace", "()V", 0, n_thr_print},
  {0}
};

// ---- java.lang.Thread
static void thread_main(void *arg);

static void
thread_set_name(Object *o, Thread *t, const char *name)
{
  char b[32];
  if(name){ THR_NAME(o) = new_string(name, slen(name)); return; }
  static volatile int seq;
  int n = __atomic_fetch_add(&seq, 1, __ATOMIC_SEQ_CST);
  memcpy(b, "Thread-", 7);
  int k = 7 + fmt_long(b + 7, n);
  THR_NAME(o) = new_string(b, k);
}
static void n_thread_init(Thread *t, Slot *a, Slot *r) { thread_set_name(a[0].a, t, 0); }
static void n_thread_init_r(Thread *t, Slot *a, Slot *r) { thread_set_name(a[0].a, t, 0); THR_TARGET(a[0].a) = a[1].a; }
static void n_thread_init_s(Thread *t, Slot *a, Slot *r) { THR_NAME(a[0].a) = a[1].a; }
static void n_thread_init_rs(Thread *t, Slot *a, Slot *r) { THR_TARGET(a[0].a) = a[1].a; THR_NAME(a[0].a) = a[2].a; }

static void
n_thread_start(Thread *t, Slot *a, Slot *r)
{
  Object *o = a[0].a;
  if(THR_VM(o)){ throw_new(t, "java/lang/IllegalThreadStateException", 0); return; }
  Thread *nt = new_thread(o);
  nt->entry = find_method(o->cls, intern("run"), intern("()V"));
  nt->alive = 1;
  __atomic_store_n(&THR_VM(o), nt, __ATOMIC_RELEASE);
  char *stack = vm_alloc(VM_THREAD_STACK);
  // Thread.start() happens-before everything the new thread does: the
  // kernel's clone path and our release store above publish the object.
  long h = plat_thread_start(thread_main, nt, stack, VM_THREAD_STACK);
  if(h < 0) fatal("thread_clone failed", 0);
  nt->handle = h;
  __atomic_store_n(&nt->started, 1, __ATOMIC_RELEASE);
  if(verbose) say("[mjvm] %s: start %s (VM thread T%d, kernel tid %l)\n", thread_name(t), thread_name(nt), nt->id, h);
}

static void
n_thread_run(Thread *t, Slot *a, Slot *r)
{
  Object *target = THR_TARGET(a[0].a);
  if(target){ Slot rr; call_virtual(t, target, "run", "()V", &rr); }
}

// join() is ordinary Java in the JDK:  synchronized(this){ while(isAlive()) wait(0); }
static void
n_thread_join(Thread *t, Slot *a, Slot *r)
{
  Object *o = a[0].a;
  Thread *th = __atomic_load_n(&THR_VM(o), __ATOMIC_ACQUIRE);
  if(!th) return;
  if(monitor_enter(t, o) < 0) return;
  while(__atomic_load_n(&th->alive, __ATOMIC_ACQUIRE))
    if(monitor_wait(t, o) < 0) return;
  monitor_exit(t, o);
}
static void
n_thread_isalive(Thread *t, Slot *a, Slot *r)
{
  Thread *th = __atomic_load_n(&THR_VM(a[0].a), __ATOMIC_ACQUIRE);
  r->i = th && th->alive;
}
static void n_thread_getname(Thread *t, Slot *a, Slot *r) { r->a = THR_NAME(a[0].a); }
static void n_thread_setname(Thread *t, Slot *a, Slot *r) { THR_NAME(a[0].a) = a[1].a; }
static void n_thread_setdaemon(Thread *t, Slot *a, Slot *r) { FIELDS(a[0].a)[3].i = a[1].i; }
static void n_thread_current(Thread *t, Slot *a, Slot *r) { r->a = t->obj; }
static void n_thread_yield(Thread *t, Slot *a, Slot *r) { plat_yield(); }
static void n_thread_sleep(Thread *t, Slot *a, Slot *r) { out_flush(t); plat_sleep_ms(a[0].j); }
static void n_thread_getid(Thread *t, Slot *a, Slot *r) { Thread *th = THR_VM(a[0].a); r->j = th ? th->id : 0; }
static const NativeDef thread_natives[] = {
  {"<init>", "()V", 0, n_thread_init},
  {"<init>", "(Ljava/lang/Runnable;)V", 0, n_thread_init_r},
  {"<init>", "(Ljava/lang/String;)V", 0, n_thread_init_s},
  {"<init>", "(Ljava/lang/Runnable;Ljava/lang/String;)V", 0, n_thread_init_rs},
  {"start", "()V", 0, n_thread_start},
  {"run", "()V", 0, n_thread_run},
  {"join", "()V", ACC_FINAL, n_thread_join},
  {"isAlive", "()Z", ACC_FINAL, n_thread_isalive},
  {"getName", "()Ljava/lang/String;", ACC_FINAL, n_thread_getname},
  {"setName", "(Ljava/lang/String;)V", ACC_FINAL, n_thread_setname},
  {"setDaemon", "(Z)V", ACC_FINAL, n_thread_setdaemon},
  {"getId", "()J", 0, n_thread_getid},
  {"currentThread", "()Ljava/lang/Thread;", ACC_STATIC, n_thread_current},
  {"yield", "()V", ACC_STATIC, n_thread_yield},
  {"sleep", "(J)V", ACC_STATIC, n_thread_sleep},
  {0}
};

static void
define_builtins(void)
{
  C_Object = define_builtin("java/lang/Object", 0, 0, object_natives);
  C_Class = define_builtin("java/lang/Class", "java/lang/Object", 1, class_natives);
  // every class created so far gets its mirror now that java/lang/Class exists
  for(Class *c = classes; c; c = c->next)
    if(!c->mirror){ c->mirror = new_object(C_Class); FIELDS(c->mirror)[0].p = c; }
  C_String = define_builtin("java/lang/String", "java/lang/Object", 2, string_natives);
  C_StringBuilder = define_builtin("java/lang/StringBuilder", "java/lang/Object", 3, sb_natives);
  define_builtin("java/lang/Runnable", "java/lang/Object", 0, 0)->flags |= ACC_INTERFACE;
  C_Thread = define_builtin("java/lang/Thread", "java/lang/Object", 4, thread_natives);
  Class *ps = define_builtin("java/io/PrintStream", "java/lang/Object", 0, ps_natives);
  Class *sys = define_builtin("java/lang/System", "java/lang/Object", 0, system_natives);
  // System.out is a static field
  sys->nfields = 1;
  sys->fields = vm_alloc(sizeof(Field));
  sys->fields[0].name = intern("out");
  sys->fields[0].desc = intern("Ljava/io/PrintStream;");
  sys->fields[0].flags = ACC_STATIC | ACC_PUBLIC;
  sys->fields[0].cls = sys;
  sys->fields[0].index = 0;
  sys->statics = vm_alloc(sizeof(Slot));
  sys->statics[0].a = new_object(ps);
  define_builtin("java/lang/Math", "java/lang/Object", 0, math_natives);
  C_Throwable = define_builtin("java/lang/Throwable", "java/lang/Object", 2, throwable_natives);
  static const struct { const char *n, *s; } exc[] = {
    {"java/lang/Exception", "java/lang/Throwable"},
    {"java/lang/Error", "java/lang/Throwable"},
    {"java/lang/RuntimeException", "java/lang/Exception"},
    {"java/lang/InterruptedException", "java/lang/Exception"},
    {"java/lang/IllegalMonitorStateException", "java/lang/RuntimeException"},
    {"java/lang/IllegalStateException", "java/lang/RuntimeException"},
    {"java/lang/IllegalArgumentException", "java/lang/RuntimeException"},
    {"java/lang/IllegalThreadStateException", "java/lang/IllegalArgumentException"},
    {"java/lang/ArithmeticException", "java/lang/RuntimeException"},
    {"java/lang/NullPointerException", "java/lang/RuntimeException"},
    {"java/lang/ClassCastException", "java/lang/RuntimeException"},
    {"java/lang/NegativeArraySizeException", "java/lang/RuntimeException"},
    {"java/lang/UnsupportedOperationException", "java/lang/RuntimeException"},
    {"java/lang/IndexOutOfBoundsException", "java/lang/RuntimeException"},
    {"java/lang/ArrayIndexOutOfBoundsException", "java/lang/IndexOutOfBoundsException"},
    {"java/lang/StringIndexOutOfBoundsException", "java/lang/IndexOutOfBoundsException"},
    {"java/lang/StackOverflowError", "java/lang/Error"},
    {0, 0}};
  // the <init> natives are inherited by lookup (resolution walks superclasses)
  for(int i = 0; exc[i].n; i++) define_builtin(exc[i].n, exc[i].s, 0, 0);
}

// ------------------------------------------------------------------ class loading & init

static volatile int load_lock;
static Thread *load_owner;
static int load_depth;

static void
load_lock_acquire(Thread *t)
{
  if(t && load_owner == t){ load_depth++; return; }
  spin_lock(&load_lock);
  load_owner = t;
  load_depth = 1;
}

static void
load_lock_release(Thread *t)
{
  if(--load_depth == 0){ load_owner = 0; spin_unlock(&load_lock); }
}

static Class *
find_loaded(const char *name)
{
  spin_lock(&class_lock);
  Class *c = classes;
  for(; c; c = c->next) if(c->name == name) break;
  spin_unlock(&class_lock);
  return c;
}

static Class *
array_class(Thread *t, const char *elemdesc)
{
  char b[256];
  int n = slen(elemdesc);
  if(n > 250) fatal("array type too long", 0);
  b[0] = '[';
  memcpy(b + 1, elemdesc, n + 1);
  char *name = intern(b);
  Class *c = find_loaded(name);
  if(c) return c;
  load_lock_acquire(t);
  c = find_loaded(name);
  if(!c){
    c = vm_alloc(sizeof(Class));
    c->name = name;
    c->supername = C_Object->name;
    c->elemdesc = intern(elemdesc);
    link_class(t, c);
    c->state = CLS_INITIALIZED;
    spin_lock(&class_lock);
    c->next = classes;
    classes = c;
    spin_unlock(&class_lock);
  }
  load_lock_release(t);
  return c;
}

static Class *
load_class(Thread *t, const char *name)
{
  name = intern(name);
  Class *c = find_loaded(name);
  if(c) return c;
  if(name[0] == '[') return array_class(t, name + 1);
  load_lock_acquire(t);
  c = find_loaded(name);
  if(!c){
    char path[512];
    int pl = slen(classpath), nl = slen(name);
    if(pl + nl + 8 > (int)sizeof path) fatal("class name too long: ", name);
    memcpy(path, classpath, pl);
    path[pl] = '/';
    memcpy(path + pl + 1, name, nl);
    memcpy(path + pl + 1 + nl, ".class", 7);
    char *data;
    int len;
    if(plat_read_file(path, &data, &len) < 0){
      load_lock_release(t);
      fatal("java.lang.NoClassDefFoundError: ", name);
    }
    c = parse_class(name, (uint8_t *)data, len);
    if(c->name != name) fatal("class file contains wrong class: ", path);
    link_class(t, c);
    spin_lock(&class_lock);
    c->next = classes;
    classes = c;
    spin_unlock(&class_lock);
    if(verbose) say("[mjvm] loaded %s from %s (%d methods, %d fields, cp %d)\n", name, path, c->nmethods, c->nfields, c->ncp);
  }
  load_lock_release(t);
  return c;
}

// JVMS 5.5: class initialization is itself synchronized on the class's
// initialization lock -- here literally the mirror's monitor + wait/notifyAll.
static int
init_class(Thread *t, Class *c)
{
  if(__atomic_load_n(&c->state, __ATOMIC_ACQUIRE) == CLS_INITIALIZED) return 0;
  Object *lk = c->mirror;
  monitor_enter(t, lk);
  while(c->state == CLS_INITIALIZING && c->init_thread != t) monitor_wait(t, lk);
  if(c->state == CLS_INITIALIZED || c->init_thread == t){ monitor_exit(t, lk); return 0; }
  c->state = CLS_INITIALIZING;
  c->init_thread = t;
  monitor_exit(t, lk);

  int rc = 0;
  if(c->super) rc = init_class(t, c->super);
  Method *clinit = 0;
  for(int i = 0; i < c->nmethods; i++)
    if(strcmp(c->methods[i].name, "<clinit>") == 0) clinit = &c->methods[i];
  if(!rc && clinit){
    Slot r;
    rc = invoke(t, clinit, 0, &r);
  }
  monitor_enter(t, lk);
  __atomic_store_n(&c->state, rc ? CLS_ERROR : CLS_INITIALIZED, __ATOMIC_RELEASE);
  c->init_thread = 0;
  if(LW_TAG(lk->lock) == LW_INFLATED) monitor_notify(t, lk, 1);   // waiters imply inflated
  monitor_exit(t, lk);
  return rc;
}

// ------------------------------------------------------------------ constant pool resolution

static Class *
resolve_class(Thread *t, Class *c, int idx)
{
  CPEntry *e = &c->cp[idx];
  Class *k = __atomic_load_n((Class **)&e->resolved, __ATOMIC_ACQUIRE);
  if(k) return k;
  k = load_class(t, cp_utf8(c, e->a));
  __atomic_store_n((Class **)&e->resolved, k, __ATOMIC_RELEASE);
  return k;
}

static void
nat(Class *c, int idx, char **name, char **desc)
{
  CPEntry *n = &c->cp[c->cp[idx].b];
  *name = cp_utf8(c, n->a);
  *desc = cp_utf8(c, n->b);
}

static Field *
resolve_field(Thread *t, Class *c, int idx)
{
  CPEntry *e = &c->cp[idx];
  Field *f = __atomic_load_n((Field **)&e->resolved, __ATOMIC_ACQUIRE);
  if(f) return f;
  Class *k = resolve_class(t, c, e->a);
  char *name, *desc;
  nat(c, idx, &name, &desc);
  f = find_field(k, name, desc);
  if(!f) fatal("java.lang.NoSuchFieldError: ", name);
  __atomic_store_n((Field **)&e->resolved, f, __ATOMIC_RELEASE);
  return f;
}

static Method *
resolve_method(Thread *t, Class *c, int idx)
{
  CPEntry *e = &c->cp[idx];
  Method *m = __atomic_load_n((Method **)&e->resolved, __ATOMIC_ACQUIRE);
  if(m) return m;
  Class *k = resolve_class(t, c, e->a);
  char *name, *desc;
  nat(c, idx, &name, &desc);
  m = find_method(k, name, desc);
  if(!m && strcmp(name, "<init>") == 0 && k->super)
    m = find_method(k->super, name, desc);        // built-in exception ctors
  if(!m){
    char b[300];
    int n = slen(k->name);
    memcpy(b, k->name, n); b[n++] = '.';
    int nn = slen(name); memcpy(b + n, name, nn); n += nn;
    int dn = slen(desc); if(n + dn < 290){ memcpy(b + n, desc, dn); n += dn; }
    b[n] = 0;
    fatal("java.lang.NoSuchMethodError: ", b);
  }
  __atomic_store_n((Method **)&e->resolved, m, __ATOMIC_RELEASE);
  return m;
}

// ------------------------------------------------------------------ the interpreter

static void
aioobe(Thread *t, int i, int len)
{
  char b[96];
  int n = 0;
  memcpy(b, "Index ", 6); n = 6;
  n += fmt_long(b + n, i);
  memcpy(b + n, " out of bounds for length ", 26); n += 26;
  n += fmt_long(b + n, len);
  b[n] = 0;
  throw_new(t, "java/lang/ArrayIndexOutOfBoundsException", b);
}

static int
find_handler(Thread *t, Method *m, int pc, Object *exc)
{
  for(int i = 0; i < m->nexc; i++){
    ExcEntry *x = &m->exc[i];
    if(pc < x->start || pc >= x->end) continue;
    if(x->type == 0) return x->handler;                       // "any" (finally / synchronized)
    if(instance_of(exc, resolve_class(t, m->cls, x->type))) return x->handler;
  }
  return -1;
}

static int
invoke(Thread *t, Method *m, Slot *args, Slot *ret)
{
  if(m->native){
    m->native(t, args, ret);
    return t->exc ? -1 : 0;
  }
  if(!m->code) fatal("abstract or missing method body: ", m->name);
  if(++t->depth > 600){
    t->depth--;
    throw_new(t, "java/lang/StackOverflowError", 0);
    return -1;
  }

  Slot locals[m->max_locals + 1];
  Slot stack[m->max_stack + 2];
  Slot *sp = stack;
  Class *cls = m->cls;
  CPEntry *cp = cls->cp;
  uint8_t *code = m->code;
  int pc = 0, ipc = 0;
  Object *syncobj = 0;

  memset(locals, 0, sizeof(Slot) * (m->max_locals + 1));
  memcpy(locals, args, sizeof(Slot) * m->argslots);

  // ACC_SYNCHRONIZED: the VM does the monitorenter/monitorexit itself
  if(m->flags & ACC_SYNCHRONIZED){
    syncobj = (m->flags & ACC_STATIC) ? cls->mirror : locals[0].a;
    monitor_enter(t, syncobj);
  }

#define U1() (code[pc++])
#define S1() ((int8_t)code[pc++])
#define U2() (pc += 2, (uint16_t)((code[pc - 2] << 8) | code[pc - 1]))
#define S2() ((int16_t)U2())
#define S4() (pc += 4, (int32_t)(((uint32_t)code[pc - 4] << 24) | ((uint32_t)code[pc - 3] << 16) | ((uint32_t)code[pc - 2] << 8) | code[pc - 1]))
#define PUSHI(v) ((sp++)->j = (int32_t)(v))
#define PUSHA(v) ((sp++)->a = (v))
#define PUSHJ(v) (sp->j = (v), sp += 2)
#define POPI() ((--sp)->i)
#define POPA() ((--sp)->a)
#define POPJ() (sp -= 2, sp->j)
#define THROW(cn, msg) do { throw_new(t, cn, msg); goto exception; } while(0)
#define NULLCHK(o) do { if(!(o)) THROW("java/lang/NullPointerException", 0); } while(0)

  for(;;){
    ipc = pc;
    uint8_t op = U1();
    t->bytecodes++;
    switch(op){
    case 0x00: break;                                                   // nop
    case 0x01: PUSHA(0); break;                                         // aconst_null
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08:
      PUSHI(op - 0x03); break;                                          // iconst_m1..5
    case 0x09: case 0x0a: PUSHJ(op - 0x09); break;                      // lconst_0/1
    case 0x10: PUSHI(S1()); break;                                      // bipush
    case 0x11: PUSHI(S2()); break;                                      // sipush
    case 0x12: case 0x13: {                                             // ldc, ldc_w
      int idx = op == 0x12 ? U1() : U2();
      CPEntry *e = &cp[idx];
      if(e->tag == 3) PUSHI(e->v.i);
      else if(e->tag == 8){
        Object *s = __atomic_load_n((Object **)&e->resolved, __ATOMIC_ACQUIRE);
        if(!s){
          char *u = cp_utf8(cls, e->a);
          s = intern_string(u, slen(u));
          __atomic_store_n((Object **)&e->resolved, s, __ATOMIC_RELEASE);
        }
        PUSHA(s);
      } else if(e->tag == 7) PUSHA(resolve_class(t, cls, idx)->mirror);
      else fatal("unsupported ldc constant (float?) in ", m->name);
      break;
    }
    case 0x14: { int idx = U2(); PUSHJ(cp[idx].v.j); break; }          // ldc2_w (long)
    case 0x15: case 0x19: (sp++)->j = locals[U1()].j; break;           // iload, aload
    case 0x16: { int i = U1(); PUSHJ(locals[i].j); break; }             // lload
    case 0x1a: case 0x1b: case 0x1c: case 0x1d: (sp++)->j = locals[op - 0x1a].j; break;
    case 0x1e: case 0x1f: case 0x20: case 0x21: PUSHJ(locals[op - 0x1e].j); break;
    case 0x2a: case 0x2b: case 0x2c: case 0x2d: (sp++)->j = locals[op - 0x2a].j; break;
    case 0x2e: case 0x33: case 0x34: case 0x35: case 0x32: case 0x2f: { // xaload
      int i = POPI();
      Array *a = (Array *)POPA();
      NULLCHK(a);
      if(i < 0 || i >= a->length){ aioobe(t, i, a->length); goto exception; }
      switch(op){
      case 0x2e: PUSHI(((int32_t *)a->data)[i]); break;
      case 0x33: PUSHI(a->esize == 1 ? ((int8_t *)a->data)[i] : 0); break;
      case 0x34: PUSHI(((uint16_t *)a->data)[i]); break;
      case 0x35: PUSHI(((int16_t *)a->data)[i]); break;
      case 0x32: PUSHA(((Object **)a->data)[i]); break;
      case 0x2f: PUSHJ(((int64_t *)a->data)[i]); break;
      }
      break;
    }
    case 0x36: case 0x3a: locals[U1()].j = (--sp)->j; break;           // istore, astore
    case 0x37: { int i = U1(); locals[i].j = POPJ(); break; }           // lstore
    case 0x3b: case 0x3c: case 0x3d: case 0x3e: locals[op - 0x3b].j = (--sp)->j; break;
    case 0x3f: case 0x40: case 0x41: case 0x42: locals[op - 0x3f].j = POPJ(); break;
    case 0x4b: case 0x4c: case 0x4d: case 0x4e: locals[op - 0x4b].j = (--sp)->j; break;
    case 0x4f: case 0x54: case 0x55: case 0x56: case 0x53: case 0x50: { // xastore
      Slot v;
      if(op == 0x50) v.j = POPJ(); else v = *--sp;
      int i = POPI();
      Array *a = (Array *)POPA();
      NULLCHK(a);
      if(i < 0 || i >= a->length){ aioobe(t, i, a->length); goto exception; }
      switch(op){
      case 0x4f: ((int32_t *)a->data)[i] = v.i; break;
      case 0x54: ((int8_t *)a->data)[i] = (int8_t)v.i; break;
      case 0x55: ((uint16_t *)a->data)[i] = (uint16_t)v.i; break;
      case 0x56: ((int16_t *)a->data)[i] = (int16_t)v.i; break;
      case 0x53: ((Object **)a->data)[i] = v.a; break;
      case 0x50: ((int64_t *)a->data)[i] = v.j; break;
      }
      break;
    }
    case 0x57: sp--; break;                                             // pop
    case 0x58: sp -= 2; break;                                          // pop2
    case 0x59: *sp = sp[-1]; sp++; break;                               // dup
    case 0x5a: { Slot a = sp[-1], b = sp[-2]; sp[-2] = a; sp[-1] = b; *sp++ = a; break; }  // dup_x1
    case 0x5b: { Slot a = sp[-1], b = sp[-2], c = sp[-3]; sp[-3] = a; sp[-2] = c; sp[-1] = b; *sp++ = a; break; } // dup_x2
    case 0x5c: sp[0] = sp[-2]; sp[1] = sp[-1]; sp += 2; break;          // dup2
    case 0x5f: { Slot a = sp[-1]; sp[-1] = sp[-2]; sp[-2] = a; break; } // swap
    case 0x60: { int32_t b = POPI(), a = POPI(); PUSHI((int32_t)((uint32_t)a + (uint32_t)b)); break; }
    case 0x64: { int32_t b = POPI(), a = POPI(); PUSHI((int32_t)((uint32_t)a - (uint32_t)b)); break; }
    case 0x68: { int32_t b = POPI(), a = POPI(); PUSHI((int32_t)((uint32_t)a * (uint32_t)b)); break; }
    case 0x6c: case 0x70: {                                             // idiv, irem
      int32_t b = POPI(), a = POPI();
      if(b == 0) THROW("java/lang/ArithmeticException", "/ by zero");
      if(a == (int32_t)0x80000000 && b == -1) PUSHI(op == 0x6c ? a : 0);
      else PUSHI(op == 0x6c ? a / b : a % b);
      break;
    }
    case 0x61: { int64_t b = POPJ(), a = POPJ(); PUSHJ((int64_t)((uint64_t)a + (uint64_t)b)); break; }
    case 0x65: { int64_t b = POPJ(), a = POPJ(); PUSHJ((int64_t)((uint64_t)a - (uint64_t)b)); break; }
    case 0x69: { int64_t b = POPJ(), a = POPJ(); PUSHJ((int64_t)((uint64_t)a * (uint64_t)b)); break; }
    case 0x6d: case 0x71: {
      int64_t b = POPJ(), a = POPJ();
      if(b == 0) THROW("java/lang/ArithmeticException", "/ by zero");
      PUSHJ(op == 0x6d ? a / b : a % b);
      break;
    }
    case 0x74: { int32_t a = POPI(); PUSHI((int32_t)(0u - (uint32_t)a)); break; }   // ineg
    case 0x75: { int64_t a = POPJ(); PUSHJ(-a); break; }               // lneg
    case 0x78: { int32_t b = POPI(), a = POPI(); PUSHI((int32_t)((uint32_t)a << (b & 31))); break; }
    case 0x7a: { int32_t b = POPI(), a = POPI(); PUSHI(a >> (b & 31)); break; }
    case 0x7c: { int32_t b = POPI(), a = POPI(); PUSHI((int32_t)((uint32_t)a >> (b & 31))); break; }
    case 0x7e: { int32_t b = POPI(), a = POPI(); PUSHI(a & b); break; }
    case 0x80: { int32_t b = POPI(), a = POPI(); PUSHI(a | b); break; }
    case 0x82: { int32_t b = POPI(), a = POPI(); PUSHI(a ^ b); break; }
    case 0x7f: { int64_t b = POPJ(), a = POPJ(); PUSHJ(a & b); break; }
    case 0x81: { int64_t b = POPJ(), a = POPJ(); PUSHJ(a | b); break; }
    case 0x84: { int i = U1(); int c = S1(); locals[i].j = (int32_t)((uint32_t)locals[i].i + c); break; } // iinc
    case 0x85: { int32_t a = POPI(); PUSHJ((int64_t)a); break; }       // i2l
    case 0x88: { int64_t a = POPJ(); PUSHI((int32_t)a); break; }       // l2i
    case 0x91: { int32_t a = POPI(); PUSHI((int8_t)a); break; }        // i2b
    case 0x92: { int32_t a = POPI(); PUSHI((uint16_t)a); break; }      // i2c
    case 0x93: { int32_t a = POPI(); PUSHI((int16_t)a); break; }       // i2s
    case 0x94: { int64_t b = POPJ(), a = POPJ(); PUSHI(a < b ? -1 : a > b); break; }  // lcmp
    case 0x99: case 0x9a: case 0x9b: case 0x9c: case 0x9d: case 0x9e: { // if<cond>
      int off = S2();
      int32_t a = POPI();
      int take = op == 0x99 ? a == 0 : op == 0x9a ? a != 0 : op == 0x9b ? a < 0 :
                 op == 0x9c ? a >= 0 : op == 0x9d ? a > 0 : a <= 0;
      if(take) pc = ipc + off;
      break;
    }
    case 0x9f: case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4: { // if_icmp<cond>
      int off = S2();
      int32_t b = POPI(), a = POPI();
      int take = op == 0x9f ? a == b : op == 0xa0 ? a != b : op == 0xa1 ? a < b :
                 op == 0xa2 ? a >= b : op == 0xa3 ? a > b : a <= b;
      if(take) pc = ipc + off;
      break;
    }
    case 0xa5: case 0xa6: {                                             // if_acmpeq/ne
      int off = S2();
      Object *b = POPA(), *a = POPA();
      if((a == b) == (op == 0xa5)) pc = ipc + off;
      break;
    }
    case 0xa7: { int off = S2(); pc = ipc + off; break; }              // goto
    case 0xc8: { int off = S4(); pc = ipc + off; break; }              // goto_w
    case 0xaa: {                                                        // tableswitch
      pc = (pc + 3) & ~3;
      int def = S4(), lo = S4(), hi = S4();
      int32_t k = POPI();
      if(k < lo || k > hi) pc = ipc + def;
      else { pc += (k - lo) * 4; pc = ipc + S4(); }
      break;
    }
    case 0xab: {                                                        // lookupswitch
      pc = (pc + 3) & ~3;
      int def = S4(), n = S4();
      int32_t k = POPI();
      int target = def;
      for(int i = 0; i < n; i++){ int32_t key = S4(); int off = S4(); if(key == k){ target = off; break; } }
      pc = ipc + target;
      break;
    }
    case 0xac: case 0xb0: case 0xad: case 0xb1: {                      // ireturn areturn lreturn return
      if(op == 0xad) ret->j = POPJ();
      else if(op != 0xb1) *ret = *--sp;
      if(syncobj && monitor_exit(t, syncobj) < 0) goto exception_noframe;
      t->depth--;
      return 0;
    }
    case 0xb2: case 0xb3: {                                             // getstatic, putstatic
      Field *f = resolve_field(t, cls, U2());
      if(init_class(t, f->cls) < 0) goto exception;
      Slot *p = &f->cls->statics[f->index];
      int wide = f->desc[0] == 'J' || f->desc[0] == 'D';
      int vol = f->flags & ACC_VOLATILE;
      if(op == 0xb2){
        int64_t v = vol ? __atomic_load_n(&p->j, __ATOMIC_ACQUIRE) : p->j;
        if(wide) PUSHJ(v); else (sp++)->j = v;
      } else {
        int64_t v = wide ? POPJ() : (--sp)->j;
        if(vol) __atomic_store_n(&p->j, v, __ATOMIC_SEQ_CST); else p->j = v;
      }
      break;
    }
    case 0xb4: {                                                        // getfield
      Field *f = resolve_field(t, cls, U2());
      Object *o = POPA();
      NULLCHK(o);
      Slot *p = &FIELDS(o)[f->index];
      // volatile read = load-acquire (LDAR)
      int64_t v = (f->flags & ACC_VOLATILE) ? __atomic_load_n(&p->j, __ATOMIC_ACQUIRE) : p->j;
      if(f->desc[0] == 'J' || f->desc[0] == 'D') PUSHJ(v); else (sp++)->j = v;
      break;
    }
    case 0xb5: {                                                        // putfield
      Field *f = resolve_field(t, cls, U2());
      int64_t v = (f->desc[0] == 'J' || f->desc[0] == 'D') ? POPJ() : (--sp)->j;
      Object *o = POPA();
      NULLCHK(o);
      Slot *p = &FIELDS(o)[f->index];
      // volatile write = store-release (STLR); SEQ_CST also orders it
      // against a later volatile read, as the JMM requires
      if(f->flags & ACC_VOLATILE) __atomic_store_n(&p->j, v, __ATOMIC_SEQ_CST); else p->j = v;
      break;
    }
    case 0xb6: case 0xb7: case 0xb8: case 0xb9: {                      // invoke*
      Method *rm;
      int idx = U2();
      if(op == 0xb9){ pc += 2; }
      rm = resolve_method(t, cls, idx);
      Method *target = rm;
      int n = rm->argslots;
      Slot *args0 = sp - n;
      if(op == 0xb8){
        if(init_class(t, rm->cls) < 0) goto exception;
      } else {
        Object *recv = args0[0].a;
        NULLCHK(recv);
        if(op == 0xb6 && rm->vindex >= 0 && recv->cls->vtable && rm->vindex < recv->cls->vtlen)
          target = recv->cls->vtable[rm->vindex];               // virtual dispatch
        else if(op == 0xb9){
          target = find_method(recv->cls, rm->name, rm->desc);  // interface: search
          if(!target) fatal("AbstractMethodError: ", rm->name);
        }
      }
      Slot r;
      r.j = 0;
      sp = args0;
      if(invoke(t, target, args0, &r) < 0) goto exception;
      if(target->rettype == 'J' || target->rettype == 'D') PUSHJ(r.j);
      else if(target->rettype != 'V') *sp++ = r;
      break;
    }
    case 0xbb: {                                                        // new
      Class *k = resolve_class(t, cls, U2());
      if(init_class(t, k) < 0) goto exception;
      PUSHA(new_object(k));
      break;
    }
    case 0xbc: {                                                        // newarray
      int atype = U1();
      int n = POPI();
      if(n < 0) THROW("java/lang/NegativeArraySizeException", 0);
      static const char *descs[] = {0,0,0,0,"Z","C","F","D","B","S","I","J"};
      static const int sizes[] = {0,0,0,0,1,2,4,8,1,2,4,8};
      if(atype < 4 || atype > 11) fatal("bad newarray type", 0);
      PUSHA((Object *)new_array(array_class(t, descs[atype]), n, sizes[atype]));
      break;
    }
    case 0xbd: {                                                        // anewarray
      CPEntry *e = &cp[U2()];
      int n = POPI();
      if(n < 0) THROW("java/lang/NegativeArraySizeException", 0);
      char *en = cp_utf8(cls, e->a);
      char b[256];
      int l = slen(en);
      if(en[0] == '[') memcpy(b, en, l + 1);
      else { b[0] = 'L'; memcpy(b + 1, en, l); b[l + 1] = ';'; b[l + 2] = 0; }
      PUSHA((Object *)new_array(array_class(t, b), n, 8));
      break;
    }
    case 0xbe: { Array *a = (Array *)POPA(); NULLCHK(a); PUSHI(a->length); break; }   // arraylength
    case 0xbf: {                                                        // athrow
      Object *e = POPA();
      NULLCHK(e);
      t->exc = e;
      goto exception;
    }
    case 0xc0: case 0xc1: {                                             // checkcast, instanceof
      Class *k = resolve_class(t, cls, U2());
      Object *o = sp[-1].a;
      int ok = !o || instance_of(o, k) || (k->name[0] == '[' && o->cls == k);
      if(op == 0xc1){ sp[-1].j = o ? ok : 0; }
      else if(!ok){
        char b1[128], b2[300];
        dotted(o->cls->name, b2, 128);
        int n = slen(b2);
        const char *mid = " cannot be cast to ";
        memcpy(b2 + n, mid, 19); n += 19;
        dotted(k->name, b1, sizeof b1);
        memcpy(b2 + n, b1, slen(b1) + 1);
        THROW("java/lang/ClassCastException", b2);
      }
      break;
    }
    case 0xc2: {                                                        // monitorenter
      Object *o = POPA();
      NULLCHK(o);
      if(monitor_enter(t, o) < 0) goto exception;
      break;
    }
    case 0xc3: {                                                        // monitorexit
      Object *o = POPA();
      NULLCHK(o);
      if(monitor_exit(t, o) < 0) goto exception;
      break;
    }
    case 0xc4: {                                                        // wide
      uint8_t op2 = U1();
      int i = U2();
      switch(op2){
      case 0x15: case 0x19: (sp++)->j = locals[i].j; break;
      case 0x16: PUSHJ(locals[i].j); break;
      case 0x36: case 0x3a: locals[i].j = (--sp)->j; break;
      case 0x37: locals[i].j = POPJ(); break;
      case 0x84: { int c = S2(); locals[i].j = (int32_t)((uint32_t)locals[i].i + c); break; }
      default: fatal("unsupported wide opcode", 0);
      }
      break;
    }
    case 0xc6: case 0xc7: {                                             // ifnull, ifnonnull
      int off = S2();
      Object *o = POPA();
      if((o == 0) == (op == 0xc6)) pc = ipc + off;
      break;
    }
    default: {
      char b[8];
      b[0] = "0123456789abcdef"[op >> 4]; b[1] = "0123456789abcdef"[op & 15]; b[2] = 0;
      say("mjvm: unsupported opcode 0x%s in %s.%s\n", b, cls->name, m->name);
      plat_exit(1);
    }
    }
    continue;

  exception: {
      // look for a handler in this method's exception table (javac puts
      // the "any" handler of every synchronized block here)
      Object *e = t->exc;
      int h = find_handler(t, m, ipc, e);
      if(h >= 0){
        t->exc = 0;
        sp = stack;
        PUSHA(e);
        pc = h;
        continue;
      }
      record_frame(e, m, ipc);
      if(syncobj){
        t->exc = 0;
        monitor_exit(t, syncobj);       // ACC_SYNCHRONIZED: unlock on the way out
        t->exc = e;
      }
    }
  exception_noframe:
    t->depth--;
    return -1;
  }
}

// ------------------------------------------------------------------ thread entry & main

static void
thread_finish(Thread *t)
{
  out_flush(t);
  Object *o = t->obj;
  // the VM's side of Thread.join(): mark dead and notifyAll on the Thread
  monitor_enter(t, o);
  __atomic_store_n(&t->alive, 0, __ATOMIC_RELEASE);
  if(LW_TAG(o->lock) == LW_INFLATED) monitor_notify(t, o, 1);     // only join() waits
  monitor_exit(t, o);
}

static void
thread_main(void *arg)
{
  Thread *t = arg;
  Slot args[2], r;
  args[0].a = t->entry_arg ? t->entry_arg : t->obj;
  int rc;
  if(t->entry->flags & ACC_STATIC){
    // main(String[]): make sure the class is initialized first
    rc = init_class(t, t->entry->cls);
    if(!rc) rc = invoke(t, t->entry, args, &r);
  } else {
    rc = invoke(t, t->entry, args, &r);
  }
  if(rc < 0 && t->exc){
    print_exception(t, t->exc);
    if(t->id == 1) exit_status = 1;
    t->exc = 0;
  }
  thread_finish(t);
  if(verbose) say("[mjvm] %s: exits\n", thread_name(t));
  plat_thread_exit();
}

int
main(int argc, char **argv)
{
  int i = 1;
  for(; i < argc && argv[i][0] == '-'; i++){
    if(strcmp(argv[i], "-v") == 0) verbose = show_stats = 1;
    else if(strcmp(argv[i], "-stats") == 0) show_stats = 1;
    else if(strcmp(argv[i], "-cp") == 0 && i + 1 < argc) classpath = argv[++i];
    else { say("unknown option %s\n", argv[i]); return 2; }
  }
  if(i >= argc){
    say("usage: mjvm [-v | -stats] [-cp dir] MainClass [args...]\n");
    return 2;
  }
  char mainname[256];
  int n = 0;
  for(const char *p = argv[i]; *p && n < 250; p++) mainname[n++] = *p == '.' ? '/' : *p;
  mainname[n] = 0;

  define_builtins();

  // the main thread gets a java.lang.Thread object named "main"
  Object *mainobj = new_object(C_Thread);
  Thread *mt = new_thread(mainobj);
  THR_VM(mainobj) = mt;
  THR_NAME(mainobj) = new_string("main", 4);
  mt->alive = 1;

  Class *mc = load_class(mt, mainname);
  Method *mm = find_method(mc, intern("main"), intern("([Ljava/lang/String;)V"));
  if(!mm || !(mm->flags & ACC_STATIC)) fatal("no public static void main(String[]) in ", mainname);
  Array *jargs = new_array(array_class(mt, "Ljava/lang/String;"), argc - i - 1, 8);
  for(int k = i + 1; k < argc; k++)
    ((Object **)jargs->data)[k - i - 1] = new_string(argv[k], slen(argv[k]));
  mt->entry = mm;
  mt->entry_arg = (Object *)jargs;

  // run Java's main on its own kernel thread with a big stack (the xv6
  // process stack is only 16 KB); this thread just waits like the JVM's
  // launcher does (DestroyJavaVM waits for all non-daemon threads).
  char *stack = vm_alloc(VM_THREAD_STACK);
  long start = plat_now_us();
  mt->handle = plat_thread_start(thread_main, mt, stack, VM_THREAD_STACK);
  if(mt->handle < 0) fatal("cannot start main thread", 0);
  mt->started = 1;
  plat_thread_join(mt->handle);
  for(int k = 2; k <= nthreads; k++){
    Thread *t = threads[k];
    while(t && !__atomic_load_n(&t->started, __ATOMIC_ACQUIRE)) plat_yield();
    if(t) plat_thread_join(t->handle);
  }
  for(int k = 1; k <= nthreads; k++) if(threads[k]) st_bytecodes += threads[k]->bytecodes;
  if(show_stats)
    say("[mjvm] exit: %l bytecodes, %d threads, %l KB heap, contended-thin=%l inflations=%l parks=%l waits=%l, %l ms\n",
        st_bytecodes, nthreads, heap_used / 1024, st_contended, st_inflations, st_parks, st_waits,
        (plat_now_us() - start) / 1000);
  plat_exit(exit_status);
}
