// mjavac -- a small javac: MiniJava-subset source -> standard .class files.
//
//   mjavac [-d outdir] A.java B.java ...
//
// Pipeline: lexer -> recursive-descent parser -> AST -> type-checking
// bytecode generator -> class file writer (constant pool, fields, methods,
// Code attributes with exception tables).  The output is version 49.0
// (Java 5) so no StackMapTable is needed; HotSpot (`java`) and mjvm both
// load it, and `javap -c -v` can disassemble it.
//
// What it does the way javac does:
//   * synchronized (x) { ... }  ->  dup; astore t; monitorenter
//                                   ...; aload t; monitorexit; goto end
//                                   handler(any): astore e; aload t; monitorexit; aload e; athrow
//     with exception-table entries [body) -> handler and [handler) -> handler
//   * synchronized methods      ->  only the ACC_SYNCHRONIZED flag; the VM locks
//   * "a" + x                   ->  new StringBuilder().append(..)...toString()
//   * constructors              ->  super(...) first, then field initializers
//   * x++ on an int local       ->  iinc
//
// Subset: classes with single inheritance, fields (int, boolean, String,
// references, arrays; static; volatile; with initializers), methods
// (static or instance, no overloading), constructors (one per class,
// optional super(...)), if/else, while, for, return, try/catch, throw,
// synchronized, assignments incl. += -= ++ --, int arithmetic, comparisons,
// && || !, String concatenation, arrays (new T[n], a[i], a.length),
// System.out.print/println, and the built-in library classes that mjvm
// implements (Object, String, Thread, Throwable and the common exceptions).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

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
  return strcpy(xcalloc(strlen(s) + 1), s);
}

static char *
fmt(const char *f, ...)
{
  char buf[1024];
  va_list ap;
  va_start(ap, f);
  vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return xstrdup(buf);
}

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

