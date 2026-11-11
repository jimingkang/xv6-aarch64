// Force-included (-include) into every C++ translation unit built for xv6.
//
// xv6 has no C++ standard library and the cross compilers used for it ship
// only libgcc, so user/cxx provides a small header-only library plus a tiny
// runtime (user/cxx/cxxrt.cpp).  C++ exceptions need an unwinder and the
// C++ ABI support library, neither of which exists here.  Code is built with
// -fno-exceptions and the three keywords are mapped to a "report and exit"
// model:
//
//   throw E(...)               prints E's what() to fd 2 and exits with 1
//   try { body }               runs body
//   catch (T name) { ... }     never runs (needs a named parameter)
//
// That matches programs whose handlers only report and give up.  Code that
// recovers from an exception and carries on is not supported.
#ifndef XV6_CXX_CONFIG_H
#define XV6_CXX_CONFIG_H

#define _XV6_CXX 1

namespace __xv6cxx {
[[noreturn]] void fail_message(const char *kind, const char *what);

// Converts to any reference type; only named so dead catch blocks compile.
struct any_value {
  template <class T> operator T &() const { fail_message("catch", "unreachable"); }
};

struct thrower {
  template <class E>
  [[noreturn]] void operator<<=(const E &e) const {
    report(e, 0);
  }
private:
  template <class E>
  [[noreturn]] static void report(const E &e, decltype(e.what(), 0)) {
    fail_message("exception", e.what());
  }
  template <class E>
  [[noreturn]] static void report(const E &, long) {
    fail_message("exception", "(non-std::exception object)");
  }
};
}

#define try if (true)
#define catch(...) else if (false) for (__VA_ARGS__ = ::__xv6cxx::any_value(); false;)
#define throw ::__xv6cxx::thrower() <<=
#define thread_local

#endif
