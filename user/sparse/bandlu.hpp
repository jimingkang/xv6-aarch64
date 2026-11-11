// bandlu.hpp -- sparse direct solve for the xv6 Eigen and PETSc subsets.
//
// The matrix is given in compressed form (any square sparsity pattern).  It
// is reordered with reverse Cuthill-McKee on the pattern of A + A^T, which
// gives finite-element matrices a narrow band, and then factored as a band
// matrix with partial pivoting (LAPACK dgbtf2 scheme: L keeps bandwidth kl,
// U grows to kl + ku).  Work is O(n (kl+ku) kl), storage n (2 kl + ku + 1).
#pragma once
#include <vector>
#include <cmath>
#include <cstddef>
#include <algorithm>

namespace xv6sparse {

class BandLU {
public:
  // Column-compressed input: column c holds rows idx[ptr[c] .. ptr[c+1]).
  // Returns false if the matrix is structurally or numerically singular.
  bool factor(long n, const long *ptr, const long *idx, const double *val) {
    n_ = n;
    ok_ = false;
    order(n, ptr, idx);
    // Bandwidths in the permuted numbering.
    kl_ = ku_ = 0;
    for(long c = 0; c < n; c++)
      for(long k = ptr[c]; k < ptr[c + 1]; k++) {
        long i = inv_[idx[k]], j = inv_[c];
        if(i - j > kl_) kl_ = i - j;
        if(j - i > ku_) ku_ = j - i;
      }
    w_ = 2 * kl_ + ku_ + 1;
    band_.assign((std::size_t)(n * w_), 0.0);
    double scale = 0;
    for(long c = 0; c < n; c++)
      for(long k = ptr[c]; k < ptr[c + 1]; k++) {
        at(inv_[idx[k]], inv_[c]) += val[k];
        scale = std::max(scale, std::abs(val[k]));
      }
    piv_.assign((std::size_t)n, 0);
    const double tiny = scale * 1e-300;
    const long reach = kl_ + ku_;
    for(long k = 0; k < n; k++) {
      long last = std::min(n - 1, k + kl_), r = k;
      double best = std::abs(at(k, k));
      for(long i = k + 1; i <= last; i++)
        if(std::abs(at(i, k)) > best) { best = std::abs(at(i, k)); r = i; }
      if(!(best > tiny) || !std::isfinite(best)) return false;
      piv_[k] = r;
      long end = std::min(n - 1, k + reach);
      if(r != k)
        for(long j = k; j <= end; j++) std::swap(at(k, j), at(r, j));
      const double inv = 1.0 / at(k, k);
      for(long i = k + 1; i <= last; i++) {
        double l = at(i, k) * inv;
        at(i, k) = l;
        if(l == 0.0) continue;
        double *ri = row(i);
        const double *rk = row(k);
        for(long j = k + 1; j <= end; j++) ri[j] -= l * rk[j];
      }
    }
    ok_ = true;
    return true;
  }

  // Solves A x = b in place.
  void solve(double *x) const {
    const long n = n_, reach = kl_ + ku_;
    std::vector<double> y((std::size_t)n);
    for(long i = 0; i < n; i++) y[inv_[i]] = x[i];
    for(long k = 0; k < n; k++) {
      if(piv_[k] != k) std::swap(y[k], y[piv_[k]]);
      double v = y[k];
      if(v == 0.0) continue;
      long last = std::min(n - 1, k + kl_);
      for(long i = k + 1; i <= last; i++) y[i] -= at(i, k) * v;
    }
    for(long i = n - 1; i >= 0; i--) {
      double s = y[i];
      long end = std::min(n - 1, i + reach);
      const double *ri = row(i);
      for(long j = i + 1; j <= end; j++) s -= ri[j] * y[j];
      y[i] = s / at(i, i);
    }
    for(long i = 0; i < n; i++) x[i] = y[inv_[i]];
  }

  bool ok() const { return ok_; }
  long lower() const { return kl_; }
  long upper() const { return ku_; }

private:
  long n_ = 0, kl_ = 0, ku_ = 0, w_ = 1;
  bool ok_ = false;
  std::vector<double> band_;
  std::vector<long> piv_, inv_;   // inv_[original] = permuted index

  // Row-major band rows: A(i,j) is band_[i*w + j - i + kl] = row(i)[j].
  double *row(long i) { return band_.data() + i * (w_ - 1) + kl_; }
  const double *row(long i) const { return band_.data() + i * (w_ - 1) + kl_; }
  double &at(long i, long j) { return row(i)[j]; }
  const double &at(long i, long j) const { return row(i)[j]; }

  void order(long n, const long *ptr, const long *idx) {
    // Symmetric adjacency without the diagonal.
    std::vector<long> deg((std::size_t)n, 0);
    for(long c = 0; c < n; c++)
      for(long k = ptr[c]; k < ptr[c + 1]; k++)
        if(idx[k] != c) { deg[c]++; deg[idx[k]]++; }
    std::vector<long> start((std::size_t)n + 1, 0);
    for(long i = 0; i < n; i++) start[i + 1] = start[i] + deg[i];
    std::vector<long> adj((std::size_t)start[n]), fill(start.begin(), start.end() - 1);
    for(long c = 0; c < n; c++)
      for(long k = ptr[c]; k < ptr[c + 1]; k++)
        if(idx[k] != c) { adj[fill[c]++] = idx[k]; adj[fill[idx[k]]++] = c; }
    // Drop duplicate neighbours and use the true degree.
    for(long i = 0; i < n; i++) {
      auto b = adj.begin() + start[i], e = adj.begin() + start[i + 1];
      std::sort(b, e);
      deg[i] = std::unique(b, e) - b;
    }
    std::vector<long> perm;
    perm.reserve((std::size_t)n);
    std::vector<char> seen((std::size_t)n, 0), vis;
    std::vector<long> level((std::size_t)n), trial;
    // Breadth-first visit from root, neighbours by increasing degree.
    auto bfs = [&](long root, std::vector<long> &out, std::vector<char> &mark) {
      std::size_t head = out.size();
      out.push_back(root);
      mark[root] = 1;
      level[root] = 0;
      for(; head < out.size(); head++) {
        long v = out[head];
        std::size_t first = out.size();
        for(long k = start[v]; k < start[v] + deg[v]; k++) {
          long u = adj[k];
          if(!mark[u]) { mark[u] = 1; level[u] = level[v] + 1; out.push_back(u); }
        }
        std::sort(out.begin() + first, out.end(), [&](long a, long b) { return deg[a] < deg[b]; });
      }
    };
    for(long s = 0; s < n; s++) {
      if(seen[s]) continue;
      // Pseudo-peripheral root: restart from a low-degree node of the last
      // level while that makes the level structure deeper.
      long root = s, depth = -1;
      for(int it = 0; it < 5; it++) {
        trial.clear();
        vis = seen;
        bfs(root, trial, vis);
        long far = trial.back(), lv = level[far];
        if(lv <= depth) break;
        depth = lv;
        for(long v : trial) if(level[v] == lv && deg[v] < deg[far]) far = v;
        root = far;
      }
      bfs(root, perm, seen);
    }
    std::reverse(perm.begin(), perm.end());
    inv_.assign((std::size_t)n, 0);
    for(long i = 0; i < n; i++) inv_[perm[i]] = i;
  }
};

}  // namespace xv6sparse
