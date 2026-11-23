// MiniJava runtime for xv6-aarch64: objects, threads, monitors, exceptions.
//
// The monitor design follows HotSpot closely, scaled down:
//
//   lock word (8 bytes at obj+8, HotSpot calls it the "mark word"):
//     0                                  unlocked
//     owner_id << 32 | rec << 2 | 0b01   thin lock  (HotSpot: stack/fast lock)
//     ObjectMonitor*      | 0b10         inflated   (HotSpot: ObjectMonitor)
//
//   * Uncontended enter/exit is a single LDAXR/STXR (STLXR on exit) done
//     inline by the compiler; see emit_monitorenter() in mjc.c.
//   * Recursion, contention, wait/notify go through the functions below.
//   * A contender spins a little, then *inflates* the lock: it allocates an
//     ObjectMonitor that records the current owner, and CASes the monitor
//     pointer into the lock word.  The owner's next thin-unlock CAS fails and
//     it falls into mj_monitor_exit_slow, which now finds the monitor.
//   * Blocked threads queue on the monitor's entry list (HotSpot: cxq /
//     EntryList) and sleep on their own per-thread semaphore ("ParkEvent"),
//     which is an xv6 kernel sync object (sync_create(SYNC_SEM, 0)).
//   * wait() moves the thread to the monitor's wait set and releases the
//     monitor; notify() moves one waiter from the wait set to the entry list.
//
// Compiled code keeps the current MJThread* in x28; this file is built with
// -ffixed-x28 so the C compiler never reuses that register.

#include <uapi/types.h>
#include <uapi/stat.h>
#include "user/user.h"

#define SYNC_SEM 2

#define LW_UNLOCKED 0L
#define LW_THIN     1L
#define LW_INFLATED 2L
#define LW_TAG(w)   ((w) & 3L)
#define THIN(id, rec) (((long)(id) << 32) | ((long)(rec) << 2) | LW_THIN)
#define THIN_OWNER(w) ((int)((unsigned long)(w) >> 32))
#define THIN_REC(w)   ((int)(((w) >> 2) & 0x3fffffff))
#define THIN_REC_MAX  1000
#define SPIN_LIMIT    64

#define MAX_THREADS   48
#define STACK_SIZE    (64 * 1024)

typedef struct Class Class;
typedef struct Object {
  void **vtable;               // [0] vtable; vtable[0] = Class*
  volatile long lock;          // [8] lock word
  long fields[];
} Object;

struct Class {
  void *vt;                    // [0]
  volatile long lock;          // [8]  class object is lockable
  const char *name;            // [16]
  Class *super;                // [24]
  long size;                   // [32]
  void **vtable;               // [40]
};

typedef struct MJThread MJThread;
struct MJThread {
  int id;                      // [0]  used by the inline lock fast path
  int pad;
  void *handlers;              // [8]  exception handler chain (mj_throw)
  int park;                    // [16] kernel semaphore handle (ParkEvent)
  int ktid;                    // [20] kernel thread id
  long unused;                 // [24]
  Object *obj;                 // [32] the java.lang.Thread object
  MJThread *next;              // queue link: monitor entry list / wait set
  int saved_rec;               // recursion count to restore when we own again
  volatile int alive;
  volatile int started;
  int nblock_traces;
  char *stack;
  int outlen;
  char outbuf[200];            // per-thread line buffer for System.out
};

typedef struct ObjectMonitor {
  volatile int spin;           // protects the fields below
  MJThread *owner;
  long rec;
  MJThread *entry_head, *entry_tail;   // blocked in monitorenter
  MJThread *wait_head, *wait_tail;     // blocked in wait()
  Object *obj;
} ObjectMonitor;

extern Class MJC_IllegalMonitorStateException, MJC_NullPointerException,
             MJC_ArithmeticException, MJC_ArrayIndexOutOfBoundsException,
             MJC_IllegalThreadStateException;
extern void mj_thread_entry(MJThread *);
void mj_throw(Object *) __attribute__((noreturn));
void mj_thread_finish(void) __attribute__((noreturn));

static MJThread *threads[MAX_THREADS + 1];
static volatile int nthreads;
static int verbose;
static volatile long stat_fast_contended, stat_inflations, stat_parks, stat_waits;

static inline MJThread *
self(void)
{
  MJThread *t;
  asm volatile("mov %0, x28" : "=r"(t));
  return t;
}

