// Classes, inheritance, virtual dispatch, static fields, arrays, strings,
// exceptions.  Plain Java: compiles with mjavac and with javac --release 8.
class Shape {
  static int created;
  int id;
  Shape(int id) { this.id = id; created = created + 1; }
  int area() { return 0; }
  String kind() { return "shape"; }
  public String toString() { return kind() + "#" + id + "(area " + area() + ")"; }
}

class Rect extends Shape {
  int w, h;
  Rect(int id, int w, int h) { super(id); this.w = w; this.h = h; }
  int area() { return w * h; }
  String kind() { return "rect"; }
}

class Square extends Rect {
  Square(int id, int s) { super(id, s, s); }
  String kind() { return "square"; }
}

class MyError extends RuntimeException {
  int code;
  MyError(String msg, int code) { super(msg); this.code = code; }
}

public class Hello {
  static int fib(int n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
  }

  public static void main(String[] args) {
    System.out.println("Hello from Java on xv6-aarch64!");
    Shape a = new Rect(1, 3, 4);
    Shape b = new Square(2, 5);
    System.out.println(a);
    System.out.println(b);
    System.out.println("shapes created: " + Shape.created);

    int[] xs = new int[10];
    for (int i = 0; i < xs.length; i++) xs[i] = fib(i);
    String s = "fib:";
    for (int i = 0; i < xs.length; i++) s = s + " " + xs[i];
    System.out.println(s);

    try {
      throw new MyError("custom", 42);
    } catch (MyError e) {
      System.out.println("caught " + e.getMessage() + " code=" + e.code);
    }
    try {
      int z = 0;
      System.out.println(10 / z);
    } catch (ArithmeticException e) {
      System.out.println("caught " + e);
    }
    try {
      xs[10] = 1;
    } catch (RuntimeException e) {
      System.out.println("caught " + e);
    }
    String hello = "Hello";
    System.out.println("literals are interned: " + (hello == "Hello"));
  }
}
