// The classic data race, and how synchronized fixes it.
//
// Phase 1 (no locks):
//   racy      : count = count + 1 is getfield / iadd / putfield -> lost updates
//   volatile  : each access is ordered (LDAR / STLR) but + 1 is still not atomic
// Phase 2 (locks):
//   synchronized method : ACC_SYNCHRONIZED flag, the VM locks `this`
//   synchronized block  : monitorenter / monitorexit on a lock object
//
// Run "mjvm -v Counter" to watch thin locks being inflated under contention.

class Counts {
  int racy;
  volatile int vol;
  int safe;
  int blockSafe;
  Object lock = new Object();

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
  Counts c;
  int n;
  boolean locked;
  Worker(Counts c, int n, boolean locked) { this.c = c; this.n = n; this.locked = locked; }

  public void run() {
    if (locked) {
      for (int i = 0; i < n; i++) { c.incSafe(); c.incBlock(); }
    } else {
      for (int i = 0; i < n; i++) { c.incRacy(); c.incVolatile(); }
    }
  }
}

public class Counter {
  static void phase(Counts c, int n, boolean locked) {
    Worker[] ws = new Worker[4];
    for (int i = 0; i < ws.length; i++) ws[i] = new Worker(c, n, locked);
    for (int i = 0; i < ws.length; i++) ws[i].start();
    try {
      for (int i = 0; i < ws.length; i++) ws[i].join();
    } catch (InterruptedException e) {
      System.out.println("interrupted");
    }
  }

  public static void main(String[] args) {
    int n = 20000;
    int expected = 4 * n;
    Counts c = new Counts();
    phase(c, n, false);
    System.out.println("4 threads x " + n + " increments, expected " + expected);
    if (c.racy == expected) System.out.println("racy             = " + c.racy);
    else System.out.println("racy             = " + c.racy + "   <-- lost " + (expected - c.racy) + " updates");
    if (c.vol == expected) System.out.println("volatile         = " + c.vol);
    else System.out.println("volatile         = " + c.vol + "   <-- lost " + (expected - c.vol) + " (volatile is not atomic)");

    phase(c, n, true);
    System.out.println("synchronized m() = " + c.safe);
    System.out.println("synchronized {}  = " + c.blockSafe);
  }
}