// ------------------------------------------------------------------ output

static int
fmt_long(char *p, long v)
{
  char tmp[24];
  int n = 0, len = 0;
  unsigned long u = v < 0 ? -(unsigned long)v : (unsigned long)v;
  do { tmp[n++] = '0' + u % 10; u /= 10; } while(u);
  if(v < 0) p[len++] = '-';
  while(n) p[len++] = tmp[--n];
  return len;
}

static int
fmt_hex(char *p, unsigned long v)
{
  static const char d[] = "0123456789abcdef";
  int len = 0, started = 0;
  p[len++] = '0'; p[len++] = 'x';
  for(int sh = 60; sh >= 0; sh -= 4){
    int x = (v >> sh) & 15;
    if(x || started || sh == 0){ p[len++] = d[x]; started = 1; }
  }
  return len;
}

// tiny formatter for runtime traces: %d %s %p only; one write() per line
static void
trace(const char *fmt, ...)
{
  char buf[256];
  int n = 0;
  __builtin_va_list ap;
  __builtin_va_start(ap, fmt);
  for(const char *f = fmt; *f && n < 200; f++){
    if(*f != '%'){ buf[n++] = *f; continue; }
    f++;
    if(*f == 'd') n += fmt_long(buf + n, __builtin_va_arg(ap, int));
    else if(*f == 'p') n += fmt_hex(buf + n, (unsigned long)__builtin_va_arg(ap, void *));
    else if(*f == 's'){
      const char *s = __builtin_va_arg(ap, const char *);
      while(*s && n < 220) buf[n++] = *s++;
    }
  }
  __builtin_va_end(ap);
  buf[n++] = '\n';
  write(1, buf, n);
}

static void
out_append(const char *s, int n)
{
  MJThread *t = self();
  for(int i = 0; i < n; i++){
    if(t->outlen >= (int)sizeof(t->outbuf)){
      write(1, t->outbuf, t->outlen);
      t->outlen = 0;
    }
    t->outbuf[t->outlen++] = s[i];
  }
}

void mj_print_str(const char *s) { out_append(s, strlen(s)); }
void mj_print_int(long v) { char b[24]; out_append(b, fmt_long(b, (int)v)); }
void mj_print_bool(long v) { mj_print_str(v ? "true" : "false"); }

void
mj_print_obj(Object *o)
{
  if(!o){ mj_print_str("null"); return; }
  Class *c = (Class *)o->vtable[0];
  char b[24];
  mj_print_str(c->name);
  out_append("@", 1);
  out_append(b, fmt_hex(b, (unsigned long)o));
}

// System.out.println: one write() per line so lines from threads never mix
void
mj_println(void)
{
  MJThread *t = self();
  out_append("\n", 1);
  write(1, t->outbuf, t->outlen);
  t->outlen = 0;
}

// ------------------------------------------------------------------ spinlocks / heap

