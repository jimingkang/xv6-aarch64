#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/elf.h"
#include "user/user.h"

// A deliberately small, self-hosted C compiler for xv6/AArch64.
// It emits a complete ELF file directly, so no assembler or linker is needed.

#define SRCMAX   16384
#define CODEMAX   4096
#define STRMAX    4096
#define NVAR        32
#define NPATCH      64

enum { T_EOF = 256, T_ID, T_NUM, T_STR };

static char source[SRCMAX + 1], ident[64], literal[512];
static char *cur;
static int tok, line = 1;
static uint64 number;
static uint32 code[CODEMAX];
static int ncode;
static uchar strings[STRMAX];
static int nstrings;
static struct { int insn, off; } patches[NPATCH];
static int npatch;
static struct { char name[32]; int off; } vars[NVAR];
static int nvar, failed, returned;

static void
error(char *s)
{
  if(!failed)
    fprintf(2, "tcc:%d: %s\n", line, s);
  failed = 1;
}

static int
isalpha_(int c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int
isdigit_(int c)
{
  return c >= '0' && c <= '9';
}

static void
next(void)
{
  int i, c;
again:
  while(*cur == ' ' || *cur == '\t' || *cur == '\r' || *cur == '\n'){
    if(*cur++ == '\n') line++;
  }
  if(cur[0] == '/' && cur[1] == '/'){
    cur += 2;
    while(*cur && *cur != '\n') cur++;
    goto again;
  }
  if(cur[0] == '/' && cur[1] == '*'){
    cur += 2;
    while(*cur && !(cur[0] == '*' && cur[1] == '/')){
      if(*cur++ == '\n') line++;
    }
    if(*cur) cur += 2;
    goto again;
  }
  if(*cur == 0){ tok = T_EOF; return; }
  if(isalpha_(*cur)){
    for(i = 0; isalpha_(*cur) || isdigit_(*cur); cur++)
      if(i < (int)sizeof(ident)-1) ident[i++] = *cur;
    ident[i] = 0;
    tok = T_ID;
    return;
  }
  if(isdigit_(*cur)){
    number = 0;
    while(isdigit_(*cur)) number = number * 10 + (*cur++ - '0');
    tok = T_NUM;
    return;
  }
  if(*cur == '"'){
    cur++;
    for(i = 0; *cur && *cur != '"'; cur++){
      c = *cur;
      if(c == '\\'){
        c = *++cur;
        if(c == 'n') c = '\n';
        else if(c == 'r') c = '\r';
        else if(c == 't') c = '\t';
      }
      if(i < (int)sizeof(literal)-1) literal[i++] = c;
    }
    if(*cur == '"') cur++;
    else error("unterminated string");
    literal[i] = 0;
    tok = T_STR;
    return;
  }
  tok = (uchar)*cur++;
}

static int
word(char *s)
{
  return tok == T_ID && strcmp(ident, s) == 0;
}

static void
need(int t, char *message)
{
  if(tok != t) error(message);
  else next();
}

static void
emit(uint32 insn)
{
  if(ncode >= CODEMAX) error("generated program is too large");
  else code[ncode++] = insn;
}

static void
mov64(int rd, uint64 v)
{
  int hw;
  emit(0xd2800000U | ((uint32)(v & 0xffff) << 5) | rd); // MOVZ
  for(hw = 1; hw < 4; hw++)
    if((v >> (hw * 16)) & 0xffff)
      emit(0xf2800000U | ((uint32)hw << 21) |
           ((uint32)((v >> (hw * 16)) & 0xffff) << 5) | rd); // MOVK
}

static int
findvar(char *name)
{
  int i;
  for(i = 0; i < nvar; i++)
    if(strcmp(vars[i].name, name) == 0) return i;
  return -1;
}

static void expr(void);

static void
primary(void)
{
  int v;
  if(tok == T_NUM){
    mov64(0, number);
    next();
  } else if(tok == T_ID){
    v = findvar(ident);
    if(v < 0) error("unknown variable");
    else emit(0xf9400000U | ((uint32)(vars[v].off / 8) << 10) | (29 << 5));
    next();
  } else if(tok == '('){
    next(); expr(); need(')', "expected ')'");
  } else {
    error("expected expression");
  }
}

static void
unary(void)
{
  if(tok == '-'){
    next(); unary();
    emit(0xcb0003e0U);                    // sub x0, xzr, x0
  } else if(tok == '+'){
    next(); unary();
  } else primary();
}

static void
mul(void)
{
  int op;
  unary();
  while(tok == '*' || tok == '/'){
    op = tok; next();
    emit(0xd10043ffU);                    // sub sp, sp, #16
    emit(0xf90003e0U);                    // str x0, [sp]
    unary();
    emit(0xf94003e1U);                    // ldr x1, [sp]
    emit(0x910043ffU);                    // add sp, sp, #16
    emit(op == '*' ? 0x9b007c20U          // mul x0, x1, x0
                   : 0x9ac00c20U);        // sdiv x0, x1, x0
  }
}

static void
expr(void)
{
  int op;
  mul();
  while(tok == '+' || tok == '-'){
    op = tok; next();
    emit(0xd10043ffU);
    emit(0xf90003e0U);
    mul();
    emit(0xf94003e1U);
    emit(0x910043ffU);
    emit(op == '+' ? 0x8b000020U          // add x0, x1, x0
                   : 0xcb000020U);        // sub x0, x1, x0
  }
}

static void
emit_print(int newline)
{
  int len = strlen(literal), off = nstrings;
  if(nstrings + len + newline > STRMAX || npatch >= NPATCH){
    error("too many string literals");
    return;
  }
  memmove(strings + nstrings, literal, len);
  nstrings += len;
  if(newline) strings[nstrings++] = '\n';
  mov64(0, 1);                            // stdout
  patches[npatch].insn = ncode;
  patches[npatch++].off = off;
  emit(0);                                // patched to adr x1, string
  mov64(2, len + newline);
  mov64(7, 16);                           // SYS_write
  emit(0xd4000001U);                      // svc #0
}

static void
statement(void)
{
  char name[32];
  int v;
  if(word("int")){
    next();
    if(tok != T_ID){ error("expected variable name"); return; }
    if(nvar >= NVAR){ error("too many variables"); return; }
    strcpy(name, ident); next();
    if(findvar(name) >= 0){ error("duplicate variable"); return; }
    strcpy(vars[nvar].name, name);
    vars[nvar].off = nvar * 8;
    v = nvar++;
    if(tok == '='){ next(); expr(); }
    else mov64(0, 0);
    emit(0xf9000000U | ((uint32)(vars[v].off / 8) << 10) | (29 << 5));
    need(';', "expected ';'");
  } else if(word("return")){
    next(); expr(); need(';', "expected ';'");
    mov64(7, 2);                          // SYS_exit
    emit(0xd4000001U);
    returned = 1;
  } else if(word("printf") || word("puts")){
    int nl = word("puts");
    next(); need('(', "expected '('");
    if(tok != T_STR){ error("only a string literal is supported here"); return; }
    emit_print(nl); next();
    need(')', "expected ')'"); need(';', "expected ';'");
  } else if(tok == T_ID){
    strcpy(name, ident); next();
    v = findvar(name);
    if(v < 0){ error("unknown variable"); return; }
    need('=', "expected '='"); expr();
    emit(0xf9000000U | ((uint32)(vars[v].off / 8) << 10) | (29 << 5));
    need(';', "expected ';'");
  } else if(tok == ';') next();
  else error("unsupported statement");
}

static void
compile(void)
{
  int sawmain = 0;
  next();
  while(tok != T_EOF && tok != '{'){
    if(word("main")) sawmain = 1;
    next();
  }
  if(!sawmain){ error("expected main function"); return; }
  need('{', "expected '{'");
  emit(0xd10403ffU);                       // sub sp, sp, #256
  emit(0x910003fdU);                       // mov x29, sp
  while(tok != '}' && tok != T_EOF && !failed) statement();
  need('}', "expected '}'");
  if(!returned){ mov64(0, 0); mov64(7, 2); emit(0xd4000001U); }
}

static int
writeall(int fd, void *p, int n)
{
  int m;
  while(n > 0){
    m = write(fd, p, n);
    if(m <= 0) return -1;
    p = (char*)p + m; n -= m;
  }
  return 0;
}

static int
output(char *path)
{
  struct elfhdr eh;
  struct proghdr ph;
  int fd, i, payload = ncode * 4 + nstrings;
  memset(&eh, 0, sizeof(eh));
  memset(&ph, 0, sizeof(ph));
  eh.magic = ELF_MAGIC;
  eh.elf[0] = 2; eh.elf[1] = 1; eh.elf[2] = 1;
  eh.type = 2; eh.machine = 183; eh.version = 1;
  eh.entry = 0; eh.phoff = sizeof(eh);
  eh.ehsize = sizeof(eh); eh.phentsize = sizeof(ph); eh.phnum = 1;
  ph.type = ELF_PROG_LOAD;
  ph.flags = ELF_PROG_FLAG_READ | ELF_PROG_FLAG_WRITE | ELF_PROG_FLAG_EXEC;
  ph.off = sizeof(eh) + sizeof(ph);
  ph.vaddr = 0; ph.paddr = 0; ph.filesz = payload; ph.memsz = payload; ph.align = 8;
  for(i = 0; i < npatch; i++){
    int at = patches[i].insn * 4;
    int target = ncode * 4 + patches[i].off;
    int delta = target - at;
    uint32 immlo = delta & 3;
    uint32 immhi = ((uint32)(delta >> 2)) & 0x7ffff;
    code[patches[i].insn] = 0x10000001U | (immlo << 29) | (immhi << 5);
  }
  fd = open(path, O_CREATE | O_TRUNC | O_WRONLY);
  if(fd < 0){ fprintf(2, "tcc: cannot create %s\n", path); return -1; }
  if(writeall(fd, &eh, sizeof(eh)) < 0 || writeall(fd, &ph, sizeof(ph)) < 0 ||
     writeall(fd, code, ncode * 4) < 0 || writeall(fd, strings, nstrings) < 0){
    fprintf(2, "tcc: write failed\n"); close(fd); unlink(path); return -1;
  }
  close(fd);
  return 0;
}

int
main(int argc, char **argv)
{
  char *in = 0, *out = "a.out";
  int fd, n = 0, r, i;
  for(i = 1; i < argc; i++){
    if(strcmp(argv[i], "-o") == 0 && i + 1 < argc) out = argv[++i];
    else if(in == 0) in = argv[i];
    else { fprintf(2, "usage: tcc source.c [-o program]\n"); exit(1); }
  }
  if(in == 0){ fprintf(2, "usage: tcc source.c [-o program]\n"); exit(1); }
  fd = open(in, O_RDONLY);
  if(fd < 0){ fprintf(2, "tcc: cannot open %s\n", in); exit(1); }
  while(n < SRCMAX && (r = read(fd, source + n, SRCMAX - n)) > 0) n += r;
  close(fd);
  if(n == SRCMAX){ fprintf(2, "tcc: source is too large\n"); exit(1); }
  source[n] = 0; cur = source;
  compile();
  if(failed || output(out) < 0) exit(1);
  printf("tcc: wrote %s (%d bytes code, %d bytes data)\n", out, ncode*4, nstrings);
  exit(0);
}
