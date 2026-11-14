#ifndef XV6_FBCON_H
#define XV6_FBCON_H
// Text console on a linear XRGB8888 frame buffer, 8x16 glyphs. Every
// character written to the kernel console is mirrored here (the console
// UART driver calls fbcon_putc()).
void fbcon_register(uint32 *fb, uint32 xres, uint32 yres);
void fbcon_putc(int c);
// 1: stop drawing (an application owns /dev/fb0); 0: clear and resume.
void fbcon_pause(int pause);
#endif
