// mjc -- a MiniJava-subset compiler that emits AArch64 assembly for xv6.
//
// Pipeline:  source -> lexer -> recursive-descent parser -> AST
//            -> class/vtable layout -> type-checking code generator -> .S
//
// The interesting part for this prototype is how Java's `synchronized`
// is lowered.  Like javac + HotSpot's template interpreter, a
//
//     synchronized (obj) { body }
//
// becomes
//
//     monitorenter(obj)            ; inline thin-lock CAS (LDAXR/STXR),
//                                  ; slow path calls mj_monitor_enter_slow
//     try { body }                 ; exception-table entry -> handler
//     monitorexit(obj)             ; inline STLXR release, slow path in C
//     ...
//   handler:                       ; "any" exception
//     monitorexit(obj); rethrow
//
// and `return` inside the block runs the monitorexit before leaving.
//
// The generated code reserves x28 as the "current thread" register
// (HotSpot's aarch64 port uses r28 = rthread for the same purpose).
//
// Usage: mjc input.java > output.S
//
// Supported subset (see minijava/README.md): classes with single
// inheritance, instance/static fields (int, boolean, int[], references,
// volatile), instance/static methods, one constructor per class, virtual
// dispatch, if/while/for, int arithmetic, &&, ||, !, arrays, String
// literals and "a" + x concatenation inside System.out.print/println,
// synchronized blocks and methods, try/catch/throw, Thread subclasses
// with start()/join(), Object.wait()/notify()/notifyAll(),
// Thread.yield(), Thread.sleep(ms).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

// ---------------------------------------------------------------- utilities

static const char *srcname;

static void
die(int line, const char *fmt, ...)
{
  va_list ap;
  fprintf(stderr, "%s:%d: error: ", srcname, line);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, "\n");
  exit(1);
}

static void *
xcalloc(size_t n)
{
  void *p = calloc(1, n);
  if(!p){ fprintf(stderr, "out of memory\n"); exit(1); }
  return p;
}

static char *
xstrdup(const char *s)
{
  char *p = xcalloc(strlen(s) + 1);
  strcpy(p, s);
  return p;
}

// growable pointer vector
typedef struct { void **v; int n, cap; } Vec;
static void
vpush(Vec *a, void *p)
{
  if(a->n == a->cap){
    a->cap = a->cap ? a->cap * 2 : 8;
    a->v = realloc(a->v, a->cap * sizeof(void *));
  }
  a->v[a->n++] = p;
}

// ---------------------------------------------------------------- lexer

enum { T_EOF, T_ID, T_NUM, T_STR, T_OP };
typedef struct { int kind; char *text; long num; int line; } Token;

static Token *toks;
static int ntoks, tpos;

static void
lex(const char *s)
{
  int cap = 1024, line = 1;
  toks = xcalloc(cap * sizeof(Token));
  ntoks = 0;
  while(1){
    if(ntoks + 1 >= cap){
      cap *= 2;
      toks = realloc(toks, cap * sizeof(Token));
    }
    Token *t = &toks[ntoks];
    while(*s){
      if(*s == '\n'){ line++; s++; }
      else if(isspace((unsigned char)*s)) s++;
      else if(s[0] == '/' && s[1] == '/'){ while(*s && *s != '\n') s++; }
      else if(s[0] == '/' && s[1] == '*'){
        s += 2;
        while(*s && !(s[0] == '*' && s[1] == '/')){ if(*s == '\n') line++; s++; }
        if(*s) s += 2;
      } else break;
    }
    t->line = line;
    if(!*s){ t->kind = T_EOF; t->text = "<eof>"; ntoks++; return; }
    if(isalpha((unsigned char)*s) || *s == '_'){
      const char *b = s;
      while(isalnum((unsigned char)*s) || *s == '_') s++;
      t->kind = T_ID;
      t->text = strndup(b, s - b);
    } else if(isdigit((unsigned char)*s)){
      const char *b = s;
      t->num = strtol(s, (char **)&s, 10);
      t->kind = T_NUM;
      t->text = strndup(b, s - b);
    } else if(*s == '"'){
      const char *b = ++s;
      while(*s && *s != '"'){ if(*s == '\\' && s[1]) s++; s++; }
      if(!*s) die(line, "unterminated string");
      t->kind = T_STR;
      t->text = strndup(b, s - b);
      s++;
    } else if(*s == '\''){
      // char literal -> int
      s++;
      long v = *s;
      if(*s == '\\'){ s++; v = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s == '0' ? 0 : *s; }
      s++;
      if(*s != '\'') die(line, "bad char literal");
      s++;
      t->kind = T_NUM; t->num = v; t->text = "<char>";
    } else {
      static const char *ops2[] = { "==", "!=", "<=", ">=", "&&", "||", "++", "--", "+=", "-=", 0 };
      t->kind = T_OP;
      t->text = 0;
      for(int i = 0; ops2[i]; i++)
        if(strncmp(s, ops2[i], 2) == 0){ t->text = xstrdup(ops2[i]); s += 2; break; }
      if(!t->text){
        if(!strchr("{}()[];,.=<>+-*/%!", *s)) die(line, "unexpected character '%c'", *s);
        t->text = strndup(s, 1);
        s++;
      }
    }
    ntoks++;
  }
}

static Token *peek(void) { return &toks[tpos]; }
static Token *peekn(int n) { return &toks[tpos + n < ntoks ? tpos + n : ntoks - 1]; }
static Token *next(void) { Token *t = &toks[tpos]; if(t->kind != T_EOF) tpos++; return t; }
static int is(const char *s) { return strcmp(peek()->text, s) == 0 && peek()->kind != T_STR; }
static int isn(int n, const char *s) { return strcmp(peekn(n)->text, s) == 0 && peekn(n)->kind != T_STR; }
static int accept(const char *s) { if(is(s)){ next(); return 1; } return 0; }
static void
expect(const char *s)
{
  if(!accept(s)) die(peek()->line, "expected '%s' but found '%s'", s, peek()->text);
}
static char *
ident(void)
{
  Token *t = next();
  if(t->kind != T_ID) die(t->line, "expected identifier but found '%s'", t->text);
  return t->text;
}

// ---------------------------------------------------------------- types / AST

enum { TY_VOID, TY_INT, TY_BOOL, TY_STR, TY_NULL, TY_INTARR, TY_CLASS };
struct Class;
typedef struct { int k; struct Class *c; char *cname; } Ty;

enum {
  // expressions
  E_NUM, E_BOOL, E_STR, E_NULL, E_THIS, E_NAME, E_FIELD, E_INDEX, E_CALL,
  E_NEW, E_NEWARR, E_BIN, E_NOT, E_NEG, E_CLASSLIT,
  // statements
  S_BLOCK, S_VAR, S_ASSIGN, S_EXPR, S_IF, S_WHILE, S_FOR, S_RETURN, S_SYNC,
  S_TRY, S_THROW, S_PRINT,
};

typedef struct Node {
  int kind, line;
  char *name;           // identifier / operator / string
  long num;
  Ty ty;                // S_VAR declared type, E_CLASSLIT class
  struct Node *a, *b, *c, *d;
  Vec list;             // block statements, call args, catch clauses
  int newline;          // S_PRINT
} Node;

static Node *
mk(int kind, int line)
{
  Node *n = xcalloc(sizeof(Node));
  n->kind = kind;
  n->line = line;
  return n;
}

typedef struct Field {
  char *name; Ty ty; int isstatic, isvolatile;
  int offset;                  // instance field offset in bytes
  struct Class *owner;
} Field;

