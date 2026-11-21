#ifndef XV6_WIFI_TRACE_H
#define XV6_WIFI_TRACE_H

// WIFI_TRACE: the Raspberry Pi 3 on-board SDIO Wi-Fi path (Arasan SDHCI ->
// SDIO core -> brcmfmac -> net), told as numbered steps that match
// doc/readme/rpi3/sdio_wifi_illustrated.md and its diagrams:
//
//   wifi1  Arasan host probe: GPIO34-39 ALT3, base clock, 400 kHz / 25 MHz
//   wifi2  SDIO enumeration: CMD5 OCR, IORDY, RCA, 4-bit, function IDs
//   wifi3  brcmf_probe: F1 enable, ALP clock, ChipCommon chip id, files
//   wifi4  firmware load over F1: cores, TCM, halt CR4, BIN, NVRAM, release
//   wifi5  F2 ready: protocol 4, FWREADY mailbox, IRQ enabled
//   wifi6  BCDC control: every brcmf_dcmd request and its reply
//   wifi7  join: SET_SSID, firmware events, BSSID, EAPOL M1-M4, DHCP, PM off
//   wifi8  IRQ: DAT1 card interrupts counted by the top half (no printf in
//          IRQ context: the count is printed by the next NAPI poll)
//   wifi9  RX: one SDPCM frame read from F2 (len, channel, seq, tx_max)
//   wifi10 TX: one data frame written to F2 (seq, tx_max, credit wait)
//   wifi11 NAPI: one brcmf_napi_poll pass (frames, drained, spurious)
//
//   make WIFI_TRACE=0   off (default; only the driver's normal messages)
//   make WIFI_TRACE=1   steps 1-7 in full, steps 8-11 for the first
//                       WIFI_TRACE_LIMIT lines (0 = unlimited)
//   make WIFI_TRACE=2   + one line per 32 KB firmware window, and the RX
//                       frames read while a BCDC command waits for its reply
//
// Every line is "wifi<step>: t=<ms since boot> ...", so
// `grep '^wifi[0-9]' log` gives the whole story, and the animation in
// tools/sdio_wifi_diagrams/manim/ replays it with the real timing.
#ifndef WIFI_TRACE
#define WIFI_TRACE 0
#endif
#ifndef WIFI_TRACE_LIMIT
#define WIFI_TRACE_LIMIT 300
#endif

#define WIFITRACE(level) (WIFI_TRACE >= (level))

static inline int
wifi_trace_ms(void)
{
  return (int)(r_cntvct_el0() * 1000 / r_cntfrq_el0());
}

// Budget shared by the data-path steps 8-11 (brcmfmac.c owns the counter).
int wifi_trace_budget(void);

#define WTRACE(level, step, fmt, ...) do { \
    if(WIFITRACE(level)) \
      printf("wifi%d: t=%d " fmt "\n", step, wifi_trace_ms(), ##__VA_ARGS__); \
  } while(0)

#define WTRACE_DATA(step, fmt, ...) do { \
    if(WIFITRACE(1) && wifi_trace_budget()) \
      printf("wifi%d: t=%d " fmt "\n", step, wifi_trace_ms(), ##__VA_ARGS__); \
  } while(0)

#endif
