// Bounded buffer with wait()/notifyAll() -- the textbook monitor.
//
//   put():  while (full)  wait();   ... notifyAll();
//   take(): while (empty) wait();   ... notifyAll();
//
// wait() must be called while holding the monitor; it releases it, sleeps
// in the object's wait set, and re-acquires it before returning.

class Buffer {
  int[] items;
  int head, tail, count;

  Buffer(int cap) { items = new int[cap]; }

  synchronized void put(int v) throws InterruptedException {
    while (count == items.length) wait();
    items[tail] = v;
    tail = (tail + 1) % items.length;
    count++;
    notifyAll();
  }

  synchronized int take() throws InterruptedException {
    while (count == 0) wait();
    int v = items[head];
    head = (head + 1) % items.length;
    count--;
    notifyAll();
    return v;
  }
}

class Producer extends Thread {
  Buffer b; int from; int n;
  Producer(Buffer b, int from, int n) { this.b = b; this.from = from; this.n = n; }
  public void run() {
    try {
      for (int i = 0; i < n; i++) b.put(from + i);
    } catch (InterruptedException e) {
      System.out.println("producer interrupted");
    }
  }
}

class Consumer extends Thread {
  Buffer b; int n; int sum;
  Consumer(Buffer b, int n) { this.b = b; this.n = n; }
  public void run() {
    try {
      for (int i = 0; i < n; i++) sum += b.take();
    } catch (InterruptedException e) {
      System.out.println("consumer interrupted");
    }
    System.out.println(getName() + " took " + n + " items, sum=" + sum);
  }
}

public class ProdCons {
  public static void main(String[] args) throws InterruptedException {
    Buffer b = new Buffer(4);
    Producer p1 = new Producer(b, 1, 500);       // 1..500
    Producer p2 = new Producer(b, 1001, 500);    // 1001..1500
    Consumer c1 = new Consumer(b, 400);
    Consumer c2 = new Consumer(b, 600);
    c1.start(); c2.start(); p1.start(); p2.start();
    p1.join(); p2.join(); c1.join(); c2.join();
    int total = c1.sum + c2.sum;
    // sum(1..500) + sum(1001..1500) = 125250 + 625250
    if (total == 750500) System.out.println("total=" + total + "  OK");
    else System.out.println("total=" + total + "  WRONG, expected 750500");
  }
}
