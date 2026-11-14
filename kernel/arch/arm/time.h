#ifndef XV6_ARM_TIME_H
#define XV6_ARM_TIME_H
// The periodic tick and the free-running counter behind r_cntvct_el0() on
// Cortex-A8 parts, which have no Generic Timer.  A clocksource driver calls
// register_clocksource() and then arm32_timer_tick() from its interrupt.
#define HZ_TICK 100   // timer interrupts per second; xv6 ticks are HZ_TICK/10

void register_clocksource(char *name, uint64 (*read)(void), uint hz);
void arm32_timer_tick(void);
void arm32_time_init(void);
#endif