// ================================================================ lexer

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
  tpos = 0;
  for(;;){
    if(ntoks + 1 >= cap){ cap *= 2; toks = realloc(toks, cap * sizeof(Token)); }
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
    if(isalpha((unsigned char)*s) || *s == '_' || *s == '$'){
      const char *b = s;
      while(isalnum((unsigned char)*s) || *s == '_' || *s == '$') s++;
      t->kind = T_ID; t->text = strndup(b, s - b);
    } else if(isdigit((unsigned char)*s)){
      const char *b = s;
      t->num = strtol(s, (char **)&s, 10);
      if(*s == 'L' || *s == 'l') s++;
      t->kind = T_NUM; t->text = strndup(b, s - b);
    } else if(*s == '"'){
      // decode escapes into raw bytes; the class writer re-encodes as modified UTF-8
      s++;
      char *buf = xcalloc(strlen(s) + 1);
      int n = 0;
      while(*s && *s != '"'){
        if(*s == '\\' && s[1]){
          s++;
          switch(*s){
          case 'n': buf[n++] = '\n'; break;
          case 't': buf[n++] = '\t'; break;
          case '0': buf[n++] = 0; break;
          default: buf[n++] = *s; break;
          }
          s++;
        } else buf[n++] = *s++;
      }
      if(!*s) die(line, "unterminated string");
      s++;
      t->kind = T_STR; t->text = buf; t->num = n;
    } else if(*s == '\''){
      s++;
      long v = *s;
      if(*s == '\\'){ s++; v = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s == '0' ? 0 : *s; }
      s++;
      if(*s != '\'') die(line, "bad char literal");
      s++;
      t->kind = T_NUM; t->num = v; t->text = "<char>";
    } else {
      static const char *ops2[] = { "==", "!=", "<=", ">=", "&&", "||", "++", "--", "+=", "-=", "*=", "/=", 0 };
      t->kind = T_OP;
      t->text = 0;
      for(int i = 0; ops2[i]; i++)
        if(strncmp(s, ops2[i], 2) == 0){ t->text = xstrdup(ops2[i]); s += 2; break; }
      if(!t->text){
        if(!strchr("{}()[];,.=<>+-*/%!?:", *s)) die(line, "unexpected character '%c'", *s);
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
static int isn(int n, const char *s) { return peekn(n)->kind != T_STR && strcmp(peekn(n)->text, s) == 0; }
static int is(const char *s) { return isn(0, s); }
static int accept(const char *s) { if(is(s)){ next(); return 1; } return 0; }
static void expect(const char *s) { if(!accept(s)) die(peek()->line, "expected '%s' but found '%s'", s, peek()->text); }
static char *
ident(void)
{
  Token *t = next();
  if(t->kind != T_ID) die(t->line, "expected identifier but found '%s'", t->text);
  return t->text;
}

// ================================================================ types & AST

enum { TY_VOID, TY_INT, TY_BOOL, TY_LONG, TY_NULL, TY_REF, TY_ARRAY };
struct Class;
typedef struct Ty { int k; struct Class *c; char *cname; struct Ty *elem; } Ty;

enum {
  E_NUM, E_BOOL, E_STR, E_NULL, E_THIS, E_NAME, E_FIELD, E_INDEX, E_CALL, E_NEW,
  E_NEWARR, E_BIN, E_NOT, E_NEG, E_COND,
  S_BLOCK, S_VAR, S_ASSIGN, S_EXPR, S_IF, S_WHILE, S_FOR, S_RETURN, S_SYNC, S_TRY,
  S_THROW, S_PRINT, S_SUPER, S_BREAK, S_CONTINUE,
};

typedef struct Node {
  int kind, line;
  char *name;
  long num;
  int slen;                      // E_STR byte length
  Ty ty;                         // S_VAR / catch type / E_NEWARR element
  struct Node *a, *b, *c, *d;
  Vec list;
  int newline;
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
  Node *init;
  struct Class *owner;
} Field;

typedef struct Method {
  char *name; Ty ret; int isstatic, issync, isctor, builtin;
  Vec params;                    // Field*
  Node *body;
  struct Class *owner;
  int line;
} Method;

typedef struct Class {
  char *name;                    // source name ("Thread")
  char *jname;                   // internal name ("java/lang/Thread")
  char *supername;
  struct Class *super;
  Vec fields, methods, ctors;
  int builtin, laid_out, line;
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

// ================================================================ parser

static Ty
parse_type(void)
{
  Ty t = {0};
  char *n = ident();
  if(!strcmp(n, "int") || !strcmp(n, "char") || !strcmp(n, "short") || !strcmp(n, "byte")) t.k = TY_INT;
  else if(!strcmp(n, "boolean")) t.k = TY_BOOL;
  else if(!strcmp(n, "void")) t.k = TY_VOID;
  else if(!strcmp(n, "long")) t.k = TY_LONG;
  else { t.k = TY_REF; t.cname = n; }
  while(is("[") && isn(1, "]")){
    next(); next();
    Ty *e = xcalloc(sizeof(Ty));
    *e = t;
    memset(&t, 0, sizeof t);
    t.k = TY_ARRAY;
    t.elem = e;
  }
  return t;
}

static int
looks_like_decl(void)
{
  if(peekn(0)->kind != T_ID) return 0;
  if(peekn(1)->kind == T_ID) return 1;
  int i = 1;
  while(isn(i, "[") && isn(i + 1, "]")) i += 2;
  return i > 1 && peekn(i)->kind == T_ID;
}

static Node *parse_expr(void);
static Node *parse_stmt(void);
static Node *parse_block(void);

static void
parse_args(Node *call)
{
  expect("(");
  if(!is(")")) do vpush(&call->list, parse_expr()); while(accept(","));
  expect(")");
}

static Node *
parse_primary(void)
{
  Token *t = peek();
  Node *n;
  if(t->kind == T_NUM){ next(); n = mk(E_NUM, t->line); n->num = t->num; }
  else if(t->kind == T_STR){ next(); n = mk(E_STR, t->line); n->name = t->text; n->slen = (int)t->num; }
  else if(accept("(")){ n = parse_expr(); expect(")"); }
  else if(accept("true")){ n = mk(E_BOOL, t->line); n->num = 1; }
  else if(accept("false")){ n = mk(E_BOOL, t->line); n->num = 0; }
  else if(accept("null")) n = mk(E_NULL, t->line);
  else if(accept("this")) n = mk(E_THIS, t->line);
  else if(accept("new")){
    char *cn = ident();
    if(is("[")){
      next();
      n = mk(E_NEWARR, t->line);
      Ty et = {0};
      if(!strcmp(cn, "int")) et.k = TY_INT;
      else if(!strcmp(cn, "boolean")) et.k = TY_BOOL;
      else { et.k = TY_REF; et.cname = cn; }
      n->ty = et;
      n->a = parse_expr();
      expect("]");
    } else {
      n = mk(E_NEW, t->line);
      n->name = cn;
      parse_args(n);
    }
  } else if(t->kind == T_ID){
    next();
    if(is("(")){ n = mk(E_CALL, t->line); n->name = t->text; parse_args(n); }
    else { n = mk(E_NAME, t->line); n->name = t->text; }
  } else die(t->line, "unexpected '%s' in expression", t->text);

  for(;;){
    Token *p = peek();
    if(accept(".")){
      char *m = ident();
      if(is("(")){ Node *c = mk(E_CALL, p->line); c->name = m; c->a = n; parse_args(c); n = c; }
      else { Node *f = mk(E_FIELD, p->line); f->name = m; f->a = n; n = f; }
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
    {"||", 1}, {"&&", 2}, {"==", 3}, {"!=", 3}, {"<", 4}, {"<=", 4}, {">", 4}, {">=", 4},
    {"+", 5}, {"-", 5}, {"*", 6}, {"/", 6}, {"%", 6}, {0, 0}};
  for(int i = 0; tab[i].op; i++) if(!strcmp(op, tab[i].op)) return tab[i].p;
  return 0;
}

static Node *
parse_binary(int minp)
{
  Node *l = parse_unary();
  for(;;){
    Token *t = peek();
    int p = t->kind == T_OP ? prec(t->text) : 0;
    if(!p || p < minp) return l;
    next();
    Node *n = mk(E_BIN, t->line);
    n->name = t->text; n->a = l; n->b = parse_binary(p + 1);
    l = n;
  }
}

static Node *
parse_expr(void)
{
  Node *c = parse_binary(1);
  if(is("?")){
    Token *t = next();
    Node *n = mk(E_COND, t->line);
    n->a = c;
    n->b = parse_expr();
    expect(":");
    n->c = parse_expr();
    return n;
  }
  return c;
}

static Node *
parse_block(void)
{
  Node *b = mk(S_BLOCK, peek()->line);
  expect("{");
  while(!accept("}")){
    if(peek()->kind == T_EOF) die(peek()->line, "unexpected end of file");
    vpush(&b->list, parse_stmt());
  }
  return b;
}

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
  if(is("+=") || is("-=") || is("*=") || is("/=") || is("++") || is("--")){
    char *op = next()->text;
    Node *rhs;
    if(op[1] == '=') rhs = parse_expr();
    else { rhs = mk(E_NUM, t->line); rhs->num = 1; }
    Node *bin = mk(E_BIN, t->line);
    bin->name = op[0] == '+' ? "+" : op[0] == '-' ? "-" : op[0] == '*' ? "*" : "/";
    bin->a = e; bin->b = rhs;
    Node *n = mk(S_ASSIGN, t->line);
    n->a = e; n->b = bin;
    n->num = op[1] != '=';     // ++/--: candidate for iinc
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
  if(accept("break")){ expect(";"); return mk(S_BREAK, t->line); }
  if(accept("continue")){ expect(";"); return mk(S_CONTINUE, t->line); }
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
    if(is("finally")) die(peek()->line, "finally is not supported");
    if(!n->list.n) die(t->line, "try without catch");
    return n;
  }
  if(accept("throw")){
    n = mk(S_THROW, t->line);
    n->a = parse_expr();
    expect(";");
    return n;
  }
  if(is("super") && isn(1, "(")){
    next();
    n = mk(S_SUPER, t->line);
    parse_args(n);
    expect(";");
    return n;
  }
  if(is("System") && isn(1, ".") && isn(2, "out") && isn(3, ".")){
    tpos += 4;
    char *m = ident();
    n = mk(S_PRINT, t->line);
    if(!strcmp(m, "println")) n->newline = 1;
    else if(strcmp(m, "print")) die(t->line, "unknown System.out.%s", m);
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
skip_throws(void)
{
  if(accept("throws")) do ident(); while(accept(","));
}

static void
parse_params(Method *m)
{
  expect("(");
  if(!is(")")) do {
    accept("final");
    Field *p = xcalloc(sizeof(Field));
    p->ty = parse_type();
    p->name = ident();
    vpush(&m->params, p);
  } while(accept(","));
  expect(")");
}

static void
parse_class(void)
{
  Class *c = xcalloc(sizeof(Class));
  c->line = peek()->line;
  expect("class");
  c->name = ident();
  c->jname = c->name;
  if(findclass(c->name)) die(c->line, "duplicate class %s", c->name);
  c->supername = accept("extends") ? ident() : "Object";
  if(is("implements")) die(peek()->line, "interfaces are not supported by mjavac (mjvm runs javac output that uses Runnable)");
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
    if(t->kind == T_ID && !strcmp(t->text, c->name) && isn(1, "(")){
      next();
      Method *m = xcalloc(sizeof(Method));
      m->name = "<init>"; m->isctor = 1; m->owner = c; m->line = t->line;
      m->ret.k = TY_VOID;
      parse_params(m);
      skip_throws();
      m->body = parse_block();
      if(c->ctors.n) die(t->line, "only one constructor per class is supported");
      vpush(&c->ctors, m);
      continue;
    }
    Ty ty = parse_type();
    char *name = ident();
    if(is("(")){
      Method *m = xcalloc(sizeof(Method));
      m->name = name; m->ret = ty; m->isstatic = isstatic; m->issync = issync;
      m->owner = c; m->line = t->line;
      parse_params(m);
      skip_throws();
      m->body = parse_block();
      vpush(&c->methods, m);
    } else {
      for(;;){
        Field *f = xcalloc(sizeof(Field));
        f->name = name; f->ty = ty; f->isstatic = isstatic; f->isvolatile = isvol; f->owner = c;
        if(accept("=")) f->init = parse_expr();
        vpush(&c->fields, f);
        if(!accept(",")) break;
        name = ident();
      }
      expect(";");
    }
  }
}

// ================================================================ built-in library model

static Class *cls_object, *cls_string, *cls_thread, *cls_throwable;

static Ty mkty(int k) { Ty t = {0}; t.k = k; return t; }
static Ty refty(Class *c) { Ty t = {0}; t.k = TY_REF; t.c = c; t.cname = c->name; return t; }

static Class *
builtin(const char *name, const char *jname, const char *super)
{
  Class *c = xcalloc(sizeof(Class));
  c->name = xstrdup(name);
  c->jname = xstrdup(jname);
  c->supername = super ? xstrdup(super) : 0;
  c->builtin = 1;
  vpush(&classes, c);
  return c;
}

// sig: "name(params)ret" using I Z J S(String) O(Object) T(Thread) V
static Ty
sigty(char ch)
{
  switch(ch){
  case 'I': return mkty(TY_INT);
  case 'Z': return mkty(TY_BOOL);
  case 'J': return mkty(TY_LONG);
  case 'V': return mkty(TY_VOID);
  case 'S': return refty(cls_string);
  case 'O': return refty(cls_object);
  case 'T': return refty(cls_thread);
  }
  fprintf(stderr, "bad builtin sig\n");
  exit(1);
}

static void
bmethod(Class *c, const char *sig, int isstatic)
{
  Method *m = xcalloc(sizeof(Method));
  const char *p = strchr(sig, '(');
  m->name = strndup(sig, p - sig);
  m->owner = c;
  m->builtin = 1;
  m->isstatic = isstatic;
  for(p++; *p != ')'; p++){
    Field *f = xcalloc(sizeof(Field));
    f->ty = sigty(*p);
    vpush(&m->params, f);
  }
  m->ret = sigty(p[1]);
  if(!strcmp(m->name, "<init>")){ m->isctor = 1; vpush(&c->ctors, m); }
  else vpush(&c->methods, m);
}

static void
add_builtins(void)
{
  cls_object = builtin("Object", "java/lang/Object", 0);
  cls_string = builtin("String", "java/lang/String", "Object");
  cls_thread = builtin("Thread", "java/lang/Thread", "Object");
  bmethod(cls_object, "<init>()V", 0);
  bmethod(cls_object, "wait()V", 0);
  bmethod(cls_object, "notify()V", 0);
  bmethod(cls_object, "notifyAll()V", 0);
  bmethod(cls_object, "toString()S", 0);
  bmethod(cls_object, "hashCode()I", 0);
  bmethod(cls_object, "equals(O)Z", 0);
  bmethod(cls_string, "length()I", 0);
  bmethod(cls_string, "charAt(I)I", 0);    // char, treated as int (see desc_of)
  bmethod(cls_thread, "<init>()V", 0);
  bmethod(cls_thread, "<init>(S)V", 0);
  bmethod(cls_thread, "start()V", 0);
  bmethod(cls_thread, "run()V", 0);
  bmethod(cls_thread, "join()V", 0);
  bmethod(cls_thread, "isAlive()Z", 0);
  bmethod(cls_thread, "getName()S", 0);
  bmethod(cls_thread, "setName(S)V", 0);
  bmethod(cls_thread, "currentThread()T", 1);
  bmethod(cls_thread, "yield()V", 1);
  bmethod(cls_thread, "sleep(J)V", 1);
  Class *math = builtin("Math", "java/lang/Math", "Object");
  bmethod(math, "abs(I)I", 1);
  bmethod(math, "max(II)I", 1);
  bmethod(math, "min(II)I", 1);
  Class *sys = builtin("System", "java/lang/System", "Object");
  bmethod(sys, "currentTimeMillis()J", 1);
  cls_throwable = builtin("Throwable", "java/lang/Throwable", "Object");
  bmethod(cls_throwable, "getMessage()S", 0);
  bmethod(cls_throwable, "printStackTrace()V", 0);
  static const char *exc[][2] = {
    {"Throwable", 0},
    {"Exception", "Throwable"}, {"Error", "Throwable"},
    {"RuntimeException", "Exception"}, {"InterruptedException", "Exception"},
    {"IllegalMonitorStateException", "RuntimeException"},
    {"IllegalStateException", "RuntimeException"},
    {"IllegalArgumentException", "RuntimeException"},
    {"IllegalThreadStateException", "IllegalArgumentException"},
    {"ArithmeticException", "RuntimeException"},
    {"NullPointerException", "RuntimeException"},
    {"ClassCastException", "RuntimeException"},
    {"UnsupportedOperationException", "RuntimeException"},
    {"IndexOutOfBoundsException", "RuntimeException"},
    {"ArrayIndexOutOfBoundsException", "IndexOutOfBoundsException"},
    {0, 0}};
  for(int i = 0; exc[i][0]; i++){
    Class *c = i == 0 ? cls_throwable : builtin(exc[i][0], fmt("java/lang/%s", exc[i][0]), exc[i][1]);
    bmethod(c, "<init>()V", 0);
    bmethod(c, "<init>(S)V", 0);
  }
}

// ================================================================ resolution

static Ty
resolve(Ty t, int line)
{
  if(t.k == TY_REF && !t.c){
    t.c = findclass(t.cname);
    if(!t.c) die(line, "unknown class %s", t.cname);
  }
  if(t.k == TY_ARRAY){
    Ty *e = xcalloc(sizeof(Ty));
    *e = resolve(*t.elem, line);
    t.elem = e;
  }
  return t;
}

static Method *
findmethod(Class *c, const char *name)
{
  for(; c; c = c->super)
    for(int i = 0; i < c->methods.n; i++){
      Method *m = c->methods.v[i];
      if(!strcmp(m->name, name)) return m;
    }
  return 0;
}

static Field *
findfield(Class *c, const char *name)
{
  for(; c; c = c->super)
    for(int i = 0; i < c->fields.n; i++){
      Field *f = c->fields.v[i];
      if(!strcmp(f->name, name)) return f;
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
  }
  for(int i = 0; i < c->fields.n; i++){
    Field *f = c->fields.v[i];
    f->ty = resolve(f->ty, c->line);
  }
  Vec *lists[2] = { &c->methods, &c->ctors };
  for(int l = 0; l < 2; l++)
    for(int i = 0; i < lists[l]->n; i++){
      Method *m = lists[l]->v[i];
      m->ret = resolve(m->ret, m->line);
      for(int j = 0; j < m->params.n; j++){
        Field *p = m->params.v[j];
        p->ty = resolve(p->ty, m->line);
      }
      if(!c->builtin && l == 0)
        for(int j = 0; j < i; j++)
          if(!strcmp(((Method *)c->methods.v[j])->name, m->name))
            die(m->line, "overloading is not supported (%s.%s)", c->name, m->name);
    }
  c->laid_out = 2;
}

// descriptors
static char *
desc_of(Ty t)
{
  switch(t.k){
  case TY_VOID: return "V";
  case TY_INT: return "I";
  case TY_BOOL: return "Z";
  case TY_LONG: return "J";
  case TY_REF: return fmt("L%s;", t.c->jname);
  case TY_ARRAY: return fmt("[%s", desc_of(*t.elem));
  }
  return "?";
}

static char *
mdesc(Method *m)
{
  char buf[1024] = "(";
  for(int i = 0; i < m->params.n; i++) strcat(buf, desc_of(((Field *)m->params.v[i])->ty));
  strcat(buf, ")");
  if(m->builtin && !strcmp(m->name, "charAt")) strcat(buf, "C");
  else strcat(buf, desc_of(m->ret));
  return xstrdup(buf);
}

static const char *
tyname(Ty t)
{
  switch(t.k){
  case TY_VOID: return "void";
  case TY_INT: return "int";
  case TY_BOOL: return "boolean";
  case TY_LONG: return "long";
  case TY_NULL: return "null";
  case TY_REF: return t.c ? t.c->name : t.cname;
  case TY_ARRAY: return fmt("%s[]", tyname(*t.elem));
  }
  return "?";
}

static int
isref(Ty t)
{
  return t.k == TY_REF || t.k == TY_ARRAY || t.k == TY_NULL;
}

static int
same(Ty a, Ty b)
{
  if(a.k != b.k) return 0;
  if(a.k == TY_REF) return a.c == b.c;
  if(a.k == TY_ARRAY) return same(*a.elem, *b.elem);
  return 1;
}

static int
assignable(Ty to, Ty from)
{
  if(to.k == TY_REF){
    if(from.k == TY_NULL) return 1;
    if(to.c == cls_object && isref(from)) return 1;
    return from.k == TY_REF && subclass_of(from.c, to.c);
  }
  if(to.k == TY_ARRAY) return from.k == TY_NULL || same(to, from);
  if(to.k == TY_LONG) return from.k == TY_INT || from.k == TY_LONG;
  return to.k == from.k;
}

static int isstring(Ty t) { return t.k == TY_REF && t.c == cls_string; }

// ================================================================ constant pool & class writer

typedef struct { unsigned char *b; int n, cap; } Buf;
static void
bput(Buf *b, int v)
{
  if(b->n == b->cap){ b->cap = b->cap ? b->cap * 2 : 256; b->b = realloc(b->b, b->cap); }
  b->b[b->n++] = (unsigned char)v;
}
static void bput2(Buf *b, int v) { bput(b, v >> 8); bput(b, v); }
static void bput4(Buf *b, long v) { bput2(b, (int)(v >> 16)); bput2(b, (int)v); }
static void bputn(Buf *b, const void *p, int n) { for(int i = 0; i < n; i++) bput(b, ((unsigned char *)p)[i]); }

typedef struct { int tag; char *s; int slen; int a, b; long v; } CPE;
static CPE cpool[65536];
static int ncp;

static int
cp_find_or_add(CPE e)
{
  for(int i = 1; i < ncp; i++){
    CPE *x = &cpool[i];
    if(x->tag != e.tag) continue;
    if(e.tag == 1){ if(x->slen == e.slen && !memcmp(x->s, e.s, e.slen)) return i; }
    else if(x->a == e.a && x->b == e.b && x->v == e.v) return i;
  }
  if(ncp >= 65535){ fprintf(stderr, "constant pool overflow\n"); exit(1); }
  cpool[ncp] = e;
  return ncp++;
}
static int cp_utf8n(const char *s, int n) { CPE e = {1}; e.s = (char *)s; e.slen = n; return cp_find_or_add(e); }
static int cp_utf8(const char *s) { return cp_utf8n(s, strlen(s)); }
static int cp_class(const char *jn) { CPE e = {7}; e.a = cp_utf8(jn); return cp_find_or_add(e); }
static int cp_string(const char *s, int n) { CPE e = {8}; e.a = cp_utf8n(s, n); return cp_find_or_add(e); }
static int cp_int(long v) { CPE e = {3}; e.v = v; return cp_find_or_add(e); }
static int cp_nat(const char *n, const char *d) { CPE e = {12}; e.a = cp_utf8(n); e.b = cp_utf8(d); return cp_find_or_add(e); }
static int cp_field(const char *c, const char *n, const char *d) { CPE e = {9}; e.a = cp_class(c); e.b = cp_nat(n, d); return cp_find_or_add(e); }
static int cp_method(const char *c, const char *n, const char *d) { CPE e = {10}; e.a = cp_class(c); e.b = cp_nat(n, d); return cp_find_or_add(e); }

static void
put_utf8(Buf *o, const char *s, int n)
{
  // modified UTF-8: NUL is 0xC0 0x80; bytes >= 0x80 are passed through
  Buf t = {0};
  for(int i = 0; i < n; i++){
    if(s[i] == 0){ bput(&t, 0xC0); bput(&t, 0x80); }
    else bput(&t, s[i]);
  }
  bput2(o, t.n);
  bputn(o, t.b, t.n);
  free(t.b);
}

// ================================================================ bytecode generation

static Buf code;
static int depth, maxdepth, maxlocals, nextlocal;
static int dry;                    // >0: type inference only, emit nothing

typedef struct { int start, end, handler, type; } XE;
static XE xtab[1024];
static int nxtab;

static int labelpos[100000];
static int nlabels;
typedef struct { int at, insn, label, wide; } Fix;
static Fix fixes[100000];
static int nfixes;

static int newlabel(void) { labelpos[nlabels] = -1; return nlabels++; }
static void place(int l) { if(!dry) labelpos[l] = code.n; }

static void
adj(int d)
{
  if(dry) return;
  depth += d;
  if(depth < 0){ fprintf(stderr, "internal: stack underflow\n"); exit(1); }
  if(depth > maxdepth) maxdepth = depth;
}

static void op(int o, int d) { if(dry) return; bput(&code, o); adj(d); }
static void op1(int o, int a, int d) { if(dry) return; bput(&code, o); bput(&code, a); adj(d); }
static void op2(int o, int a, int d) { if(dry) return; bput(&code, o); bput2(&code, a); adj(d); }

static void
branch(int o, int label, int d)
{
  if(dry) return;
  fixes[nfixes].insn = code.n;
  bput(&code, o);
  fixes[nfixes].at = code.n;
  fixes[nfixes].label = label;
  nfixes++;
  bput2(&code, 0);
  adj(d);
}

static void
iconst(long v)
{
  if(v >= -1 && v <= 5) op(0x03 + v, 1);
  else if(v >= -128 && v <= 127) op1(0x10, v & 0xff, 1);
  else if(v >= -32768 && v <= 32767) op2(0x11, v & 0xffff, 1);
  else { int i = cp_int(v); if(i < 256) op1(0x12, i, 1); else op2(0x13, i, 1); }
}

static void
ldc_string(const char *s, int n)
{
  if(dry) return;
  int i = cp_string(s, n);
  if(i < 256) op1(0x12, i, 1); else op2(0x13, i, 1);
}

static void
load_local(Ty t, int slot)
{
  int base = isref(t) ? 0x2a : t.k == TY_LONG ? 0x1e : 0x1a;
  int wop = isref(t) ? 0x19 : t.k == TY_LONG ? 0x16 : 0x15;
  int d = t.k == TY_LONG ? 2 : 1;
  if(slot < 4) op(base + slot, d); else op1(wop, slot, d);
}

static void
store_local(Ty t, int slot)
{
  int base = isref(t) ? 0x4b : t.k == TY_LONG ? 0x3f : 0x3b;
  int wop = isref(t) ? 0x3a : t.k == TY_LONG ? 0x37 : 0x36;
  int d = t.k == TY_LONG ? -2 : -1;
  if(slot < 4) op(base + slot, d); else op1(wop, slot, d);
}

static int
slots(Ty t)
{
  return t.k == TY_LONG ? 2 : t.k == TY_VOID ? 0 : 1;
}

static int
alloc_local(Ty t)
{
  int s = nextlocal;
  nextlocal += slots(t) ? slots(t) : 1;
  if(nextlocal > maxlocals) maxlocals = nextlocal;
  return s;
}

typedef struct Local { char *name; Ty ty; int slot; int depth; } Local;
enum { CTX_LOOP, CTX_SYNC, CTX_TRY };
typedef struct Ctx { int kind; int slot; int brk, cont; } Ctx;

static Class *curclass;
static Method *curmeth;
static Vec locals, ctxs;
static int scope;

static Local *
findlocal(const char *name)
{
  for(int i = locals.n - 1; i >= 0; i--){
    Local *l = locals.v[i];
    if(!strcmp(l->name, name)) return l;
  }
  return 0;
}

static Local *
addlocal(const char *name, Ty t, int line)
{
  Local *l = findlocal(name);
  if(l && l->depth == scope) die(line, "variable %s is already defined", name);
  l = xcalloc(sizeof(Local));
  l->name = (char *)name; l->ty = t; l->slot = alloc_local(t); l->depth = scope;
  vpush(&locals, l);
  return l;
}

static void
popscope(void)
{
  while(locals.n && ((Local *)locals.v[locals.n - 1])->depth == scope) locals.n--;
  scope--;
}

static int
is_classname(Node *e)
{
  return e && e->kind == E_NAME && !findlocal(e->name) && !findfield(curclass, e->name) && findclass(e->name);
}

static Ty gen_expr(Node *e);
static void gen_stmt(Node *s);
static void gen_cond(Node *e, int falselabel);

static Ty
typeof_expr(Node *e)
{
  dry++;
  Ty t = gen_expr(e);
  dry--;
  return t;
}

// ---- string concatenation via StringBuilder (javac <= 8 style)
static void
concat_parts(Node *e, Vec *parts)
{
  if(e->kind == E_BIN && !strcmp(e->name, "+") && isstring(typeof_expr(e))){
    concat_parts(e->a, parts);
    concat_parts(e->b, parts);
  } else vpush(parts, e);
}

static const char *
append_desc(Ty t)
{
  if(isstring(t)) return "(Ljava/lang/String;)Ljava/lang/StringBuilder;";
  if(t.k == TY_INT) return "(I)Ljava/lang/StringBuilder;";
  if(t.k == TY_BOOL) return "(Z)Ljava/lang/StringBuilder;";
  if(t.k == TY_LONG) return "(J)Ljava/lang/StringBuilder;";
  return "(Ljava/lang/Object;)Ljava/lang/StringBuilder;";
}

static void
gen_concat(Node *e)
{
  Vec parts = {0};
  concat_parts(e, &parts);
  op2(0xbb, dry ? 0 : cp_class("java/lang/StringBuilder"), 1);            // new
  op(0x59, 1);                                                             // dup
  op2(0xb7, dry ? 0 : cp_method("java/lang/StringBuilder", "<init>", "()V"), -1);
  for(int i = 0; i < parts.n; i++){
    Ty t = gen_expr(parts.v[i]);
    if(t.k == TY_VOID) die(((Node *)parts.v[i])->line, "cannot concatenate void");
    op2(0xb6, dry ? 0 : cp_method("java/lang/StringBuilder", "append", append_desc(t)), -slots(t));
  }
  op2(0xb6, dry ? 0 : cp_method("java/lang/StringBuilder", "toString", "()Ljava/lang/String;"), 0);
  free(parts.v);
}

// ---- method calls
static Ty
emit_invoke(Method *m, int kind)
{
  int argsl = 0;
  for(int i = 0; i < m->params.n; i++) argsl += slots(((Field *)m->params.v[i])->ty);
  int d = -argsl + slots(m->ret) - (kind == 0xb8 ? 0 : 1);
  if(m->isctor) d = -argsl - 1;
  op2(kind, dry ? 0 : cp_method(m->owner->jname, m->name, mdesc(m)), d);
  return m->isctor ? mkty(TY_VOID) : m->ret;
}

static void
gen_args(Method *m, Vec *args, int line)
{
  if(m->params.n != args->n) die(line, "%s expects %d argument(s), got %d", m->isctor ? m->owner->name : m->name, m->params.n, args->n);
  for(int i = 0; i < args->n; i++){
    Field *p = m->params.v[i];
    Ty t = gen_expr(args->v[i]);
    if(!assignable(p->ty, t)) die(line, "argument %d: %s expected, got %s", i + 1, tyname(p->ty), tyname(t));
    if(p->ty.k == TY_LONG && t.k == TY_INT) op(0x85, 1);                   // i2l
  }
}

// constructor choice for built-ins: by arity / String argument
static Method *
pick_ctor(Class *c, Vec *args, int line)
{
  Method *best = 0;
  for(int i = 0; i < c->ctors.n; i++){
    Method *m = c->ctors.v[i];
    if(m->params.n != args->n) continue;
    int ok = 1;
    for(int j = 0; j < args->n && ok; j++)
      ok = assignable(((Field *)m->params.v[j])->ty, typeof_expr(args->v[j]));
    if(ok){ best = m; break; }
  }
  if(!best){
    if(!c->ctors.n && !args->n) return 0;   // implicit default constructor
    die(line, "no constructor %s(...) with %d argument(s)", c->name, args->n);
  }
  return best;
}

static Ty
gen_call(Node *e)
{
  Method *m;
  if(e->a && is_classname(e->a)){
    Class *c = findclass(e->a->name);
    m = findmethod(c, e->name);
    if(!m || !m->isstatic) die(e->line, "no static method %s.%s", c->name, e->name);
    gen_args(m, &e->list, e->line);
    return emit_invoke(m, 0xb8);
  }
  Class *target;
  if(e->a){
    Ty rt = gen_expr(e->a);
    if(rt.k == TY_ARRAY || rt.k == TY_NULL) rt = refty(cls_object);
    if(rt.k != TY_REF) die(e->line, "method call on %s", tyname(rt));
    target = rt.c;
    m = findmethod(target, e->name);
    if(!m) die(e->line, "class %s has no method %s", target->name, e->name);
    if(m->isstatic) die(e->line, "static method %s called through an instance", e->name);
  } else {
    m = findmethod(curclass, e->name);
    if(!m) die(e->line, "unknown method %s", e->name);
    if(m->isstatic){
      gen_args(m, &e->list, e->line);
      return emit_invoke(m, 0xb8);
    }
    if(curmeth->isstatic) die(e->line, "instance method %s called from a static context", e->name);
    op(0x2a, 1);                                                           // aload_0
  }
  gen_args(m, &e->list, e->line);
  return emit_invoke(m, 0xb6);
}

// ---- lvalues
typedef struct LV { int kind; Ty ty; Local *local; Field *field; } LV;   // kind: 0 local 1 static 2 field 3 array

static LV
gen_lvalue_prefix(Node *e)
{
  LV lv = {0};
  if(e->kind == E_NAME){
    Local *l = findlocal(e->name);
    if(l){ lv.kind = 0; lv.local = l; lv.ty = l->ty; return lv; }
    Field *f = findfield(curclass, e->name);
    if(!f) die(e->line, "unknown variable %s", e->name);
    lv.field = f; lv.ty = f->ty;
    if(f->isstatic){ lv.kind = 1; return lv; }
    if(curmeth->isstatic) die(e->line, "instance field %s used in a static context", e->name);
    op(0x2a, 1);
    lv.kind = 2;
    return lv;
  }
  if(e->kind == E_FIELD){
    if(is_classname(e->a)){
      Class *c = findclass(e->a->name);
      Field *f = findfield(c, e->name);
      if(!f || !f->isstatic) die(e->line, "no static field %s.%s", c->name, e->name);
      lv.kind = 1; lv.field = f; lv.ty = f->ty;
      return lv;
    }
    Ty ot = gen_expr(e->a);
    if(ot.k != TY_REF) die(e->line, "field access on %s", tyname(ot));
    Field *f = findfield(ot.c, e->name);
    if(!f) die(e->line, "class %s has no field %s", ot.c->name, e->name);
    if(f->isstatic){ op(0x57, -1); lv.kind = 1; }
    else lv.kind = 2;
    lv.field = f; lv.ty = f->ty;
    return lv;
  }
  if(e->kind == E_INDEX){
    Ty at = gen_expr(e->a);
    if(at.k != TY_ARRAY) die(e->line, "indexing a non-array (%s)", tyname(at));
    if(gen_expr(e->b).k != TY_INT) die(e->line, "array index must be int");
    lv.kind = 3; lv.ty = *at.elem;
    return lv;
  }
  die(e->line, "not assignable");
  return lv;
}

static int
fieldref(Field *f)
{
  return dry ? 0 : cp_field(f->owner->jname, f->name, desc_of(f->ty));
}

static void
lv_load(LV *lv)
{
  switch(lv->kind){
  case 0: load_local(lv->ty, lv->local->slot); break;
  case 1: op2(0xb2, fieldref(lv->field), slots(lv->ty)); break;
  case 2: op2(0xb4, fieldref(lv->field), slots(lv->ty) - 1); break;
  case 3: op(isref(lv->ty) ? 0x32 : lv->ty.k == TY_BOOL ? 0x33 : 0x2e, -1); break;
  }
}

static void
lv_store(LV *lv)
{
  switch(lv->kind){
  case 0: store_local(lv->ty, lv->local->slot); break;
  case 1: op2(0xb3, fieldref(lv->field), -slots(lv->ty)); break;
  case 2: op2(0xb5, fieldref(lv->field), -slots(lv->ty) - 1); break;
  case 3: op(isref(lv->ty) ? 0x53 : lv->ty.k == TY_BOOL ? 0x54 : 0x4f, -3); break;
  }
}

static void
lv_dup_prefix(LV *lv)
{
  // duplicate the object/array+index under the value for read-modify-write
  if(lv->kind == 2) op(0x59, 1);          // dup
  else if(lv->kind == 3) op(0x5c, 2);     // dup2
}

// ---- expressions
static Ty
gen_bool_from_cond(Node *e)
{
  int f = newlabel(), end = newlabel();
  gen_cond(e, f);
  op(0x04, 1);                 // iconst_1
  branch(0xa7, end, 0);
  adj(-1);
  place(f);
  op(0x03, 1);                 // iconst_0
  place(end);
  return mkty(TY_BOOL);
}

static Ty
gen_expr(Node *e)
{
  Ty t, u;
  switch(e->kind){
  case E_NUM: iconst(e->num); return mkty(TY_INT);
  case E_BOOL: iconst(e->num); return mkty(TY_BOOL);
  case E_NULL: op(0x01, 1); return mkty(TY_NULL);
  case E_STR: ldc_string(e->name, e->slen); if(dry) adj(0); if(dry) {} return refty(cls_string);
  case E_THIS:
    if(curmeth->isstatic) die(e->line, "'this' in a static context");
    op(0x2a, 1);
    return refty(curclass);
  case E_NAME:
    if(is_classname(e)) die(e->line, "class name %s used as a value", e->name);
    /* fallthrough */
  case E_INDEX: {
    LV lv = gen_lvalue_prefix(e);
    lv_load(&lv);
    return lv.ty;
  }
  case E_FIELD:
    if(!strcmp(e->name, "length") && !is_classname(e->a)){
      Ty at = typeof_expr(e->a);
      if(at.k == TY_ARRAY){
        gen_expr(e->a);
        op(0xbe, 0);
        return mkty(TY_INT);
      }
    }
    {
      LV lv = gen_lvalue_prefix(e);
      lv_load(&lv);
      return lv.ty;
    }
  case E_CALL: return gen_call(e);
  case E_NEW: {
    Class *c = findclass(e->name);
    if(!c) die(e->line, "unknown class %s", e->name);
    op2(0xbb, dry ? 0 : cp_class(c->jname), 1);
    op(0x59, 1);
    Method *ctor = pick_ctor(c, &e->list, e->line);
    if(ctor){
      gen_args(ctor, &e->list, e->line);
      emit_invoke(ctor, 0xb7);
    } else {
      op2(0xb7, dry ? 0 : cp_method(c->jname, "<init>", "()V"), -1);
    }
    return refty(c);
  }
  case E_NEWARR: {
    Ty et = resolve(e->ty, e->line);
    if(gen_expr(e->a).k != TY_INT) die(e->line, "array size must be int");
    if(et.k == TY_INT) op1(0xbc, 10, 0);
    else if(et.k == TY_BOOL) op1(0xbc, 4, 0);
    else op2(0xbd, dry ? 0 : cp_class(et.c->jname), 0);
    Ty at = mkty(TY_ARRAY);
    at.elem = xcalloc(sizeof(Ty));
    *at.elem = et;
    return at;
  }
  case E_NOT: case E_COND: case E_BIN:
    if(e->kind == E_BIN && !strcmp(e->name, "+")){
      t = typeof_expr(e->a);
      u = typeof_expr(e->b);
      if(isstring(t) || isstring(u)){ if(!dry) gen_concat(e); return refty(cls_string); }
    }
    if(e->kind == E_COND){
      int f = newlabel(), end = newlabel();
      gen_cond(e->a, f);
      t = gen_expr(e->b);
      branch(0xa7, end, 0);
      adj(-slots(t));
      place(f);
      u = gen_expr(e->c);
      place(end);
      if(t.k == TY_NULL) return u;
      return t;
    }
    if(e->kind == E_NOT || strchr("<>=!&|", e->name[0])) return gen_bool_from_cond(e);
    t = gen_expr(e->a);
    u = gen_expr(e->b);
    if(t.k != TY_INT || u.k != TY_INT) die(e->line, "operator %s needs ints (got %s, %s)", e->name, tyname(t), tyname(u));
    switch(e->name[0]){
    case '+': op(0x60, -1); break;
    case '-': op(0x64, -1); break;
    case '*': op(0x68, -1); break;
    case '/': op(0x6c, -1); break;
    case '%': op(0x70, -1); break;
    }
    return mkty(TY_INT);
  case E_NEG:
    if(gen_expr(e->a).k != TY_INT) die(e->line, "unary - needs int");
    op(0x74, 0);
    return mkty(TY_INT);
  }
  die(e->line, "bad expression");
  return mkty(TY_VOID);
}

// jump to falselabel when e is false; fall through when true
static void
gen_cond(Node *e, int f)
{
  if(e->kind == E_NOT){
    int t = newlabel();
    gen_cond(e->a, t);
    branch(0xa7, f, 0);
    place(t);
    return;
  }
  if(e->kind == E_BIN && !strcmp(e->name, "&&")){
    gen_cond(e->a, f);
    gen_cond(e->b, f);
    return;
  }
  if(e->kind == E_BIN && !strcmp(e->name, "||")){
    int t = newlabel(), nx = newlabel();
    gen_cond(e->a, nx);
    branch(0xa7, t, 0);
    place(nx);
    gen_cond(e->b, f);
    place(t);
    return;
  }
  if(e->kind == E_BIN && strchr("<>=!", e->name[0]) && strcmp(e->name, "!") != 0){
    Ty a = gen_expr(e->a);
    Ty b = gen_expr(e->b);
    const char *o = e->name;
    int eq = !strcmp(o, "=="), ne = !strcmp(o, "!=");
    if(isref(a) || isref(b)){
      if(!(eq || ne) || !(isref(a) && isref(b))) die(e->line, "cannot compare %s with %s", tyname(a), tyname(b));
      branch(eq ? 0xa6 : 0xa5, f, -2);                  // if_acmpne / if_acmpeq (inverted)
      return;
    }
    if(a.k != b.k) die(e->line, "cannot compare %s with %s", tyname(a), tyname(b));
    if(a.k == TY_BOOL && !(eq || ne)) die(e->line, "%s on booleans", o);
    // inverted condition: jump to f when the comparison is false
    int inv = eq ? 0xa0 : ne ? 0x9f : !strcmp(o, "<") ? 0xa2 : !strcmp(o, "<=") ? 0xa3 :
              !strcmp(o, ">") ? 0xa4 : 0xa1;
    branch(inv, f, -2);
    return;
  }
  Ty t = gen_expr(e);
  if(t.k != TY_BOOL) die(e->line, "condition must be boolean, got %s", tyname(t));
  branch(0x99, f, -1);                                   // ifeq
}

// ---- statements

static void
add_xentry(int start, int end, int handler, int type)
{
  if(dry) return;
  xtab[nxtab].start = start; xtab[nxtab].end = end;
  xtab[nxtab].handler = handler; xtab[nxtab].type = type;
  nxtab++;
}

// run the monitorexits of enclosing synchronized blocks (innermost first)
// down to (not including) context index `upto`
static void
gen_unwind(int upto)
{
  for(int i = ctxs.n - 1; i >= upto; i--){
    Ctx *c = ctxs.v[i];
    if(c->kind == CTX_SYNC){
      load_local(refty(cls_object), c->slot);
      op(0xc3, -1);                                      // monitorexit
    }
  }
}

static void
gen_field_inits(Class *c, int statics)
{
  for(int i = 0; i < c->fields.n; i++){
    Field *f = c->fields.v[i];
    if(!f->init || f->isstatic != statics) continue;
    if(!statics) op(0x2a, 1);
    Ty t = gen_expr(f->init);
    if(!assignable(f->ty, t)) die(f->init->line, "cannot initialize %s with %s", tyname(f->ty), tyname(t));
    op2(statics ? 0xb3 : 0xb5, fieldref(f), statics ? -slots(f->ty) : -slots(f->ty) - 1);
  }
}

// does control never fall off the end of this statement?
static int
ends_with_return(Node *b)
{
  if(!b) return 0;
  if(b->kind == S_RETURN || b->kind == S_THROW) return 1;
  if(b->kind == S_BLOCK && b->list.n) return ends_with_return(b->list.v[b->list.n - 1]);
  if(b->kind == S_IF && b->c) return ends_with_return(b->b) && ends_with_return(b->c);
  if(b->kind == S_SYNC) return ends_with_return(b->b);
  if(b->kind == S_TRY){
    if(!ends_with_return(b->a)) return 0;
    for(int i = 0; i < b->list.n; i++) if(!ends_with_return(((Node *)b->list.v[i])->a)) return 0;
    return 1;
  }
  return 0;
}

static void
gen_sync(Node *s)
{
  // javac's exact shape for synchronized (x) { body }
  int lock = alloc_local(refty(cls_object));
  int exc = alloc_local(refty(cls_object));
  Ty t = gen_expr(s->a);
  if(!isref(t) || t.k == TY_NULL) die(s->line, "synchronized needs an object, got %s", tyname(t));
  op(0x59, 1);                                           // dup
  store_local(refty(cls_object), lock);                  // astore lock
  op(0xc2, -1);                                          // monitorenter
  int start = code.n;
  Ctx *c = xcalloc(sizeof(Ctx));
  c->kind = CTX_SYNC; c->slot = lock;
  vpush(&ctxs, c);
  gen_stmt(s->b);
  ctxs.n--;
  int after = newlabel();
  if(!ends_with_return(s->b)){
    load_local(refty(cls_object), lock);
    op(0xc3, -1);                                        // monitorexit
  }
  int end = code.n;
  if(!ends_with_return(s->b)) branch(0xa7, after, 0);   // goto after
  int handler = code.n;
  adj(1);                                                // exception on the stack
  store_local(refty(cls_object), exc);                   // astore exc
  load_local(refty(cls_object), lock);
  op(0xc3, -1);                                          // monitorexit
  int hend = code.n;
  load_local(refty(cls_object), exc);
  op(0xbf, -1);                                          // athrow
  place(after);
  add_xentry(start, end, handler, 0);                    // any
  add_xentry(handler, hend, handler, 0);                 // any
}

static void
gen_stmt(Node *s)
{
  switch(s->kind){
  case S_BLOCK:
    scope++;
    for(int i = 0; i < s->list.n; i++) gen_stmt(s->list.v[i]);
    popscope();
    return;
  case S_VAR: {
    Ty t = resolve(s->ty, s->line);
    if(t.k == TY_VOID) die(s->line, "void variable");
    if(s->a){
      Ty it = gen_expr(s->a);
      if(!assignable(t, it)) die(s->line, "cannot initialize %s with %s", tyname(t), tyname(it));
      if(t.k == TY_LONG && it.k == TY_INT) op(0x85, 1);
      Local *l = addlocal(s->name, t, s->line);
      store_local(t, l->slot);
    } else {
      Local *l = addlocal(s->name, t, s->line);
      // definite assignment is not checked; initialise so the verifier is happy
      if(isref(t)) op(0x01, 1); else if(t.k == TY_LONG) op(0x09, 2); else op(0x03, 1);
      store_local(t, l->slot);
    }
    return;
  }
  case S_ASSIGN: {
    // i++ / i += c on an int local -> iinc
    if(s->a->kind == E_NAME && s->b->kind == E_BIN && s->b->b->kind == E_NUM &&
       s->b->a->kind == E_NAME && !strcmp(s->b->a->name, s->a->name) &&
       (!strcmp(s->b->name, "+") || !strcmp(s->b->name, "-"))){
      Local *l = findlocal(s->a->name);
      long c = s->b->name[0] == '+' ? s->b->b->num : -s->b->b->num;
      if(l && l->ty.k == TY_INT && c >= -128 && c <= 127 && l->slot < 256){
        if(!dry){ bput(&code, 0x84); bput(&code, l->slot); bput(&code, (int)c & 0xff); }
        return;
      }
    }
    // read-modify-write of a field/array element: evaluate the prefix once
    if(s->b->kind == E_BIN && s->b->a == s->a && s->a->kind != E_NAME){
      LV lv = gen_lvalue_prefix(s->a);
      lv_dup_prefix(&lv);
      lv_load(&lv);
      Ty rt;
      if(isstring(lv.ty)){
        // s.f += x  for strings: build via StringBuilder on the loaded value
        op2(0xbb, dry ? 0 : cp_class("java/lang/StringBuilder"), 1);
        op(0x5a, 1);                                   // dup_x1
        op(0x5f, 0);                                   // swap
        op2(0xb7, dry ? 0 : cp_method("java/lang/StringBuilder", "<init>", "(Ljava/lang/String;)V"), -2);
        Ty bt = gen_expr(s->b->b);
        op2(0xb6, dry ? 0 : cp_method("java/lang/StringBuilder", "append", append_desc(bt)), -slots(bt));
        op2(0xb6, dry ? 0 : cp_method("java/lang/StringBuilder", "toString", "()Ljava/lang/String;"), 0);
        rt = lv.ty;
      } else {
        rt = gen_expr(s->b->b);
        if(lv.ty.k != TY_INT || rt.k != TY_INT) die(s->line, "compound assignment needs ints");
        switch(s->b->name[0]){
        case '+': op(0x60, -1); break;
        case '-': op(0x64, -1); break;
        case '*': op(0x68, -1); break;
        case '/': op(0x6c, -1); break;
        }
      }
      lv_store(&lv);
      return;
    }
    LV lv = gen_lvalue_prefix(s->a);
    Ty rt = gen_expr(s->b);
    if(!assignable(lv.ty, rt)) die(s->line, "cannot assign %s to %s", tyname(rt), tyname(lv.ty));
    if(lv.ty.k == TY_LONG && rt.k == TY_INT) op(0x85, 1);
    lv_store(&lv);
    return;
  }
  case S_EXPR: {
    Ty t = gen_expr(s->a);
    if(slots(t) == 1) op(0x57, -1);
    else if(slots(t) == 2) op(0x58, -2);
    return;
  }
  case S_IF: {
    int els = newlabel(), end = newlabel();
    gen_cond(s->a, els);
    gen_stmt(s->b);
    if(s->c && !ends_with_return(s->b)) branch(0xa7, end, 0);
    place(els);
    if(s->c){ gen_stmt(s->c); place(end); }
    return;
  }
  case S_WHILE: case S_FOR: {
    int top = newlabel(), cont = newlabel(), end = newlabel();
    scope++;
    if(s->kind == S_FOR && s->a) gen_stmt(s->a);
    place(top);
    Node *cond = s->kind == S_FOR ? s->b : s->a;
    if(cond) gen_cond(cond, end);
    Ctx *c = xcalloc(sizeof(Ctx));
    c->kind = CTX_LOOP; c->brk = end; c->cont = cont;
    vpush(&ctxs, c);
    gen_stmt(s->kind == S_FOR ? s->d : s->b);
    ctxs.n--;
    place(cont);
    if(s->kind == S_FOR && s->c) gen_stmt(s->c);
    branch(0xa7, top, 0);
    place(end);
    popscope();
    return;
  }
  case S_BREAK: case S_CONTINUE: {
    int i = ctxs.n - 1;
    while(i >= 0 && ((Ctx *)ctxs.v[i])->kind != CTX_LOOP) i--;
    if(i < 0) die(s->line, "%s outside a loop", s->kind == S_BREAK ? "break" : "continue");
    gen_unwind(i + 1);
    Ctx *c = ctxs.v[i];
    branch(0xa7, s->kind == S_BREAK ? c->brk : c->cont, 0);
    return;
  }
  case S_RETURN: {
    Ty rt = curmeth->ret;
    if(s->a){
      if(rt.k == TY_VOID) die(s->line, "void method returns a value");
      Ty t = gen_expr(s->a);
      if(!assignable(rt, t)) die(s->line, "return type %s expected, got %s", tyname(rt), tyname(t));
      if(rt.k == TY_LONG && t.k == TY_INT) op(0x85, 1);
      int has_sync = 0;
      for(int i = 0; i < ctxs.n; i++) if(((Ctx *)ctxs.v[i])->kind == CTX_SYNC) has_sync = 1;
      if(has_sync){
        // like javac: stash the value, run the monitorexits, reload
        int tmp = alloc_local(rt);
        store_local(rt, tmp);
        gen_unwind(0);
        load_local(rt, tmp);
      }
      op(isref(rt) ? 0xb0 : rt.k == TY_LONG ? 0xad : 0xac, -slots(rt));
    } else {
      if(rt.k != TY_VOID) die(s->line, "missing return value");
      gen_unwind(0);
      op(0xb1, 0);
    }
    return;
  }
  case S_SYNC:
    gen_sync(s);
    return;
  case S_TRY: {
    int start = code.n;
    Ctx *c = xcalloc(sizeof(Ctx));
    c->kind = CTX_TRY;
    vpush(&ctxs, c);
    gen_stmt(s->a);
    ctxs.n--;
    int end = code.n;
    int after = newlabel();
    if(!ends_with_return(s->a)) branch(0xa7, after, 0);
    for(int i = 0; i < s->list.n; i++){
      Node *cc = s->list.v[i];
      Ty ct = resolve(cc->ty, cc->line);
      if(ct.k != TY_REF || !subclass_of(ct.c, cls_throwable)) die(cc->line, "catch type must be a Throwable");
      int handler = code.n;
      adj(1);
      scope++;
      Local *l = addlocal(cc->name, ct, cc->line);
      store_local(ct, l->slot);
      gen_stmt(cc->a);
      popscope();
      if(!ends_with_return(cc->a)) branch(0xa7, after, 0);
      if(end > start) add_xentry(start, end, handler, dry ? 0 : cp_class(ct.c->jname));
    }
    place(after);
    return;
  }
  case S_THROW: {
    Ty t = gen_expr(s->a);
    if(t.k != TY_REF || !subclass_of(t.c, cls_throwable)) die(s->line, "throw needs a Throwable, got %s", tyname(t));
    op(0xbf, -1);
    return;
  }
  case S_PRINT: {
    op2(0xb2, dry ? 0 : cp_field("java/lang/System", "out", "Ljava/io/PrintStream;"), 1);
    const char *name = s->newline ? "println" : "print";
    if(!s->a){
      op2(0xb6, dry ? 0 : cp_method("java/io/PrintStream", name, "()V"), -1);
      return;
    }
    Ty t = gen_expr(s->a);
    const char *d = isstring(t) ? "(Ljava/lang/String;)V" : t.k == TY_INT ? "(I)V" :
                    t.k == TY_BOOL ? "(Z)V" : t.k == TY_LONG ? "(J)V" : "(Ljava/lang/Object;)V";
    if(t.k == TY_VOID) die(s->line, "cannot print void");
    op2(0xb6, dry ? 0 : cp_method("java/io/PrintStream", name, d), -1 - slots(t));
    return;
  }
  case S_SUPER:
    die(s->line, "super(...) must be the first statement of a constructor");
  }
  die(s->line, "bad statement");
}

// ================================================================ methods & class files

typedef struct MOut { int flags; char *name, *desc; Buf code; int maxstack, maxlocals; XE *xt; int nxt; } MOut;


static MOut *
gen_method(Class *c, Method *m, int clinit)
{
  curclass = c;
  curmeth = m;
  memset(&code, 0, sizeof code);
  depth = maxdepth = 0;
  nextlocal = maxlocals = 0;
  nxtab = 0;
  nlabels = 0;
  nfixes = 0;
  locals.n = 0;
  ctxs.n = 0;
  scope = 0;

  if(!m->isstatic) addlocal("this", refty(c), m->line);
  scope = 1;
  for(int i = 0; i < m->params.n; i++){
    Field *p = m->params.v[i];
    addlocal(p->name, p->ty, m->line);
  }

  Node *body = m->body;
  if(m->isctor){
    // super(...) first (explicit or implicit), then field initializers
    Node *first = body && body->list.n ? body->list.v[0] : 0;
    Class *sc = c->super;
    op(0x2a, 1);
    if(first && first->kind == S_SUPER){
      Method *sctor = pick_ctor(sc, &first->list, first->line);
      if(sctor){ gen_args(sctor, &first->list, first->line); emit_invoke(sctor, 0xb7); }
      else op2(0xb7, cp_method(sc->jname, "<init>", "()V"), -1);
      Node *rest = mk(S_BLOCK, body->line);
      for(int i = 1; i < body->list.n; i++) vpush(&rest->list, body->list.v[i]);
      body = rest;
    } else {
      Vec none = {0};
      Method *sctor = sc->ctors.n ? pick_ctor(sc, &none, m->line) : 0;
      if(sctor) emit_invoke(sctor, 0xb7);
      else op2(0xb7, cp_method(sc->jname, "<init>", "()V"), -1);
    }
    gen_field_inits(c, 0);
  }
  if(clinit) gen_field_inits(c, 1);
  if(body) gen_stmt(body);
  if(!ends_with_return(body)){
    if(m->ret.k != TY_VOID && !m->isctor && !clinit) die(m->line, "missing return statement in %s", m->name);
    op(0xb1, 0);
  }
  // patch branches
  for(int i = 0; i < nfixes; i++){
    int pos = labelpos[fixes[i].label];
    if(pos < 0){ fprintf(stderr, "internal: unplaced label\n"); exit(1); }
    int off = pos - fixes[i].insn;
    if(off < -32768 || off > 32767) die(m->line, "method too large");
    code.b[fixes[i].at] = (off >> 8) & 0xff;
    code.b[fixes[i].at + 1] = off & 0xff;
  }
  MOut *o = xcalloc(sizeof(MOut));
  o->flags = 0x0001 | (m->isstatic ? 0x0008 : 0) | (m->issync ? 0x0020 : 0);
  o->name = m->name;
  o->desc = mdesc(m);
  o->code = code;
  o->maxstack = maxdepth;
  o->maxlocals = maxlocals;
  o->nxt = nxtab;
  o->xt = xcalloc(sizeof(XE) * (nxtab + 1));
  memcpy(o->xt, xtab, sizeof(XE) * nxtab);
  return o;
}

static void
write_class(Class *c, const char *outdir)
{
  ncp = 1;
  Vec mouts = {0};
  int this_idx = cp_class(c->jname);
  int super_idx = cp_class(c->super->jname);
  int code_idx = cp_utf8("Code");
  int src_idx = cp_utf8("SourceFile");
  const char *base = strrchr(srcname, '/') ? strrchr(srcname, '/') + 1 : srcname;
  int srcval_idx = cp_utf8(base);

  if(!c->ctors.n){
    Method *m = xcalloc(sizeof(Method));
    m->name = "<init>"; m->isctor = 1; m->owner = c; m->line = c->line; m->ret.k = TY_VOID;
    m->body = mk(S_BLOCK, c->line);
    vpush(&c->ctors, m);
  }
  for(int i = 0; i < c->ctors.n; i++) vpush(&mouts, gen_method(c, c->ctors.v[i], 0));
  for(int i = 0; i < c->methods.n; i++) vpush(&mouts, gen_method(c, c->methods.v[i], 0));
  int has_static_init = 0;
  for(int i = 0; i < c->fields.n; i++){
    Field *f = c->fields.v[i];
    if(f->isstatic && f->init) has_static_init = 1;
  }
  if(has_static_init){
    Method *m = xcalloc(sizeof(Method));
    m->name = "<clinit>"; m->isstatic = 1; m->owner = c; m->line = c->line; m->ret.k = TY_VOID;
    vpush(&mouts, gen_method(c, m, 1));
  }
  // field name/descriptor entries
  int *fname = xcalloc(sizeof(int) * (c->fields.n + 1)), *fdesc = xcalloc(sizeof(int) * (c->fields.n + 1));
  for(int i = 0; i < c->fields.n; i++){
    Field *f = c->fields.v[i];
    fname[i] = cp_utf8(f->name);
    fdesc[i] = cp_utf8(desc_of(f->ty));
  }
  int *mname = xcalloc(sizeof(int) * (mouts.n + 1)), *mdsc = xcalloc(sizeof(int) * (mouts.n + 1));
  for(int i = 0; i < mouts.n; i++){
    MOut *o = mouts.v[i];
    mname[i] = cp_utf8(o->name);
    mdsc[i] = cp_utf8(o->desc);
  }

  Buf out = {0};
  bput4(&out, 0xCAFEBABEL);
  bput2(&out, 0);           // minor
  bput2(&out, 49);          // major: Java 5 (no StackMapTable required)
  bput2(&out, ncp);
  for(int i = 1; i < ncp; i++){
    CPE *e = &cpool[i];
    bput(&out, e->tag);
    switch(e->tag){
    case 1: put_utf8(&out, e->s, e->slen); break;
    case 3: bput4(&out, e->v); break;
    case 5: bput4(&out, e->v >> 32); bput4(&out, e->v); i++; break;
    case 7: case 8: bput2(&out, e->a); break;
    case 9: case 10: case 12: bput2(&out, e->a); bput2(&out, e->b); break;
    }
  }
  bput2(&out, 0x0021);      // ACC_PUBLIC | ACC_SUPER
  bput2(&out, this_idx);
  bput2(&out, super_idx);
  bput2(&out, 0);           // interfaces
  bput2(&out, c->fields.n);
  for(int i = 0; i < c->fields.n; i++){
    Field *f = c->fields.v[i];
    bput2(&out, 0x0001 | (f->isstatic ? 0x0008 : 0) | (f->isvolatile ? 0x0040 : 0));
    bput2(&out, fname[i]);
    bput2(&out, fdesc[i]);
    bput2(&out, 0);
  }
  bput2(&out, mouts.n);
  for(int i = 0; i < mouts.n; i++){
    MOut *o = mouts.v[i];
    bput2(&out, o->flags);
    bput2(&out, mname[i]);
    bput2(&out, mdsc[i]);
    bput2(&out, 1);
    bput2(&out, code_idx);
    bput4(&out, 12 + o->code.n + 8 * o->nxt);
    bput2(&out, o->maxstack);
    bput2(&out, o->maxlocals);
    bput4(&out, o->code.n);
    bputn(&out, o->code.b, o->code.n);
    bput2(&out, o->nxt);
    for(int k = 0; k < o->nxt; k++){
      bput2(&out, o->xt[k].start); bput2(&out, o->xt[k].end);
      bput2(&out, o->xt[k].handler); bput2(&out, o->xt[k].type);
    }
    bput2(&out, 0);         // Code attributes
  }
  bput2(&out, 1);
  bput2(&out, src_idx);
  bput4(&out, 2);
  bput2(&out, srcval_idx);

  char *path = fmt("%s/%s.class", outdir, c->jname);
  FILE *f = fopen(path, "wb");
  if(!f){ perror(path); exit(1); }
  fwrite(out.b, 1, out.n, f);
  fclose(f);
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
  const char *outdir = ".";
  int first = 1;
  if(argc > 2 && !strcmp(argv[1], "-d")){ outdir = argv[2]; first = 3; }
  if(first >= argc){
    fprintf(stderr, "usage: mjavac [-d outdir] File.java ...\n");
    return 2;
  }
  add_builtins();
  int nbuiltin = classes.n;
  // parse every file; remember which file each class came from
  const char **srcof = xcalloc(sizeof(char *) * 4096);
  for(int i = first; i < argc; i++){
    srcname = argv[i];
    lex(readfile(argv[i]));
    while(peek()->kind != T_EOF){
      while(accept("public") || accept("final") || accept("abstract")) ;
      if(is("import") || is("package")) die(peek()->line, "import/package are not supported");
      int before = classes.n;
      parse_class();
      for(int k = before; k < classes.n; k++) srcof[k] = argv[i];
    }
  }
  for(int i = 0; i < classes.n; i++){
    srcname = srcof[i] ? srcof[i] : "<builtin>";
    layout(classes.v[i]);
  }
  for(int i = nbuiltin; i < classes.n; i++){
    srcname = srcof[i];
    write_class(classes.v[i], outdir);
  }
  return 0;
}