typedef struct Method {
  char *name; Ty ret; int isstatic, issync, isctor;
  Vec params;                  // Field* (name, ty)
  Node *body;
  struct Class *owner;
  int slot;                    // vtable slot (1-based; 0 is the class pointer)
  int line;
} Method;

typedef struct Class {
  char *name, *supername;
  struct Class *super;
  Vec fields, methods;
  Method *ctor;
  int size;                    // instance size in bytes (incl. 16-byte header)
  Vec vtable;                  // Method*
  int laid_out, builtin;
  int line;
} Class;

static Vec classes;

static Class *
findclass(const char *name)
{
  for(int i = 0; i < classes.n; i++){
    Class *c = classes.v[i];
    if(strcmp(c->name, name) == 0) return c;
  }
  return 0;
}

static int
subclass_of(Class *c, Class *p)
{
  for(; c; c = c->super) if(c == p) return 1;
  return 0;
}

// ---------------------------------------------------------------- parser

static Ty
parse_type(void)
{
  Ty t = {0};
  Token *tk = peek();
  char *n = ident();
  if(strcmp(n, "int") == 0) t.k = TY_INT;
  else if(strcmp(n, "boolean") == 0) t.k = TY_BOOL;
  else if(strcmp(n, "void") == 0) t.k = TY_VOID;
  else if(strcmp(n, "String") == 0) t.k = TY_STR;
  else { t.k = TY_CLASS; t.cname = n; }
  if(accept("[")){
    expect("]");
    if(t.k == TY_INT) t.k = TY_INTARR;
    else if(t.k == TY_STR) t.k = TY_STR;     // String[] args: accepted, unused
    else die(tk->line, "only int[] arrays are supported");
  }
  return t;
}

static int
looks_like_decl(void)
{
  // Type name  |  Type [ ] name
  Token *a = peekn(0), *b = peekn(1);
  if(a->kind != T_ID) return 0;
  if(b->kind == T_ID) return 1;
  if(strcmp(b->text, "[") == 0 && strcmp(peekn(2)->text, "]") == 0) return 1;
  return 0;
}

static Node *parse_expr(void);
static Node *parse_stmt(void);

static void
parse_args(Node *call)
{
  expect("(");
  if(!is(")")){
    do vpush(&call->list, parse_expr()); while(accept(","));
  }
  expect(")");
}

static Node *
parse_primary(void)
{
  Token *t = peek();
  Node *n;
  if(t->kind == T_NUM){ next(); n = mk(E_NUM, t->line); n->num = t->num; }
  else if(t->kind == T_STR){ next(); n = mk(E_STR, t->line); n->name = t->text; }
  else if(accept("(")){ n = parse_expr(); expect(")"); }
  else if(accept("true")){ n = mk(E_BOOL, t->line); n->num = 1; }
  else if(accept("false")){ n = mk(E_BOOL, t->line); n->num = 0; }
  else if(accept("null")){ n = mk(E_NULL, t->line); }
  else if(accept("this")){ n = mk(E_THIS, t->line); }
  else if(accept("new")){
    char *cn = ident();
    if(strcmp(cn, "int") == 0){
      expect("[");
      n = mk(E_NEWARR, t->line);
      n->a = parse_expr();
      expect("]");
    } else {
      n = mk(E_NEW, t->line);
      n->name = cn;
      parse_args(n);
    }
  } else if(t->kind == T_ID){
    next();
    if(is("(")){
      n = mk(E_CALL, t->line);
      n->name = t->text;
      parse_args(n);
    } else {
      n = mk(E_NAME, t->line);
      n->name = t->text;
    }
  } else die(t->line, "unexpected '%s' in expression", t->text);

  for(;;){
    Token *p = peek();
    if(accept(".")){
      char *m = ident();
      if(is("(")){
        Node *c = mk(E_CALL, p->line);
        c->name = m; c->a = n;
        parse_args(c);
        n = c;
      } else {
        Node *f = mk(E_FIELD, p->line);
        f->name = m; f->a = n;
        n = f;
      }
    } else if(accept("[")){
      Node *x = mk(E_INDEX, p->line);
      x->a = n; x->b = parse_expr();
      expect("]");
      n = x;
    } else break;
  }
  return n;
}

static Node *
parse_unary(void)
{
  Token *t = peek();
  if(accept("!")){ Node *n = mk(E_NOT, t->line); n->a = parse_unary(); return n; }
  if(accept("-")){ Node *n = mk(E_NEG, t->line); n->a = parse_unary(); return n; }
  return parse_primary();
}

static int
prec(const char *op)
{
  static const struct { const char *op; int p; } tab[] = {
    {"||", 1}, {"&&", 2}, {"==", 3}, {"!=", 3},
    {"<", 4}, {"<=", 4}, {">", 4}, {">=", 4},
    {"+", 5}, {"-", 5}, {"*", 6}, {"/", 6}, {"%", 6}, {0, 0}};
  for(int i = 0; tab[i].op; i++) if(strcmp(op, tab[i].op) == 0) return tab[i].p;
  return 0;
}

static Node *
parse_binary(int minp)
{
  Node *l = parse_unary();
  for(;;){
    Token *t = peek();
    int p = t->kind == T_OP ? prec(t->text) : 0;
    if(p == 0 || p < minp) return l;
    next();
    Node *n = mk(E_BIN, t->line);
    n->name = t->text; n->a = l; n->b = parse_binary(p + 1);
    l = n;
  }
}

static Node *parse_expr(void) { return parse_binary(1); }

static Node *
parse_block(void)
{
  Node *b = mk(S_BLOCK, peek()->line);
  expect("{");
  while(!accept("}")){
    if(peek()->kind == T_EOF) die(peek()->line, "unexpected end of file in block");
    vpush(&b->list, parse_stmt());
  }
  return b;
}

// assignment-ish simple statement (no trailing ';'): used by stmt and for()
static Node *
parse_simple(void)
{
  Token *t = peek();
  if(looks_like_decl()){
    Node *n = mk(S_VAR, t->line);
    n->ty = parse_type();
    n->name = ident();
    if(accept("=")) n->a = parse_expr();
    return n;
  }
  Node *e = parse_expr();
  if(accept("=")){
    Node *n = mk(S_ASSIGN, t->line);
    n->a = e; n->b = parse_expr();
    return n;
  }
  if(is("+=") || is("-=") || is("++") || is("--")){
    char *op = next()->text;
    Node *rhs;
    if(op[1] == '='){ rhs = parse_expr(); }
    else { rhs = mk(E_NUM, t->line); rhs->num = 1; }
    Node *bin = mk(E_BIN, t->line);
    bin->name = op[0] == '+' ? "+" : "-";
    bin->a = e; bin->b = rhs;
    Node *n = mk(S_ASSIGN, t->line);
    n->a = e; n->b = bin;          // lvalue evaluated twice: fine for names/fields
    return n;
  }
  if(e->kind != E_CALL && e->kind != E_NEW) die(t->line, "not a statement");
  Node *n = mk(S_EXPR, t->line);
  n->a = e;
  return n;
}

