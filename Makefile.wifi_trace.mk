# Included from Makefile (after the PROC_SCHED_TRACE block):
#   include Makefile.wifi_trace.mk
#
# SDIO Wi-Fi teaching trace (include/kernel/wifi_trace.h): numbered
# "wifi<step>:" lines for host init, enumeration, firmware download, F2,
# BCDC commands, join/WPA2/DHCP, and the IRQ/NAPI/RX/TX data path.
#   make WIFI_TRACE=1                    steps + first WIFI_TRACE_LIMIT data-path lines
#   make WIFI_TRACE=2                    + firmware windows, RX while a command waits
#   make WIFI_TRACE=1 WIFI_TRACE_LIMIT=0 no data-path limit (slows TCP)
WIFI_TRACE ?= 0
WIFI_TRACE_LIMIT ?= 300
CFLAGS += -DWIFI_TRACE=$(WIFI_TRACE) -DWIFI_TRACE_LIMIT=$(WIFI_TRACE_LIMIT)
WIFI_TRACE_CONFIG = $K/.wifi_trace.config
.PHONY: FORCE_WIFI_TRACE_CONFIG
FORCE_WIFI_TRACE_CONFIG:

$(WIFI_TRACE_CONFIG): FORCE_WIFI_TRACE_CONFIG
	@cfg='$(WIFI_TRACE) $(WIFI_TRACE_LIMIT)'; \
	old=$$(test -f $@ && cat $@ || true); \
	if test "$$old" != "$$cfg"; then printf '%s\n' "$$cfg" > $@; fi

$K/drivers/net/wireless/brcmfmac.o $K/drivers/mmc/core/sdio.o \
$K/drivers/mmc/host/arasan_sdio.o: $(WIFI_TRACE_CONFIG) include/kernel/wifi_trace.h
