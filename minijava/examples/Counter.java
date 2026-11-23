// The classic data race, and how synchronized fixes it.
//
// Phase 1 (no locks):
//   racy      : plain field, count = count + 1 is LDR / ADD / STR -> lost updates
//   volatile  : LDAR / STLR order each access, but the + 1 is still not atomic
// Phase 2 (locks):
//   synchronized method : ACC_SYNCHRONIZED, locks `this`
//   synchronized block  : monitorenter / monitorexit on a separate lock object
//
// Run "mjCounter -v" to watch thin locks being inflated under contention.

class Counter {
  int racy;
  volatile int vol;
  int safe;
  int blockSafe;
  Object lock;

  Counter() { lock = new Object(); }

  void incRacy() { racy = racy + 1; }
  void incVolatile() { vol = vol + 1; }
  synchronized void incSafe() { safe = safe + 1; }
  void incBlock() {
    synchronized (lock) {
      blockSafe = blockSafe + 1;
    }
  }
}

class Worker extends Thread {
  Counter c;
  int n;
  boolean locked;
  Worker(Counter c, int n, boolean locked) { this.c = c; this.n = n; this.locked = locked; }

  public void run() {
    if (locked) {
      for (int i = 0; i < n; i++) { c.incSafe(); c.incBlock(); }
    } else {
      for (int i = 0; i < n; i++) { c.incRacy(); c.incVolatile(); }
    }
  }
}

public class CounterMain {
  static void phase(Counter c, int n, boolean locked) {
    Worker w0 = new Worker(c, n, locked);
    Worker w1 = new Worker(c, n, locked);
    Worker w2 = new Worker(c, n, locked);
    Worker w3 = new Worker(c, n, locked);
    w0.start(); w1.start(); w2.start(); w3.start();
    w0.join(); w1.join(); w2.join(); w3.join();
  }

  public static void main(String[] args) {
    int n = 100000;
    int expected = 4 * n;
    Counter c = new Counter();
    phase(c, n, false);
    System.out.println("4 threads x " + n + " increments, expected " + expected);
    System.out.print("racy             = " + c.racy);
    if (c.racy != expected) System.out.print("   <-- lost " + (expected - c.racy) + " updates");
    System.out.println();
    System.out.print("volatile         = " + c.vol);
    if (c.vol != expected) System.out.print("   <-- lost " + (expected - c.vol) + " (volatile is not atomic)");
    System.out.println();

    n = 20000;
    expected = 4 * n;
    phase(c, n, true);
    System.out.println("4 threads x " + n + " locked increments, expected " + expected);
    System.out.println("synchronized m() = " + c.safe);
    System.out.println("synchronized {}  = " + c.blockSafe);
  }
}