static Node *
parse_stmt(void)
{
  Token *t = peek();
  Node *n;
  if(is("{")) return parse_block();
  if(accept("if")){
    n = mk(S_IF, t->line);
    expect("("); n->a = parse_expr(); expect(")");
    n->b = parse_stmt();
    if(accept("else")) n->c = parse_stmt();
    return n;
  }
  if(accept("while")){
    n = mk(S_WHILE, t->line);
    expect("("); n->a = parse_expr(); expect(")");
    n->b = parse_stmt();
    return n;
  }
  if(accept("for")){
    n = mk(S_FOR, t->line);
    expect("(");
    if(!is(";")) n->a = parse_simple();
    expect(";");
    if(!is(";")) n->b = parse_expr();
    expect(";");
    if(!is(")")) n->c = parse_simple();
    expect(")");
    n->d = parse_stmt();
    return n;
  }
  if(accept("return")){
    n = mk(S_RETURN, t->line);
    if(!is(";")) n->a = parse_expr();
    expect(";");
    return n;
  }
  if(accept("synchronized")){
    n = mk(S_SYNC, t->line);
    expect("("); n->a = parse_expr(); expect(")");
    n->b = parse_block();
    return n;
  }
  if(accept("try")){
    n = mk(S_TRY, t->line);
    n->a = parse_block();
    while(is("catch")){
      Token *ct = next();
      Node *c = mk(S_VAR, ct->line);
      expect("(");
      c->ty = parse_type();
      c->name = ident();
      expect(")");
      c->a = parse_block();
      vpush(&n->list, c);
    }
    if(is("finally")) die(peek()->line, "finally is not supported (see README)");
    if(n->list.n == 0) die(t->line, "try without catch");
    return n;
  }
  if(accept("throw")){
    n = mk(S_THROW, t->line);
    n->a = parse_expr();
    expect(";");
    return n;
  }
  if(is("System") && isn(1, ".") && isn(2, "out") && isn(3, ".")){
    tpos += 4;
    char *m = ident();
    n = mk(S_PRINT, t->line);
    if(strcmp(m, "println") == 0) n->newline = 1;
    else if(strcmp(m, "print") != 0) die(t->line, "unknown System.out.%s", m);
    expect("(");
    if(!is(")")) n->a = parse_expr();
    expect(")"); expect(";");
    return n;
  }
  if(accept(";")) return mk(S_BLOCK, t->line);
  n = parse_simple();
  expect(";");
  return n;
}

static void
parse_class(void)
{
  Class *c = xcalloc(sizeof(Class));
  c->line = peek()->line;
  expect("class");
  c->name = ident();
  if(findclass(c->name)) die(c->line, "duplicate class %s", c->name);
  c->supername = accept("extends") ? ident() : "Object";
  vpush(&classes, c);
  expect("{");
  while(!accept("}")){
    int isstatic = 0, issync = 0, isvol = 0;
    for(;;){
      if(accept("public") || accept("private") || accept("protected") || accept("final")) continue;
      if(accept("static")){ isstatic = 1; continue; }
      if(accept("synchronized")){ issync = 1; continue; }
      if(accept("volatile")){ isvol = 1; continue; }
      break;
    }
    Token *t = peek();
    if(t->kind == T_ID && strcmp(t->text, c->name) == 0 && isn(1, "(")){
      // constructor
      next();
      Method *m = xcalloc(sizeof(Method));
      m->name = "<init>"; m->isctor = 1; m->owner = c; m->line = t->line;
      m->ret.k = TY_VOID;
      expect("(");
      if(!is(")")) do {
        Field *p = xcalloc(sizeof(Field));
        p->ty = parse_type(); p->name = ident();
        vpush(&m->params, p);
      } while(accept(","));
      expect(")");
      m->body = parse_block();
      if(c->ctor) die(t->line, "only one constructor per class is supported");
      c->ctor = m;
      continue;
    }
    Ty ty = parse_type();
    char *name = ident();
    if(accept("(")){
      Method *m = xcalloc(sizeof(Method));
      m->name = name; m->ret = ty; m->isstatic = isstatic; m->issync = issync;
      m->owner = c; m->line = t->line;
      if(!is(")")) do {
        Field *p = xcalloc(sizeof(Field));
        p->ty = parse_type(); p->name = ident();
        vpush(&m->params, p);
      } while(accept(","));
      expect(")");
      if(m->params.n > 7) die(t->line, "at most 7 parameters are supported");
      m->body = parse_block();
      vpush(&c->methods, m);
    } else {
      do {
        Field *f = xcalloc(sizeof(Field));
        f->name = name; f->ty = ty; f->isstatic = isstatic; f->isvolatile = isvol;
        f->owner = c;
        if(is("=")) die(t->line, "field initializers are not supported; assign in the constructor");
        vpush(&c->fields, f);
        if(accept(",")) name = ident(); else break;
      } while(1);
      expect(";");
    }
  }
}

// ---------------------------------------------------------------- built-in classes

static Class *cls_object, *cls_thread, *cls_exception;

static Class *
builtin(const char *name, const char *super)
{
  Class *c = xcalloc(sizeof(Class));
  c->name = xstrdup(name);
  c->supername = super ? xstrdup(super) : 0;
  c->builtin = 1;
  vpush(&classes, c);
  return c;
}

static void
add_builtins(void)
{
  cls_object = builtin("Object", 0);
  cls_thread = builtin("Thread", "Object");
  // hidden field: pointer to the runtime's MJThread (offset 16)
  Field *f = xcalloc(sizeof(Field));
  f->name = "$rt"; f->ty.k = TY_INT; f->owner = cls_thread;
  vpush(&cls_thread->fields, f);
  Method *run = xcalloc(sizeof(Method));
  run->name = "run"; run->ret.k = TY_VOID; run->owner = cls_thread;
  run->body = mk(S_BLOCK, 0);
  vpush(&cls_thread->methods, run);
  cls_exception = builtin("Exception", "Object");
  builtin("InterruptedException", "Exception");
  builtin("RuntimeException", "Exception");
  builtin("IllegalMonitorStateException", "RuntimeException");
  builtin("IllegalThreadStateException", "RuntimeException");
  builtin("ArithmeticException", "RuntimeException");
  builtin("NullPointerException", "RuntimeException");
  builtin("ArrayIndexOutOfBoundsException", "RuntimeException");
}

// ---------------------------------------------------------------- layout

static Ty
resolve(Ty t, int line)
{
  if(t.k == TY_CLASS && !t.c){
    t.c = findclass(t.cname);
    if(!t.c) die(line, "unknown class %s", t.cname);
  }
  return t;
}

static Method *
findmethod(Class *c, const char *name)
{
  for(; c; c = c->super)
    for(int i = 0; i < c->methods.n; i++){
      Method *m = c->methods.v[i];
      if(strcmp(m->name, name) == 0) return m;
    }
  return 0;
}

static Field *
findfield(Class *c, const char *name)
{
  for(; c; c = c->super)
    for(int i = 0; i < c->fields.n; i++){
      Field *f = c->fields.v[i];
      if(strcmp(f->name, name) == 0) return f;
    }
  return 0;
}

