// petsc.cpp -- implementation of the xv6 PETSc subset (user/petsc/include).
//
// Distributed objects follow PETSc's layout rules: a parallel vector or
// matrix owns a contiguous block of rows, PETSC_DECIDE gives rank r
// N/size rows plus one if r < N%size, and a square matrix uses the same
// layout for its columns.  All communication goes through one primitive, a
// Plan: a fixed list of global entries that a rank needs from (or sends to)
// the entries' owners.  It is built once, collectively, and then moves
// values with point-to-point MPI_Isend/MPI_Irecv between neighbours only.
//
// Solvers: GMRES (left preconditioned, classical Gram-Schmidt applied twice,
// Givens rotations, restarts), CG, Richardson and PREONLY.  Preconditioners:
// restricted additive Schwarz with any overlap, whose subdomain problems are
// solved exactly by a band LU after reverse Cuthill-McKee ordering
// (user/sparse/bandlu.hpp); BJACOBI is ASM with overlap 0; JACOBI; LU on one
// rank; NONE.  Real PETSc solves ASM/BJACOBI blocks with ILU(0) by default.
#include <petscksp.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../sparse/bandlu.hpp"

namespace {

int rank_ = 0, size_ = 1;
bool initialized_ = false;
std::string sync_buf_;                  // PetscSynchronizedPrintf text not yet flushed
// -ksp_trace: mat_mult remembers the off-process x entries it received.
bool trace_ = false;
std::vector<PetscInt> trace_cols_;
std::vector<PetscScalar> trace_vals_;
std::vector<std::string> options_;

// Tags above anything the programs here use; per-pair FIFO order keeps
// consecutive operations apart.
constexpr int TAG_XCHG = 0x3fff0001;

PetscErrorCode error(int line, const char *func, PetscErrorCode n, const char *msg) {
  return PetscError(PETSC_COMM_WORLD, line, func, __FILE__, n, 1, "%s", msg);
}
#define FAIL(n, msg) return error(__LINE__, __func__, n, msg)

// --------------------------------------------------------------------------
// Ownership

struct Layout {
  PetscInt n = 0, N = 0, rstart = 0, rend = 0;
  std::vector<PetscInt> ranges;   // size_ + 1 entries

  int owner(PetscInt g) const {
    return int(std::upper_bound(ranges.begin(), ranges.end(), g) - ranges.begin()) - 1;
  }
  bool owns(PetscInt g) const { return g >= rstart && g < rend; }
};

PetscErrorCode make_layout(PetscInt n, PetscInt N, Layout &L) {
  if(n == PETSC_DECIDE && N < 0) FAIL(PETSC_ERR_ARG_WRONG, "need a local or a global size");
  if(n == PETSC_DECIDE) n = N / size_ + (rank_ < N % size_ ? 1 : 0);
  std::vector<int> all(size_);
  int mine = n;
  PetscCallMPI(MPI_Allgather(&mine, 1, MPI_INT, all.data(), 1, MPI_INT, PETSC_COMM_WORLD));
  L.ranges.assign(size_ + 1, 0);
  for(int r = 0; r < size_; r++) L.ranges[r + 1] = L.ranges[r] + all[r];
  if(N >= 0 && N != L.ranges[size_]) FAIL(PETSC_ERR_ARG_SIZ, "local sizes do not add up to the global size");
  L.n = n;
  L.N = L.ranges[size_];
  L.rstart = L.ranges[rank_];
  L.rend = L.ranges[rank_ + 1];
  return PETSC_SUCCESS;
}

Layout seq_layout(PetscInt n) {
  Layout L;
  L.n = L.N = n;
  L.rstart = 0;
  L.rend = n;
  L.ranges.assign(size_ + 1, n);
  for(int r = 0; r <= rank_; r++) L.ranges[r] = 0;
  return L;
}

// --------------------------------------------------------------------------
// Point-to-point exchange of grouped buffers

// Sends sbuf[soff[k] .. soff[k+1]) to sranks[k] and receives
// rbuf[roff[k] .. roff[k+1]) from rranks[k].
template <class T>
PetscErrorCode xchg(const std::vector<int> &sranks, const std::vector<long> &soff, const T *sbuf,
                    const std::vector<int> &rranks, const std::vector<long> &roff, T *rbuf) {
  std::vector<MPI_Request> q(sranks.size() + rranks.size(), MPI_REQUEST_NULL);
  std::size_t nq = 0;
  for(std::size_t k = 0; k < rranks.size(); k++)
    PetscCallMPI(MPI_Irecv(rbuf + roff[k], int((roff[k + 1] - roff[k]) * sizeof(T)), MPI_BYTE,
                           rranks[k], TAG_XCHG, PETSC_COMM_WORLD, &q[nq++]));
  for(std::size_t k = 0; k < sranks.size(); k++)
    PetscCallMPI(MPI_Isend(sbuf + soff[k], int((soff[k + 1] - soff[k]) * sizeof(T)), MPI_BYTE,
                           sranks[k], TAG_XCHG, PETSC_COMM_WORLD, &q[nq++]));
  PetscCallMPI(MPI_Waitall(int(nq), q.data(), MPI_STATUSES_IGNORE));
  return PETSC_SUCCESS;
}

// A Plan: the entries this rank needs from their owners ("requests",
// grouped by owner) and, on the owner side, the entries it serves
// ("serves", grouped by requester).  Values flow forward (owner ->
// requester) or in reverse (requester -> owner).
struct Plan {
  std::vector<int> req_ranks, srv_ranks;
  std::vector<long> req_off{0}, srv_off{0};
  std::vector<PetscInt> req_slot;   // requester's slot of each request
  std::vector<PetscInt> srv_glob;   // owner side: the global index requested
  std::vector<PetscInt> srv_pos;    // owner side: where to read or add (caller's numbering)

  std::size_t nreq() const { return req_slot.size(); }
  std::size_t nsrv() const { return srv_glob.size(); }

  // Collective.  Needs k: global index glob[k], requester slot slot[k];
  // every glob[k] must be owned by another rank.
  PetscErrorCode build(const Layout &L, std::size_t count, const PetscInt *glob, const PetscInt *slot) {
    std::vector<int> cnt(size_, 0), rcnt(size_, 0);
    for(std::size_t k = 0; k < count; k++) cnt[L.owner(glob[k])]++;
    PetscCallMPI(MPI_Alltoall(cnt.data(), 1, MPI_INT, rcnt.data(), 1, MPI_INT, PETSC_COMM_WORLD));
    req_ranks.clear(); srv_ranks.clear();
    req_off.assign(1, 0); srv_off.assign(1, 0);
    std::vector<long> start(size_ + 1, 0);
    for(int r = 0; r < size_; r++) {
      start[r + 1] = start[r] + cnt[r];
      if(cnt[r]) { req_ranks.push_back(r); req_off.push_back(start[r + 1]); }
      if(rcnt[r]) { srv_ranks.push_back(r); srv_off.push_back(srv_off.back() + rcnt[r]); }
    }
    req_slot.assign(count, 0);
    std::vector<PetscInt> gl(count);
    std::vector<long> fill(start.begin(), start.end() - 1);
    for(std::size_t k = 0; k < count; k++) {
      long at = fill[L.owner(glob[k])]++;
      req_slot[at] = slot[k];
      gl[at] = glob[k];
    }
    srv_glob.assign(srv_off.back(), 0);
    PetscCall(xchg(req_ranks, req_off, gl.data(), srv_ranks, srv_off, srv_glob.data()));
    srv_pos = srv_glob;
    for(auto &p : srv_pos) p -= L.rstart;   // default: owner-local index
    return PETSC_SUCCESS;
  }

