#ifndef XV6_CLK_H
#define XV6_CLK_H
// Minimal clock API (Linux clk_get_rate() by name), backed by the SoC clock
// driver in kernel/drivers/clk/.  Returns 0 for an unknown clock.
uint clk_get_rate(const char *name);
#endif