static void
layout(Class *c)
{
  if(c->laid_out == 2) return;
  if(c->laid_out == 1) die(c->line, "cyclic inheritance involving %s", c->name);
  c->laid_out = 1;
  if(c->supername){
    c->super = findclass(c->supername);
    if(!c->super) die(c->line, "unknown superclass %s", c->supername);
    layout(c->super);
    c->size = c->super->size;
    for(int i = 0; i < c->super->vtable.n; i++) vpush(&c->vtable, c->super->vtable.v[i]);
  } else {
    c->size = 16;          // [0] vtable pointer, [8] lock word ("mark word")
  }
  for(int i = 0; i < c->fields.n; i++){
    Field *f = c->fields.v[i];
    f->ty = resolve(f->ty, c->line);
    if(!f->isstatic){ f->offset = c->size; c->size += 8; }
  }
  for(int i = 0; i < c->methods.n; i++){
    Method *m = c->methods.v[i];
    for(int j = 0; j < i; j++)
      if(strcmp(((Method *)c->methods.v[j])->name, m->name) == 0)
        die(m->line, "overloading is not supported (%s.%s)", c->name, m->name);
    m->ret = resolve(m->ret, m->line);
    for(int j = 0; j < m->params.n; j++){
      Field *p = m->params.v[j];
      p->ty = resolve(p->ty, m->line);
    }
    if(m->isstatic) continue;
    int slot = -1;
    for(int j = 0; j < c->vtable.n; j++)
      if(strcmp(((Method *)c->vtable.v[j])->name, m->name) == 0) slot = j;
    if(slot >= 0){
      Method *old = c->vtable.v[slot];
      if(old->params.n != m->params.n) die(m->line, "override of %s changes arity", m->name);
      c->vtable.v[slot] = m;
    } else {
      slot = c->vtable.n;
      vpush(&c->vtable, m);
    }
    m->slot = slot + 1;
  }
  if(c->ctor){
    for(int j = 0; j < c->ctor->params.n; j++){
      Field *p = c->ctor->params.v[j];
      p->ty = resolve(p->ty, c->ctor->line);
    }
    if(c->ctor->params.n > 6) die(c->ctor->line, "at most 6 constructor parameters");
  }
  c->laid_out = 2;
}

// ---------------------------------------------------------------- code generation

static FILE *out;              // current output (method body buffer or file)
static int labelno;
static int newlabel(void) { return ++labelno; }

static void
emit(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vfprintf(out, fmt, ap);
  va_end(ap);
  fputc('\n', out);
}

// string literal pool
static Vec strlits;
static int
strlit(const char *s)
{
  for(int i = 0; i < strlits.n; i++) if(strcmp(strlits.v[i], s) == 0) return i;
  vpush(&strlits, (void *)s);
  return strlits.n - 1;
}

typedef struct Local { char *name; Ty ty; int off; int depth; } Local;
enum { CTX_TRY, CTX_SYNC };
typedef struct Ctx { int kind; int rec; int objslot; } Ctx;

static Class *curclass;
static Method *curmeth;
static Vec locals;             // Local*
static int scope_depth;
static int frame_size;         // bytes of locals area (x29 .. x29+frame_size)
static Vec ctxs;               // Ctx*
static int retslot, retlabel;

static int
newslot(int bytes)
{
  int off = frame_size;
  frame_size += (bytes + 7) & ~7;
  return off;
}

static Local *
findlocal(const char *name)
{
  for(int i = locals.n - 1; i >= 0; i--){
    Local *l = locals.v[i];
    if(strcmp(l->name, name) == 0) return l;
  }
  return 0;
}

static Local *
addlocal(const char *name, Ty ty, int line)
{
  Local *l = findlocal(name);
  if(l && l->depth == scope_depth) die(line, "duplicate variable %s", name);
  l = xcalloc(sizeof(Local));
  l->name = (char *)name; l->ty = ty; l->off = newslot(8); l->depth = scope_depth;
  vpush(&locals, l);
  return l;
}

static void
popscope(void)
{
  while(locals.n > 0 && ((Local *)locals.v[locals.n - 1])->depth == scope_depth) locals.n--;
  scope_depth--;
}

static const char *
tyname(Ty t)
{
  switch(t.k){
  case TY_VOID: return "void";
  case TY_INT: return "int";
  case TY_BOOL: return "boolean";
  case TY_STR: return "String";
  case TY_NULL: return "null";
  case TY_INTARR: return "int[]";
  default: return t.c ? t.c->name : t.cname;
  }
}

static int
assignable(Ty to, Ty from)
{
  if(to.k == TY_CLASS){
    if(from.k == TY_NULL) return 1;
    return from.k == TY_CLASS && subclass_of(from.c, to.c);
  }
  if(to.k == TY_INTARR) return from.k == TY_INTARR || from.k == TY_NULL;
  return to.k == from.k;
}

static void push(void) { emit("\tstr x0, [sp, #-16]!"); }
static void pop(const char *r) { emit("\tldr %s, [sp], #16", r); }

static void
load_addr(const char *reg, const char *sym)
{
  emit("\tadrp %s, %s", reg, sym);
  emit("\tadd %s, %s, :lo12:%s", reg, reg, sym);
}

static void
load_imm(long v)
{
  unsigned long u = (unsigned long)v;
  emit("\tmov x0, #%lu", u & 0xffff);
  for(int sh = 16; sh < 64; sh += 16)
    if((u >> sh) & 0xffff) emit("\tmovk x0, #%lu, lsl #%d", (u >> sh) & 0xffff, sh);
}

static void
nullcheck(const char *reg)
{
  int l = newlabel();
  emit("\tcbnz %s, .L%d", reg, l);
  emit("\tbl mj_throw_npe");
  emit(".L%d:", l);
}

// x0 = object.  Inline thin-lock fast path; slow path in runtime.
//   lock word: 0 = unlocked
//              owner_id<<32 | recursion<<2 | 0b01  = thin (lightweight) lock
//              ObjectMonitor* | 0b10               = inflated (heavyweight)
static void
emit_monitorenter(void)
{
  int retry = newlabel(), slow = newlabel(), done = newlabel();
  emit("\t// ---- monitorenter: thin-lock fast path  (JVM bytecode: monitorenter)");
  emit("\tadd x1, x0, #8              // &obj->lockword");
  emit("\tldr w2, [x28]               // x28 = current MJThread, [0] = thread id");
  emit("\tlsl x2, x2, #32");
  emit("\tadd x2, x2, #5              // owner<<32 | rec 1<<2 | THIN");
  emit(".L%d:", retry);
  emit("\tldaxr x3, [x1]              // load-acquire exclusive");
  emit("\tcbnz x3, .L%d                // locked/inflated -> runtime", slow);
  emit("\tstxr w4, x2, [x1]           // try to claim");
  emit("\tcbnz w4, .L%d                // lost the reservation, retry", retry);
  emit("\tb .L%d", done);
  emit(".L%d:", slow);
  emit("\tclrex");
  emit("\tbl mj_monitor_enter_slow     // recursion, contention, inflation");
  emit(".L%d:", done);
}

static void
emit_monitorexit(void)
{
  int retry = newlabel(), slow = newlabel(), done = newlabel();
  emit("\t// ---- monitorexit: thin-lock fast path  (JVM bytecode: monitorexit)");
  emit("\tadd x1, x0, #8");
  emit("\tldr w2, [x28]");
  emit("\tlsl x2, x2, #32");
  emit("\tadd x2, x2, #5              // the word we expect: owned by us, rec 1");
  emit(".L%d:", retry);
  emit("\tldxr x3, [x1]");
  emit("\tcmp x3, x2");
  emit("\tb.ne .L%d", slow);
  emit("\tstlxr w4, xzr, [x1]         // store-release 0: unlock");
  emit("\tcbnz w4, .L%d", retry);
  emit("\tb .L%d", done);
  emit(".L%d:", slow);
  emit("\tclrex");
  emit("\tbl mj_monitor_exit_slow");
  emit(".L%d:", done);
}

// push an exception-handler record (prev, fp, sp, handler pc) in the frame
// and link it into the current thread's handler chain ([x28, #8]).
static void
emit_push_handler(int rec, int handler)
{
  emit("\t// install exception handler record (prev, fp, sp, pc)");
  emit("\tadd x1, x29, #%d", rec);
  emit("\tldr x2, [x28, #8]");
  emit("\tstr x2, [x1]");
  emit("\tstr x29, [x1, #8]");
  emit("\tmov x3, sp");
  emit("\tstr x3, [x1, #16]");
  emit("\tadr x3, .L%d", handler);
  emit("\tstr x3, [x1, #24]");
  emit("\tstr x1, [x28, #8]");
}

