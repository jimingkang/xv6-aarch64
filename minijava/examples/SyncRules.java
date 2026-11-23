// The rules of synchronized, one at a time.
//
//  1. Reentrancy: a thread may re-enter a monitor it already holds
//     (recursion count in the thin lock word).
//  2. An exception thrown inside synchronized still releases the monitor
//     (the compiler's "any" handler runs monitorexit and rethrows).
//  3. return inside synchronized runs monitorexit first.
//  4. wait()/notify() without owning the monitor throw
//     IllegalMonitorStateException.
//  5. static synchronized locks the class object, not an instance.

class Account {
  int balance;

  synchronized void deposit(int v) { balance = balance + v; }

  synchronized void depositTwice(int v) {   // re-enters this monitor
    deposit(v);
    deposit(v);
  }

  synchronized int withdraw(int v) {
    if (v > balance) throw new RuntimeException();
    balance = balance - v;
    return balance;                         // monitorexit before ret
  }

  int depth(int n) {
    synchronized (this) {
      if (n == 0) return 0;
      return 1 + depth(n - 1);              // nested re-entry n times
    }
  }
}

class Registry {
  static int created;
  static synchronized void register() { created = created + 1; }
}

class Bumper extends Thread {
  Account a;
  Bumper(Account a) { this.a = a; }
  public void run() {
    for (int i = 0; i < 1000; i++) {
      a.deposit(1);
      Registry.register();
    }
  }
}

public class SyncRules {
  public static void main(String[] args) {
    Account a = new Account();

    a.depositTwice(5);
    System.out.println("1. reentrant depositTwice -> balance=" + a.balance);
    System.out.println("   nested synchronized depth = " + a.depth(50));

    try {
      a.withdraw(1000);
    } catch (RuntimeException e) {
      System.out.println("2. withdraw threw " + e + " ...");
    }
    // if the monitor had leaked, this other thread would block forever
    Bumper t = new Bumper(a);
    t.start();
    t.join();
    System.out.println("   ...and another thread could still lock it: balance=" + a.balance);

    System.out.println("3. withdraw(3) returned " + a.withdraw(3));

    Object o = new Object();
    try {
      o.notify();
    } catch (IllegalMonitorStateException e) {
      System.out.println("4. notify() without the lock -> IllegalMonitorStateException");
    }
    try {
      o.wait();
    } catch (IllegalMonitorStateException e) {
      System.out.println("   wait() without the lock   -> IllegalMonitorStateException");
    }

    Bumper b1 = new Bumper(a);
    Bumper b2 = new Bumper(a);
    b1.start(); b2.start();
    b1.join(); b2.join();
    System.out.println("5. static synchronized register() count = " + Registry.created + " (expected 3000)");
    System.out.println("   balance = " + a.balance + " (expected " + (10 + 3000 - 3) + ")");
  }
}
