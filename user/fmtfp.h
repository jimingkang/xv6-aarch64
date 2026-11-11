#ifndef XV6_FMTFP_H
#define XV6_FMTFP_H

// Flags for fmt_double (printf's "-+ 0#").
#define FMT_LEFT  1
#define FMT_PLUS  2
#define FMT_SPACE 4
#define FMT_ZERO  8
#define FMT_ALT   16

#ifdef __cplusplus
extern "C" {
#endif

// Format value like printf("%*.*<conv>") with conv one of e E f F g G.
// prec < 0 means the default (6).  Writes at most cap-1 bytes plus a NUL
// and returns the full length, like snprintf.
int fmt_double(char *buf, int cap, double value, int width, int prec,
               int flags, int conv);

#ifdef __cplusplus
}
#endif

#endif