static void
emit_pop_handler(int rec)
{
  emit("\tldr x1, [x29, #%d]           // unlink handler record", rec);
  emit("\tstr x1, [x28, #8]");
}

static Ty gen_expr(Node *e);
static void gen_stmt(Node *s);

static Ty
mkty(int k)
{
  Ty t = {0};
  t.k = k;
  return t;
}

static Ty
classty(Class *c)
{
  Ty t = {0};
  t.k = TY_CLASS; t.c = c; t.cname = c->name;
  return t;
}

static int
is_classname(Node *e)
{
  return e && e->kind == E_NAME && !findlocal(e->name) &&
         !findfield(curclass, e->name) && findclass(e->name);
}

// lvalue: leaves the address in x0 and returns its type; *vol set for volatile
static Ty
gen_addr(Node *e, int *vol)
{
  *vol = 0;
  if(e->kind == E_NAME){
    Local *l = findlocal(e->name);
    if(l){
      emit("\tadd x0, x29, #%d", l->off);
      return l->ty;
    }
    Field *f = findfield(curclass, e->name);
    if(!f) die(e->line, "unknown variable %s", e->name);
    *vol = f->isvolatile;
    if(f->isstatic){
      char sym[256];
      snprintf(sym, sizeof sym, "MJS_%s_%s", f->owner->name, f->name);
      load_addr("x0", sym);
      return f->ty;
    }
    if(curmeth->isstatic) die(e->line, "instance field %s used in static method", e->name);
    emit("\tldr x0, [x29, #0]           // this");
    emit("\tadd x0, x0, #%d             // .%s", f->offset, f->name);
    return f->ty;
  }
  if(e->kind == E_FIELD){
    if(is_classname(e->a)){
      Class *c = findclass(e->a->name);
      Field *f = findfield(c, e->name);
      if(!f || !f->isstatic) die(e->line, "no static field %s.%s", c->name, e->name);
      *vol = f->isvolatile;
      char sym[256];
      snprintf(sym, sizeof sym, "MJS_%s_%s", f->owner->name, f->name);
      load_addr("x0", sym);
      return f->ty;
    }
    Ty ot = gen_expr(e->a);
    if(ot.k != TY_CLASS) die(e->line, "field access on non-object (%s)", tyname(ot));
    Field *f = findfield(ot.c, e->name);
    if(!f) die(e->line, "class %s has no field %s", ot.c->name, e->name);
    *vol = f->isvolatile;
    if(f->isstatic){
      char sym[256];
      snprintf(sym, sizeof sym, "MJS_%s_%s", f->owner->name, f->name);
      load_addr("x0", sym);
      return f->ty;
    }
    nullcheck("x0");
    emit("\tadd x0, x0, #%d             // .%s", f->offset, f->name);
    return f->ty;
  }
  if(e->kind == E_INDEX){
    Ty at = gen_expr(e->a);
    if(at.k != TY_INTARR) die(e->line, "indexing a non-array");
    push();
    Ty it = gen_expr(e->b);
    if(it.k != TY_INT) die(e->line, "array index must be int");
    pop("x1");
    nullcheck("x1");
    int ok = newlabel();
    emit("\tldr x2, [x1, #16]           // length");
    emit("\tcmp x0, x2");
    emit("\tb.lo .L%d", ok);
    emit("\tbl mj_throw_aioobe");
    emit(".L%d:", ok);
    emit("\tadd x1, x1, #24");
    emit("\tadd x0, x1, x0, lsl #3");
    return mkty(TY_INT);
  }
  die(e->line, "not assignable");
  return mkty(TY_VOID);
}

static void
emit_load(int vol)
{
  if(vol) emit("\tldar x0, [x0]               // volatile read = load-acquire");
  else emit("\tldr x0, [x0]");
}

static Ty
gen_call(Node *e)
{
  Method *m = 0;
  Class *target = 0;
  int is_static_call = 0;
  int nargs = e->list.n;

  // ---- static calls / intrinsics: Thread.yield(), Thread.sleep(n), Foo.bar()
  if(e->a && is_classname(e->a)){
    Class *c = findclass(e->a->name);
    if(c == cls_thread && strcmp(e->name, "yield") == 0 && nargs == 0){
      emit("\tbl mj_yield");
      return mkty(TY_VOID);
    }
    if(c == cls_thread && strcmp(e->name, "sleep") == 0 && nargs == 1){
      if(gen_expr(e->list.v[0]).k != TY_INT) die(e->line, "Thread.sleep(int)");
      emit("\tbl mj_sleep");
      return mkty(TY_VOID);
    }
    m = findmethod(c, e->name);
    if(!m || !m->isstatic) die(e->line, "no static method %s.%s", c->name, e->name);
    is_static_call = 1;
  } else if(!e->a){
    m = findmethod(curclass, e->name);
    int intrinsic = nargs == 0 && (!strcmp(e->name, "wait") || !strcmp(e->name, "notify") ||
                    !strcmp(e->name, "notifyAll") ||
                    (subclass_of(curclass, cls_thread) && (!strcmp(e->name, "start") || !strcmp(e->name, "join"))));
    if(!m && intrinsic){
      if(curmeth->isstatic) die(e->line, "%s() needs an object in a static method", e->name);
    } else if(!m) die(e->line, "unknown method %s", e->name);
    else if(m->isstatic) is_static_call = 1;
    else if(curmeth->isstatic) die(e->line, "instance method %s called from static context", e->name);
  }

  if(is_static_call){
    if(m->params.n != nargs) die(e->line, "%s expects %d arguments", m->name, m->params.n);
    for(int i = 0; i < nargs; i++){
      Field *p = m->params.v[i];
      Ty t = gen_expr(e->list.v[i]);
      if(!assignable(p->ty, t)) die(e->line, "argument %d of %s: %s expected, got %s", i + 1, m->name, tyname(p->ty), tyname(t));
      push();
    }
    for(int i = nargs - 1; i >= 0; i--){ char r[16]; snprintf(r, sizeof r, "x%d", i); pop(r); }
    emit("\tbl MJM_%s_%s", m->owner->name, m->name);
    return m->ret;
  }

  // ---- instance call
  Ty rt;
  if(e->a){
    rt = gen_expr(e->a);
    if(rt.k != TY_CLASS) die(e->line, "method call on non-object (%s)", tyname(rt));
    target = rt.c;
  } else {
    emit("\tldr x0, [x29, #0]           // this");
    target = curclass;
  }
  // intrinsics on any object: wait / notify / notifyAll; on Thread: start / join
  if(nargs == 0){
    const char *rtfn = 0;
    if(strcmp(e->name, "wait") == 0) rtfn = "mj_wait";
    else if(strcmp(e->name, "notify") == 0) rtfn = "mj_notify";
    else if(strcmp(e->name, "notifyAll") == 0) rtfn = "mj_notify_all";
    else if(subclass_of(target, cls_thread) && strcmp(e->name, "start") == 0) rtfn = "mj_thread_start";
    else if(subclass_of(target, cls_thread) && strcmp(e->name, "join") == 0) rtfn = "mj_thread_join";
    if(rtfn){
      nullcheck("x0");
      emit("\tbl %s", rtfn);
      return mkty(TY_VOID);
    }
  }
  m = findmethod(target, e->name);
  if(!m) die(e->line, "class %s has no method %s", target->name, e->name);
  if(m->isstatic) die(e->line, "static method %s called through an instance", e->name);
  if(m->params.n != nargs) die(e->line, "%s expects %d arguments", m->name, m->params.n);
  if(nargs > 6) die(e->line, "too many arguments");
  push();
  for(int i = 0; i < nargs; i++){
    Field *p = m->params.v[i];
    Ty t = gen_expr(e->list.v[i]);
    if(!assignable(p->ty, t)) die(e->line, "argument %d of %s: %s expected, got %s", i + 1, m->name, tyname(p->ty), tyname(t));
    push();
  }
  for(int i = nargs; i >= 0; i--){ char r[16]; snprintf(r, sizeof r, "x%d", i); pop(r); }
  nullcheck("x0");
  emit("\tldr x9, [x0]                // vtable");
  emit("\tldr x9, [x9, #%d]           // slot %d: %s", m->slot * 8, m->slot, m->name);
  emit("\tblr x9");
  return m->ret;
}