  // Owner -> requester: aligned buffers (srv order in, req order out).
  template <class T> PetscErrorCode forward_aligned(const T *srv_vals, T *req_vals) const {
    return xchg(srv_ranks, srv_off, srv_vals, req_ranks, req_off, req_vals);
  }
  // Requester -> owner: aligned buffers (req order in, srv order out).
  template <class T> PetscErrorCode reverse_aligned(const T *req_vals, T *srv_vals) const {
    return xchg(req_ranks, req_off, req_vals, srv_ranks, srv_off, srv_vals);
  }
  // dst[req_slot[k]] = src[srv_pos[k]].
  PetscErrorCode forward(const PetscScalar *src, PetscScalar *dst) const {
    std::vector<PetscScalar> s(nsrv()), r(nreq());
    for(std::size_t k = 0; k < nsrv(); k++) s[k] = src[srv_pos[k]];
    PetscCall(forward_aligned(s.data(), r.data()));
    for(std::size_t k = 0; k < nreq(); k++) dst[req_slot[k]] = r[k];
    return PETSC_SUCCESS;
  }
  // dst[srv_pos[k]] += src[req_slot[k]].
  PetscErrorCode reverse_add(const PetscScalar *src, PetscScalar *dst) const {
    std::vector<PetscScalar> s(nreq()), r(nsrv());
    for(std::size_t k = 0; k < nreq(); k++) s[k] = src[req_slot[k]];
    PetscCall(reverse_aligned(s.data(), r.data()));
    for(std::size_t k = 0; k < nsrv(); k++) dst[srv_pos[k]] += r[k];
    return PETSC_SUCCESS;
  }
};

// Splits COO-style global rows into own entries and a Plan for the rest.
// local[k] = owner-local row or -1 (remote or ignored negative index).
PetscErrorCode route_entries(const Layout &L, PetscCount n, const PetscInt *rows,
                             std::vector<PetscInt> &local, Plan &plan) {
  local.assign((std::size_t)n, -1);
  std::vector<PetscInt> glob, slot;
  for(PetscCount k = 0; k < n; k++) {
    PetscInt g = rows[k];
    if(g < 0) continue;
    if(g >= L.N) FAIL(PETSC_ERR_ARG_OUTOFRANGE, "COO index out of range");
    if(L.owns(g)) local[k] = g - L.rstart;
    else { glob.push_back(g); slot.push_back(PetscInt(k)); }
  }
  return plan.build(L, glob.size(), glob.data(), slot.data());
}

bool option_index(const char *pre, const char *name, std::size_t &at) {
  std::string key = name;
  if(pre && *pre) key = std::string("-") + pre + (name[0] == '-' ? name + 1 : name);
  for(std::size_t i = options_.size(); i-- > 0;)
    if(options_[i] == key) { at = i; return true; }
  return false;
}
const char *option_value(std::size_t at) {
  if(at + 1 < options_.size()) {
    const std::string &v = options_[at + 1];
    // A following "-x" is the next option unless it is a negative number.
    if(v.size() && (v[0] != '-' || (v.size() > 1 && (v[1] == '.' || (v[1] >= '0' && v[1] <= '9')))))
      return v.c_str();
  }
  return nullptr;
}
double parse_real(const char *s) {
  // Minimal strtod: [sign] digits [. digits] [e [sign] digits].
  double v = 0, sign = 1;
  if(*s == '-') { sign = -1; s++; } else if(*s == '+') s++;
  while(*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
  if(*s == '.') {
    double f = 0.1;
    for(s++; *s >= '0' && *s <= '9'; s++, f /= 10) v += (*s - '0') * f;
  }
  if(*s == 'e' || *s == 'E') {
    int e = 0, es = 1;
    s++;
    if(*s == '-') { es = -1; s++; } else if(*s == '+') s++;
    while(*s >= '0' && *s <= '9') e = e * 10 + (*s++ - '0');
    v *= std::pow(10.0, es * e);
  }
  return sign * v;
}

}  // namespace

// ==========================================================================
// Objects

struct _p_Vec {
  Layout L;
  bool seq = false;
  std::vector<PetscScalar> storage;
  PetscScalar *data = nullptr;          // storage.data(), or the parent's
  PetscInt nghost = 0;
  std::vector<PetscInt> ghosts;
  Plan ghost_plan;
  Vec local = nullptr;                  // local form (views data)
  bool is_local_form = false;
  bool coo = false;
  std::vector<PetscInt> coo_local;
  Plan coo_plan;
  // VecSetValues stash for entries owned elsewhere.
  std::vector<PetscInt> stash_i;
  std::vector<PetscScalar> stash_v;
  std::vector<InsertMode> stash_m;
};

struct _p_VecScatter {
  bool to_zero = false;
  std::vector<int> counts, displs;
};

struct _p_Mat {
  PetscInt m = PETSC_DECIDE, M = -1;
  Layout L;
  bool assembled = false;
  std::vector<PetscInt> rowptr{0}, colg, coll;   // per local row, sorted global columns
  std::vector<PetscScalar> val;
  std::vector<PetscInt> col_ghosts;               // sorted off-process columns
  Plan col_plan;                                  // gathers x at col_ghosts
  std::vector<PetscInt> coo_pos;                  // value position of own COO entries
  Plan coo_plan;                                  // remote COO entries; srv_pos = value position
  long state = 0;                                 // bumped when values change
  long pattern = 0;                               // bumped when the pattern changes
};

struct _p_PC {
  std::string type = PCBJACOBI;
  PetscInt overlap = 1;
  Mat A = nullptr;
  long state = -1;
  std::string set_type;            // type the setup was done for
  Mat set_mat = nullptr;           // matrix and pattern the subdomain was built for
  long set_pattern = -1;
  PetscInt set_overlap = -1;
  bool failed = false;
  // ASM / LU
  std::vector<PetscInt> sub;       // sorted global indices of the subdomain
  std::vector<PetscInt> sub_own;   // position in sub of each owned row
  std::vector<PetscInt> ext;       // sub entries owned elsewhere
  Plan ext_plan;                   // brings x[ext] to positions in sub
  xv6sparse::BandLU lu;
  // JACOBI
  std::vector<PetscScalar> dinv;
};

struct _p_KSP {
  std::string type = KSPGMRES;
  PetscInt restart = 30, maxits = 10000;
  PetscReal rtol = 1e-5, atol = 1e-50, dtol = 1e5;
  bool guess_nonzero = false, monitor = false, show_reason = false, trace = false;
  PC pc = nullptr;
  Mat A = nullptr, P = nullptr;
  KSPConvergedReason reason = KSP_CONVERGED_ITERATING;
  PetscInt its = 0;
  PetscReal rnorm = 0;
};

namespace {

PetscErrorCode vec_new(const Layout &L, bool seq, PetscInt nghost, Vec *v) {
  Vec x = new _p_Vec;
  x->L = L;
  x->seq = seq;
  x->nghost = nghost;
  x->storage.assign((std::size_t)(L.n + nghost), 0.0);
  x->data = x->storage.data();
  *v = x;
  return PETSC_SUCCESS;
}

PetscErrorCode check_same(Vec x, Vec y) {
  if(!x || !y) FAIL(PETSC_ERR_ARG_WRONG, "null vector");
  if(x->L.n != y->L.n || x->L.N != y->L.N) FAIL(PETSC_ERR_ARG_SIZ, "vector layouts differ");
  return PETSC_SUCCESS;
}

double global_sum(double local, bool seq) {
  double r = local;
  if(!seq && size_ > 1) MPI_Allreduce(&local, &r, 1, MPI_DOUBLE, MPI_SUM, PETSC_COMM_WORLD);
  return r;
}

// y = A x on the owned rows; x and y hold the owned entries.
PetscErrorCode mat_mult(Mat A, const PetscScalar *x, PetscScalar *y) {
  if(!A->assembled) FAIL(PETSC_ERR_ARG_WRONGSTATE, "matrix is not assembled");
  const PetscInt n = A->L.n;
  std::vector<PetscScalar> xg(A->col_ghosts.size());
  PetscCall(A->col_plan.forward(x, xg.data()));
  if(trace_) { trace_cols_ = A->col_ghosts; trace_vals_ = xg; }
  for(PetscInt i = 0; i < n; i++) {
    PetscScalar s = 0;
    for(PetscInt k = A->rowptr[i]; k < A->rowptr[i + 1]; k++) {
      PetscInt c = A->coll[k];
      s += A->val[k] * (c < n ? x[c] : xg[c - n]);
    }
    y[i] = s;
  }
  return PETSC_SUCCESS;
}

// Rows of A owned elsewhere: global ids rows[] -> CSR (ptr, cols, vals).
PetscErrorCode fetch_rows(Mat A, const std::vector<PetscInt> &rows, std::vector<PetscInt> &ptr,
                          std::vector<PetscInt> &cols, std::vector<PetscScalar> &vals) {
  Plan p;
  std::vector<PetscInt> slot(rows.size());
  for(std::size_t k = 0; k < rows.size(); k++) slot[k] = PetscInt(k);
  PetscCall(p.build(A->L, rows.size(), rows.data(), slot.data()));
  // Owner: row lengths, then the rows themselves, in serve order.
  std::vector<PetscInt> slen(p.nsrv()), rlen(p.nreq());
  for(std::size_t k = 0; k < p.nsrv(); k++) {
    PetscInt i = p.srv_pos[k];
    slen[k] = A->rowptr[i + 1] - A->rowptr[i];
  }
  PetscCall(p.forward_aligned(slen.data(), rlen.data()));
  std::vector<long> soff{0}, roff{0};
  for(std::size_t g = 0; g + 1 < p.srv_off.size(); g++) {
    long t = 0;
    for(long k = p.srv_off[g]; k < p.srv_off[g + 1]; k++) t += slen[k];
    soff.push_back(soff.back() + t);
  }
  for(std::size_t g = 0; g + 1 < p.req_off.size(); g++) {
    long t = 0;
    for(long k = p.req_off[g]; k < p.req_off[g + 1]; k++) t += rlen[k];
    roff.push_back(roff.back() + t);
  }
  std::vector<PetscInt> sc((std::size_t)soff.back()), rc((std::size_t)roff.back());
  std::vector<PetscScalar> sv((std::size_t)soff.back()), rv((std::size_t)roff.back());
  long at = 0;
  for(std::size_t k = 0; k < p.nsrv(); k++) {
    PetscInt i = p.srv_pos[k];
    for(PetscInt e = A->rowptr[i]; e < A->rowptr[i + 1]; e++, at++) {
      sc[at] = A->colg[e];
      sv[at] = A->val[e];
    }
  }
  PetscCall(xchg(p.srv_ranks, soff, sc.data(), p.req_ranks, roff, rc.data()));
  PetscCall(xchg(p.srv_ranks, soff, sv.data(), p.req_ranks, roff, rv.data()));
  // Requests are grouped by owner; put rows back in the caller's order.
  std::vector<long> start(p.nreq() + 1, 0);
  for(std::size_t k = 0; k < p.nreq(); k++) start[k + 1] = start[k] + rlen[k];
  ptr.assign(rows.size() + 1, 0);
  std::vector<std::size_t> which(rows.size());
  for(std::size_t k = 0; k < p.nreq(); k++) which[p.req_slot[k]] = k;
  for(std::size_t r = 0; r < rows.size(); r++) ptr[r + 1] = ptr[r] + rlen[which[r]];
  cols.assign((std::size_t)ptr.back(), 0);
  vals.assign((std::size_t)ptr.back(), 0.0);
  for(std::size_t r = 0; r < rows.size(); r++) {
    long from = start[which[r]];
    for(PetscInt e = 0; e < ptr[r + 1] - ptr[r]; e++) {
      cols[ptr[r] + e] = rc[from + e];
      vals[ptr[r] + e] = rv[from + e];
    }
  }
  return PETSC_SUCCESS;
}

bool sorted_has(const std::vector<PetscInt> &v, PetscInt x) {
  return std::binary_search(v.begin(), v.end(), x);
}

PetscErrorCode pc_setup_subdomain(PC pc) {
  Mat A = pc->A;
  const Layout &L = A->L;
  const bool lu = pc->type == PCLU;
  PetscInt overlap = pc->type == PCASM ? pc->overlap : 0;
  if(lu) {
    if(size_ > 1) FAIL(PETSC_ERR_SUP, "PCLU needs one rank here; use PCASM");
    overlap = 0;
  }
  if(pc->set_type != pc->type || pc->set_overlap != overlap || pc->set_mat != A ||
     pc->set_pattern != A->pattern) {
    // Grow the subdomain by `overlap` layers of the matrix graph.
    std::vector<PetscInt> sub, frontier;
    for(PetscInt g = L.rstart; g < L.rend; g++) sub.push_back(g);
    frontier = sub;
    for(PetscInt level = 0; level < overlap; level++) {
      std::vector<PetscInt> remote, ptr, cols;
      std::vector<PetscScalar> vals;
      for(PetscInt g : frontier) if(!L.owns(g)) remote.push_back(g);
      PetscCall(fetch_rows(A, remote, ptr, cols, vals));
      std::vector<PetscInt> grow;
      std::size_t r = 0;
      for(PetscInt g : frontier) {
        if(L.owns(g)) {
          PetscInt i = g - L.rstart;
          for(PetscInt e = A->rowptr[i]; e < A->rowptr[i + 1]; e++) grow.push_back(A->colg[e]);
        } else {
          for(PetscInt e = ptr[r]; e < ptr[r + 1]; e++) grow.push_back(cols[e]);
          r++;
        }
      }
      std::sort(grow.begin(), grow.end());
      grow.erase(std::unique(grow.begin(), grow.end()), grow.end());
      frontier.clear();
      for(PetscInt g : grow) if(!sorted_has(sub, g)) frontier.push_back(g);
      std::vector<PetscInt> merged;
      merged.reserve(sub.size() + frontier.size());
      std::size_t a = 0, b = 0;
      while(a < sub.size() || b < frontier.size())
        merged.push_back(b == frontier.size() || (a < sub.size() && sub[a] < frontier[b]) ? sub[a++] : frontier[b++]);
      sub.swap(merged);
    }
    pc->sub = sub;
    pc->sub_own.clear();
    pc->ext.clear();
    std::vector<PetscInt> slots;
    for(std::size_t k = 0; k < sub.size(); k++) {
      if(L.owns(sub[k])) pc->sub_own.push_back(PetscInt(k));
      else { pc->ext.push_back(sub[k]); slots.push_back(PetscInt(k)); }
    }
    PetscCall(pc->ext_plan.build(L, pc->ext.size(), pc->ext.data(), slots.data()));
    pc->set_type = pc->type;
    pc->set_overlap = overlap;
    pc->set_mat = A;
    pc->set_pattern = A->pattern;
  }
  // Values of the subdomain matrix A[sub, sub].
  std::vector<PetscInt> ptr, cols;
  std::vector<PetscScalar> vals;
  PetscCall(fetch_rows(A, pc->ext, ptr, cols, vals));
  const std::vector<PetscInt> &sub = pc->sub;
  const long ns = long(sub.size());
  std::vector<long> cnt(ns + 1, 0);
  std::vector<long> ri, ci;
  std::vector<double> vv;
  auto add_row = [&](long row, const PetscInt *c, const PetscScalar *v, PetscInt len) {
    for(PetscInt e = 0; e < len; e++) {
      auto it = std::lower_bound(sub.begin(), sub.end(), c[e]);
      if(it == sub.end() || *it != c[e]) continue;
      ri.push_back(row);
      ci.push_back(long(it - sub.begin()));
      vv.push_back(v[e]);
    }
  };
  std::size_t er = 0;
  for(long k = 0; k < ns; k++) {
    if(L.owns(sub[k])) {
      PetscInt i = sub[k] - L.rstart;
      add_row(k, &A->colg[A->rowptr[i]], &A->val[A->rowptr[i]], A->rowptr[i + 1] - A->rowptr[i]);
    } else {
      add_row(k, cols.data() + ptr[er], vals.data() + ptr[er], ptr[er + 1] - ptr[er]);
      er++;
    }
  }
  // Column-compressed form for the band LU.
  for(long c : ci) cnt[c + 1]++;
  for(long c = 0; c < ns; c++) cnt[c + 1] += cnt[c];
  std::vector<long> idx(ri.size());
  std::vector<double> cv(ri.size());
  std::vector<long> fill(cnt.begin(), cnt.end() - 1);
  for(std::size_t e = 0; e < ri.size(); e++) {
    long at = fill[ci[e]]++;
    idx[at] = ri[e];
    cv[at] = vv[e];
  }
  int ok = ns == 0 || pc->lu.factor(ns, cnt.data(), idx.data(), cv.data());
  int all_ok = ok;
  PetscCallMPI(MPI_Allreduce(&ok, &all_ok, 1, MPI_INT, MPI_MIN, PETSC_COMM_WORLD));
  pc->failed = !all_ok;
  return PETSC_SUCCESS;
}

PetscErrorCode pc_setup(PC pc) {
  if(!pc->A) FAIL(PETSC_ERR_ARG_WRONGSTATE, "PC has no operator");
  if(pc->state == pc->A->state && pc->set_mat == pc->A && pc->set_type == pc->type &&
     (pc->type != PCASM || pc->set_overlap == pc->overlap))
    return PETSC_SUCCESS;
  Mat A = pc->A;
  pc->failed = false;
  if(pc->type == PCNONE) {
  } else if(pc->type == PCJACOBI) {
    pc->dinv.assign((std::size_t)A->L.n, 1.0);
    for(PetscInt i = 0; i < A->L.n; i++)
      for(PetscInt k = A->rowptr[i]; k < A->rowptr[i + 1]; k++)
        if(A->colg[k] == A->L.rstart + i && A->val[k] != 0.0) pc->dinv[i] = 1.0 / A->val[k];
  } else if(pc->type == PCASM || pc->type == PCBJACOBI || pc->type == PCLU) {
    PetscCall(pc_setup_subdomain(pc));
  } else {
    FAIL(PETSC_ERR_SUP, "unsupported PC type");
  }
  pc->set_type = pc->type;
  pc->set_mat = A;
  pc->state = A->state;
  return PETSC_SUCCESS;
}

// y = B x on owned entries.
PetscErrorCode pc_apply(PC pc, const PetscScalar *x, PetscScalar *y) {
  const PetscInt n = pc->A->L.n;
  if(pc->type == PCNONE) {
    for(PetscInt i = 0; i < n; i++) y[i] = x[i];
  } else if(pc->type == PCJACOBI) {
    for(PetscInt i = 0; i < n; i++) y[i] = pc->dinv[i] * x[i];
  } else {
    // Restricted additive Schwarz: gather x on the subdomain, solve, keep
    // the owned part.
    std::vector<PetscScalar> w(pc->sub.size());
    for(PetscInt i = 0; i < n; i++) w[pc->sub_own[i]] = x[i];
    PetscCall(pc->ext_plan.forward(x, w.data()));
    if(!w.empty()) pc->lu.solve(w.data());
    for(PetscInt i = 0; i < n; i++) y[i] = w[pc->sub_own[i]];
  }
  return PETSC_SUCCESS;
}

double dot_local(const std::vector<PetscScalar> &a, const std::vector<PetscScalar> &b) {
  double s = 0;
  for(std::size_t i = 0; i < a.size(); i++) s += a[i] * b[i];
  return s;
}

bool test_conv(KSP ksp, double res, double rnorm0) {
  ksp->rnorm = res;
  if(!std::isfinite(res)) { ksp->reason = KSP_DIVERGED_NANORINF; return true; }
  if(res <= ksp->atol) { ksp->reason = KSP_CONVERGED_ATOL; return true; }
  if(res <= ksp->rtol * rnorm0) { ksp->reason = KSP_CONVERGED_RTOL; return true; }
  if(rnorm0 > 0 && res >= ksp->dtol * rnorm0) { ksp->reason = KSP_DIVERGED_DTOL; return true; }
  return false;
}

void monitor(KSP ksp, double res) {
  if(ksp->monitor && rank_ == 0)
    printf("%s%d KSP Residual norm %e\n", ksp->its < 10 ? "  " : ksp->its < 100 ? " " : "", int(ksp->its), res);
}

// z = B (b - A x).
PetscErrorCode precond_residual(KSP ksp, const PetscScalar *b, const std::vector<PetscScalar> &x,
                                bool zero_x, std::vector<PetscScalar> &t, std::vector<PetscScalar> &z) {
  const std::size_t n = x.size();
  if(zero_x) for(std::size_t i = 0; i < n; i++) t[i] = b[i];
  else {
    PetscCall(mat_mult(ksp->A, x.data(), t.data()));
    for(std::size_t i = 0; i < n; i++) t[i] = b[i] - t[i];
  }
  return pc_apply(ksp->pc, t.data(), z.data());
}

// -ksp_trace output, printed per rank in rank order.
std::string fmt_vec(const PetscScalar *a, std::size_t n) {
  std::string s = "[";
  char buf[40];
  for(std::size_t i = 0; i < n; i++) {
    snprintf(buf, sizeof(buf), "%s%.12g", i ? "," : "", a[i]);
    s += buf;
  }
  return s + "]";
}
std::string fmt_vec(const std::vector<PetscScalar> &a) { return fmt_vec(a.data(), a.size()); }
std::string fmt_num(double x) {
  char buf[40];
  snprintf(buf, sizeof(buf), "%.12g", x);
  return buf;
}
std::string fmt_recv() {
  std::string s = "recv col[";
  char buf[16];
  for(std::size_t i = 0; i < trace_cols_.size(); i++) {
    snprintf(buf, sizeof(buf), "%s%d", i ? "," : "", int(trace_cols_[i]));
    s += buf;
  }
  return s + "]=" + fmt_vec(trace_vals_);
}
void trace(int step, const std::string &text) {
  PetscSynchronizedPrintf(PETSC_COMM_WORLD, "[gmres] rank %d step %d %s\n", rank_, step, text.c_str());
}

// True residual of the current GMRES iterate x + V y, where y solves the
// rotated (upper triangular) j x j system; used only by -ksp_trace.
PetscErrorCode trace_true_residual(KSP ksp, int step, const PetscScalar *b, const std::vector<PetscScalar> &x,
                                   const std::vector<std::vector<PetscScalar>> &V, const std::vector<double> &H,
                                   const std::vector<double> &g, int m, int j, double estimate) {
  const std::size_t n = x.size();
  auto Hij = [&](int i, int k) { return H[(std::size_t)k * (m + 1) + i]; };
  std::vector<double> y(j > 0 ? j : 1);
  for(int i = j - 1; i >= 0; i--) {
    double s = g[i];
    for(int k = i + 1; k < j; k++) s -= Hij(i, k) * y[k];
    y[i] = Hij(i, i) != 0.0 ? s / Hij(i, i) : 0.0;
  }
  std::vector<PetscScalar> xj(x), r(n);
  for(int i = 0; i < j; i++)
    for(std::size_t k = 0; k < n; k++) xj[k] += y[i] * V[i][k];
  PetscCall(mat_mult(ksp->A, xj.data(), r.data()));
  for(std::size_t k = 0; k < n; k++) r[k] = b[k] - r[k];
  double local = dot_local(r, r), global = global_sum(local, false);
  trace(step, "x=" + fmt_vec(xj) + " " + fmt_recv() + " r=b-Kx=" + fmt_vec(r));
  trace(step, "local r.r=" + fmt_num(local) + " allreduce=" + fmt_num(global) +
              " true|r|=" + fmt_num(std::sqrt(global)) + " KSP=" + fmt_num(estimate));
  return PetscSynchronizedFlush(PETSC_COMM_WORLD, nullptr);
}

PetscErrorCode solve_gmres(KSP ksp, const PetscScalar *b, std::vector<PetscScalar> &x) {
  const std::size_t n = x.size();
  const int m = std::max<PetscInt>(1, ksp->restart);
  std::vector<std::vector<PetscScalar>> V(m + 1, std::vector<PetscScalar>(n));
  std::vector<double> H((std::size_t)(m + 1) * m), cs(m), sn(m), g(m + 1), h(m + 1), h2(m + 1), y(m);
  auto Hij = [&](int i, int j) -> double & { return H[(std::size_t)j * (m + 1) + i]; };
  std::vector<PetscScalar> t(n), w(n);
  bool zero_x = !ksp->guess_nonzero;
  double rnorm0 = -1;
  for(;;) {
    PetscCall(precond_residual(ksp, b, x, zero_x, t, V[0]));
    zero_x = false;
    double beta = std::sqrt(global_sum(dot_local(V[0], V[0]), false));
    if(ksp->trace) {
      trace(ksp->its, "r0=" + fmt_vec(t) + " local r0.r0=" + fmt_num(dot_local(t, t)) +
                      " beta=" + fmt_num(beta));
    }
    if(rnorm0 < 0) {
      rnorm0 = beta;
      monitor(ksp, beta);
      if(test_conv(ksp, beta, rnorm0) || beta == 0.0) {
        if(beta == 0.0) ksp->reason = KSP_CONVERGED_ATOL;
        return PETSC_SUCCESS;
      }
    }
    for(std::size_t i = 0; i < n; i++) V[0][i] /= beta;
    if(ksp->trace) {
      trace(ksp->its, "v1=" + fmt_vec(V[0]));
      PetscCall(trace_true_residual(ksp, ksp->its, b, x, V, H, g, m, 0, beta));
    }
    std::fill(g.begin(), g.end(), 0.0);
    g[0] = beta;
    int j = 0;
    bool done = false;
    for(; j < m && ksp->its < ksp->maxits; j++) {
      ksp->its++;
      PetscCall(mat_mult(ksp->A, V[j].data(), t.data()));
      PetscCall(pc_apply(ksp->pc, t.data(), w.data()));
      const int step = int(ksp->its);
      const std::string col = std::to_string(j + 1);
      if(ksp->trace)
        trace(step, "w=B K v" + col + ": v" + col + "=" + fmt_vec(V[j]) + " " + fmt_recv() +
                    " Kv=" + fmt_vec(t) + " w=" + fmt_vec(w));
      // Classical Gram-Schmidt, applied twice; one reduction per pass.
      for(int pass = 0; pass < 2; pass++) {
        std::vector<double> &hh = pass ? h2 : h;
        std::vector<double> loc(j + 1);
        for(int i = 0; i <= j; i++) loc[i] = dot_local(V[i], w);
        if(size_ > 1)
          PetscCallMPI(MPI_Allreduce(loc.data(), hh.data(), j + 1, MPI_DOUBLE, MPI_SUM, PETSC_COMM_WORLD));
        else
          for(int i = 0; i <= j; i++) hh[i] = loc[i];
        if(ksp->trace) {
          std::string dots;
          for(int i = 0; i <= j; i++)
            dots += " h" + std::to_string(i + 1) + col + ":" + fmt_num(loc[i]) + "->" + fmt_num(hh[i]);
          trace(step, std::string(pass ? "CGS pass 2" : "CGS pass 1") + " local v.w -> reduced" + dots);
        }
        for(int i = 0; i <= j; i++)
          for(std::size_t k = 0; k < n; k++) w[k] -= hh[i] * V[i][k];
      }
      for(int i = 0; i <= j; i++) Hij(i, j) = h[i] + h2[i];
      const double ww = dot_local(w, w);
      double hn = std::sqrt(global_sum(ww, false));
      Hij(j + 1, j) = hn;
      if(ksp->trace)
        trace(step, "w-sum(h v)=" + fmt_vec(w) + " local w.w=" + fmt_num(ww) + " h" +
                    std::to_string(j + 2) + col + "=" + fmt_num(hn));
      for(int i = 0; i < j; i++) {
        double a = Hij(i, j), c = Hij(i + 1, j);
        Hij(i, j) = cs[i] * a + sn[i] * c;
        Hij(i + 1, j) = -sn[i] * a + cs[i] * c;
      }
      double a = Hij(j, j), c = Hij(j + 1, j), r = std::hypot(a, c);
      if(r == 0.0) { cs[j] = 1; sn[j] = 0; } else { cs[j] = a / r; sn[j] = c / r; }
      Hij(j, j) = r;
      Hij(j + 1, j) = 0;
      g[j + 1] = -sn[j] * g[j];
      g[j] = cs[j] * g[j];
      double res = std::abs(g[j + 1]);
      monitor(ksp, res);
      if(ksp->trace) {
        if(hn != 0.0) {
          std::vector<PetscScalar> next(n);
          for(std::size_t k = 0; k < n; k++) next[k] = w[k] / hn;
          trace(step, "v" + std::to_string(j + 2) + "=" + fmt_vec(next));
        }
        PetscCall(trace_true_residual(ksp, step, b, x, V, H, g, m, j + 1, res));
      }
      if(test_conv(ksp, res, rnorm0)) { j++; done = true; break; }
      if(hn == 0.0) { ksp->reason = KSP_CONVERGED_HAPPY_BREAKDOWN; j++; done = true; break; }
      for(std::size_t k = 0; k < n; k++) V[j + 1][k] = w[k] / hn;
    }
    // x += V y with H y = g.
    for(int i = j - 1; i >= 0; i--) {
      double s = g[i];
      for(int k = i + 1; k < j; k++) s -= Hij(i, k) * y[k];
      y[i] = Hij(i, i) != 0.0 ? s / Hij(i, i) : 0.0;
    }
    for(int i = 0; i < j; i++)
      for(std::size_t k = 0; k < n; k++) x[k] += y[i] * V[i][k];
    if(done) return PETSC_SUCCESS;
    if(ksp->its >= ksp->maxits) { ksp->reason = KSP_DIVERGED_ITS; return PETSC_SUCCESS; }
  }
}

PetscErrorCode solve_cg(KSP ksp, const PetscScalar *b, std::vector<PetscScalar> &x) {
  const std::size_t n = x.size();
  std::vector<PetscScalar> r(n), z(n), p(n), q(n);
  if(ksp->guess_nonzero) {
    PetscCall(mat_mult(ksp->A, x.data(), q.data()));
    for(std::size_t i = 0; i < n; i++) r[i] = b[i] - q[i];
  } else {
    for(std::size_t i = 0; i < n; i++) r[i] = b[i];
  }
  PetscCall(pc_apply(ksp->pc, r.data(), z.data()));
  double rz = global_sum(dot_local(r, z), false);
  double rnorm0 = std::sqrt(std::abs(rz)), res = rnorm0;
  monitor(ksp, res);
  if(test_conv(ksp, res, rnorm0) || res == 0.0) {
    if(res == 0.0) ksp->reason = KSP_CONVERGED_ATOL;
    return PETSC_SUCCESS;
  }
  p = z;
  while(ksp->its < ksp->maxits) {
    ksp->its++;
    PetscCall(mat_mult(ksp->A, p.data(), q.data()));
    double pq = global_sum(dot_local(p, q), false);
    if(pq <= 0) { ksp->reason = KSP_DIVERGED_INDEFINITE_PC; return PETSC_SUCCESS; }
    double alpha = rz / pq;
    for(std::size_t i = 0; i < n; i++) { x[i] += alpha * p[i]; r[i] -= alpha * q[i]; }
    PetscCall(pc_apply(ksp->pc, r.data(), z.data()));
    double rz1 = global_sum(dot_local(r, z), false);
    res = std::sqrt(std::abs(rz1));
    monitor(ksp, res);
    if(test_conv(ksp, res, rnorm0)) return PETSC_SUCCESS;
    for(std::size_t i = 0; i < n; i++) p[i] = z[i] + rz1 / rz * p[i];
    rz = rz1;
  }
  ksp->reason = KSP_DIVERGED_ITS;
  return PETSC_SUCCESS;
}

PetscErrorCode solve_richardson(KSP ksp, const PetscScalar *b, std::vector<PetscScalar> &x) {
  const std::size_t n = x.size();
  std::vector<PetscScalar> t(n), z(n);
  bool zero_x = !ksp->guess_nonzero;
  double rnorm0 = -1;
  for(;;) {
    PetscCall(precond_residual(ksp, b, x, zero_x, t, z));
    zero_x = false;
    double res = std::sqrt(global_sum(dot_local(z, z), false));
    if(rnorm0 < 0) rnorm0 = res;
    monitor(ksp, res);
    if(test_conv(ksp, res, rnorm0)) return PETSC_SUCCESS;
    if(ksp->its >= ksp->maxits) { ksp->reason = KSP_DIVERGED_ITS; return PETSC_SUCCESS; }
    ksp->its++;
    for(std::size_t i = 0; i < n; i++) x[i] += z[i];
  }
}

const char *reason_name(KSPConvergedReason r) {
  switch(r) {
  case KSP_CONVERGED_RTOL: return "CONVERGED_RTOL";
  case KSP_CONVERGED_ATOL: return "CONVERGED_ATOL";
  case KSP_CONVERGED_ITS: return "CONVERGED_ITS";
  case KSP_CONVERGED_HAPPY_BREAKDOWN: return "CONVERGED_HAPPY_BREAKDOWN";
  case KSP_DIVERGED_ITS: return "DIVERGED_ITS";
  case KSP_DIVERGED_DTOL: return "DIVERGED_DTOL";
  case KSP_DIVERGED_NANORINF: return "DIVERGED_NANORINF";
  case KSP_DIVERGED_PC_FAILED: return "DIVERGED_PC_FAILED";
  case KSP_DIVERGED_INDEFINITE_PC: return "DIVERGED_INDEFINITE_PC";
  default: return "ITERATING";
  }
}

}  // namespace

// ==========================================================================
// System

extern "C" {

PetscErrorCode PetscInitialize(int *argc, char ***argv, const char *, const char *) {
  int flag = 0;
  MPI_Initialized(&flag);
  if(!flag) {
    if(MPI_Init(argc, argv) != MPI_SUCCESS) return PETSC_ERR_MPI;
  }
  MPI_Comm_rank(PETSC_COMM_WORLD, &rank_);
  MPI_Comm_size(PETSC_COMM_WORLD, &size_);
  options_.clear();
  if(argc && argv)
    for(int i = 1; i < *argc; i++) options_.push_back((*argv)[i]);
  initialized_ = true;
  return PETSC_SUCCESS;
}

PetscErrorCode PetscInitialized(PetscBool *flag) {
  *flag = initialized_ ? PETSC_TRUE : PETSC_FALSE;
  return PETSC_SUCCESS;
}

PetscErrorCode PetscFinalize(void) {
  if(!initialized_) return PETSC_SUCCESS;
  initialized_ = false;
  return MPI_Finalize() == MPI_SUCCESS ? PETSC_SUCCESS : PETSC_ERR_MPI;
}

PetscErrorCode PetscError(MPI_Comm, int line, const char *func, const char *file, PetscErrorCode n,
                          int p, const char *mess, ...) {
  char buf[256] = "";
  if(p && mess) {
    va_list ap;
    va_start(ap, mess);
    vsnprintf(buf, sizeof(buf), mess, ap);
    va_end(ap);
  }
  fprintf(2, "[%d]PETSC ERROR: %s() at %s:%d: %s (error code %d)\n", rank_, func ? func : "?",
          file ? file : "?", line, buf[0] ? buf : "-", n);
  return n;
}

PetscErrorCode PetscPrintf(MPI_Comm, const char format[], ...) {
  if(rank_ != 0) return PETSC_SUCCESS;
  va_list ap;
  va_start(ap, format);
  vprintf(format, ap);
  va_end(ap);
  return PETSC_SUCCESS;
}

PetscErrorCode PetscSynchronizedPrintf(MPI_Comm, const char format[], ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, format);
  vsnprintf(buf, sizeof(buf), format, ap);
  va_end(ap);
  sync_buf_ += buf;
  return PETSC_SUCCESS;
}

// Collective: rank 0 prints its text, then each other rank's, in rank order.
PetscErrorCode PetscSynchronizedFlush(MPI_Comm comm, void *) {
  constexpr int TAG_SYNC = 0x3fff0002;
  if(rank_ == 0) {
    printf("%s", sync_buf_.c_str());
    for(int r = 1; r < size_; r++) {
      int len;
      PetscCallMPI(MPI_Recv(&len, 1, MPI_INT, r, TAG_SYNC, comm, MPI_STATUS_IGNORE));
      std::string text((std::size_t)len, '\0');
      if(len) PetscCallMPI(MPI_Recv(&text[0], len, MPI_CHAR, r, TAG_SYNC, comm, MPI_STATUS_IGNORE));
      printf("%s", text.c_str());
    }
  } else {
    int len = int(sync_buf_.size());
    PetscCallMPI(MPI_Send(&len, 1, MPI_INT, 0, TAG_SYNC, comm));
    if(len) PetscCallMPI(MPI_Send(&sync_buf_[0], len, MPI_CHAR, 0, TAG_SYNC, comm));
  }
  sync_buf_.clear();
  return PETSC_SUCCESS;
}

PetscErrorCode PetscOptionsHasName(PetscOptions, const char pre[], const char name[], PetscBool *set) {
  std::size_t at;
  *set = option_index(pre, name, at) ? PETSC_TRUE : PETSC_FALSE;
  return PETSC_SUCCESS;
}

PetscErrorCode PetscOptionsGetBool(PetscOptions, const char pre[], const char name[], PetscBool *value,
                                   PetscBool *set) {
  std::size_t at;
  bool found = option_index(pre, name, at);
  if(set) *set = found ? PETSC_TRUE : PETSC_FALSE;
  if(!found) return PETSC_SUCCESS;
  const char *v = option_value(at);
  bool r = true;
  if(v) {
    std::string s = v;
    if(s == "0" || s == "false" || s == "no" || s == "FALSE" || s == "NO" || s == "off") r = false;
    else if(!(s == "1" || s == "true" || s == "yes" || s == "TRUE" || s == "YES" || s == "on"))
      r = true;   // not a boolean word: it belongs to the next argument
  }
  if(value) *value = r ? PETSC_TRUE : PETSC_FALSE;
  return PETSC_SUCCESS;
}

PetscErrorCode PetscOptionsGetInt(PetscOptions, const char pre[], const char name[], PetscInt *value,
                                  PetscBool *set) {
  std::size_t at;
  bool found = option_index(pre, name, at);
  const char *v = found ? option_value(at) : nullptr;
  if(set) *set = v ? PETSC_TRUE : PETSC_FALSE;
  if(v && value) *value = PetscInt(strtol(v, nullptr, 10));
  return PETSC_SUCCESS;
}

PetscErrorCode PetscOptionsGetReal(PetscOptions, const char pre[], const char name[], PetscReal *value,
                                   PetscBool *set) {
  std::size_t at;
  bool found = option_index(pre, name, at);
  const char *v = found ? option_value(at) : nullptr;
  if(set) *set = v ? PETSC_TRUE : PETSC_FALSE;
  if(v && value) *value = parse_real(v);
  return PETSC_SUCCESS;
}

PetscErrorCode PetscOptionsGetString(PetscOptions, const char pre[], const char name[], char value[],
                                     unsigned long len, PetscBool *set) {
  std::size_t at;
  bool found = option_index(pre, name, at);
  const char *v = found ? option_value(at) : nullptr;
  if(set) *set = v ? PETSC_TRUE : PETSC_FALSE;
  if(v && value && len) {
    std::size_t k = 0;
    for(; v[k] && k + 1 < len; k++) value[k] = v[k];
    value[k] = 0;
  }
  return PETSC_SUCCESS;
}

PetscErrorCode PetscSplitOwnership(MPI_Comm, PetscInt *n, PetscInt *N) {
  if(*n == PETSC_DECIDE) {
    *n = *N / size_ + (rank_ < *N % size_ ? 1 : 0);
  } else if(*N == PETSC_DECIDE) {
    int mine = *n, sum = 0;
    PetscCallMPI(MPI_Allreduce(&mine, &sum, 1, MPI_INT, MPI_SUM, PETSC_COMM_WORLD));
    *N = sum;
  }
  return PETSC_SUCCESS;
}

PetscErrorCode PetscBarrier(void *) {
  PetscCallMPI(MPI_Barrier(PETSC_COMM_WORLD));
  return PETSC_SUCCESS;
}

double PetscTime(void) { return MPI_Wtime(); }

// ==========================================================================
// Vec

PetscErrorCode VecCreateMPI(MPI_Comm, PetscInt n, PetscInt N, Vec *v) {
  Layout L;
  PetscCall(make_layout(n, N, L));
  return vec_new(L, false, 0, v);
}

PetscErrorCode VecCreateSeq(MPI_Comm, PetscInt n, Vec *v) { return vec_new(seq_layout(n), true, 0, v); }

PetscErrorCode VecCreateGhost(MPI_Comm, PetscInt n, PetscInt N, PetscInt nghost, const PetscInt ghosts[],
                              Vec *v) {
  Layout L;
  PetscCall(make_layout(n, N, L));
  for(PetscInt k = 0; k < nghost; k++)
    if(ghosts[k] < 0 || ghosts[k] >= L.N || L.owns(ghosts[k]))
      FAIL(PETSC_ERR_ARG_OUTOFRANGE, "ghost index must be a valid index owned by another rank");
  PetscCall(vec_new(L, false, nghost, v));
  Vec x = *v;
  x->ghosts.assign(ghosts, ghosts + nghost);
  std::vector<PetscInt> slot(nghost);
  for(PetscInt k = 0; k < nghost; k++) slot[k] = L.n + k;
  return x->ghost_plan.build(L, (std::size_t)nghost, x->ghosts.data(), slot.data());
}

PetscErrorCode VecDuplicate(Vec v, Vec *w) {
  PetscCall(vec_new(v->L, v->seq, v->nghost, w));
  (*w)->ghosts = v->ghosts;
  (*w)->ghost_plan = v->ghost_plan;
  return PETSC_SUCCESS;
}

PetscErrorCode VecDestroy(Vec *v) {
  if(!v || !*v) return PETSC_SUCCESS;
  if((*v)->local) delete (*v)->local;
  delete *v;
  *v = nullptr;
  return PETSC_SUCCESS;
}

PetscErrorCode VecGetSize(Vec v, PetscInt *N) { *N = v->L.N; return PETSC_SUCCESS; }
PetscErrorCode VecGetLocalSize(Vec v, PetscInt *n) { *n = v->L.n; return PETSC_SUCCESS; }
PetscErrorCode VecGetOwnershipRange(Vec v, PetscInt *lo, PetscInt *hi) {
  if(lo) *lo = v->L.rstart;
  if(hi) *hi = v->L.rend;
  return PETSC_SUCCESS;
}
PetscErrorCode VecGetArray(Vec v, PetscScalar **a) { *a = v->data; return PETSC_SUCCESS; }
PetscErrorCode VecRestoreArray(Vec, PetscScalar **a) { if(a) *a = nullptr; return PETSC_SUCCESS; }
PetscErrorCode VecGetArrayRead(Vec v, const PetscScalar **a) { *a = v->data; return PETSC_SUCCESS; }
PetscErrorCode VecRestoreArrayRead(Vec, const PetscScalar **a) { if(a) *a = nullptr; return PETSC_SUCCESS; }

PetscErrorCode VecSet(Vec v, PetscScalar a) {
  for(PetscInt i = 0; i < v->L.n; i++) v->data[i] = a;
  return PETSC_SUCCESS;
}

PetscErrorCode VecSetValues(Vec v, PetscInt n, const PetscInt idx[], const PetscScalar y[], InsertMode mode) {
  for(PetscInt k = 0; k < n; k++) {
    PetscInt g = idx[k];
    if(g < 0) continue;
    if(g >= v->L.N) FAIL(PETSC_ERR_ARG_OUTOFRANGE, "VecSetValues index out of range");
    if(v->L.owns(g)) {
      PetscScalar &d = v->data[g - v->L.rstart];
      d = mode == ADD_VALUES ? d + y[k] : y[k];
    } else {
      v->stash_i.push_back(g);
      v->stash_v.push_back(y[k]);
      v->stash_m.push_back(mode);
    }
  }
  return PETSC_SUCCESS;
}

PetscErrorCode VecAssemblyBegin(Vec) { return PETSC_SUCCESS; }

PetscErrorCode VecAssemblyEnd(Vec v) {
  if(v->seq) { v->stash_i.clear(); v->stash_v.clear(); v->stash_m.clear(); return PETSC_SUCCESS; }
  Plan p;
  std::vector<PetscInt> slot(v->stash_i.size());
  for(std::size_t k = 0; k < slot.size(); k++) slot[k] = PetscInt(k);
  PetscCall(p.build(v->L, v->stash_i.size(), v->stash_i.data(), slot.data()));
  std::vector<PetscScalar> sv(p.nreq()), rv(p.nsrv());
  std::vector<int> sm(p.nreq()), rm(p.nsrv());
  for(std::size_t k = 0; k < p.nreq(); k++) {
    sv[k] = v->stash_v[p.req_slot[k]];
    sm[k] = v->stash_m[p.req_slot[k]];
  }
  PetscCall(p.reverse_aligned(sv.data(), rv.data()));
  PetscCall(p.reverse_aligned(sm.data(), rm.data()));
  for(std::size_t k = 0; k < p.nsrv(); k++) {
    PetscScalar &d = v->data[p.srv_pos[k]];
    d = rm[k] == ADD_VALUES ? d + rv[k] : rv[k];
  }
  v->stash_i.clear(); v->stash_v.clear(); v->stash_m.clear();
  return PETSC_SUCCESS;
}

PetscErrorCode VecCopy(Vec x, Vec y) {
  PetscCall(check_same(x, y));
  for(PetscInt i = 0; i < x->L.n; i++) y->data[i] = x->data[i];
  return PETSC_SUCCESS;
}

PetscErrorCode VecScale(Vec x, PetscScalar a) {
  for(PetscInt i = 0; i < x->L.n; i++) x->data[i] *= a;
  return PETSC_SUCCESS;
}

PetscErrorCode VecAXPY(Vec y, PetscScalar a, Vec x) {
  PetscCall(check_same(x, y));
  for(PetscInt i = 0; i < x->L.n; i++) y->data[i] += a * x->data[i];
  return PETSC_SUCCESS;
}

PetscErrorCode VecAYPX(Vec y, PetscScalar b, Vec x) {
  PetscCall(check_same(x, y));
  for(PetscInt i = 0; i < x->L.n; i++) y->data[i] = x->data[i] + b * y->data[i];
  return PETSC_SUCCESS;
}

PetscErrorCode VecWAXPY(Vec w, PetscScalar a, Vec x, Vec y) {
  PetscCall(check_same(x, y));
  PetscCall(check_same(w, y));
  for(PetscInt i = 0; i < x->L.n; i++) w->data[i] = a * x->data[i] + y->data[i];
  return PETSC_SUCCESS;
}

PetscErrorCode VecPointwiseMult(Vec w, Vec x, Vec y) {
  PetscCall(check_same(x, y));
  PetscCall(check_same(w, y));
  for(PetscInt i = 0; i < x->L.n; i++) w->data[i] = x->data[i] * y->data[i];
  return PETSC_SUCCESS;
}

PetscErrorCode VecDot(Vec x, Vec y, PetscScalar *d) {
  PetscCall(check_same(x, y));
  double s = 0;
  for(PetscInt i = 0; i < x->L.n; i++) s += x->data[i] * y->data[i];
  *d = global_sum(s, x->seq);
  return PETSC_SUCCESS;
}

PetscErrorCode VecNorm(Vec x, NormType type, PetscReal *r) {
  double s = 0;
  if(type == NORM_INFINITY) {
    for(PetscInt i = 0; i < x->L.n; i++) {
      double a = std::abs(x->data[i]);
      if(a > s || a != a) s = a;
    }
    double g = s;
    if(!x->seq && size_ > 1) PetscCallMPI(MPI_Allreduce(&s, &g, 1, MPI_DOUBLE, MPI_MAX, PETSC_COMM_WORLD));
    *r = g;
  } else if(type == NORM_1) {
    for(PetscInt i = 0; i < x->L.n; i++) s += std::abs(x->data[i]);
    *r = global_sum(s, x->seq);
  } else if(type == NORM_2 || type == NORM_FROBENIUS) {
    for(PetscInt i = 0; i < x->L.n; i++) s += x->data[i] * x->data[i];
    *r = std::sqrt(global_sum(s, x->seq));
  } else {
    FAIL(PETSC_ERR_SUP, "unsupported norm type");
  }
  return PETSC_SUCCESS;
}

PetscErrorCode VecSetPreallocationCOO(Vec v, PetscCount ncoo, const PetscInt coo_i[]) {
  if(v->seq) {
    v->coo_local.assign((std::size_t)ncoo, -1);
    for(PetscCount k = 0; k < ncoo; k++) {
      if(coo_i[k] >= v->L.N) FAIL(PETSC_ERR_ARG_OUTOFRANGE, "COO index out of range");
      v->coo_local[k] = coo_i[k];
    }
  } else {
    PetscCall(route_entries(v->L, ncoo, coo_i, v->coo_local, v->coo_plan));
  }
  v->coo = true;
  return PETSC_SUCCESS;
}

// INSERT_VALUES replaces the vector by the sum of the entries (duplicates
// and contributions from other ranks add up); ADD_VALUES adds that sum.
PetscErrorCode VecSetValuesCOO(Vec v, const PetscScalar values[], InsertMode mode) {
  if(!v->coo) FAIL(PETSC_ERR_ARG_WRONGSTATE, "call VecSetPreallocationCOO first");
  if(mode == INSERT_VALUES)
    for(PetscInt i = 0; i < v->L.n; i++) v->data[i] = 0;
  for(std::size_t k = 0; k < v->coo_local.size(); k++)
    if(v->coo_local[k] >= 0) v->data[v->coo_local[k]] += values[k];
  if(!v->seq) PetscCall(v->coo_plan.reverse_add(values, v->data));
  return PETSC_SUCCESS;
}

PetscErrorCode VecGhostUpdateBegin(Vec v, InsertMode mode, ScatterMode smode) {
  if(mode == INSERT_VALUES && smode == SCATTER_FORWARD) return v->ghost_plan.forward(v->data, v->data);
  if(mode == ADD_VALUES && smode == SCATTER_REVERSE) {
    PetscCall(v->ghost_plan.reverse_add(v->data, v->data));
    return PETSC_SUCCESS;
  }
  FAIL(PETSC_ERR_SUP, "only INSERT_VALUES/SCATTER_FORWARD and ADD_VALUES/SCATTER_REVERSE");
}

PetscErrorCode VecGhostUpdateEnd(Vec, InsertMode, ScatterMode) { return PETSC_SUCCESS; }

PetscErrorCode VecGhostGetLocalForm(Vec v, Vec *l) {
  if(v->is_local_form || v->seq) { *l = nullptr; return PETSC_SUCCESS; }
  if(!v->local) {
    Vec x = new _p_Vec;
    x->L = seq_layout(v->L.n + v->nghost);
    x->seq = true;
    x->is_local_form = true;
    v->local = x;
  }
  v->local->data = v->data;
  *l = v->local;
  return PETSC_SUCCESS;
}

PetscErrorCode VecGhostRestoreLocalForm(Vec, Vec *l) {
  if(l) *l = nullptr;
  return PETSC_SUCCESS;
}

PetscErrorCode VecScatterCreateToAll(Vec v, VecScatter *ctx, Vec *all) {
  VecScatter s = new _p_VecScatter;
  s->counts.resize(size_);
  s->displs.resize(size_);
  for(int r = 0; r < size_; r++) {
    s->displs[r] = v->L.ranges[r];
    s->counts[r] = v->L.ranges[r + 1] - v->L.ranges[r];
  }
  *ctx = s;
  return vec_new(seq_layout(v->L.N), true, 0, all);
}

PetscErrorCode VecScatterCreateToZero(Vec v, VecScatter *ctx, Vec *zero) {
  PetscCall(VecScatterCreateToAll(v, ctx, zero));
  (*ctx)->to_zero = true;
  if(rank_ != 0) {
    PetscCall(VecDestroy(zero));
    return vec_new(seq_layout(0), true, 0, zero);
  }
  return PETSC_SUCCESS;
}

PetscErrorCode VecScatterBegin(VecScatter ctx, Vec x, Vec y, InsertMode mode, ScatterMode smode) {
  if(mode != INSERT_VALUES || smode != SCATTER_FORWARD)
    FAIL(PETSC_ERR_SUP, "only INSERT_VALUES/SCATTER_FORWARD scatters");
  if(ctx->to_zero)
    PetscCallMPI(MPI_Gatherv(x->data, x->L.n, MPI_DOUBLE, y->data, ctx->counts.data(), ctx->displs.data(),
                             MPI_DOUBLE, 0, PETSC_COMM_WORLD));
  else
    PetscCallMPI(MPI_Allgatherv(x->data, x->L.n, MPI_DOUBLE, y->data, ctx->counts.data(), ctx->displs.data(),
                                MPI_DOUBLE, PETSC_COMM_WORLD));
  return PETSC_SUCCESS;
}

PetscErrorCode VecScatterEnd(VecScatter, Vec, Vec, InsertMode, ScatterMode) { return PETSC_SUCCESS; }

PetscErrorCode VecScatterDestroy(VecScatter *ctx) {
  if(ctx && *ctx) { delete *ctx; *ctx = nullptr; }
  return PETSC_SUCCESS;
}

PetscErrorCode VecView(Vec v, void *) {
  std::vector<PetscScalar> all;
  VecScatter s;
  Vec z;
  PetscCall(VecScatterCreateToZero(v, &s, &z));
  PetscCall(VecScatterBegin(s, v, z, INSERT_VALUES, SCATTER_FORWARD));
  if(rank_ == 0) {
    printf("Vec Object: %d MPI process%s\n", size_, size_ > 1 ? "es" : "");
    for(PetscInt i = 0; i < z->L.n; i++) {
      char b[64];
      fmt_double(b, sizeof(b), z->data[i], 0, 6, 0, 'g');
      printf("%s\n", b);
    }
  }
  PetscCall(VecScatterDestroy(&s));
  return VecDestroy(&z);
}

// ==========================================================================
// Mat

PetscErrorCode MatCreate(MPI_Comm, Mat *A) { *A = new _p_Mat; return PETSC_SUCCESS; }

PetscErrorCode MatSetSizes(Mat A, PetscInt m, PetscInt n, PetscInt M, PetscInt N) {
  if(M != N || m != n) FAIL(PETSC_ERR_SUP, "only square matrices with matching row/column layouts");
  A->m = m;
  A->M = M;
  return PETSC_SUCCESS;
}

PetscErrorCode MatSetType(Mat, MatType type) {
  std::string t = type;
  if(t != MATAIJ && t != MATMPIAIJ && t != MATSEQAIJ) FAIL(PETSC_ERR_SUP, "only AIJ matrices");
  return PETSC_SUCCESS;
}
PetscErrorCode MatSetFromOptions(Mat) { return PETSC_SUCCESS; }
PetscErrorCode MatSetUp(Mat) { return PETSC_SUCCESS; }

PetscErrorCode MatSetPreallocationCOO(Mat A, PetscCount ncoo, PetscInt coo_i[], PetscInt coo_j[]) {
  PetscCall(make_layout(A->m, A->M, A->L));
  const Layout &L = A->L;
  std::vector<PetscInt> local;
  PetscCall(route_entries(L, ncoo, coo_i, local, A->coo_plan));
  for(PetscCount k = 0; k < ncoo; k++)
    if(coo_i[k] >= 0 && (coo_j[k] < 0 || coo_j[k] >= L.N))
      FAIL(PETSC_ERR_ARG_OUTOFRANGE, "COO column out of range");
  // Columns of the remote entries travel to their owners.
  std::vector<PetscInt> sj(A->coo_plan.nreq()), rj(A->coo_plan.nsrv());
  for(std::size_t k = 0; k < sj.size(); k++) sj[k] = coo_j[A->coo_plan.req_slot[k]];
  PetscCall(A->coo_plan.reverse_aligned(sj.data(), rj.data()));
  // Sparsity of the owned rows: own entries plus received ones.
  std::vector<PetscInt> cnt(L.n + 1, 0);
  for(PetscCount k = 0; k < ncoo; k++) if(local[k] >= 0 && coo_j[k] >= 0) cnt[local[k] + 1]++;
  for(std::size_t k = 0; k < rj.size(); k++) if(rj[k] >= 0) cnt[A->coo_plan.srv_pos[k] + 1]++;
  for(PetscInt i = 0; i < L.n; i++) cnt[i + 1] += cnt[i];
  std::vector<PetscInt> cols((std::size_t)cnt[L.n]), fill(cnt.begin(), cnt.end() - 1);
  for(PetscCount k = 0; k < ncoo; k++) if(local[k] >= 0 && coo_j[k] >= 0) cols[fill[local[k]]++] = coo_j[k];
  for(std::size_t k = 0; k < rj.size(); k++) if(rj[k] >= 0) cols[fill[A->coo_plan.srv_pos[k]]++] = rj[k];
  A->rowptr.assign(L.n + 1, 0);
  A->colg.clear();
  for(PetscInt i = 0; i < L.n; i++) {
    auto b = cols.begin() + cnt[i], e = cols.begin() + cnt[i + 1];
    std::sort(b, e);
    e = std::unique(b, e);
    A->colg.insert(A->colg.end(), b, e);
    A->rowptr[i + 1] = PetscInt(A->colg.size());
  }
  A->val.assign(A->colg.size(), 0.0);
  auto position = [&](PetscInt i, PetscInt j) {
    auto b = A->colg.begin() + A->rowptr[i], e = A->colg.begin() + A->rowptr[i + 1];
    return PetscInt(std::lower_bound(b, e, j) - A->colg.begin());
  };
  A->coo_pos.assign((std::size_t)ncoo, -1);
  for(PetscCount k = 0; k < ncoo; k++)
    if(local[k] >= 0 && coo_j[k] >= 0) A->coo_pos[k] = position(local[k], coo_j[k]);
  // srv_pos becomes the value position; a negative column is dropped by
  // pointing it at a scratch slot past the values.
  for(std::size_t k = 0; k < rj.size(); k++)
    A->coo_plan.srv_pos[k] = rj[k] >= 0 ? position(A->coo_plan.srv_pos[k], rj[k]) : PetscInt(A->val.size());
  // Off-process columns and the plan that gathers x there for MatMult.
  A->col_ghosts.clear();
  for(PetscInt c : A->colg) if(!L.owns(c)) A->col_ghosts.push_back(c);
  std::sort(A->col_ghosts.begin(), A->col_ghosts.end());
  A->col_ghosts.erase(std::unique(A->col_ghosts.begin(), A->col_ghosts.end()), A->col_ghosts.end());
  A->coll.resize(A->colg.size());
  for(std::size_t k = 0; k < A->colg.size(); k++) {
    PetscInt c = A->colg[k];
    A->coll[k] = L.owns(c) ? c - L.rstart
                           : L.n + PetscInt(std::lower_bound(A->col_ghosts.begin(), A->col_ghosts.end(), c) -
                                            A->col_ghosts.begin());
  }
  std::vector<PetscInt> slot(A->col_ghosts.size());
  for(std::size_t k = 0; k < slot.size(); k++) slot[k] = PetscInt(k);
  PetscCall(A->col_plan.build(L, A->col_ghosts.size(), A->col_ghosts.data(), slot.data()));
  A->assembled = true;
  A->state++;
  A->pattern++;
  return PETSC_SUCCESS;
}

PetscErrorCode MatSetValuesCOO(Mat A, const PetscScalar values[], InsertMode mode) {
  if(!A->assembled)
    FAIL(PETSC_ERR_ARG_WRONGSTATE, "call MatSetPreallocationCOO first");
  std::vector<PetscScalar> &val = A->val;
  val.push_back(0.0);   // scratch slot for dropped entries
  if(mode == INSERT_VALUES) std::fill(val.begin(), val.end(), 0.0);
  for(std::size_t k = 0; k < A->coo_pos.size(); k++)
    if(A->coo_pos[k] >= 0) val[A->coo_pos[k]] += values[k];
  PetscErrorCode e = A->coo_plan.reverse_add(values, val.data());
  val.pop_back();
  PetscCall(e);
  A->state++;
  return PETSC_SUCCESS;
}

PetscErrorCode MatAssemblyBegin(Mat, MatAssemblyType) { return PETSC_SUCCESS; }
PetscErrorCode MatAssemblyEnd(Mat, MatAssemblyType) { return PETSC_SUCCESS; }

PetscErrorCode MatGetSize(Mat A, PetscInt *M, PetscInt *N) {
  if(M) *M = A->L.N;
  if(N) *N = A->L.N;
  return PETSC_SUCCESS;
}
PetscErrorCode MatGetLocalSize(Mat A, PetscInt *m, PetscInt *n) {
  if(m) *m = A->L.n;
  if(n) *n = A->L.n;
  return PETSC_SUCCESS;
}
PetscErrorCode MatGetOwnershipRange(Mat A, PetscInt *lo, PetscInt *hi) {
  if(lo) *lo = A->L.rstart;
  if(hi) *hi = A->L.rend;
  return PETSC_SUCCESS;
}

PetscErrorCode MatCreateVecs(Mat A, Vec *right, Vec *left) {
  if(A->L.ranges.empty()) FAIL(PETSC_ERR_ARG_WRONGSTATE, "matrix sizes are not set up");
  if(right) PetscCall(vec_new(A->L, false, 0, right));
  if(left) PetscCall(vec_new(A->L, false, 0, left));
  return PETSC_SUCCESS;
}

PetscErrorCode MatMult(Mat A, Vec x, Vec y) {
  if(x == y) FAIL(PETSC_ERR_ARG_WRONG, "x and y must be different vectors");
  if(x->L.n != A->L.n || y->L.n != A->L.n) FAIL(PETSC_ERR_ARG_SIZ, "vector and matrix layouts differ");
  return mat_mult(A, x->data, y->data);
}

PetscErrorCode MatGetRow(Mat A, PetscInt row, PetscInt *ncols, const PetscInt **cols, const PetscScalar **vals) {
  if(!A->assembled) FAIL(PETSC_ERR_ARG_WRONGSTATE, "matrix is not assembled");
  if(!A->L.owns(row)) FAIL(PETSC_ERR_ARG_OUTOFRANGE, "MatGetRow: row is owned by another rank");
  const PetscInt i = row - A->L.rstart, k = A->rowptr[i];
  *ncols = A->rowptr[i + 1] - k;
  if(cols) *cols = A->colg.data() + k;
  if(vals) *vals = A->val.data() + k;
  return PETSC_SUCCESS;
}

PetscErrorCode MatRestoreRow(Mat, PetscInt, PetscInt *, const PetscInt **cols, const PetscScalar **vals) {
  if(cols) *cols = nullptr;
  if(vals) *vals = nullptr;
  return PETSC_SUCCESS;
}

PetscErrorCode MatGetDiagonal(Mat A, Vec d) {
  for(PetscInt i = 0; i < A->L.n; i++) {
    d->data[i] = 0;
    for(PetscInt k = A->rowptr[i]; k < A->rowptr[i + 1]; k++)
      if(A->colg[k] == A->L.rstart + i) d->data[i] = A->val[k];
  }
  return PETSC_SUCCESS;
}

PetscErrorCode MatDestroy(Mat *A) {
  if(A && *A) { delete *A; *A = nullptr; }
  return PETSC_SUCCESS;
}

// ==========================================================================
// PC

PetscErrorCode PCSetType(PC pc, PCType type) {
  std::string t = type;
  if(t != PCNONE && t != PCJACOBI && t != PCBJACOBI && t != PCASM && t != PCLU)
    FAIL(PETSC_ERR_SUP, "unsupported PC type");
  pc->type = t;
  return PETSC_SUCCESS;
}

PetscErrorCode PCGetType(PC pc, PCType *type) { *type = pc->type.c_str(); return PETSC_SUCCESS; }

PetscErrorCode PCASMSetOverlap(PC pc, PetscInt overlap) {
  if(overlap < 0) FAIL(PETSC_ERR_ARG_OUTOFRANGE, "negative ASM overlap");
  pc->overlap = overlap;
  return PETSC_SUCCESS;
}

PetscErrorCode PCSetFromOptions(PC pc) {
  char t[32];
  PetscBool set;
  PetscCall(PetscOptionsGetString(nullptr, nullptr, "-pc_type", t, sizeof(t), &set));
  if(set) PetscCall(PCSetType(pc, t));
  PetscInt ov;
  PetscCall(PetscOptionsGetInt(nullptr, nullptr, "-pc_asm_overlap", &ov, &set));
  if(set) PetscCall(PCASMSetOverlap(pc, ov));
  return PETSC_SUCCESS;
}

PetscErrorCode PCSetUp(PC pc) { return pc_setup(pc); }

PetscErrorCode PCApply(PC pc, Vec x, Vec y) {
  PetscCall(pc_setup(pc));
  return pc_apply(pc, x->data, y->data);
}

// ==========================================================================
// KSP

PetscErrorCode KSPCreate(MPI_Comm, KSP *ksp) {
  KSP k = new _p_KSP;
  k->pc = new _p_PC;
  k->pc->type = size_ > 1 ? PCBJACOBI : PCLU;
  *ksp = k;
  return PETSC_SUCCESS;
}

PetscErrorCode KSPSetType(KSP ksp, KSPType type) {
  std::string t = type;
  if(t != KSPGMRES && t != KSPCG && t != KSPRICHARDSON && t != KSPPREONLY)
    FAIL(PETSC_ERR_SUP, "unsupported KSP type");
  ksp->type = t;
  return PETSC_SUCCESS;
}

PetscErrorCode KSPGMRESSetRestart(KSP ksp, PetscInt restart) {
  if(restart < 1) FAIL(PETSC_ERR_ARG_OUTOFRANGE, "GMRES restart must be positive");
  ksp->restart = restart;
  return PETSC_SUCCESS;
}

PetscErrorCode KSPGetPC(KSP ksp, PC *pc) { *pc = ksp->pc; return PETSC_SUCCESS; }

PetscErrorCode KSPSetTolerances(KSP ksp, PetscReal rtol, PetscReal abstol, PetscReal dtol, PetscInt maxits) {
  if(rtol != PETSC_DEFAULT) ksp->rtol = rtol;
  if(abstol != PETSC_DEFAULT) ksp->atol = abstol;
  if(dtol != PETSC_DEFAULT) ksp->dtol = dtol;
  if(maxits != PETSC_DEFAULT) ksp->maxits = maxits;
  return PETSC_SUCCESS;
}

PetscErrorCode KSPSetInitialGuessNonzero(KSP ksp, PetscBool flg) {
  ksp->guess_nonzero = flg;
  return PETSC_SUCCESS;
}

PetscErrorCode KSPSetFromOptions(KSP ksp) {
  char t[32];
  PetscBool set, flag;
  PetscCall(PetscOptionsGetString(nullptr, nullptr, "-ksp_type", t, sizeof(t), &set));
  if(set) PetscCall(KSPSetType(ksp, t));
  PetscReal r;
  PetscCall(PetscOptionsGetReal(nullptr, nullptr, "-ksp_rtol", &r, &set));
  if(set) ksp->rtol = r;
  PetscCall(PetscOptionsGetReal(nullptr, nullptr, "-ksp_atol", &r, &set));
  if(set) ksp->atol = r;
  PetscCall(PetscOptionsGetReal(nullptr, nullptr, "-ksp_divtol", &r, &set));
  if(set) ksp->dtol = r;
  PetscInt i;
  PetscCall(PetscOptionsGetInt(nullptr, nullptr, "-ksp_max_it", &i, &set));
  if(set) ksp->maxits = i;
  PetscCall(PetscOptionsGetInt(nullptr, nullptr, "-ksp_gmres_restart", &i, &set));
  if(set) PetscCall(KSPGMRESSetRestart(ksp, i));
  PetscCall(PetscOptionsGetBool(nullptr, nullptr, "-ksp_monitor", &flag, &set));
  if(set) ksp->monitor = flag;
  PetscCall(PetscOptionsGetBool(nullptr, nullptr, "-ksp_converged_reason", &flag, &set));
  if(set) ksp->show_reason = flag;
  PetscCall(PetscOptionsGetBool(nullptr, nullptr, "-ksp_trace", &flag, &set));
  if(set) ksp->trace = flag;
  trace_ = trace_ || ksp->trace;
  return PCSetFromOptions(ksp->pc);
}

PetscErrorCode KSPSetOperators(KSP ksp, Mat A, Mat P) {
  if(A != P) FAIL(PETSC_ERR_SUP, "the preconditioning matrix must be the operator");
  ksp->A = A;
  ksp->P = P;
  ksp->pc->A = P;
  return PETSC_SUCCESS;
}

PetscErrorCode KSPSetUp(KSP ksp) {
  if(!ksp->A) FAIL(PETSC_ERR_ARG_WRONGSTATE, "KSPSetOperators first");
  return pc_setup(ksp->pc);
}

PetscErrorCode KSPSetUpOnBlocks(KSP ksp) { return KSPSetUp(ksp); }

PetscErrorCode KSPSolve(KSP ksp, Vec b, Vec x) {
  PetscCall(KSPSetUp(ksp));
  PetscCall(check_same(b, x));
  if(b->L.n != ksp->A->L.n) FAIL(PETSC_ERR_ARG_SIZ, "vector and matrix layouts differ");
  ksp->its = 0;
  ksp->reason = KSP_CONVERGED_ITERATING;
  std::vector<PetscScalar> xs(x->data, x->data + x->L.n);
  if(!ksp->guess_nonzero) std::fill(xs.begin(), xs.end(), 0.0);
  if(ksp->pc->failed) {
    ksp->reason = KSP_DIVERGED_PC_FAILED;
  } else if(ksp->type == KSPGMRES) {
    PetscCall(solve_gmres(ksp, b->data, xs));
  } else if(ksp->type == KSPCG) {
    PetscCall(solve_cg(ksp, b->data, xs));
  } else if(ksp->type == KSPRICHARDSON) {
    PetscCall(solve_richardson(ksp, b->data, xs));
  } else {
    PetscCall(pc_apply(ksp->pc, b->data, xs.data()));
    ksp->its = 1;
    ksp->reason = KSP_CONVERGED_ITS;
  }
  for(PetscInt i = 0; i < x->L.n; i++) x->data[i] = xs[i];
  if(ksp->show_reason && rank_ == 0)
    printf("Linear solve %s due to %s iterations %d\n", ksp->reason > 0 ? "converged" : "did not converge",
           reason_name(ksp->reason), int(ksp->its));
  return PETSC_SUCCESS;
}

PetscErrorCode KSPGetConvergedReason(KSP ksp, KSPConvergedReason *reason) { *reason = ksp->reason; return PETSC_SUCCESS; }
PetscErrorCode KSPGetIterationNumber(KSP ksp, PetscInt *its) { *its = ksp->its; return PETSC_SUCCESS; }
PetscErrorCode KSPGetResidualNorm(KSP ksp, PetscReal *rnorm) { *rnorm = ksp->rnorm; return PETSC_SUCCESS; }

PetscErrorCode KSPDestroy(KSP *ksp) {
  if(!ksp || !*ksp) return PETSC_SUCCESS;
  delete (*ksp)->pc;
  delete *ksp;
  *ksp = nullptr;
  return PETSC_SUCCESS;
}

}  // extern "C"