static inline void
spin_lock(volatile int *l)
{
  while(__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
    while(__atomic_load_n(l, __ATOMIC_RELAXED))
      sched_yield();
}

static inline void
spin_unlock(volatile int *l)
{
  __atomic_store_n(l, 0, __ATOMIC_RELEASE);
}

static volatile int heap_lock;
static char *heap_cur, *heap_end;

// Bump allocator shared by all threads.  No GC in this prototype: a real
// collector would need safepoints so every thread stops at a known place.
static void *
mj_alloc(long size)
{
  char *p;
  size = (size + 15) & ~15L;
  spin_lock(&heap_lock);
  if(heap_cur + size > heap_end){
    long chunk = size > (1 << 20) ? size : (1 << 20);
    char *m = sbrk(chunk);
    if(m == (char *)-1){
      spin_unlock(&heap_lock);
      trace("mj: out of memory");
      exit(1);
    }
    heap_cur = m;
    heap_end = m + chunk;
  }
  p = heap_cur;
  heap_cur += size;
  spin_unlock(&heap_lock);
  memset(p, 0, size);
  return p;
}

Object *
mj_new(Class *c)
{
  Object *o = mj_alloc(c->size);
  o->vtable = c->vtable;
  return o;
}

static Class intarray_class = { 0, 0, "int[]", 0, 0, 0 };
static void *intarray_vtable[1] = { &intarray_class };

static Object *
new_exception(Class *c)
{
  return mj_new(c);
}

Object *
mj_new_array(long n)
{
  if(n < 0) mj_throw(new_exception(&MJC_ArrayIndexOutOfBoundsException));
  Object *a = mj_alloc(24 + n * 8);
  a->vtable = intarray_vtable;
  a->fields[0] = n;              // length at +16, data from +24
  return a;
}

long
mj_instanceof(Object *o, Class *c)
{
  if(!o) return 0;
  for(Class *k = (Class *)o->vtable[0]; k; k = k->super)
    if(k == c) return 1;
  return 0;
}

void mj_throw_npe(void) { mj_throw(new_exception(&MJC_NullPointerException)); }
void mj_throw_arith(void) { mj_throw(new_exception(&MJC_ArithmeticException)); }
void mj_throw_aioobe(void) { mj_throw(new_exception(&MJC_ArrayIndexOutOfBoundsException)); }

static void __attribute__((noreturn))
throw_imse(void)
{
  mj_throw(new_exception(&MJC_IllegalMonitorStateException));
}

// ------------------------------------------------------------------ park / unpark

static void park(MJThread *t)   { stat_parks++; sync_wait(t->park); }
static void unpark(MJThread *t) { sync_signal(t->park, 1); }

static MJThread *
new_thread(Object *obj)
{
  MJThread *t = mj_alloc(sizeof(MJThread));
  int id = __atomic_add_fetch(&nthreads, 1, __ATOMIC_SEQ_CST);
  if(id > MAX_THREADS){
    trace("mj: too many threads");
    exit(1);
  }
  t->id = id;
  t->obj = obj;
  t->park = sync_create(SYNC_SEM, 0);
  if(t->park < 0){
    trace("mj: out of kernel sync objects for thread %d", id);
    exit(1);
  }
  threads[id] = t;
  return t;
}

static const char *
thread_name(MJThread *t, char *buf)
{
  if(t->id == 1) return "main";
  memcpy(buf, "Thread-", 7);
  buf[7 + fmt_long(buf + 7, t->id - 2)] = 0;
  return buf;
}

// ------------------------------------------------------------------ monitors

static void
q_append(MJThread **head, MJThread **tail, MJThread *t)
{
  t->next = 0;
  if(*tail) (*tail)->next = t; else *head = t;
  *tail = t;
}

static MJThread *
q_take(MJThread **head, MJThread **tail)
{
  MJThread *t = *head;
  if(t){
    *head = t->next;
    if(!*head) *tail = 0;
    t->next = 0;
  }
  return t;
}

// Inflate obj's lock while its word is still `w` (unlocked or thin).
static void
inflate(Object *o, long w, const char *why)
{
  ObjectMonitor *m = mj_alloc(sizeof(ObjectMonitor));
  m->obj = o;
  if(LW_TAG(w) == LW_THIN){
    m->owner = threads[THIN_OWNER(w)];
    m->rec = THIN_REC(w);
  }
  long expect = w;
  if(__atomic_compare_exchange_n(&o->lock, &expect, (long)m | LW_INFLATED, 0,
                                 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)){
    stat_inflations++;
    if(verbose){
      char b1[16], b2[16];
      trace("[mj] T%d inflate %p (%s): thin owner=%s rec=%d -> ObjectMonitor %p",
            self()->id, o, why, m->owner ? thread_name(m->owner, b1) : "-",
            (int)m->rec, m);
      (void)b2;
    }
  }
  // On failure the monitor is simply leaked (no GC); the caller re-reads.
}

static void
monitor_enter(ObjectMonitor *m, MJThread *me)
{
  spin_lock(&m->spin);
  if(m->owner == 0){
    m->owner = me;
    m->rec = 1;
    spin_unlock(&m->spin);
    return;
  }
  if(m->owner == me){
    m->rec++;
    spin_unlock(&m->spin);
    return;
  }
  // contended: queue on the entry list and sleep.  The exiting owner hands
  // the monitor to us directly (owner = me) before waking us.
  me->saved_rec = 1;
  q_append(&m->entry_head, &m->entry_tail, me);
  if(verbose && ++me->nblock_traces <= 2){
    char b[16];
    trace("[mj] T%d blocks on %p (owner %s), parking%s", me->id, m->obj,
          thread_name(m->owner, b), me->nblock_traces == 2 ? "  (further blocks not shown)" : "");
  }
  spin_unlock(&m->spin);
  while(1){
    park(me);
    spin_lock(&m->spin);
    int mine = m->owner == me;
    spin_unlock(&m->spin);
    if(mine) break;
  }
}

// Give the monitor to the next queued thread, or free it.  Caller holds spin.
static MJThread *
monitor_handoff(ObjectMonitor *m)
{
  MJThread *next = q_take(&m->entry_head, &m->entry_tail);
  if(next){
    m->owner = next;
    m->rec = next->saved_rec;
  } else {
    m->owner = 0;
    m->rec = 0;
  }
  return next;
}

static void
monitor_exit(ObjectMonitor *m, MJThread *me)
{
  spin_lock(&m->spin);
  if(m->owner != me){
    spin_unlock(&m->spin);
    throw_imse();
  }
  if(--m->rec > 0){
    spin_unlock(&m->spin);
    return;
  }
  MJThread *next = monitor_handoff(m);
  spin_unlock(&m->spin);
  if(next) unpark(next);
}

void
mj_monitor_enter_slow(Object *o)
{
  MJThread *me = self();
  int spins = 0;
  for(;;){
    long w = __atomic_load_n(&o->lock, __ATOMIC_ACQUIRE);
    if(w == LW_UNLOCKED){
      if(__atomic_compare_exchange_n(&o->lock, &w, THIN(me->id, 1), 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return;
      continue;
    }
    if(LW_TAG(w) == LW_THIN){
      if(THIN_OWNER(w) == me->id){
        // recursive enter: bump the count in the lock word
        if(THIN_REC(w) < THIN_REC_MAX){
          if(__atomic_compare_exchange_n(&o->lock, &w, w + 4, 0,
                                         __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return;
        } else {
          inflate(o, w, "recursion overflow");
        }
        continue;
      }
      // owned by someone else: brief spin, then inflate and block
      if(spins == 0) stat_fast_contended++;
      if(++spins < SPIN_LIMIT){
        if(spins > 8) sched_yield();
        continue;
      }
      inflate(o, w, "contention");
      continue;
    }
    monitor_enter((ObjectMonitor *)(w & ~3L), me);
    return;
  }
}

void
mj_monitor_exit_slow(Object *o)
{
  MJThread *me = self();
  for(;;){
    long w = __atomic_load_n(&o->lock, __ATOMIC_ACQUIRE);
    if(LW_TAG(w) == LW_THIN){
      if(THIN_OWNER(w) != me->id) throw_imse();
      long nw = THIN_REC(w) > 1 ? w - 4 : LW_UNLOCKED;
      if(__atomic_compare_exchange_n(&o->lock, &w, nw, 0,
                                     __ATOMIC_RELEASE, __ATOMIC_RELAXED))
        return;
      continue;                       // someone inflated meanwhile: retry
    }
    if(LW_TAG(w) == LW_INFLATED){
      monitor_exit((ObjectMonitor *)(w & ~3L), me);
      return;
    }
    throw_imse();                     // unlocked: we never owned it
  }
}

// wait/notify need a real monitor (a wait set), so they inflate first,
// exactly as HotSpot does.
static ObjectMonitor *
owned_monitor(Object *o, const char *why)
{
  MJThread *me = self();
  for(;;){
    long w = __atomic_load_n(&o->lock, __ATOMIC_ACQUIRE);
    if(LW_TAG(w) == LW_INFLATED){
      ObjectMonitor *m = (ObjectMonitor *)(w & ~3L);
      if(m->owner != me) throw_imse();
      return m;
    }
    if(LW_TAG(w) != LW_THIN || THIN_OWNER(w) != me->id) throw_imse();
    inflate(o, w, why);
  }
}

void
mj_wait(Object *o)
{
  MJThread *me = self();
  ObjectMonitor *m = owned_monitor(o, "wait");
  stat_waits++;
  spin_lock(&m->spin);
  me->saved_rec = m->rec;                 // restored when we own it again
  q_append(&m->wait_head, &m->wait_tail, me);
  MJThread *next = monitor_handoff(m);    // fully release the monitor
  if(verbose) trace("[mj] T%d wait() on %p, releases monitor", me->id, o);
  spin_unlock(&m->spin);
  if(next) unpark(next);
  while(1){
    park(me);
    spin_lock(&m->spin);
    int mine = m->owner == me;
    spin_unlock(&m->spin);
    if(mine) break;
  }
  if(verbose) trace("[mj] T%d returns from wait() on %p", me->id, o);
}

static void
notify(Object *o, int all)
{
  ObjectMonitor *m = owned_monitor(o, all ? "notifyAll" : "notify");
  spin_lock(&m->spin);
  int n = 0;
  MJThread *t;
  while((t = q_take(&m->wait_head, &m->wait_tail)) != 0){
    // the notified thread competes for the monitor again: it moves to the
    // entry list and gets the lock when the notifier (we) exit.
    q_append(&m->entry_head, &m->entry_tail, t);
    n++;
    if(!all) break;
  }
  spin_unlock(&m->spin);
  if(verbose && n) trace("[mj] T%d %s on %p moved %d waiter(s) to the entry list",
                         self()->id, all ? "notifyAll" : "notify", o, n);
}

void mj_notify(Object *o) { notify(o, 0); }
void mj_notify_all(Object *o) { notify(o, 1); }

// ------------------------------------------------------------------ threads

#define THREAD_RT(o) (*(MJThread **)&(o)->fields[0])     // Thread.$rt at +16

void
mj_thread_start(Object *o)
{
  if(THREAD_RT(o)) mj_throw(new_exception(&MJC_IllegalThreadStateException));
  MJThread *t = new_thread(o);
  t->stack = mj_alloc(STACK_SIZE);
  t->alive = 1;
  __atomic_store_n(&THREAD_RT(o), t, __ATOMIC_RELEASE);
  int tid = thread_clone((void (*)(void *))mj_thread_entry, t, t->stack + STACK_SIZE);
  if(tid < 0){
    trace("mj: thread_clone failed");
    exit(1);
  }
  t->ktid = tid;
  __atomic_store_n(&t->started, 1, __ATOMIC_RELEASE);
  if(verbose) trace("[mj] T%d started T%d (kernel tid %d)", self()->id, t->id, tid);
}

// Thread.join() is ordinary Java in the JDK:  while(isAlive()) wait(0);
// and the VM calls notifyAll() on the Thread object when the thread ends.
void
mj_thread_join(Object *o)
{
  MJThread *t = __atomic_load_n(&THREAD_RT(o), __ATOMIC_ACQUIRE);
  if(!t) return;
  mj_monitor_enter_slow(o);
  while(__atomic_load_n(&t->alive, __ATOMIC_ACQUIRE))
    mj_wait(o);
  mj_monitor_exit_slow(o);
}

void
mj_thread_finish(void)
{
  MJThread *me = self();
  if(me->outlen){ write(1, me->outbuf, me->outlen); me->outlen = 0; }
  Object *o = me->obj;
  mj_monitor_enter_slow(o);
  __atomic_store_n(&me->alive, 0, __ATOMIC_RELEASE);
  mj_notify_all(o);
  mj_monitor_exit_slow(o);
  if(verbose) trace("[mj] T%d exits", me->id);
  texit(0);
}

void mj_yield(void) { sched_yield(); }
void mj_sleep(long ms) { if(ms > 0) usleep(ms * 1000); }

void
mj_uncaught(Object *exc)
{
  MJThread *me = self();
  char b[16];
  Class *c = (Class *)exc->vtable[0];
  if(me->outlen){ write(1, me->outbuf, me->outlen); me->outlen = 0; }
  trace("Exception in thread \"%s\" %s", thread_name(me, b), c->name);
  if(me->id == 1) exit(1);
  mj_thread_finish();
}

MJThread *
mj_init(int argc, char **argv)
{
  for(int i = 1; i < argc; i++)
    if(strcmp(argv[i], "-v") == 0) verbose = 1;
  return new_thread(0);
}

void
mj_main_exit(void)
{
  MJThread *me = self();
  if(me->outlen){ write(1, me->outbuf, me->outlen); me->outlen = 0; }
  // the JVM exits only after all non-daemon threads finish
  for(int i = 2; i <= nthreads; i++){
    MJThread *t = threads[i];
    if(t && t->started) tjoin(t->ktid, 0);
  }
  if(verbose)
    trace("[mj] stats: contended-thin=%d inflations=%d parks=%d waits=%d",
          (int)stat_fast_contended, (int)stat_inflations, (int)stat_parks, (int)stat_waits);
  exit(0);
}