static Ty
gen_expr(Node *e)
{
  int vol;
  Ty t, u;
  switch(e->kind){
  case E_NUM: load_imm(e->num); return mkty(TY_INT);
  case E_BOOL: emit("\tmov x0, #%ld", e->num); return mkty(TY_BOOL);
  case E_NULL: emit("\tmov x0, #0"); return mkty(TY_NULL);
  case E_STR: {
    char sym[32];
    snprintf(sym, sizeof sym, ".Lstr%d", strlit(e->name));
    load_addr("x0", sym);
    return mkty(TY_STR);
  }
  case E_THIS:
    if(curmeth->isstatic) die(e->line, "'this' in static method");
    emit("\tldr x0, [x29, #0]           // this");
    return classty(curclass);
  case E_CLASSLIT: {
    char sym[256];
    snprintf(sym, sizeof sym, "MJC_%s", e->ty.c->name);
    load_addr("x0", sym);
    return classty(cls_object);
  }
  case E_NAME:
    if(is_classname(e)) die(e->line, "class name %s used as a value", e->name);
    t = gen_addr(e, &vol);
    emit_load(vol);
    return t;
  case E_FIELD:
    if(strcmp(e->name, "length") == 0 && !is_classname(e->a)){
      t = gen_expr(e->a);
      if(t.k == TY_INTARR){
        nullcheck("x0");
        emit("\tldr x0, [x0, #16]           // .length");
        return mkty(TY_INT);
      }
      // not an array: fall back to a normal field named "length"
      Node tmp = *e;
      tmp.a = e->a;
      t = gen_addr(&tmp, &vol);
      emit_load(vol);
      return t;
    }
    t = gen_addr(e, &vol);
    emit_load(vol);
    return t;
  case E_INDEX:
    t = gen_addr(e, &vol);
    emit_load(0);
    return t;
  case E_CALL:
    return gen_call(e);
  case E_NEW: {
    Class *c = findclass(e->name);
    if(!c) die(e->line, "unknown class %s", e->name);
    if(c == cls_object && 0) {}
    char sym[256];
    snprintf(sym, sizeof sym, "MJC_%s", c->name);
    load_addr("x0", sym);
    emit("\tbl mj_new");
    int nargs = e->list.n;
    if(c->ctor){
      Method *m = c->ctor;
      if(m->params.n != nargs) die(e->line, "constructor %s expects %d arguments", c->name, m->params.n);
      push();
      for(int i = 0; i < nargs; i++){
        Field *p = m->params.v[i];
        Ty at = gen_expr(e->list.v[i]);
        if(!assignable(p->ty, at)) die(e->line, "constructor argument %d: %s expected, got %s", i + 1, tyname(p->ty), tyname(at));
        push();
      }
      for(int i = nargs; i >= 1; i--){ char r[16]; snprintf(r, sizeof r, "x%d", i); pop(r); }
      emit("\tldr x0, [sp]");
      emit("\tbl MJK_%s", c->name);
      pop("x0");
    } else if(nargs) die(e->line, "class %s has no constructor", c->name);
    return classty(c);
  }
  case E_NEWARR:
    if(gen_expr(e->a).k != TY_INT) die(e->line, "array size must be int");
    emit("\tbl mj_new_array");
    return mkty(TY_INTARR);
  case E_NOT:
    if(gen_expr(e->a).k != TY_BOOL) die(e->line, "! needs boolean");
    emit("\teor x0, x0, #1");
    return mkty(TY_BOOL);
  case E_NEG:
    if(gen_expr(e->a).k != TY_INT) die(e->line, "unary - needs int");
    emit("\tneg w0, w0");
    emit("\tsxtw x0, w0");
    return mkty(TY_INT);
  case E_BIN: {
    const char *op = e->name;
    if(strcmp(op, "&&") == 0 || strcmp(op, "||") == 0){
      int end = newlabel();
      if(gen_expr(e->a).k != TY_BOOL) die(e->line, "%s needs booleans", op);
      emit("\t%s x0, .L%d", op[0] == '&' ? "cbz" : "cbnz", end);
      if(gen_expr(e->b).k != TY_BOOL) die(e->line, "%s needs booleans", op);
      emit(".L%d:", end);
      return mkty(TY_BOOL);
    }
    t = gen_expr(e->a);
    push();
    u = gen_expr(e->b);
    pop("x1");                     // x1 = left, x0 = right
    if(t.k == TY_STR || u.k == TY_STR)
      die(e->line, "string concatenation is only supported inside System.out.print/println");
    if(strcmp(op, "==") == 0 || strcmp(op, "!=") == 0){
      int ok = (t.k == u.k) || (t.k == TY_NULL && (u.k == TY_CLASS || u.k == TY_INTARR)) ||
               (u.k == TY_NULL && (t.k == TY_CLASS || t.k == TY_INTARR));
      if(!ok) die(e->line, "cannot compare %s with %s", tyname(t), tyname(u));
      emit("\tcmp x1, x0");
      emit("\tcset x0, %s", op[0] == '=' ? "eq" : "ne");
      return mkty(TY_BOOL);
    }
    if(t.k != TY_INT || u.k != TY_INT) die(e->line, "operator %s needs ints", op);
    if(op[0] == '<' || op[0] == '>'){
      const char *cc = strcmp(op, "<") == 0 ? "lt" : strcmp(op, "<=") == 0 ? "le" :
                       strcmp(op, ">") == 0 ? "gt" : "ge";
      emit("\tcmp x1, x0");
      emit("\tcset x0, %s", cc);
      return mkty(TY_BOOL);
    }
    switch(op[0]){
    case '+': emit("\tadd w0, w1, w0"); break;
    case '-': emit("\tsub w0, w1, w0"); break;
    case '*': emit("\tmul w0, w1, w0"); break;
    case '/': case '%': {
      int ok = newlabel();
      emit("\tcbnz w0, .L%d", ok);
      emit("\tbl mj_throw_arith          // / by zero");
      emit(".L%d:", ok);
      emit("\tsdiv w2, w1, w0");
      if(op[0] == '/') emit("\tmov w0, w2");
      else emit("\tmsub w0, w2, w0, w1");
      break;
    }
    }
    emit("\tsxtw x0, w0                 // Java int is 32-bit");
    return mkty(TY_INT);
  }
  }
  die(e->line, "bad expression");
  return mkty(TY_VOID);
}

// ---- print with string concatenation: flatten "a" + x + "b"
static int
has_string(Node *e)
{
  if(e->kind == E_STR) return 1;
  if(e->kind == E_BIN && strcmp(e->name, "+") == 0) return has_string(e->a) || has_string(e->b);
  return 0;
}

