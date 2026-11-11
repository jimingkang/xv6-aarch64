// cxxrt.cpp -- the C++ runtime for xv6 user programs: operator new/delete,
// the static-destructor list (__cxa_atexit), the "exception" reporting used
// by __xv6_cxx_config.h, and std::cout/cerr/clog.
#include <cstddef>
#include <new>
#include <exception>
#include <iostream>
#include <__xv6_libc.h>
#include "../include/mpi.h"

void *__dso_handle = &__dso_handle;

namespace {
struct atexit_entry {
  void (*fn)(void *);
  void *arg;
};
atexit_entry *entries;
int nentries, capentries;
bool finishing;

void run_atexit() {
  while(nentries > 0) {
    atexit_entry e = entries[--nentries];
    e.fn(e.arg);
  }
}

void flush_streams() {
  std::cout.flush();
  std::clog.flush();
  std::cerr.flush();
}

[[noreturn]] void oom() {
  static const char m[] = "c++: out of memory\n";
  std::cout.flush();
  ::write(2, m, sizeof(m) - 1);
  ::exit(1);
}

void *xalloc(std::size_t n) {
  void *p = ::malloc((unsigned)(n ? n : 1));
  if(!p) oom();
  return p;
}
}

namespace std {
const nothrow_t nothrow{};
// cerr is unbuffered and, like clog, flushes cout first.
ostream cout(1);
ostream cerr(2, &cout, true);
ostream clog(2, &cout);

[[noreturn]] void exit(int status) {
  if(!finishing) {
    finishing = true;
    run_atexit();
  }
  flush_streams();
  ::exit(status);
}
[[noreturn]] void abort() {
  flush_streams();
  ::exit(134);
}
[[noreturn]] void terminate() noexcept { __xv6cxx::fail_message("terminate", "std::terminate called"); }
[[noreturn]] void rethrow_exception(exception_ptr) { __xv6cxx::fail_message("exception", "rethrow of an unknown exception"); }
[[noreturn]] void __throw_out_of_range(const char *w) { __xv6cxx::fail_message("std::out_of_range", w); }
[[noreturn]] void __throw_length_error(const char *w) { __xv6cxx::fail_message("std::length_error", w); }
[[noreturn]] void __throw_bad_function_call() { __xv6cxx::fail_message("std::bad_function_call", "empty std::function"); }
}

namespace __xv6cxx {
[[noreturn]] void fail_message(const char *kind, const char *what) {
  flush_streams();
  ::fprintf(2, "%s: %s\n", kind, what);
  // In an MPI job the other ranks would wait for this one forever.
  int mpi = 0;
  if(MPI_Initialized(&mpi) == MPI_SUCCESS && mpi)
    MPI_Abort(MPI_COMM_WORLD, 1);
  ::exit(1);
}
}

extern "C" {
int __cxa_atexit(void (*fn)(void *), void *arg, void *) {
  if(nentries == capentries) {
    int nc = capentries ? 2 * capentries : 32;
    atexit_entry *ne = static_cast<atexit_entry *>(::realloc(entries, nc * sizeof(atexit_entry)));
    if(!ne) return -1;
    entries = ne;
    capentries = nc;
  }
  entries[nentries++] = {fn, arg};
  return 0;
}
static void call_plain(void *f) { reinterpret_cast<void (*)()>(f)(); }
int atexit(void (*fn)()) { return __cxa_atexit(call_plain, reinterpret_cast<void *>(fn), nullptr); }
void __cxa_pure_virtual() { __xv6cxx::fail_message("c++", "pure virtual function called"); }
void __cxa_deleted_virtual() { __xv6cxx::fail_message("c++", "deleted virtual function called"); }
void __xv6cxx_fini() {
  if(!finishing) {
    finishing = true;
    run_atexit();
  }
  flush_streams();
}
}

void *operator new(std::size_t n) { return xalloc(n); }
void *operator new[](std::size_t n) { return xalloc(n); }
void *operator new(std::size_t n, const std::nothrow_t &) noexcept { return ::malloc((unsigned)(n ? n : 1)); }
void *operator new[](std::size_t n, const std::nothrow_t &) noexcept { return ::malloc((unsigned)(n ? n : 1)); }
void *operator new(std::size_t n, std::align_val_t a) {
  // umalloc aligns to 16; larger alignments over-allocate and stash the base.
  std::size_t al = (std::size_t)a;
  if(al <= 16) return xalloc(n);
  char *raw = static_cast<char *>(xalloc(n + al + sizeof(void *)));
  char *p = (char *)(((unsigned long)(raw + sizeof(void *)) + al - 1) & ~(al - 1));
  ((void **)p)[-1] = raw;
  return p;
}
void *operator new[](std::size_t n, std::align_val_t a) { return operator new(n, a); }
void operator delete(void *p) noexcept { ::free(p); }
void operator delete[](void *p) noexcept { ::free(p); }
void operator delete(void *p, std::size_t) noexcept { ::free(p); }
void operator delete[](void *p, std::size_t) noexcept { ::free(p); }
void operator delete(void *p, std::align_val_t a) noexcept {
  if(!p) return;
  if((std::size_t)a <= 16) ::free(p);
  else ::free(((void **)p)[-1]);
}
void operator delete[](void *p, std::align_val_t a) noexcept { operator delete(p, a); }
void operator delete(void *p, std::size_t, std::align_val_t a) noexcept { operator delete(p, a); }
void operator delete[](void *p, std::size_t, std::align_val_t a) noexcept { operator delete(p, a); }
