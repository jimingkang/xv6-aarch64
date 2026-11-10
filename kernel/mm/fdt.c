// Minimal flattened device tree parser.
//
// A DTB is a header, a memory-reservation block (pairs of big-endian
// 64-bit address/size ending in 0/0), a structure block of 32-bit tokens,
// and a strings block that property names point into:
//
//   FDT_BEGIN_NODE "name\0" (padded to 4)   FDT_PROP len nameoff value (padded)
//   FDT_END_NODE                            FDT_NOP   FDT_END
//
// The walk below keeps a small stack of #address-cells/#size-cells so reg
// properties are decoded with their parent's cell sizes, and reports:
//   /memory or /memory@N (or any node with device_type = "memory"): reg
//   /reserved-memory/<child>: reg  (children with only a size, such as the
//     Pi's dynamically placed linux,cma pool, have nothing fixed to reserve)
//   the /memreserve/ block

#include <kernel/types.h>
#include <kernel/param.h>
#include "aarch64.h"
#include <kernel/defs.h>
#include <kernel/fdt.h>

#define FDT_BEGIN_NODE 1
#define FDT_END_NODE   2
#define FDT_PROP       3
#define FDT_NOP        4
#define FDT_END        9

#define FDT_MAX_DEPTH  8
#define FDT_MAX_SIZE   (2 * 1024 * 1024)

struct fdt_header {
  uint32 magic, totalsize, off_dt_struct, off_dt_strings, off_mem_rsvmap;
  uint32 version, last_comp_version, boot_cpuid_phys;
  uint32 size_dt_strings, size_dt_struct;
};

static uint32
be32(const void *p)
{
  const uchar *b = p;
  return ((uint32)b[0] << 24) | ((uint32)b[1] << 16) | ((uint32)b[2] << 8) | b[3];
}

static uint64
be64(const void *p)
{
  return ((uint64)be32(p) << 32) | be32((const char *)p + 4);
}

// Read `cells` 32-bit cells as one number (1 or 2 cells in practice).
static uint64
read_cells(const uchar **p, int cells)
{
  uint64 v = 0;
  for(int i = 0; i < cells; i++){
    v = (v << 32) | be32(*p);
    *p += 4;
  }
  return v;
}

uint32
fdt_totalsize(const void *blob)
{
  const struct fdt_header *h = blob;
  uint32 size;

  if(blob == 0 || ((uint64)blob & 7) != 0 || be32(&h->magic) != FDT_MAGIC)
    return 0;
  size = be32(&h->totalsize);
  if(size < sizeof(*h) || size > FDT_MAX_SIZE || be32(&h->version) < 16)
    return 0;
  if(be32(&h->off_dt_struct) >= size || be32(&h->off_dt_strings) >= size ||
     be32(&h->off_mem_rsvmap) >= size)
    return 0;
  return size;
}

static int
name_is(const char *name, const char *want)
{
  // "memory" matches "memory" and "memory@0".
  while(*want && *name == *want){
    name++;
    want++;
  }
  return *want == 0 && (*name == 0 || *name == '@');
}

static void
report_reg(const uchar *val, int len, int ac, int sc,
           void (*fn)(uint64, uint64))
{
  int entry = 4 * (ac + sc);

  if(fn == 0 || ac < 1 || ac > 2 || sc < 1 || sc > 2)
    return;
  for(const uchar *p = val; p + entry <= val + len; ){
    uint64 base = read_cells(&p, ac);
    uint64 size = read_cells(&p, sc);
    if(size)
      fn(base, size);
  }
}

int
fdt_scan(const void *blob, struct fdt_scan_ops *ops)
{
  const struct fdt_header *h = blob;
  uint32 total = fdt_totalsize(blob);
  const uchar *base = blob, *p, *end, *strings;
  // Cell sizes that apply to the children of the node at each depth.
  int ac[FDT_MAX_DEPTH], sc[FDT_MAX_DEPTH];
  int depth = 0, in_resmem = 0;
  // The current node: its kind is known from its name or device_type, and
  // its reg may come before device_type, so it is reported at END_NODE.
  int is_memory = 0;
  const uchar *reg = 0;
  int reglen = 0;

  if(total == 0)
    return -1;

  // Memory reservation block.
  for(p = base + be32(&h->off_mem_rsvmap); p + 16 <= base + total; p += 16){
    uint64 a = be64(p), s = be64(p + 8);
    if(a == 0 && s == 0)
      break;
    if(ops->reserve)
      ops->reserve(a, s);
  }

  strings = base + be32(&h->off_dt_strings);
  p = base + be32(&h->off_dt_struct);
  end = p + be32(&h->size_dt_struct);
  if(end > base + total)
    end = base + total;
  ac[0] = 2;                    // DT spec defaults for the root's children
  sc[0] = 1;

  while(p + 4 <= end){
    uint32 tok = be32(p);
    p += 4;
    if(tok == FDT_BEGIN_NODE){
      const char *name = (const char *)p;
      int n = strlen(name);
      p += (n + 1 + 3) & ~3;
      if(depth + 1 >= FDT_MAX_DEPTH)
        return -1;
      depth++;
      ac[depth] = 2;            // until this node says otherwise
      sc[depth] = 1;
      if(depth == 2 && name_is(name, "reserved-memory"))
        in_resmem = 1;
      is_memory = depth == 2 && name_is(name, "memory");
      reg = 0;
      reglen = 0;
    } else if(tok == FDT_END_NODE){
      if(depth <= 0)
        return -1;
      if(reg){
        // reg is decoded with the parent's cell sizes.
        if(is_memory)
          report_reg(reg, reglen, ac[depth - 1], sc[depth - 1], ops->memory);
        else if(in_resmem && depth == 3)
          report_reg(reg, reglen, ac[depth - 1], sc[depth - 1], ops->reserve);
      }
      if(depth == 2)
        in_resmem = 0;
      depth--;
      is_memory = 0;
      reg = 0;
    } else if(tok == FDT_PROP){
      if(p + 8 > end)
        return -1;
      int len = be32(p);
      const char *pname = (const char *)strings + be32(p + 4);
      const uchar *val = p + 8;
      p = val + ((len + 3) & ~3);
      if(p > end)
        return -1;
      if(strncmp(pname, "#address-cells", 15) == 0 && len == 4)
        ac[depth] = be32(val);
      else if(strncmp(pname, "#size-cells", 12) == 0 && len == 4)
        sc[depth] = be32(val);
      else if(strncmp(pname, "reg", 4) == 0){
        reg = val;
        reglen = len;
      } else if(strncmp(pname, "device_type", 12) == 0 && depth == 2 &&
                strncmp((const char *)val, "memory", 7) == 0)
        is_memory = 1;
      else if(depth == 1 && ops->model && strncmp(pname, "model", 6) == 0){
        int n = len < ops->model_len - 1 ? len : ops->model_len - 1;
        memmove(ops->model, val, n);
        ops->model[n] = 0;
      }
    } else if(tok == FDT_NOP){
      continue;
    } else if(tok == FDT_END){
      return 0;
    } else {
      return -1;
    }
  }
  return -1;
}