static void
gen_print_piece(Node *e)
{
  if(e->kind == E_BIN && strcmp(e->name, "+") == 0 && has_string(e)){
    // Java evaluates left to right; any '+' whose subtree contains a string
    // is string concatenation (good enough for this subset).
    gen_print_piece(e->a);
    gen_print_piece(e->b);
    return;
  }
  Ty t = gen_expr(e);
  switch(t.k){
  case TY_INT: emit("\tbl mj_print_int"); break;
  case TY_BOOL: emit("\tbl mj_print_bool"); break;
  case TY_STR: emit("\tbl mj_print_str"); break;
  case TY_NULL: case TY_CLASS: case TY_INTARR: emit("\tbl mj_print_obj"); break;
  default: die(e->line, "cannot print %s", tyname(t));
  }
}

// run the cleanups (handler unlink, monitorexit) of every enclosing
// try/synchronized context, innermost first -- used by `return`.
static void
gen_unwind_all(void)
{
  for(int i = ctxs.n - 1; i >= 0; i--){
    Ctx *c = ctxs.v[i];
    emit_pop_handler(c->rec);
    if(c->kind == CTX_SYNC){
      emit("\tldr x0, [x29, #%d]          // the locked object", c->objslot);
      emit_monitorexit();
    }
  }
}

static void
gen_cond(Node *e)
{
  Ty t = gen_expr(e);
  if(t.k != TY_BOOL) die(e->line, "condition must be boolean, got %s", tyname(t));
}

static void
gen_sync(Node *s, Node *objexpr)
{
  int objslot = newslot(8), excslot = newslot(8), rec = newslot(32);
  int handler = newlabel(), end = newlabel();
  emit("\t// ===== synchronized (line %d) =====", s->line);
  Ty t = gen_expr(objexpr);
  if(t.k != TY_CLASS && t.k != TY_INTARR) die(s->line, "synchronized needs an object, got %s", tyname(t));
  nullcheck("x0");
  emit("\tstr x0, [x29, #%d]", objslot);
  emit_monitorenter();
  emit_push_handler(rec, handler);
  Ctx *c = xcalloc(sizeof(Ctx));
  c->kind = CTX_SYNC; c->rec = rec; c->objslot = objslot;
  vpush(&ctxs, c);
  gen_stmt(s->b);
  ctxs.n--;
  emit_pop_handler(rec);
  emit("\tldr x0, [x29, #%d]", objslot);
  emit_monitorexit();
  emit("\tb .L%d", end);
  emit(".L%d:                          // exception inside synchronized: unlock + rethrow", handler);
  emit("\tstr x0, [x29, #%d]", excslot);
  emit("\tldr x0, [x29, #%d]", objslot);
  emit_monitorexit();
  emit("\tldr x0, [x29, #%d]", excslot);
  emit("\tbl mj_throw");
  emit(".L%d:", end);
  emit("\t// ===== end synchronized =====");
}

static void
gen_stmt(Node *s)
{
  int vol;
  switch(s->kind){
  case S_BLOCK:
    scope_depth++;
    for(int i = 0; i < s->list.n; i++) gen_stmt(s->list.v[i]);
    popscope();
    return;
  case S_VAR: {
    Ty t = resolve(s->ty, s->line);
    if(t.k == TY_VOID) die(s->line, "void variable");
    Local *l;
    if(s->a){
      Ty it = gen_expr(s->a);
      if(!assignable(t, it)) die(s->line, "cannot initialize %s with %s", tyname(t), tyname(it));
      l = addlocal(s->name, t, s->line);
    } else {
      l = addlocal(s->name, t, s->line);
      emit("\tmov x0, #0");
    }
    emit("\tstr x0, [x29, #%d]          // %s", l->off, s->name);
    return;
  }
  case S_ASSIGN: {
    Ty lt = gen_addr(s->a, &vol);
    push();
    Ty rt = gen_expr(s->b);
    if(!assignable(lt, rt)) die(s->line, "cannot assign %s to %s", tyname(rt), tyname(lt));
    pop("x1");
    if(vol) emit("\tstlr x0, [x1]               // volatile write = store-release");
    else emit("\tstr x0, [x1]");
    return;
  }
  case S_EXPR:
    gen_expr(s->a);
    return;
  case S_IF: {
    int els = newlabel(), end = newlabel();
    gen_cond(s->a);
    emit("\tcbz x0, .L%d", els);
    gen_stmt(s->b);
    emit("\tb .L%d", end);
    emit(".L%d:", els);
    if(s->c) gen_stmt(s->c);
    emit(".L%d:", end);
    return;
  }
  case S_WHILE: {
    int top = newlabel(), end = newlabel();
    emit(".L%d:", top);
    gen_cond(s->a);
    emit("\tcbz x0, .L%d", end);
    gen_stmt(s->b);
    emit("\tb .L%d", top);
    emit(".L%d:", end);
    return;
  }
  case S_FOR: {
    int top = newlabel(), end = newlabel();
    scope_depth++;
    if(s->a) gen_stmt(s->a);
    emit(".L%d:", top);
    if(s->b){ gen_cond(s->b); emit("\tcbz x0, .L%d", end); }
    gen_stmt(s->d);
    if(s->c) gen_stmt(s->c);
    emit("\tb .L%d", top);
    emit(".L%d:", end);
    popscope();
    return;
  }
  case S_RETURN: {
    if(s->a){
      if(curmeth->ret.k == TY_VOID) die(s->line, "void method returns a value");
      Ty t = gen_expr(s->a);
      if(!assignable(curmeth->ret, t)) die(s->line, "return type %s expected, got %s", tyname(curmeth->ret), tyname(t));
      emit("\tstr x0, [x29, #%d]", retslot);
    } else if(curmeth->ret.k != TY_VOID) die(s->line, "missing return value");
    if(ctxs.n) emit("\t// return crosses try/synchronized: run their exits first");
    gen_unwind_all();
    emit("\tb .L%d", retlabel);
    return;
  }
  case S_SYNC:
    gen_sync(s, s->a);
    return;
  case S_TRY: {
    int rec = newslot(32), excslot = newslot(8);
    int handler = newlabel(), end = newlabel();
    emit_push_handler(rec, handler);
    Ctx *c = xcalloc(sizeof(Ctx));
    c->kind = CTX_TRY; c->rec = rec;
    vpush(&ctxs, c);
    gen_stmt(s->a);
    ctxs.n--;
    emit_pop_handler(rec);
    emit("\tb .L%d", end);
    emit(".L%d:                          // catch dispatch", handler);
    emit("\tstr x0, [x29, #%d]", excslot);
    for(int i = 0; i < s->list.n; i++){
      Node *cc = s->list.v[i];
      Ty ct = resolve(cc->ty, cc->line);
      if(ct.k != TY_CLASS || !subclass_of(ct.c, cls_exception))
        die(cc->line, "catch type must be an Exception class");
      int nxt = newlabel();
      char sym[256];
      emit("\tldr x0, [x29, #%d]", excslot);
      snprintf(sym, sizeof sym, "MJC_%s", ct.c->name);
      load_addr("x1", sym);
      emit("\tbl mj_instanceof");
      emit("\tcbz x0, .L%d", nxt);
      scope_depth++;
      Local *l = addlocal(cc->name, ct, cc->line);
      emit("\tldr x0, [x29, #%d]", excslot);
      emit("\tstr x0, [x29, #%d]          // %s", l->off, cc->name);
      gen_stmt(cc->a);
      popscope();
      emit("\tb .L%d", end);
      emit(".L%d:", nxt);
    }
    emit("\tldr x0, [x29, #%d]          // no catch matched: rethrow", excslot);
    emit("\tbl mj_throw");
    emit(".L%d:", end);
    return;
  }
  case S_THROW: {
    Ty t = gen_expr(s->a);
    if(t.k != TY_CLASS || !subclass_of(t.c, cls_exception)) die(s->line, "throw needs an Exception, got %s", tyname(t));
    nullcheck("x0");
    emit("\tbl mj_throw");
    return;
  }
  case S_PRINT:
    if(s->a) gen_print_piece(s->a);
    if(s->newline) emit("\tbl mj_println");
    return;
  }
  die(s->line, "bad statement");
}

