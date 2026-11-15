#ifndef XV6_KERNEL_WIN32K_H
#define XV6_KERNEL_WIN32K_H

#include "types.h"

void win32k_input_event(uint16 type, uint16 code, int value);
void win32k_exit(int tgid);
int win32k_owns_input(void);

#endif
