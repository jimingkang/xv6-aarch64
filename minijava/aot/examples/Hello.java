// Basic classes, inheritance, virtual dispatch, arrays, exceptions.
class Shape {
  int id;
  Shape(int id) { this.id = id; }
  int area() { return 0; }
  String kind() { return "shape"; }
}

class Rect extends Shape {
  int w, h;
  Rect(int id, int w, int h) { this.id = id; this.w = w; this.h = h; }
  int area() { return w * h; }
  String kind() { return "rect"; }
}

class Square extends Rect {
  Square(int id, int s) { this.id = id; this.w = s; this.h = s; }
  String kind() { return "square"; }
}

class MyError extends RuntimeException {
  int code;
  MyError(int code) { this.code = code; }
}

public class Hello {
  static int fib(int n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
  }

  public static void main(String[] args) {
    System.out.println("Hello from MiniJava on xv6-aarch64!");
    Shape a = new Rect(1, 3, 4);
    Shape b = new Square(2, 5);
    System.out.println(a.kind() + " area=" + a.area());
    System.out.println(b.kind() + " area=" + b.area());

    int[] xs = new int[10];
    for (int i = 0; i < xs.length; i++) xs[i] = fib(i);
    System.out.print("fib:");
    for (int i = 0; i < xs.length; i++) System.out.print(" " + xs[i]);
    System.out.println();

    try {
      throw new MyError(42);
    } catch (MyError e) {
      System.out.println("caught MyError code=" + e.code);
    }
    try {
      int z = 0;
      int y = 10 / z;
    } catch (ArithmeticException e) {
      System.out.println("caught ArithmeticException (/ by zero)");
    }
    try {
      xs[10] = 1;
    } catch (RuntimeException e) {
      System.out.println("caught " + e);
    }
  }
}