static void
gen_method(Method *m, FILE *final)
{
  char *buf = 0;
  size_t len = 0;
  out = open_memstream(&buf, &len);
  curclass = m->owner;
  curmeth = m;
  locals.n = 0;
  ctxs.n = 0;
  scope_depth = 0;
  frame_size = 0;
  retlabel = newlabel();

  int argn = 0;
  if(!m->isstatic){
    Ty t = classty(m->owner);
    Local *l = addlocal("this", t, m->line);   // offset 0
    emit("\tstr x%d, [x29, #%d]          // this", argn++, l->off);
  }
  scope_depth = 1;
  for(int i = 0; i < m->params.n; i++){
    Field *p = m->params.v[i];
    Local *l = addlocal(p->name, p->ty, m->line);
    emit("\tstr x%d, [x29, #%d]          // %s", argn++, l->off, p->name);
  }
  retslot = newslot(8);
  emit("\tstr xzr, [x29, #%d]", retslot);

  if(m->issync){
    // synchronized method == synchronized(this / ClassObject) around the body
    Node fake = {0};
    fake.kind = S_SYNC; fake.line = m->line; fake.b = m->body;
    Node *obj;
    if(m->isstatic){ obj = mk(E_CLASSLIT, m->line); obj->ty = classty(m->owner); }
    else obj = mk(E_THIS, m->line);
    emit("\t// synchronized method: ACC_SYNCHRONIZED -> implicit monitorenter/exit");
    gen_sync(&fake, obj);
  } else {
    gen_stmt(m->body);
  }
  emit(".L%d:", retlabel);
  emit("\tldr x0, [x29, #%d]", retslot);
  fclose(out);

  int frame = (frame_size + 15) & ~15;
  if(frame > 4000) die(m->line, "method frame too large");
  out = final;
  char sym[256];
  if(m->isctor) snprintf(sym, sizeof sym, "MJK_%s", m->owner->name);
  else snprintf(sym, sizeof sym, "MJM_%s_%s", m->owner->name, m->name);
  emit("");
  emit("// %s.%s%s%s", m->owner->name, m->isctor ? m->owner->name : m->name,
       m->isstatic ? " [static]" : "", m->issync ? " [synchronized]" : "");
  emit("\t.text");
  emit("\t.balign 4");
  emit("\t.globl %s", sym);
  emit("\t.type %s, %%function", sym);
  emit("%s:", sym);
  emit("\tstp x29, x30, [sp, #-16]!");
  emit("\tsub sp, sp, #%d", frame);
  emit("\tmov x29, sp                 // locals live at [x29, #0 .. #%d]", frame);
  fwrite(buf, 1, len, out);
  emit("\tadd sp, x29, #%d", frame);
  emit("\tldp x29, x30, [sp], #16");
  emit("\tret");
  emit("\t.size %s, .-%s", sym, sym);
  free(buf);
}

static void
emit_class_data(Class *c)
{
  emit("");
  emit("// class %s extends %s  (instance size %d bytes)", c->name, c->super ? c->super->name : "-", c->size);
  emit("\t.data");
  emit("\t.balign 16");
  emit("\t.globl MJC_%s", c->name);
  emit("MJC_%s:                         // class object (lockable: static synchronized)", c->name);
  emit("\t.quad 0                      // [0]  vtable of java.lang.Class (unused)");
  emit("\t.quad 0                      // [8]  lock word");
  emit("\t.quad .Lcname_%s             // [16] name", c->name);
  if(c->super) emit("\t.quad MJC_%s                 // [24] super", c->super->name);
  else emit("\t.quad 0                      // [24] super");
  emit("\t.quad %d                     // [32] instance size", c->size);
  emit("\t.quad MJV_%s                 // [40] vtable", c->name);
  emit("\t.section .data.rel.ro, \"aw\"");
  emit("\t.balign 8");
  emit("MJV_%s:", c->name);
  emit("\t.quad MJC_%s", c->name);
  for(int i = 0; i < c->vtable.n; i++){
    Method *m = c->vtable.v[i];
    emit("\t.quad MJM_%s_%s           // slot %d", m->owner->name, m->name, i + 1);
  }
  emit("\t.section .rodata");
  emit(".Lcname_%s: .asciz \"%s\"", c->name, c->name);
  for(int i = 0; i < c->fields.n; i++){
    Field *f = c->fields.v[i];
    if(!f->isstatic) continue;
    emit("\t.data");
    emit("\t.balign 8");
    emit("MJS_%s_%s: .quad 0             // static%s %s", c->name, f->name, f->isvolatile ? " volatile" : "", tyname(f->ty));
  }
}

static char *
readfile(const char *path)
{
  FILE *f = fopen(path, "rb");
  if(!f){ perror(path); exit(1); }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *s = xcalloc(n + 1);
  if(fread(s, 1, n, f) != (size_t)n){ perror(path); exit(1); }
  fclose(f);
  return s;
}

int
main(int argc, char **argv)
{
  if(argc != 2){
    fprintf(stderr, "usage: mjc file.java > file.S\n");
    return 2;
  }
  srcname = argv[1];
  lex(readfile(argv[1]));
  add_builtins();
  while(peek()->kind != T_EOF){
    while(accept("public") || accept("final")) ;
    parse_class();
  }
  for(int i = 0; i < classes.n; i++) layout(classes.v[i]);
  Class *thread = cls_thread;
  if(((Method *)thread->vtable.v[0])->slot != 1) die(0, "internal: Thread.run must be slot 1");

  FILE *final = stdout;
  out = final;
  emit("// generated by mjc from %s -- MiniJava -> AArch64 (xv6)", srcname);
  emit("// object layout: [0] vtable  [8] lock word  [16..] fields");
  emit("// x28 = current MJThread (id at [x28], exception-handler chain at [x28,#8])");

  Method *mainm = 0;
  for(int i = 0; i < classes.n; i++){
    Class *c = classes.v[i];
    if(c->builtin && c != cls_thread) continue;
    for(int j = 0; j < c->methods.n; j++){
      Method *m = c->methods.v[j];
      if(strcmp(m->name, "main") == 0 && m->isstatic){
        if(mainm) die(m->line, "more than one main()");
        mainm = m;
      }
      gen_method(m, final);
      out = final;
    }
    if(c->ctor){ gen_method(c->ctor, final); out = final; }
  }
  if(!mainm) die(0, "no static main() found");
  emit("");
  emit("\t.text");
  emit("\t.globl mjmain");
  emit("mjmain:");
  emit("\tb MJM_%s_main", mainm->owner->name);
  for(int i = 0; i < classes.n; i++) emit_class_data(classes.v[i]);
  emit("\t.section .rodata");
  for(int i = 0; i < strlits.n; i++) emit(".Lstr%d: .asciz \"%s\"", i, (char *)strlits.v[i]);
  emit("\t.section .note.GNU-stack,\"\",%%progbits");
  return 0;
}
