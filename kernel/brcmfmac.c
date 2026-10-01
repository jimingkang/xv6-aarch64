// Broadcom FullMAC SDIO driver attachment point.
// Transport enumeration is owned by sdio.c; this driver matches BCM43430 and
// BCM43455 function 1.  Firmware download will be layered here after the
// BCM2837 Arasan host is freed from SD-card duty.

#include "types.h"
#include "aarch64.h"
#include "spinlock.h"
#include "defs.h"
#include "device.h"
#include "sdio.h"
#include "fat32.h"
#include "net.h"
#include "workqueue.h"

// Broadcom function-1 backplane aperture.  The three SBADDR registers select
// one 32 KiB backplane window; bit 15 in the CMD53 address selects 32-bit
// backplane access rather than the function-1 register space.
#define SBSDIO_FUNC1_SBADDRLOW       0x1000a
#define SBSDIO_FUNC1_SBADDRMID       0x1000b
#define SBSDIO_FUNC1_SBADDRHIGH      0x1000c
#define SBSDIO_FUNC1_CHIPCLKCSR      0x1000e
#define SBSDIO_FUNC1_SDIOPULLUP      0x1000f
#define SBSDIO_WATERMARK             0x10008
#define SBSDIO_DEVICE_CTL            0x10009
#define SBSDIO_FUNC1_MESBUSYCTRL     0x1001d
#define SBSDIO_SB_OFT_ADDR_MASK      0x07fff
#define SBSDIO_SB_ACCESS_2_4B_FLAG   0x08000
#define SBSDIO_SBWINDOW_MASK         0xffff8000U

#define SI_ENUM_BASE                 0x18000000U
#define CHIPCOMMON_CHIPID            (SI_ENUM_BASE + 0x000)

#define BRCM_CC_43430_CHIP_ID         43430U
#define BRCM_CC_4345_CHIP_ID          0x4345U

#define SBSDIO_FORCE_ALP              0x01
#define SBSDIO_FORCE_HT               0x02
#define SBSDIO_ALP_AVAIL_REQ          0x08
#define SBSDIO_FORCE_HW_CLKREQ_OFF    0x20
#define SBSDIO_ALP_AVAIL              0x40

#define DMP_DESC_TYPE_MSK             0x0000000f
#define DMP_DESC_VALID                0x00000001
#define DMP_DESC_COMPONENT            0x00000001
#define DMP_DESC_MASTER_PORT          0x00000003
#define DMP_DESC_ADDRESS              0x00000005
#define DMP_DESC_ADDRSIZE_GT32        0x00000008
#define DMP_DESC_EOT                  0x0000000f
#define DMP_COMP_PARTNUM              0x000fff00
#define DMP_COMP_PARTNUM_S            8
#define DMP_COMP_NUM_SWRAP            0x00f80000
#define DMP_COMP_NUM_SWRAP_S          19
#define DMP_COMP_NUM_MWRAP            0x0007c000
#define DMP_COMP_NUM_MWRAP_S          14
#define DMP_COMP_REVISION             0xff000000
#define DMP_COMP_REVISION_S           24
#define DMP_SLAVE_ADDR_BASE           0xfffff000
#define DMP_SLAVE_TYPE                0x000000c0
#define DMP_SLAVE_TYPE_S              6
#define DMP_SLAVE_TYPE_SLAVE          0
#define DMP_SLAVE_TYPE_SWRAP          2
#define DMP_SLAVE_TYPE_MWRAP          3
#define DMP_SLAVE_SIZE_TYPE           0x00000030
#define DMP_SLAVE_SIZE_TYPE_S         4
#define DMP_SLAVE_SIZE_4K             0
#define DMP_SLAVE_SIZE_8K             1
#define DMP_SLAVE_SIZE_DESC           3

#define BCMA_CORE_CHIPCOMMON          0x800
#define BCMA_CORE_80211               0x812
#define BCMA_CORE_SDIO_DEV            0x829
#define BCMA_CORE_ARM_CR4             0x83e
#define BCMA_IOCTL                    0x408
#define BCMA_IOCTL_CLK                0x0001
#define BCMA_IOCTL_FGC                0x0002
#define BCMA_RESET_CTL                0x800
#define BCMA_RESET_CTL_RESET          0x0001
#define ARMCR4_BCMA_IOCTL_CPUHALT     0x0020
#define D11_BCMA_IOCTL_PHYCLOCKEN     0x0004
#define D11_BCMA_IOCTL_PHYRESET       0x0008
#define ARMCR4_CAP                    0x04
#define ARMCR4_BANKIDX                0x40
#define ARMCR4_BANKINFO               0x44
#define ARMCR4_TCBANB_MASK            0x0f
#define ARMCR4_TCBBNB_MASK            0xf0
#define ARMCR4_BSZ_MASK               0x7f
#define ARMCR4_BSZ_MULT               8192
#define ARMCR4_BLK_1K_MASK            0x200

#define BRCMF_MAX_CORES               16
#define BRCMF_RAM_BASE_4345           0x00198000U
#define BRCMF_IO_CHUNK                512
#define BRCMF_NVRAM_MAX               4096

#define SDIO_CORE_INTSTATUS           0x20
#define SDIO_CORE_HOSTINTMASK         0x24
#define SDIO_CORE_TOSBMAILBOX         0x40
#define SDIO_CORE_TOSBMAILBOXDATA     0x48
#define SDIO_CORE_TOHOSTMAILBOXDATA   0x4c
#define SDPCM_PROT_VERSION            4
#define SMB_DATA_VERSION_SHIFT        16
#define SMB_INT_ACK                   0x2
#define HMB_DATA_DEVREADY             0x0002
#define HMB_DATA_FWREADY              0x0008
#define HMB_DATA_FWHALT               0x0010
#define HMB_DATA_VERSION_MASK         0x00ff0000
#define HMB_DATA_VERSION_SHIFT        16
#define HOSTINTMASK                   (0x000000f0U | (1U << 29))

#define I_HMB_FRAME_IND               (1U << 6)
#define SDPCM_HEADER_LEN              12
#define SDPCM_CONTROL_CHANNEL         0
#define SDPCM_EVENT_CHANNEL           1
#define SDPCM_DATA_CHANNEL            2
#define BCDC_DCMD_SET                 0x02
#define BCDC_DCMD_ERROR               0x01
#define BRCMF_C_UP                    2
#define BRCMF_C_DOWN                  3
#define BRCMF_C_SET_INFRA             20
#define BRCMF_C_SET_AUTH              22
#define BRCMF_C_GET_BSSID             23
#define BRCMF_C_SET_SSID              26
#define BRCMF_C_SCAN                  50
#define BRCMF_C_SCAN_RESULTS          51
#define BRCMF_C_SET_WSEC              134
#define BRCMF_C_GET_VAR               262
#define BRCMF_C_SET_VAR               263
#define BRCMF_C_SET_WSEC_PMK          268
#define AES_ENABLED                   0x0004
#define WPA2_AUTH_PSK                 0x0080
#define WSEC_PASSPHRASE               0x0001
#define BRCMF_DCMD_MAX                2048
#define BRCMF_FRAME_MAX               4096
#define DLOAD_HANDLER_VER             1
#define DLOAD_FLAG_VER_SHIFT          12
#define DL_BEGIN                      0x0002
#define DL_END                        0x0004
#define DL_TYPE_CLM                   2
#define BRCMF_CLM_CHUNK               1024
#define CRYPTO_ALGO_AES_CCM           4
#define WL_PRIMARY_KEY                (1U << 1)
#define BRCMF_EVENT_MASK_LEN          16
#define BRCMF_SCAN_RESULTS_LEN        1900

struct brcmf_core {
  uint16 id;
  uint8 rev;
  uint32 base;
  uint32 wrap;
};

struct brcmf_chip {
  struct brcmf_core cores[BRCMF_MAX_CORES];
  int ncores;
  uint32 rambase;
  uint32 ramsize;
  struct brcmf_core *cc;
  struct brcmf_core *cr4;
  struct brcmf_core *sdio;
};

static uint8 brcmf_io_buf[BRCMF_IO_CHUNK];
static uint8 brcmf_nvram_raw[BRCMF_NVRAM_MAX];
static uint8 brcmf_nvram_data[BRCMF_NVRAM_MAX];
static uint32 brcmf_transfer_limit = BRCMF_IO_CHUNK;

struct brcmf_bcdc_dcmd {
  uint32 cmd;
  uint32 len;
  uint32 flags;
  int status;
};

struct brcmf_bcdc_header {
  uint8 flags;
  uint8 priority;
  uint8 flags2;
  uint8 data_offset;
};

struct brcmf_event_msg {
  uint16 version;
  uint16 flags;
  uint32 event_type;
  uint32 status;
  uint32 reason;
  uint32 auth_type;
  uint32 data_len;
  uint8 addr[6];
  char ifname[16];
  uint8 ifidx;
  uint8 bsscfgidx;
} __attribute__((packed));

struct brcmf_dload_data {
  uint16 flag;
  uint16 type;
  uint32 len;
  uint32 crc;
  uint8 data[0];
};

struct brcmf_ssid {
  uint32 length;
  uint8 value[32];
};

struct brcmf_scan_params {
  struct brcmf_ssid ssid;
  uint8 bssid[6];
  char bss_type;
  uint8 scan_type;
  int nprobes;
  int active_time;
  int passive_time;
  int home_time;
  uint32 channel_num;
} __attribute__((packed));

struct brcmf_wsec_pmk {
  uint16 key_len;
  uint16 flags;
  // Firmware 7.45 on BCM43455 uses the legacy wsec_pmk_t ABI.  Its key
  // storage is WSEC_MAX_PSK_LEN (64), not the newer 128-byte SAE extension.
  uint8 key[64];
};

// Firmware ABI used by the wsec_key iovar (natural 32-bit alignment,
// matching Linux brcmf_wsec_key_le / Broadcom wl_wsec_key_t).
struct brcmf_wsec_key {
  uint32 index;
  uint32 len;
  uint8 data[32];
  uint32 pad_1[18];
  uint32 algo;
  uint32 flags;
  uint32 pad_2[3];
  uint32 iv_initialized;
  uint32 pad_3;
  uint32 rxiv_hi;
  uint16 rxiv_lo;
  uint16 rxiv_pad;
  uint32 pad_4[2];
  uint8 ea[6];
  uint8 end_pad[2];
};

struct brcmf_bus_state {
  int used;
  struct spinlock lock;
  struct spinlock iovar_lock;
  struct sdio_func *f1;
  struct sdio_func *f2;
  uint32 cc_base;
  uint32 sdio_base;
  uint8 tx_seq;
  uint16 reqid;
  int ready;
  uint8 tx[BRCMF_FRAME_MAX];
  uint8 rx[BRCMF_FRAME_MAX];
  uint8 ctl[BRCMF_DCMD_MAX];
  uint8 iovar_buf[BRCMF_DCMD_MAX];
  uint8 wpa_tx[192];
  uint8 assoc_ies[512];
  uint8 net_rx[1600];
  uint8 net_deliver[1600];
  int net_rx_len;
  int polling;
  int ctl_len;
  uint16 ctl_id;
  int eapol_rx;
  uint8 mac[6];
  uint8 pmk[32];
  uint8 anonce[32];
  uint8 snonce[32];
  uint8 ptk[64];
  uint8 gtk[16];
  uint8 m3_replay[8];
  uint8 ap[6];
  uint8 gtk_index;
  uint8 m3_desc_type;
  uint8 m3_desc_version;
  uint8 assoc_rsn_ie[64];
  int assoc_rsn_ie_len;
  int pmk_ready;
  int handshake_started;
  int keys_pending;
  int handshake_done;
  int m2_sent;
  struct workqueue *wq;
  struct napi_struct napi;
  struct delayed_work state_work;
  int connect_active;
  int connect_timedout;
  uint64 connect_deadline;
  struct net_device netdev;
  uint8 scan_results[BRCMF_SCAN_RESULTS_LEN];
};

#define BRCMF_MAX_DEVICES 2
static struct brcmf_bus_state brcmf_devices[BRCMF_MAX_DEVICES];
static struct workqueue brcmf_wq[BRCMF_MAX_DEVICES];
static char *brcmf_wq_names[BRCMF_MAX_DEVICES] = {
  "brcmf0_wq", "brcmf1_wq"
};

// Functions below operate on an explicitly selected per-device instance.
// Keeping the old field spelling makes the transport code readable while
// ensuring every access resolves through that function's local bus pointer.
#define brcmf_bus (*bus)

static int brcmf_napi_poll(struct napi_struct*, int);
static void brcmf_state_worker(struct work_struct*);

static struct brcmf_bus_state *
brcmf_alloc_bus(void)
{
  for(int i = 0; i < BRCMF_MAX_DEVICES; i++){
    if(brcmf_devices[i].used)
      continue;
    memset(&brcmf_devices[i], 0, sizeof(brcmf_devices[i]));
    brcmf_devices[i].used = 1;
    brcmf_devices[i].wq = &brcmf_wq[i];
    initlock(&brcmf_devices[i].lock, "brcmfmac");
    initlock(&brcmf_devices[i].iovar_lock, "brcmf-iovar");
    netif_napi_add(&brcmf_devices[i].netdev, &brcmf_devices[i].napi,
                   brcmf_devices[i].wq, brcmf_napi_poll, 16);
    init_delayed_work(&brcmf_devices[i].state_work, brcmf_state_worker);
    return &brcmf_devices[i];
  }
  return 0;
}

static void
brcmf_free_bus(struct brcmf_bus_state *bus)
{
  if(bus){
    bus->used = 0;
    __sync_synchronize();
    napi_disable(&bus->napi);
    cancel_delayed_work_sync(&bus->state_work);
    memset(bus, 0, sizeof(*bus));
  }
}

static int brcmfmac_xmit(struct net_device*, void*, int);
static void brcmfmac_poll_device(struct net_device*);

static const struct net_device_ops brcmf_netdev_ops = {
  .start_xmit = brcmfmac_xmit,
  .poll = brcmfmac_poll_device,
};

static const struct sdio_device_id brcmf_ids[] = {
  { SDIO_VENDOR_BROADCOM, SDIO_DEVICE_BCM43430, 1 },
  { SDIO_VENDOR_BROADCOM, SDIO_DEVICE_BCM43455, 1 },
  { 0, 0, 0 },
};

static int
brcmf_load_firmware(struct sdio_func *func, char *name, struct fat32_file *file)
{
  (void)func;
  if(fat32openroot(name, file) < 0){
    printf("brcmfmac: firmware file %s missing\n", name);
    return -1;
  }
  return 0;
}

static int
brcmf_set_backplane_window(struct sdio_func *func, uint32 addr)
{
  uint32 window = addr & SBSDIO_SBWINDOW_MASK;
  uint32 value = window >> 8;
  uint8 byte;

  byte = value & 0xff;
  if(sdio_cmd52(func, 1, SBSDIO_FUNC1_SBADDRLOW, &byte) < 0)
    return -1;
  byte = (value >> 8) & 0xff;
  if(sdio_cmd52(func, 1, SBSDIO_FUNC1_SBADDRMID, &byte) < 0)
    return -1;
  byte = (value >> 16) & 0xff;
  if(sdio_cmd52(func, 1, SBSDIO_FUNC1_SBADDRHIGH, &byte) < 0)
    return -1;
  return 0;
}

static int
brcmf_backplane_read32(struct sdio_func *func, uint32 addr, uint32 *value)
{
  uint8 data[4];
  uint32 sdio_addr;

  if(value == 0 || brcmf_set_backplane_window(func, addr) < 0)
    return -1;
  sdio_addr = (addr & SBSDIO_SB_OFT_ADDR_MASK) |
              SBSDIO_SB_ACCESS_2_4B_FLAG;
  if(sdio_cmd53(func, 0, sdio_addr, 1, data, sizeof(data)) < 0)
    return -1;
  *value = (uint32)data[0] | ((uint32)data[1] << 8) |
           ((uint32)data[2] << 16) | ((uint32)data[3] << 24);
  return 0;
}

static void
brcmf_delay_us(uint32 us)
{
  uint64 ticks = ((uint64)r_cntfrq_el0() * us + 999999) / 1000000;
  uint64 start = r_cntvct_el0();
  while(r_cntvct_el0() - start < ticks)
    asm volatile("yield" ::: "memory");
}

static int
brcmf_backplane_transfer(struct sdio_func *func, int write, uint32 addr,
                         void *buffer, uint32 len)
{
  uint8 *p = buffer;

  while(len){
    uint32 offset = addr & SBSDIO_SB_OFT_ADDR_MASK;
    uint32 chunk = 0x8000 - offset;
    uint32 sdio_addr;
    if(chunk > brcmf_transfer_limit)
      chunk = brcmf_transfer_limit;
    if(chunk > len)
      chunk = len;
    if(brcmf_set_backplane_window(func, addr) < 0)
      return -1;
    sdio_addr = offset | SBSDIO_SB_ACCESS_2_4B_FLAG;
    if(sdio_cmd53(func, write, sdio_addr, 1, p, chunk) < 0){
      // The Pi 3B+ Wi-Fi path can reject a large byte-mode CMD53 with a
      // data CRC error even though 32-bit accesses work.  Learn a smaller
      // transfer size and retry the same address after the host reset.
      if(chunk <= 4)
        return -1;
      brcmf_transfer_limit = chunk / 2;
      brcmf_transfer_limit &= ~3U;
      if(brcmf_transfer_limit < 4)
        brcmf_transfer_limit = 4;
      printf("brcmfmac: CMD53 retry with max-chunk=%d\n",
             brcmf_transfer_limit);
      continue;
    }
    addr += chunk;
    p += chunk;
    len -= chunk;
  }
  return 0;
}

static int
brcmf_backplane_write32(struct sdio_func *func, uint32 addr, uint32 value)
{
  uint8 data[4];
  data[0] = value;
  data[1] = value >> 8;
  data[2] = value >> 16;
  data[3] = value >> 24;
  return brcmf_backplane_transfer(func, 1, addr, data, sizeof(data));
}

static int
brcmf_clock_prepare(struct sdio_func *func)
{
  uint8 value = SBSDIO_FORCE_HW_CLKREQ_OFF | SBSDIO_ALP_AVAIL_REQ;
  if(sdio_cmd52(func, 1, SBSDIO_FUNC1_CHIPCLKCSR, &value) < 0)
    return -1;
  for(int i = 0; i < 15000; i++){
    value = 0;
    if(sdio_cmd52(func, 0, SBSDIO_FUNC1_CHIPCLKCSR, &value) < 0)
      return -1;
    if(value & SBSDIO_ALP_AVAIL){
      value = SBSDIO_FORCE_HW_CLKREQ_OFF | SBSDIO_FORCE_ALP;
      if(sdio_cmd52(func, 1, SBSDIO_FUNC1_CHIPCLKCSR, &value) < 0)
        return -1;
      brcmf_delay_us(65);
      value = 0;
      sdio_cmd52(func, 1, SBSDIO_FUNC1_SDIOPULLUP, &value);
      return 0;
    }
    brcmf_delay_us(1);
  }
  return -1;
}

static uint32
brcmf_erom_desc(struct sdio_func *func, uint32 *address, int *error)
{
  uint32 value = 0;
  if(*error == 0 && brcmf_backplane_read32(func, *address, &value) < 0)
    *error = -1;
  *address += 4;
  return value;
}

static int
brcmf_erom_regaddr(struct sdio_func *func, uint32 *address,
                   uint32 *regbase, uint32 *wrapbase)
{
  uint32 value, sizedesc;
  int desc, stype, sztype, wraptype, error = 0;

  *regbase = 0;
  *wrapbase = 0;
  value = brcmf_erom_desc(func, address, &error);
  desc = value & DMP_DESC_TYPE_MSK;
  if((desc & ~DMP_DESC_ADDRSIZE_GT32) == DMP_DESC_ADDRESS)
    desc = DMP_DESC_ADDRESS;
  if(desc == DMP_DESC_MASTER_PORT)
    wraptype = DMP_SLAVE_TYPE_MWRAP;
  else if(desc == DMP_DESC_ADDRESS){
    *address -= 4;
    wraptype = DMP_SLAVE_TYPE_SWRAP;
  } else {
    *address -= 4;
    return -1;
  }

  do {
    do {
      value = brcmf_erom_desc(func, address, &error);
      desc = value & DMP_DESC_TYPE_MSK;
      if((desc & ~DMP_DESC_ADDRSIZE_GT32) == DMP_DESC_ADDRESS)
        desc = DMP_DESC_ADDRESS;
      if(error || desc == DMP_DESC_EOT){
        *address -= 4;
        return -1;
      }
    } while(desc != DMP_DESC_ADDRESS && desc != DMP_DESC_COMPONENT);

    if(desc == DMP_DESC_COMPONENT){
      *address -= 4;
      return 0;
    }
    if(value & DMP_DESC_ADDRSIZE_GT32)
      brcmf_erom_desc(func, address, &error);
    sztype = (value & DMP_SLAVE_SIZE_TYPE) >> DMP_SLAVE_SIZE_TYPE_S;
    if(sztype == DMP_SLAVE_SIZE_DESC){
      sizedesc = brcmf_erom_desc(func, address, &error);
      if(sizedesc & DMP_DESC_ADDRSIZE_GT32)
        brcmf_erom_desc(func, address, &error);
    }
    if(sztype != DMP_SLAVE_SIZE_4K && sztype != DMP_SLAVE_SIZE_8K)
      continue;
    stype = (value & DMP_SLAVE_TYPE) >> DMP_SLAVE_TYPE_S;
    if(*regbase == 0 && stype == DMP_SLAVE_TYPE_SLAVE)
      *regbase = value & DMP_SLAVE_ADDR_BASE;
    if(*wrapbase == 0 && stype == wraptype)
      *wrapbase = value & DMP_SLAVE_ADDR_BASE;
  } while(!error && (*regbase == 0 || *wrapbase == 0));
  return error ? -1 : 0;
}

static struct brcmf_core *
brcmf_find_core(struct brcmf_chip *chip, uint16 id, int instance)
{
  for(int i = 0; i < chip->ncores; i++)
    if(chip->cores[i].id == id && instance-- == 0)
      return &chip->cores[i];
  return 0;
}

static int
brcmf_scan_cores(struct sdio_func *func, struct brcmf_chip *chip)
{
  uint32 address, value, base, wrap;
  int error = 0, desc = 0;

  memset(chip, 0, sizeof(*chip));
  if(brcmf_backplane_read32(func, SI_ENUM_BASE + 0xfc, &address) < 0)
    return -1;
  printf("brcmfmac: EROM at %x\n", address);
  for(int guard = 0; guard < 1024 && desc != DMP_DESC_EOT; guard++){
    uint16 id;
    uint8 rev, nmw, nsw;
    value = brcmf_erom_desc(func, &address, &error);
    if(error)
      return -1;
    desc = value & DMP_DESC_TYPE_MSK;
    if(!(value & DMP_DESC_VALID) || desc != DMP_DESC_COMPONENT)
      continue;
    id = (value & DMP_COMP_PARTNUM) >> DMP_COMP_PARTNUM_S;
    value = brcmf_erom_desc(func, &address, &error);
    if(error || (value & DMP_DESC_TYPE_MSK) != DMP_DESC_COMPONENT)
      return -1;
    nmw = (value & DMP_COMP_NUM_MWRAP) >> DMP_COMP_NUM_MWRAP_S;
    nsw = (value & DMP_COMP_NUM_SWRAP) >> DMP_COMP_NUM_SWRAP_S;
    rev = (value & DMP_COMP_REVISION) >> DMP_COMP_REVISION_S;
    if(nmw + nsw == 0)
      continue;
    if(brcmf_erom_regaddr(func, &address, &base, &wrap) < 0)
      continue;
    if(base == 0 || wrap == 0)
      continue;
    if(chip->ncores >= BRCMF_MAX_CORES)
      return -1;
    chip->cores[chip->ncores].id = id;
    chip->cores[chip->ncores].rev = rev;
    chip->cores[chip->ncores].base = base;
    chip->cores[chip->ncores].wrap = wrap;
    printf("brcmfmac: core[%d] id=%x rev=%d base=%x wrap=%x\n",
           chip->ncores, id, rev, base, wrap);
    chip->ncores++;
  }
  chip->cc = brcmf_find_core(chip, BCMA_CORE_CHIPCOMMON, 0);
  chip->cr4 = brcmf_find_core(chip, BCMA_CORE_ARM_CR4, 0);
  chip->sdio = brcmf_find_core(chip, BCMA_CORE_SDIO_DEV, 0);
  if(chip->cc == 0 || chip->cr4 == 0 || chip->sdio == 0)
    return -1;
  return 0;
}

static int
brcmf_get_raminfo(struct sdio_func *func, struct brcmf_chip *chip)
{
  uint32 cap, info, banks;
  if(brcmf_backplane_read32(func, chip->cr4->base + ARMCR4_CAP, &cap) < 0)
    return -1;
  banks = (cap & ARMCR4_TCBANB_MASK) +
          ((cap & ARMCR4_TCBBNB_MASK) >> 4);
  chip->ramsize = 0;
  for(uint32 i = 0; i < banks; i++){
    uint32 block = ARMCR4_BSZ_MULT;
    if(brcmf_backplane_write32(func, chip->cr4->base + ARMCR4_BANKIDX, i) < 0 ||
       brcmf_backplane_read32(func, chip->cr4->base + ARMCR4_BANKINFO,
                              &info) < 0)
      return -1;
    if(info & ARMCR4_BLK_1K_MASK)
      block >>= 3;
    chip->ramsize += ((info & ARMCR4_BSZ_MASK) + 1) * block;
  }
  chip->rambase = BRCMF_RAM_BASE_4345;
  if(chip->ramsize == 0 || chip->ramsize > 4 * 1024 * 1024)
    return -1;
  printf("brcmfmac: TCM RAM base=%x size=%d bytes banks=%d\n",
         chip->rambase, chip->ramsize, banks);
  return 0;
}

static int
brcmf_core_disable(struct sdio_func *func, struct brcmf_core *core,
                   uint32 prereset, uint32 reset)
{
  uint32 value;
  if(brcmf_backplane_read32(func, core->wrap + BCMA_RESET_CTL, &value) < 0)
    return -1;
  if((value & BCMA_RESET_CTL_RESET) == 0){
    if(brcmf_backplane_write32(func, core->wrap + BCMA_IOCTL,
                               prereset | BCMA_IOCTL_FGC | BCMA_IOCTL_CLK) < 0 ||
       brcmf_backplane_write32(func, core->wrap + BCMA_RESET_CTL,
                               BCMA_RESET_CTL_RESET) < 0)
      return -1;
    brcmf_delay_us(20);
    for(int i = 0; i < 300; i++){
      if(brcmf_backplane_read32(func, core->wrap + BCMA_RESET_CTL,
                                &value) < 0)
        return -1;
      if(value & BCMA_RESET_CTL_RESET)
        break;
      brcmf_delay_us(1);
    }
    if((value & BCMA_RESET_CTL_RESET) == 0)
      return -1;
  }
  return brcmf_backplane_write32(func, core->wrap + BCMA_IOCTL,
                                 reset | BCMA_IOCTL_FGC | BCMA_IOCTL_CLK);
}

static int
brcmf_core_reset(struct sdio_func *func, struct brcmf_core *core,
                 uint32 prereset, uint32 reset, uint32 postreset)
{
  uint32 value;
  if(brcmf_core_disable(func, core, prereset, reset) < 0)
    return -1;
  for(int i = 0; i < 50; i++){
    if(brcmf_backplane_read32(func, core->wrap + BCMA_RESET_CTL,
                              &value) < 0)
      return -1;
    if((value & BCMA_RESET_CTL_RESET) == 0)
      break;
    if(brcmf_backplane_write32(func, core->wrap + BCMA_RESET_CTL, 0) < 0)
      return -1;
    brcmf_delay_us(50);
  }
  if(brcmf_backplane_read32(func, core->wrap + BCMA_RESET_CTL, &value) < 0 ||
     (value & BCMA_RESET_CTL_RESET))
    return -1;
  if(brcmf_backplane_write32(func, core->wrap + BCMA_IOCTL,
                             postreset | BCMA_IOCTL_CLK) < 0)
    return -1;
  return 0;
}

static int
brcmf_set_passive(struct sdio_func *func, struct brcmf_chip *chip)
{
  uint32 value;
  if(brcmf_backplane_read32(func, chip->cr4->wrap + BCMA_IOCTL, &value) < 0)
    return -1;
  value &= ARMCR4_BCMA_IOCTL_CPUHALT;
  if(brcmf_core_reset(func, chip->cr4, value,
                      ARMCR4_BCMA_IOCTL_CPUHALT,
                      ARMCR4_BCMA_IOCTL_CPUHALT) < 0)
    return -1;
  for(int i = 0;; i++){
    struct brcmf_core *d11 = brcmf_find_core(chip, BCMA_CORE_80211, i);
    if(d11 == 0)
      break;
    if(brcmf_core_disable(func, d11,
                          D11_BCMA_IOCTL_PHYRESET |
                          D11_BCMA_IOCTL_PHYCLOCKEN,
                          D11_BCMA_IOCTL_PHYCLOCKEN) < 0)
      return -1;
  }
  printf("brcmfmac: CR4 halted; wireless cores held in reset\n");
  return 0;
}

static int
brcmf_write_firmware(struct sdio_func *func, struct brcmf_chip *chip,
                     struct fat32_file *code, uint32 *reset_vector)
{
  struct fat32_reader reader;
  uint32 offset = 0, verify;

  if(code->size < 4 || code->size > chip->ramsize ||
     fat32readerinit(&reader, code) < 0)
    return -1;
  while(offset < code->size){
    int chunk = code->size - offset;
    if(chunk > BRCMF_IO_CHUNK)
      chunk = BRCMF_IO_CHUNK;
    if(fat32readnext(&reader, brcmf_io_buf, chunk) != chunk)
      return -1;
    if(offset == 0)
      *reset_vector = (uint32)brcmf_io_buf[0] |
                      ((uint32)brcmf_io_buf[1] << 8) |
                      ((uint32)brcmf_io_buf[2] << 16) |
                      ((uint32)brcmf_io_buf[3] << 24);
    if(brcmf_backplane_transfer(func, 1, chip->rambase + offset,
                                brcmf_io_buf, chunk) < 0)
      return -1;
    offset += chunk;
    if((offset & 0xffff) == 0 || offset == code->size)
      printf("brcmfmac: firmware download %d/%d\n", offset, code->size);
  }
  if(brcmf_backplane_read32(func, chip->rambase, &verify) < 0 ||
     verify != *reset_vector){
    printf("brcmfmac: firmware verify failed got=%x expected=%x\n",
           verify, *reset_vector);
    return -1;
  }
  printf("brcmfmac: firmware reset-vector=%x verified\n", *reset_vector);
  return 0;
}

static int
brcmf_prepare_nvram(struct fat32_file *file, uint32 *length)
{
  uint32 input, output = 0, pos = 0, pad;

  if(file->size == 0 || file->size >= BRCMF_NVRAM_MAX - 8 ||
     fat32pread(file, 0, brcmf_nvram_raw, file->size) != (int)file->size)
    return -1;
  input = file->size;
  while(pos < input){
    uint32 start, end;
    while(pos < input && (brcmf_nvram_raw[pos] == ' ' ||
                          brcmf_nvram_raw[pos] == '\t' ||
                          brcmf_nvram_raw[pos] == '\r' ||
                          brcmf_nvram_raw[pos] == '\n'))
      pos++;
    start = pos;
    while(pos < input && brcmf_nvram_raw[pos] != '\r' &&
          brcmf_nvram_raw[pos] != '\n')
      pos++;
    end = pos;
    while(end > start && (brcmf_nvram_raw[end - 1] == ' ' ||
                          brcmf_nvram_raw[end - 1] == '\t'))
      end--;
    if(start < end && brcmf_nvram_raw[start] != '#'){
      int have_equal = 0;
      for(uint32 i = start; i < end; i++)
        if(brcmf_nvram_raw[i] == '=')
          have_equal = 1;
      if(have_equal){
        if(output + end - start + 1 >= BRCMF_NVRAM_MAX - 4)
          return -1;
        memmove(brcmf_nvram_data + output, brcmf_nvram_raw + start,
                end - start);
        output += end - start;
        brcmf_nvram_data[output++] = 0;
      }
    }
  }
  // Include a second NUL, align the variables to a word, then append the
  // Broadcom word-count/complement token.
  pad = output;
  output = (output + 1 + 3) & ~3U;
  if(output + 4 > BRCMF_NVRAM_MAX)
    return -1;
  while(pad < output)
    brcmf_nvram_data[pad++] = 0;
  {
    uint32 words = output / 4;
    uint32 token = (~words << 16) | (words & 0xffff);
    brcmf_nvram_data[output + 0] = token;
    brcmf_nvram_data[output + 1] = token >> 8;
    brcmf_nvram_data[output + 2] = token >> 16;
    brcmf_nvram_data[output + 3] = token >> 24;
  }
  *length = output + 4;
  return 0;
}

static int
brcmf_write_nvram(struct sdio_func *func, struct brcmf_chip *chip,
                  struct fat32_file *file)
{
  uint32 length, address, verify, expected;
  if(brcmf_prepare_nvram(file, &length) < 0 || length >= chip->ramsize)
    return -1;
  address = chip->rambase + chip->ramsize - length;
  if(brcmf_backplane_transfer(func, 1, address, brcmf_nvram_data, length) < 0)
    return -1;
  expected = (uint32)brcmf_nvram_data[length - 4] |
             ((uint32)brcmf_nvram_data[length - 3] << 8) |
             ((uint32)brcmf_nvram_data[length - 2] << 16) |
             ((uint32)brcmf_nvram_data[length - 1] << 24);
  if(brcmf_backplane_read32(func, address + length - 4, &verify) < 0 ||
     verify != expected)
    return -1;
  printf("brcmfmac: NVRAM %d bytes written at %x token=%x\n",
         length, address, expected);
  return 0;
}

static int
brcmf_set_active(struct sdio_func *func, struct brcmf_chip *chip,
                 uint32 reset_vector)
{
  if(brcmf_backplane_write32(func, chip->sdio->base + SDIO_CORE_INTSTATUS,
                             0xffffffffU) < 0 ||
     brcmf_backplane_write32(func, 0, reset_vector) < 0 ||
     brcmf_core_reset(func, chip->cr4, ARMCR4_BCMA_IOCTL_CPUHALT, 0, 0) < 0)
    return -1;
  printf("brcmfmac: CR4 released reset-vector=%x\n", reset_vector);
  return 0;
}

static int
brcmf_start_f2(struct sdio_func *func, struct brcmf_chip *chip)
{
  struct sdio_func *func2 = func->host->functions[1];
  uint8 value;
  uint32 mailbox = 0;

  value = 0;
  if(sdio_cmd52(func, 0, SBSDIO_FUNC1_CHIPCLKCSR, &value) < 0)
    return -1;
  value |= SBSDIO_FORCE_HT;
  if(sdio_cmd52(func, 1, SBSDIO_FUNC1_CHIPCLKCSR, &value) < 0 ||
     brcmf_backplane_write32(func,
       chip->sdio->base + SDIO_CORE_TOSBMAILBOXDATA,
       SDPCM_PROT_VERSION << SMB_DATA_VERSION_SHIFT) < 0)
    return -1;
  if(func2 == 0 || sdio_enable_func(func2) < 0){
    printf("brcmfmac: firmware did not make F2 ready\n");
    return -1;
  }
  if(brcmf_backplane_write32(func,
       chip->sdio->base + SDIO_CORE_HOSTINTMASK, HOSTINTMASK) < 0)
    return -1;

  value = 0x60;
  if(sdio_cmd52(func, 1, SBSDIO_WATERMARK, &value) < 0)
    return -1;
  value = 0;
  if(sdio_cmd52(func, 0, SBSDIO_DEVICE_CTL, &value) < 0)
    return -1;
  value |= 0x10;
  if(sdio_cmd52(func, 1, SBSDIO_DEVICE_CTL, &value) < 0)
    return -1;
  value = 0xd0;
  if(sdio_cmd52(func, 1, SBSDIO_FUNC1_MESBUSYCTRL, &value) < 0)
    return -1;

  for(int i = 0; i < 3000; i++){
    if(brcmf_backplane_read32(func,
         chip->sdio->base + SDIO_CORE_TOHOSTMAILBOXDATA, &mailbox) < 0)
      return -1;
    if(mailbox & HMB_DATA_FWHALT){
      printf("brcmfmac: firmware reported halt mailbox=%x\n", mailbox);
      return -1;
    }
    if(mailbox & (HMB_DATA_DEVREADY | HMB_DATA_FWREADY))
      break;
    brcmf_delay_us(1000);
  }
  if((mailbox & (HMB_DATA_DEVREADY | HMB_DATA_FWREADY)) == 0){
    printf("brcmfmac: timeout waiting FWREADY mailbox=%x\n", mailbox);
    return -1;
  }
  brcmf_backplane_write32(func,
    chip->sdio->base + SDIO_CORE_TOSBMAILBOX, SMB_INT_ACK);
  printf("brcmfmac: F2 ready mailbox=%x protocol=%d\n", mailbox,
         (mailbox & HMB_DATA_VERSION_MASK) >> HMB_DATA_VERSION_SHIFT);
  return 0;
}

// Function 2 is a FIFO onto the ChipCommon backplane address.  A complete
// SDPCM frame may be split into several byte-mode CMD53 operations; the FIFO
// address itself must not be incremented between operations.
static int
brcmf_f2_transfer(struct brcmf_bus_state *bus, int write, void *buffer, int length)
{
  uint8 *p = buffer;
  uint32 addr;
  int chunk;

  if(!brcmf_bus.ready || length <= 0)
    return -1;
  if(brcmf_set_backplane_window(brcmf_bus.f1, brcmf_bus.cc_base) < 0)
    return -1;
  addr = (brcmf_bus.cc_base & SBSDIO_SB_OFT_ADDR_MASK) |
         SBSDIO_SB_ACCESS_2_4B_FLAG;
  while(length){
    chunk = length;
    if(chunk > (int)brcmf_transfer_limit)
      chunk = brcmf_transfer_limit;
    if(sdio_cmd53(brcmf_bus.f2, write, addr, 0, p, chunk) < 0)
      return -1;
    p += chunk;
    length -= chunk;
  }
  return 0;
}

static void
brcmf_sdpcm_header(struct brcmf_bus_state *bus, uint8 *p, uint16 length,
                   uint8 channel)
{
  memset(p, 0, SDPCM_HEADER_LEN);
  p[0] = length;
  p[1] = length >> 8;
  p[2] = ~length;
  p[3] = (~length) >> 8;
  p[4] = brcmf_bus.tx_seq++;
  p[5] = channel;
  p[7] = SDPCM_HEADER_LEN;
}

static int
brcmf_send_ether_locked(struct brcmf_bus_state *bus, void *frame, int length)
{
  struct brcmf_bcdc_header *bcdc;
  int frame_len, wire_len;
  if(length < 14 || length > BRCMF_FRAME_MAX - SDPCM_HEADER_LEN - 4)
    return -1;
  memset(brcmf_bus.tx, 0, sizeof(brcmf_bus.tx));
  bcdc = (struct brcmf_bcdc_header*)(brcmf_bus.tx + SDPCM_HEADER_LEN);
  bcdc->flags = 2 << 4; // BCDC protocol version 2
  memmove(bcdc + 1, frame, length);
  frame_len = SDPCM_HEADER_LEN + sizeof(*bcdc) + length;
  wire_len = (frame_len + 3) & ~3;
  brcmf_sdpcm_header(bus, brcmf_bus.tx, frame_len, SDPCM_DATA_CHANNEL);
  return brcmf_f2_transfer(bus, 1, brcmf_bus.tx, wire_len);
}

static int
brcmf_wpa_send_m2_locked(struct brcmf_bus_state *bus, uint8 *m1, int m1len,
                         uint8 *ap)
{
  static const uint8 default_rsn_ie[] = {
    0x30, 0x14, 0x01, 0x00,             // RSN v1
    0x00, 0x0f, 0xac, 0x04,             // group cipher CCMP
    0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, // one pairwise cipher: CCMP
    0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, // one AKM: PSK
    0x00, 0x00                            // RSN capabilities
  };
  uint8 *rsn_ie = brcmf_bus.assoc_rsn_ie_len ?
                  brcmf_bus.assoc_rsn_ie : (uint8*)default_rsn_ie;
  int rsn_ie_len = brcmf_bus.assoc_rsn_ie_len ?
                   brcmf_bus.assoc_rsn_ie_len : sizeof(default_rsn_ie);
  uint8 *frame = brcmf_bus.wpa_tx, *eapol, *key;
  int body_len = 95 + rsn_ie_len;
  int total = 14 + 4 + body_len;

  if(!brcmf_bus.pmk_ready || m1len < 99)
    return -1;
  memset(frame, 0, sizeof(brcmf_bus.wpa_tx));
  memmove(frame, ap, 6);
  memmove(frame + 6, brcmf_bus.mac, 6);
  frame[12] = 0x88; frame[13] = 0x8e;
  eapol = frame + 14;
  eapol[0] = m1[0]; // use the authenticator's EAPOL version
  eapol[1] = 3;     // EAPOL-Key
  eapol[2] = body_len >> 8; eapol[3] = body_len;
  key = eapol + 4;
  key[0] = m1[4];
  // Preserve the authenticator's descriptor version.  M2 sets only Pairwise
  // and MIC; ACK/Install/Secure/Encrypted-Key-Data must remain clear.
  key[1] = 0x01;
  key[2] = 0x08 | (m1[6] & 0x07);
  // IEEE 802.11i 8.5.3.2 requires Key Length to be zero in message 2.
  // Copying M1's cipher key length (normally 16 for CCMP) makes an AP reject
  // M2 silently and retransmit M1 until its handshake timeout.
  key[3] = 0;
  key[4] = 0;
  memmove(key + 5, m1 + 9, 8);      // replay counter
  memmove(key + 13, brcmf_bus.snonce, 32);
  key[93] = rsn_ie_len >> 8;
  key[94] = rsn_ie_len;
  memmove(key + 95, rsn_ie, rsn_ie_len);
  // MIC bytes key[77..92] are still zero while calculating the MIC.
  wpa_eapol_mic(brcmf_bus.ptk, eapol, 4 + body_len, key + 77);
  if(brcmf_send_ether_locked(bus, frame, total) < 0)
    return -1;
  brcmf_bus.m2_sent++;
  printf("brcmfmac: sent WPA2 EAPOL M2 replay=%x%x%x%x rsn=%d%s\n",
         key[9], key[10], key[11], key[12], rsn_ie_len,
         brcmf_bus.assoc_rsn_ie_len ? " firmware" : " fallback");
  return 0;
}

static int
brcmf_wpa_send_m4_locked(struct brcmf_bus_state *bus)
{
  uint8 frame[14 + 4 + 95], *eapol, *key;
  int body_len = 95;

  memset(frame, 0, sizeof(frame));
  memmove(frame, brcmf_bus.ap, 6);
  memmove(frame + 6, brcmf_bus.mac, 6);
  frame[12] = 0x88; frame[13] = 0x8e;
  eapol = frame + 14;
  eapol[0] = 2;
  eapol[1] = 3;
  eapol[2] = body_len >> 8; eapol[3] = body_len;
  key = eapol + 4;
  key[0] = brcmf_bus.m3_desc_type;
  // Pairwise + MIC + Secure, preserving the M3 descriptor version.
  key[1] = 0x03;
  key[2] = 0x08 | brcmf_bus.m3_desc_version;
  memmove(key + 5, brcmf_bus.m3_replay, 8);
  wpa_eapol_mic(brcmf_bus.ptk, eapol, 4 + body_len, key + 77);
  if(brcmf_send_ether_locked(bus, frame, sizeof(frame)) < 0)
    return -1;
  printf("brcmfmac: sent WPA2 EAPOL M4\n");
  return 0;
}

static void
brcmf_wpa_rx_locked(struct brcmf_bus_state *bus, uint8 *ether, int length)
{
  uint8 *eapol, *key;
  uint16 body_len, info;
  if(length < 14 + 4 + 95)
    return;
  eapol = ether + 14;
  if(eapol[1] != 3)
    return;
  body_len = ((uint16)eapol[2] << 8) | eapol[3];
  if(body_len < 95 || 14 + 4 + body_len > length)
    return;
  key = eapol + 4;
  info = ((uint16)key[1] << 8) | key[2];
  if((info & 0x0088) == 0x0088 && (info & 0x0100) == 0){
    // M1: pairwise key, ACK set, MIC absent.
    // Retransmitted M1 frames may carry a newer replay counter but the same
    // ANonce.  Reuse SNonce/PTK for that handshake, as a normal supplicant
    // does, instead of inventing a different M2 on every retry.
    if(!brcmf_bus.handshake_started ||
       memcmp(brcmf_bus.anonce, key + 13, 32) != 0){
      memmove(brcmf_bus.anonce, key + 13, 32);
      wpa_make_snonce(brcmf_bus.pmk, key + 13, r_cntvct_el0(),
                      brcmf_bus.snonce);
      brcmf_bus.handshake_started = 1;
    }
    wpa_derive_ptk(brcmf_bus.pmk, ether + 6, brcmf_bus.mac,
                   key + 13, brcmf_bus.snonce, brcmf_bus.ptk);
    if(brcmf_wpa_send_m2_locked(bus, eapol, 4 + body_len, ether + 6) < 0)
      printf("brcmfmac: failed to transmit WPA2 EAPOL M2\n");
  } else if((info & 0x0188) == 0x0188){
    uint8 received_mic[16], calculated_mic[16], plain[64];
    int data_len = ((int)key[93] << 8) | key[94];
    int plain_len, pos, found = 0;
    printf("brcmfmac: received WPA2 EAPOL M3 key-info=%x data=%d\n",
           info, data_len);
    memmove(received_mic, key + 77, 16);
    memset(key + 77, 0, 16);
    wpa_eapol_mic(brcmf_bus.ptk, eapol, 4 + body_len, calculated_mic);
    memmove(key + 77, received_mic, 16);
    if(memcmp(received_mic, calculated_mic, 16) != 0){
      printf("brcmfmac: M3 MIC verification failed\n");
      return;
    }
    if(brcmf_bus.handshake_done){
      brcmf_wpa_send_m4_locked(bus);
      return;
    }
    if((info & 0x1000) == 0 || data_len > (int)sizeof(plain) + 8 ||
       95 + data_len > body_len){
      printf("brcmfmac: M3 encrypted key data invalid\n");
      return;
    }
    plain_len = wpa_aes_unwrap(brcmf_bus.ptk + 16, key + 95,
                               data_len, plain);
    if(plain_len < 0){
      printf("brcmfmac: M3 AES key unwrap failed\n");
      return;
    }
    // Locate the RSN GTK KDE: dd,len,00:0f:ac,01,keyid/reserved,GTK.
    for(pos = 0; pos + 2 <= plain_len; pos += 2 + plain[pos + 1]){
      int len = 2 + plain[pos + 1];
      if(pos + len > plain_len)
        break;
      if(plain[pos] == 0xdd && len >= 24 &&
         plain[pos+2] == 0x00 && plain[pos+3] == 0x0f &&
         plain[pos+4] == 0xac && plain[pos+5] == 0x01){
        brcmf_bus.gtk_index = plain[pos+6] & 3;
        memmove(brcmf_bus.gtk, plain + pos + 8, 16);
        found = 1;
        break;
      }
    }
    if(!found){
      printf("brcmfmac: M3 contains no CCMP GTK KDE\n");
      return;
    }
    memmove(brcmf_bus.ap, ether + 6, 6);
    memmove(brcmf_bus.m3_replay, key + 5, 8);
    brcmf_bus.m3_desc_type = key[0];
    brcmf_bus.m3_desc_version = info & 7;
    brcmf_bus.keys_pending = 1;
    printf("brcmfmac: M3 verified; GTK index=%d pending install\n",
           brcmf_bus.gtk_index);
  }
}

// Read one firmware frame.  Firmware pads the first transfer to 64 bytes,
// which lets the host discover the full frame length before reading the tail.
// Returns 1 for a frame, 0 when no frame is pending, and -1 on corruption/I/O.
static int
brcmf_rx_frame_locked(struct brcmf_bus_state *bus, uint8 *channel,
                      uint8 **payload, int *payload_len)
{
  uint32 intstatus;
  uint16 length, check;
  int rest, rounded, offset;

  if(brcmf_backplane_read32(brcmf_bus.f1,
       brcmf_bus.sdio_base + SDIO_CORE_INTSTATUS, &intstatus) < 0)
    return -1;
  if((intstatus & I_HMB_FRAME_IND) == 0)
    return 0;

  memset(brcmf_bus.rx, 0, sizeof(brcmf_bus.rx));
  if(brcmf_f2_transfer(bus, 0, brcmf_bus.rx, 64) < 0)
    return -1;
  length = brcmf_bus.rx[0] | ((uint16)brcmf_bus.rx[1] << 8);
  check = brcmf_bus.rx[2] | ((uint16)brcmf_bus.rx[3] << 8);
  if(length == 0 && check == 0)
    return 0;
  if((uint16)(length ^ check) != 0xffff ||
     length < SDPCM_HEADER_LEN || length > BRCMF_FRAME_MAX){
    printf("brcmfmac: bad SDPCM header len=%d check=%x\n", length, check);
    return -1;
  }
  if(length > 64){
    rest = length - 64;
    rounded = (rest + 3) & ~3;
    if(64 + rounded > BRCMF_FRAME_MAX ||
       brcmf_f2_transfer(bus, 0, brcmf_bus.rx + 64, rounded) < 0)
      return -1;
  }
  *channel = brcmf_bus.rx[5] & 0x0f;
  offset = brcmf_bus.rx[7];
  if(offset < SDPCM_HEADER_LEN || offset > length)
    return -1;
  *payload = brcmf_bus.rx + offset;
  *payload_len = length - offset;
  return 1;
}

static int
brcmf_rx_dispatch_locked(struct brcmf_bus_state *bus)
{
  struct brcmf_bcdc_dcmd *dcmd;
  uint8 channel, *payload;
  int length, result;

  result = brcmf_rx_frame_locked(bus, &channel, &payload, &length);
  if(result <= 0)
    return result;
  if(channel == SDPCM_CONTROL_CHANNEL){
    if(length < (int)sizeof(*dcmd))
      return -1;
    dcmd = (struct brcmf_bcdc_dcmd*)payload;
    brcmf_bus.ctl_id = dcmd->flags >> 16;
    brcmf_bus.ctl_len = length;
    if(brcmf_bus.ctl_len > BRCMF_DCMD_MAX)
      brcmf_bus.ctl_len = BRCMF_DCMD_MAX;
    memmove(brcmf_bus.ctl, payload, brcmf_bus.ctl_len);
  } else if(channel == SDPCM_EVENT_CHANNEL){
    // Broadcom events use an Ethernet header, a 10-byte bcmeth header and a
    // network-byte-order brcm_event_msg.  Printing status/reason makes join
    // failures distinguishable from the later GET_BSSID=-17 polling result.
    if(length >= 14 + 10 + (int)sizeof(struct brcmf_event_msg)){
      uint8 *ether = payload;
      struct brcmf_event_msg *event =
        (struct brcmf_event_msg*)(ether + 14 + 10);
      uint16 type = ((uint16)ether[12] << 8) | ether[13];
      if(type == 0x886c){
        uint32 event_type = ((uint32)((uint8*)&event->event_type)[0] << 24) |
          ((uint32)((uint8*)&event->event_type)[1] << 16) |
          ((uint32)((uint8*)&event->event_type)[2] << 8) |
          ((uint8*)&event->event_type)[3];
        uint32 status = ((uint32)((uint8*)&event->status)[0] << 24) |
          ((uint32)((uint8*)&event->status)[1] << 16) |
          ((uint32)((uint8*)&event->status)[2] << 8) |
          ((uint8*)&event->status)[3];
        uint32 reason = ((uint32)((uint8*)&event->reason)[0] << 24) |
          ((uint32)((uint8*)&event->reason)[1] << 16) |
          ((uint32)((uint8*)&event->reason)[2] << 8) |
          ((uint8*)&event->reason)[3];
        printf("brcmfmac: event type=%d status=%d reason=%d\n",
               event_type, status, reason);
      }
    }
  } else if(channel == SDPCM_DATA_CHANNEL){
    struct brcmf_bcdc_header *bcdc;
    uint8 *ether;
    int skip;
    if(length < (int)sizeof(*bcdc))
      return -1;
    bcdc = (struct brcmf_bcdc_header*)payload;
    skip = sizeof(*bcdc) + bcdc->data_offset * 4;
    if(skip > length)
      return -1;
    ether = payload + skip;
    length -= skip;
    if(length >= 14){
      uint16 type = ((uint16)ether[12] << 8) | ether[13];
      if(type == 0x888e)
        printf("brcmfmac: received EAPOL frame len=%d key-info=%x%x\n",
               length, length >= 21 ? ether[19] : 0,
               length >= 21 ? ether[20] : 0);
      if(type == 0x888e)
        brcmf_bus.eapol_rx++;
      if(type == 0x888e)
        brcmf_wpa_rx_locked(bus, ether, length);
      else if(brcmf_bus.handshake_done &&
              length <= (int)sizeof(brcmf_bus.net_rx) &&
              brcmf_bus.net_rx_len == 0){
        memmove(brcmf_bus.net_rx, ether, length);
        brcmf_bus.net_rx_len = length;
      }
    }
  }
  // Event and data channels are enabled after the control plane has supplied
  // the MAC address, CLM and association state.
  return 1;
}

static int
brcmfmac_xmit(struct net_device *netdev, void *frame, int length)
{
  struct brcmf_bus_state *bus = netdev ? netdev->priv : 0;
  int result;
  if(bus == 0)
    return -1;
  if(!brcmf_bus.handshake_done)
    return -1;
  acquire(&brcmf_bus.lock);
  result = brcmf_send_ether_locked(bus, frame, length);
  release(&brcmf_bus.lock);
  return result;
}

static void
brcmfmac_poll_device(struct net_device *netdev)
{
  struct brcmf_bus_state *bus = netdev ? netdev->priv : 0;
  if(bus && bus->ready)
    napi_schedule(&bus->napi);
}

static int
brcmf_napi_poll(struct napi_struct *napi, int budget)
{
  struct brcmf_bus_state *bus = napi && napi->dev ? napi->dev->priv : 0;
  int work_done = 0, drained = 0;

  if(bus == 0 || !bus->used || !bus->ready){
    napi_complete_done(napi, 0);
    return 0;
  }
  acquire(&brcmf_bus.lock);
  brcmf_bus.polling = 1;
  while(work_done < budget){
    int length = 0;
    int r = brcmf_rx_dispatch_locked(bus);
    if(r <= 0){
      drained = 1;
      break;
    }
    work_done++;
    if(brcmf_bus.net_rx_len){
      length = brcmf_bus.net_rx_len;
      memmove(brcmf_bus.net_deliver, brcmf_bus.net_rx, length);
      brcmf_bus.net_rx_len = 0;
    }
    if(length){
      release(&brcmf_bus.lock);
      net_rx_dev(napi->dev, brcmf_bus.net_deliver, length);
      acquire(&brcmf_bus.lock);
    }
  }
  brcmf_bus.polling = 0;
  release(&brcmf_bus.lock);

  if(drained && napi_complete_done(napi, work_done))
    arasan_sdio_irq_complete();
  return work_done;
}

static struct brcmf_bus_state *
brcmf_bus_for_work(struct work_struct *work)
{
  for(int i = 0; i < BRCMF_MAX_DEVICES; i++)
    if(work == &brcmf_devices[i].state_work.work)
      return &brcmf_devices[i];
  return 0;
}

static void
brcmf_state_worker(struct work_struct *work)
{
  struct brcmf_bus_state *bus = brcmf_bus_for_work(work);
  uint64 now;
  int active;
  if(bus == 0 || !bus->used || !bus->ready)
    return;
  now = workqueue_now();
  acquire(&bus->lock);
  active = bus->connect_active;
  if(active && (long)(now - bus->connect_deadline) >= 0){
    bus->connect_timedout = 1;
    bus->connect_active = 0;
    active = 0;
  }
  release(&bus->lock);
  if(!active)
    return;
  // Association/control events normally arrive via DAT1 IRQ.  Queue RX only
  // when the controller reports a pending card interrupt; this watchdog does
  // not restore the old unconditional SDIO polling path.
  if(arasan_sdio_irq_pending())
    napi_schedule(&bus->napi);
  // Serialize the re-arm with brcmf_connect_watch_stop().  Without this
  // second check a running callback could enqueue itself after the stop path
  // had already removed the old delayed instance.
  acquire(&bus->lock);
  if(bus->connect_active)
    queue_delayed_work(bus->wq, &bus->state_work, 5);
  release(&bus->lock);
}

static void
brcmf_connect_watch_start(struct brcmf_bus_state *bus)
{
  acquire(&bus->lock);
  bus->connect_timedout = 0;
  bus->connect_active = 1;
  bus->connect_deadline = workqueue_now() + 1000; // 10 seconds
  release(&bus->lock);
  queue_delayed_work(bus->wq, &bus->state_work, 5);
}

static int
brcmf_connect_watch_timedout(struct brcmf_bus_state *bus)
{
  int timedout;
  acquire(&bus->lock);
  timedout = bus->connect_timedout;
  release(&bus->lock);
  return timedout;
}

static void
brcmf_connect_watch_stop(struct brcmf_bus_state *bus)
{
  acquire(&bus->lock);
  bus->connect_active = 0;
  release(&bus->lock);
  cancel_delayed_work_sync(&bus->state_work);
}

void
brcmfmac_sdio_irq(void)
{
  for(int i = 0; i < BRCMF_MAX_DEVICES; i++){
    struct brcmf_bus_state *bus = &brcmf_devices[i];
    if(bus->used && bus->ready && bus->wq)
      napi_schedule(&bus->napi);
  }
}

static int
brcmf_dcmd(struct brcmf_bus_state *bus, uint32 command, void *data, int length,
           int set)
{
  struct brcmf_bcdc_dcmd *request, *reply;
  uint16 id;
  int frame_len, wire_len, n;
  uint64 deadline;

  if(length < 0 || length > BRCMF_DCMD_MAX - (int)sizeof(*request))
    return -1;
  acquire(&brcmf_bus.lock);
  memset(brcmf_bus.tx, 0, sizeof(brcmf_bus.tx));
  request = (struct brcmf_bcdc_dcmd*)(brcmf_bus.tx + SDPCM_HEADER_LEN);
  id = ++brcmf_bus.reqid;
  request->cmd = command;
  request->len = length;
  request->flags = ((uint32)id << 16) | (set ? BCDC_DCMD_SET : 0);
  if(data && length)
    memmove(request + 1, data, length);
  frame_len = SDPCM_HEADER_LEN + sizeof(*request) + length;
  wire_len = (frame_len + 3) & ~3;
  brcmf_sdpcm_header(bus, brcmf_bus.tx, frame_len, SDPCM_CONTROL_CHANNEL);
  brcmf_bus.ctl_len = 0;
  if(brcmf_f2_transfer(bus, 1, brcmf_bus.tx, wire_len) < 0){
    release(&brcmf_bus.lock);
    return -1;
  }

  deadline = r_cntvct_el0() + (uint64)r_cntfrq_el0() * 3;
  while(r_cntvct_el0() < deadline){
    int r = brcmf_rx_dispatch_locked(bus);
    if(r < 0){
      release(&brcmf_bus.lock);
      return -1;
    }
    if(brcmf_bus.ctl_len >= (int)sizeof(*reply) &&
       brcmf_bus.ctl_id == id)
      break;
    brcmf_delay_us(200);
  }
  if(brcmf_bus.ctl_len < (int)sizeof(*reply) || brcmf_bus.ctl_id != id){
    printf("brcmfmac: BCDC command %d response timeout id=%d\n", command, id);
    release(&brcmf_bus.lock);
    return -1;
  }
  reply = (struct brcmf_bcdc_dcmd*)brcmf_bus.ctl;
  if((reply->flags & BCDC_DCMD_ERROR) || reply->status != 0){
    // GET_BSSID returning NOTASSOCIATED is expected while an asynchronous
    // join is in progress; printing it forty times hides the useful event.
    if(command != BRCMF_C_GET_BSSID || reply->status != -17)
      printf("brcmfmac: BCDC command %d failed status=%d flags=%x\n",
             command, reply->status, reply->flags);
    release(&brcmf_bus.lock);
    return -1;
  }
  n = brcmf_bus.ctl_len - sizeof(*reply);
  if(n > length)
    n = length;
  if(data && n > 0)
    memmove(data, reply + 1, n);
  release(&brcmf_bus.lock);
  return n;
}

static int
brcmf_iovar(struct brcmf_bus_state *bus, char *name, void *data, int length, int set)
{
  uint8 *buffer = brcmf_bus.iovar_buf;
  int namelen = strlen(name) + 1;
  int total;

  if(namelen + length > (int)sizeof(brcmf_bus.iovar_buf))
    return -1;
  acquire(&brcmf_bus.iovar_lock);
  memset(buffer, 0, sizeof(brcmf_bus.iovar_buf));
  memmove(buffer, name, namelen);
  if(set && data && length)
    memmove(buffer + namelen, data, length);
  // A GET request carries the iovar name at the beginning of the same
  // buffer that firmware later overwrites with the reply.  Its BCDC length
  // is the actual name plus requested result capacity, not the whole local
  // scratch buffer (which would exceed BRCMF_DCMD_MAX after the BCDC header).
  total = namelen + length;
  if(brcmf_dcmd(bus, set ? BRCMF_C_SET_VAR : BRCMF_C_GET_VAR,
                buffer, total, set) < 0){
    release(&brcmf_bus.iovar_lock);
    return -1;
  }
  if(!set && data && length)
    memmove(data, buffer, length);
  release(&brcmf_bus.iovar_lock);
  return 0;
}

static int __attribute__((unused))
brcmf_scan_for_ssid(struct brcmf_bus_state *bus, char *wanted)
{
  struct brcmf_scan_params params;
  uint8 *results = brcmf_bus.scan_results;
  uint32 count, record_len;
  int n, pos = 12, found = 0;

  memset(&params, 0, sizeof(params));
  memset(params.bssid, 0xff, sizeof(params.bssid));
  params.bss_type = 2;       // any infrastructure/IBSS type
  params.scan_type = 0;      // active scan
  params.nprobes = -1;
  params.active_time = -1;
  params.passive_time = -1;
  params.home_time = -1;
  params.channel_num = 0;
  printf("brcmfmac: active scan started\n");
  if(brcmf_dcmd(bus, BRCMF_C_SCAN, &params, sizeof(params), 1) < 0){
    printf("brcmfmac: active scan command failed\n");
    return -1;
  }
  for(int wait = 0; wait < 12; wait++){
    acquire(&brcmf_bus.lock);
    for(int pending = 0; pending < 8; pending++)
      if(brcmf_rx_dispatch_locked(bus) <= 0)
        break;
    release(&brcmf_bus.lock);
    brcmf_delay_us(250000);
  }

  memset(results, 0, BRCMF_SCAN_RESULTS_LEN);
  *(uint32*)results = BRCMF_SCAN_RESULTS_LEN;
  n = brcmf_dcmd(bus, BRCMF_C_SCAN_RESULTS, results,
                 BRCMF_SCAN_RESULTS_LEN, 0);
  if(n < 12){
    printf("brcmfmac: cannot read scan results\n");
    return -1;
  }
  count = *(uint32*)(results + 8);
  printf("brcmfmac: scan results count=%d\n", count);
  for(uint32 i = 0; i < count && pos + 19 <= n; i++){
    uint8 ssid_len;
    char name[33];
    record_len = *(uint32*)(results + pos + 4);
    if(record_len < 19 || pos + (int)record_len > n)
      break;
    ssid_len = results[pos + 18];
    if(ssid_len > 32 || 19 + ssid_len > record_len)
      ssid_len = 0;
    memset(name, 0, sizeof(name));
    memmove(name, results + pos + 19, ssid_len);
    printf("brcmfmac: scan ssid=%s bssid=%x:%x:%x:%x:%x:%x\n",
           name, results[pos + 8], results[pos + 9], results[pos + 10],
           results[pos + 11], results[pos + 12], results[pos + 13]);
    if(strlen(wanted) == ssid_len &&
       strncmp(name, wanted, ssid_len) == 0)
      found = 1;
    pos += record_len;
  }
  if(!found)
    printf("brcmfmac: target SSID %s not visible in scan\n", wanted);
  return found ? 0 : -1;
}

static int
brcmf_download_clm(struct brcmf_bus_state *bus, struct fat32_file *file)
{
  uint8 buffer[sizeof(struct brcmf_dload_data) + BRCMF_CLM_CHUNK];
  struct brcmf_dload_data *download = (struct brcmf_dload_data*)buffer;
  uint32 offset = 0;
  int chunk;

  if(file == 0 || file->size == 0)
    return 0;
  while(offset < file->size){
    chunk = file->size - offset;
    if(chunk > BRCMF_CLM_CHUNK)
      chunk = BRCMF_CLM_CHUNK;
    memset(buffer, 0, sizeof(buffer));
    download->flag = DLOAD_HANDLER_VER << DLOAD_FLAG_VER_SHIFT;
    if(offset == 0)
      download->flag |= DL_BEGIN;
    if(offset + chunk == file->size)
      download->flag |= DL_END;
    download->type = DL_TYPE_CLM;
    download->len = chunk;
    if(fat32pread(file, offset, download->data, chunk) != chunk ||
       brcmf_iovar(bus, "clmload", buffer,
                   sizeof(struct brcmf_dload_data) + chunk, 1) < 0)
      return -1;
    offset += chunk;
  }
  printf("brcmfmac: CLM %d bytes downloaded\n", file->size);
  return 0;
}

static int
brcmf_control_plane_start(struct brcmf_bus_state *bus, struct sdio_func *func,
                          struct brcmf_chip *chip,
                          struct fat32_file *clm, int have_clm)
{
  char version[256];
  uint8 mac[6];

  brcmf_bus.f1 = func;
  brcmf_bus.f2 = func->host->functions[1];
  brcmf_bus.cc_base = chip->cc->base;
  brcmf_bus.sdio_base = chip->sdio->base;
  brcmf_bus.tx_seq = 0;
  brcmf_bus.reqid = 0;
  brcmf_bus.ctl_len = 0;
  brcmf_bus.ready = 1;

  memset(version, 0, sizeof(version));
  if(brcmf_iovar(bus, "ver", version, sizeof(version) - 1, 0) < 0){
    printf("brcmfmac: cannot exchange BCDC control frames\n");
    return -1;
  }
  version[sizeof(version) - 1] = 0;
  printf("brcmfmac: firmware version %s\n", version);
  if(have_clm && brcmf_download_clm(bus, clm) < 0){
    printf("brcmfmac: CLM download failed\n");
    return -1;
  }
  if(brcmf_iovar(bus, "cur_etheraddr", mac, sizeof(mac), 0) < 0){
    printf("brcmfmac: cannot read Wi-Fi MAC address\n");
    return -1;
  }
  printf("brcmfmac: wlan0 mac=%x:%x:%x:%x:%x:%x\n",
         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  memmove(brcmf_bus.mac, mac, sizeof(mac));
  if(brcmf_dcmd(bus, BRCMF_C_UP, 0, 0, 1) < 0){
    printf("brcmfmac: cannot bring firmware interface up\n");
    return -1;
  }
  brcmf_bus.netdev.dev.name = "wlan0";
  brcmf_bus.netdev.dev.parent = &func->dev;
  brcmf_bus.netdev.ops = &brcmf_netdev_ops;
  brcmf_bus.netdev.priv = bus;
  brcmf_bus.netdev.mtu = NET_MTU;
  memmove(brcmf_bus.netdev.mac, mac, sizeof(mac));
  if(register_netdev(&brcmf_bus.netdev) < 0){
    printf("brcmfmac: cannot register wlan0\n");
    return -1;
  }
  if(sdio_claim_irq(func) < 0){
    printf("brcmfmac: cannot enable SDIO function interrupt\n");
    unregister_netdev(&brcmf_bus.netdev);
    return -1;
  }
  napi_enable(&brcmf_bus.napi);
  arasan_sdio_irq_enable();
  printf("brcmfmac: SDIO DAT1 IRQ receive enabled\n");
  printf("brcmfmac: control plane ready\n");
  return 0;
}

static int
brcmf_bsscfg_iovar_int(struct brcmf_bus_state *bus, char *name, uint32 value)
{
  char full[64];
  uint32 values[2];
  char *prefix = "bsscfg:";
  int n = 0;

  while(*prefix && n < (int)sizeof(full) - 1)
    full[n++] = *prefix++;
  while(*name && n < (int)sizeof(full) - 1)
    full[n++] = *name++;
  full[n] = 0;
  values[0] = 0; // primary interface bsscfg index
  values[1] = value;
  return brcmf_iovar(bus, full, values, sizeof(values), 1);
}

static int
brcmf_install_ccmp_key(struct brcmf_bus_state *bus, uint32 index, uint8 *data,
                       uint8 *peer, int primary)
{
  uint8 request[4 + sizeof(struct brcmf_wsec_key)];
  struct brcmf_wsec_key *key;
  uint32 bssidx = 0;

  memset(request, 0, sizeof(request));
  memmove(request, &bssidx, sizeof(bssidx));
  key = (struct brcmf_wsec_key*)(request + 4);
  key->index = index;
  key->len = 16;
  memmove(key->data, data, 16);
  key->algo = CRYPTO_ALGO_AES_CCM;
  if(primary)
    key->flags = WL_PRIMARY_KEY;
  if(peer)
    memmove(key->ea, peer, 6);
  return brcmf_iovar(bus, "bsscfg:wsec_key", request, sizeof(request), 1);
}

// Called outside brcmf_bus.lock.  M3 reception only stages key material;
// issuing a synchronous iovar while dispatching RX under that lock would
// recursively acquire it and deadlock.
static int
brcmf_finish_wpa_handshake(struct brcmf_bus_state *bus)
{
  if(!brcmf_bus.keys_pending)
    return 0;
  if(brcmf_install_ccmp_key(bus, 0, brcmf_bus.ptk + 32,
                            brcmf_bus.ap, 0) < 0){
    printf("brcmfmac: pairwise CCMP key install failed\n");
    return -1;
  }
  if(brcmf_install_ccmp_key(bus, brcmf_bus.gtk_index, brcmf_bus.gtk,
                            0, 1) < 0){
    printf("brcmfmac: group CCMP key install failed\n");
    return -1;
  }
  acquire(&brcmf_bus.lock);
  if(brcmf_wpa_send_m4_locked(bus) < 0){
    release(&brcmf_bus.lock);
    printf("brcmfmac: EAPOL M4 transmit failed\n");
    return -1;
  }
  brcmf_bus.keys_pending = 0;
  brcmf_bus.handshake_done = 1;
  release(&brcmf_bus.lock);
  printf("brcmfmac: WPA2 four-way handshake complete\n");
  return 1;
}

// Read the exact RSN IE that firmware placed in its association request.
// Message 2 of the four-way handshake must repeat this IE bit-for-bit; an AP
// silently discards M2 if a locally reconstructed IE differs in capabilities
// or suite ordering.
static void
brcmf_read_assoc_rsn_ie(struct brcmf_bus_state *bus)
{
  uint8 *ies = brcmf_bus.assoc_ies;
  uint32 lengths[2];
  int req_len, pos;

  if(brcmf_bus.assoc_rsn_ie_len)
    return;
  // Although assoc_info begins with only two 32-bit lengths, Broadcom
  // firmware validates the advertised output capacity and returns
  // BCME_BUFTOOSHORT (-14) for an 8-byte request.  Use the normal
  // WL_ASSOC_INFO_MAX-style work buffer and then consume its first 8 bytes.
  memset(ies, 0, sizeof(brcmf_bus.assoc_ies));
  if(brcmf_iovar(bus, "assoc_info", ies,
                 sizeof(brcmf_bus.assoc_ies), 0) < 0)
    return;
  memmove(lengths, ies, sizeof(lengths));
  req_len = lengths[0];
  if(req_len <= 0 || req_len > (int)sizeof(brcmf_bus.assoc_ies))
    return;
  memset(ies, 0, sizeof(brcmf_bus.assoc_ies));
  if(brcmf_iovar(bus, "assoc_req_ies", ies, req_len, 0) < 0)
    return;
  for(pos = 0; pos + 2 <= req_len; pos += 2 + ies[pos + 1]){
    int len = 2 + ies[pos + 1];
    if(pos + len > req_len)
      break;
    if(ies[pos] == 48 && len <= (int)sizeof(brcmf_bus.assoc_rsn_ie)){
      memmove(brcmf_bus.assoc_rsn_ie, ies + pos, len);
      brcmf_bus.assoc_rsn_ie_len = len;
      printf("brcmfmac: association RSN IE captured len=%d caps=%x%x\n",
             len, len >= 2 ? ies[pos + len - 2] : 0,
             len >= 1 ? ies[pos + len - 1] : 0);
      return;
    }
  }
  printf("brcmfmac: association request has no usable RSN IE len=%d\n",
         req_len);
}

int
brcmfmac_connect(char *ssid, char *passphrase)
{
  struct net_device *netdev = netdev_find("wlan0");
  struct brcmf_bus_state *bus = netdev ? netdev->priv : 0;
  struct brcmf_wsec_pmk pmk;
  struct brcmf_ssid network;
  uint8 bssid[6];
  uint8 event_mask[BRCMF_EVENT_MASK_LEN];
  uint8 country[12];
  char country_name[3], country_code[3];
  uint32 value, country_rev;
  int ssid_len, key_len, host_eapol = 0, link_reported = 0;

  if(bus == 0 || !brcmf_bus.ready || ssid == 0 || passphrase == 0)
    return -1;
  ssid_len = strlen(ssid);
  key_len = strlen(passphrase);
  if(ssid_len <= 0 || ssid_len > 32 || key_len < 8 || key_len > 63)
    return -1;

  // Reinitialize the firmware interface for a fresh join.  This clears a
  // stale scan/auth state left by an earlier failed invocation of /bin/wifi.
  if(brcmf_dcmd(bus, BRCMF_C_DOWN, 0, 0, 1) < 0 ||
     brcmf_dcmd(bus, BRCMF_C_UP, 0, 0, 1) < 0){
    printf("brcmfmac: cannot reset firmware interface\n");
    return -1;
  }
  brcmf_delay_us(20000);

  // Subscribe to join/auth/assoc/link/PSK events before starting the join.
  // Firmware otherwise completes control commands but may leave the host
  // blind to the actual association failure status.
  memset(event_mask, 0, sizeof(event_mask));
  if(brcmf_iovar(bus, "event_msgs", event_mask,
                 sizeof(event_mask), 0) < 0)
    memset(event_mask, 0, sizeof(event_mask));
  static const uint8 join_events[] = {
    0, 1, 2, 3, 5, 6, 7, 8, 9, 11, 12, 16, 19, 46
  };
  for(uint i = 0; i < sizeof(join_events); i++)
    event_mask[join_events[i] / 8] |= 1U << (join_events[i] % 8);
  if(brcmf_iovar(bus, "event_msgs", event_mask,
                 sizeof(event_mask), 1) < 0)
    printf("brcmfmac: warning: cannot enable association events\n");

  // Linux brcmfmac disables minimum-power-consumption mode while scanning
  // and associating, then allows normal power management after link-up.
  value = 0;
  if(brcmf_iovar(bus, "mpc", &value, sizeof(value), 1) < 0)
    printf("brcmfmac: warning: cannot disable mpc\n");
  memset(country, 0, sizeof(country));
  if(brcmf_iovar(bus, "country", country, sizeof(country), 0) == 0){
    memmove(&country_rev, country + 4, sizeof(country_rev));
    country_name[0] = country[0] ? country[0] : '-';
    country_name[1] = country[1] ? country[1] : '-';
    country_name[2] = 0;
    country_code[0] = country[8] ? country[8] : '-';
    country_code[1] = country[9] ? country[9] : '-';
    country_code[2] = 0;
    printf("brcmfmac: regulatory country=%s rev=%d ccode=%s\n",
           country_name, country_rev, country_code);
  }
  // Do not make a legacy BRCMF_C_SCAN_RESULTS dump a prerequisite for
  // joining.  Its response can approach 2 KiB, while the current xv6 SDIO
  // host supports only single-block CMD53 transfers.  SET_SSID below asks
  // the FullMAC firmware to scan for and join this exact SSID itself.
  printf("brcmfmac: direct join target=%s\n", ssid);

  // Use the FullMAC firmware supplicant.  The host supplies WPA2 policy and
  // the ASCII passphrase; authentication, the 4-way handshake and CCMP key
  // installation are then performed inside the BCM43455 firmware.
  value = 1;
  if(brcmf_dcmd(bus, BRCMF_C_SET_INFRA, &value, sizeof(value), 1) < 0){
    printf("brcmfmac: connect step SET_INFRA failed\n");
    goto fail;
  }
  value = 0; // open-system 802.11 authentication before WPA2 association
  if(brcmf_dcmd(bus, BRCMF_C_SET_AUTH, &value, sizeof(value), 1) < 0){
    printf("brcmfmac: connect step SET_AUTH failed\n");
    goto fail;
  }
  value = AES_ENABLED;
  if(brcmf_dcmd(bus, BRCMF_C_SET_WSEC, &value, sizeof(value), 1) < 0){
    printf("brcmfmac: connect step SET_WSEC failed\n");
    goto fail;
  }
  if(brcmf_bsscfg_iovar_int(bus, "wpa_auth", WPA2_AUTH_PSK) < 0){
    printf("brcmfmac: connect step bsscfg:wpa_auth failed\n");
    goto fail;
  }
  // Some BCM43455 firmware builds return BCME_UNSUPPORTED for sup_wpa even
  // though they accept SET_WSEC_PMK and perform the PSK handshake as part of
  // the ordinary FullMAC join.  Linux treats FWSUP as a probed optional
  // feature too, so do not make this iovar a prerequisite for association.
  if(brcmf_bsscfg_iovar_int(bus, "sup_wpa", 1) < 0)
    printf("brcmfmac: firmware sup_wpa unsupported; continuing with PMK join\n");

  memset(&pmk, 0, sizeof(pmk));
  // With no firmware supplicant, pass a real 256-bit PMK rather than asking
  // firmware to hash an ASCII passphrase.  PBKDF2 uses the exact SSID bytes
  // as salt and 4096 HMAC-SHA1 iterations, as required by WPA2-PSK.
  wpa_pbkdf2(passphrase, ssid, pmk.key);
  pmk.key_len = 32;
  pmk.flags = 0;
  memmove(brcmf_bus.pmk, pmk.key, sizeof(brcmf_bus.pmk));
  brcmf_bus.pmk_ready = 1;
  brcmf_bus.assoc_rsn_ie_len = 0;
  brcmf_bus.handshake_started = 0;
  brcmf_bus.keys_pending = 0;
  brcmf_bus.handshake_done = 0;
  brcmf_bus.m2_sent = 0;
  printf("brcmfmac: host-derived WPA2 PMK ready (32 bytes)\n");
  if(brcmf_dcmd(bus, BRCMF_C_SET_WSEC_PMK, &pmk, sizeof(pmk), 1) < 0){
    printf("brcmfmac: SET_WSEC_PMK unsupported; host EAPOL required\n");
    host_eapol = 1;
  }

  memset(&network, 0, sizeof(network));
  network.length = ssid_len;
  memmove(network.value, ssid, ssid_len);
  printf("brcmfmac: associating with SSID %s (WPA2-PSK/AES)\n", ssid);
  brcmf_bus.eapol_rx = 0;
  if(brcmf_dcmd(bus, BRCMF_C_SET_SSID, &network, sizeof(network), 1) < 0){
    printf("brcmfmac: connect step SET_SSID failed\n");
    goto fail;
  }
  brcmf_connect_watch_start(bus);

  // Association and the WPA2 handshake are asynchronous.  GET_BSSID starts
  // succeeding only after firmware has a live association.
  for(int i = 0; i < 40; i++){
    int finish;
    if(brcmf_connect_watch_timedout(bus))
      break;
    memset(bssid, 0, sizeof(bssid));
    if(brcmf_dcmd(bus, BRCMF_C_GET_BSSID, bssid, sizeof(bssid), 0) >= 0 &&
       (bssid[0] | bssid[1] | bssid[2] | bssid[3] | bssid[4] | bssid[5])){
      if(!link_reported){
        printf("brcmfmac: 802.11 associated bssid=%x:%x:%x:%x:%x:%x\n",
               bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
        link_reported = 1;
        brcmf_read_assoc_rsn_ie(bus);
      }
      if(!host_eapol){
        brcmf_connect_watch_stop(bus);
        return 0;
      }
    }
    if(host_eapol && brcmf_bus.keys_pending){
      finish = brcmf_finish_wpa_handshake(bus);
      if(finish > 0){
        // M4 has reached the SDIO FIFO, but the AP still needs a short time
        // to validate it and open the controlled port.  Drain RX while that
        // transition completes before sending the first DHCP broadcast.
        uint64 ready_at = r_cntvct_el0() + r_cntfrq_el0() / 2;
        while(r_cntvct_el0() < ready_at){
          netdev_poll_one(netdev);
          asm volatile("yield" ::: "memory");
        }
        if(net_dhcp_dev(netdev) < 0){
          printf("brcmfmac: associated, but DHCP failed\n");
          brcmf_connect_watch_stop(bus);
          return -1;
        }
        printf("brcmfmac: wlan0 IPv4 ready\n");
        brcmf_connect_watch_stop(bus);
        return 0;
      }
      if(finish < 0)
        goto fail;
    }
    // Association events can arrive just after the GET_BSSID control reply.
    // Drain a bounded number now instead of leaving their status/reason codes
    // queued until the next BCDC request.
    acquire(&brcmf_bus.lock);
    for(int pending = 0; pending < 8; pending++)
      if(brcmf_rx_dispatch_locked(bus) <= 0)
        break;
    release(&brcmf_bus.lock);
    brcmf_delay_us(250000);
  }
  if(host_eapol && link_reported)
    printf("brcmfmac: host WPA2 handshake pending, EAPOL frames=%d\n",
           brcmf_bus.eapol_rx);
  else
    printf("brcmfmac: association timeout for SSID %s\n", ssid);
  brcmf_connect_watch_stop(bus);
  return -1;

fail:
  brcmf_connect_watch_stop(bus);
  printf("brcmfmac: WPA2 configuration failed for SSID %s\n", ssid);
  return -1;
}

static int
brcmf_probe(struct sdio_func *func)
{
  struct brcmf_bus_state *bus;
  struct fat32_file code, nvram, clm;
  struct brcmf_chip bc;
  char *chip, *bin_name, *txt_name, *clm_name;
  uint32 chipid, chipnum, chiprev, reset_vector = 0;
  int have_clm;

  if(sdio_enable_func(func) < 0){
    printf("brcmfmac: cannot enable SDIO function 1\n");
    return -1;
  }
  if(func->host->function_count < 2 || func->host->functions[1] == 0){
    printf("brcmfmac: SDIO function 2 is missing\n");
    return -1;
  }

  printf("brcmfmac: F1 ready; F2 deferred until firmware starts\n");
  if(brcmf_clock_prepare(func) < 0){
    printf("brcmfmac: ALP clock unavailable\n");
    return -1;
  }
  if(brcmf_backplane_read32(func, CHIPCOMMON_CHIPID, &chipid) < 0){
    printf("brcmfmac: cannot read ChipCommon chipid through F1 backplane\n");
    return -1;
  }
  chipnum = chipid & 0xffff;
  chiprev = (chipid >> 16) & 0xf;
  printf("brcmfmac: ChipCommon chipid=%x chip=%x rev=%d pkg=%d\n",
         chipid, chipnum, chiprev, (chipid >> 20) & 0xf);

  // The SDIO product ID is not necessarily the ChipCommon chip number.
  // In particular, BCM4345 rev 6 is the BCM43455 used by Raspberry Pi 3B+.
  if(chipnum == BRCM_CC_43430_CHIP_ID){
    chip = "BCM43430";
    bin_name = "BCM43430BIN";
    txt_name = "BCM43430TXT";
    clm_name = "BCM43430CLM";
  } else if(chipnum == BRCM_CC_4345_CHIP_ID && chiprev != 9){
    chip = "BCM43455";
    bin_name = "BCM43455BIN";
    txt_name = "BCM43455TXT";
    clm_name = "BCM43455CLM";
  } else {
    printf("brcmfmac: unsupported chip=%x rev=%d (SDIO device=%x)\n",
           chipnum, chiprev, func->device);
    return -1;
  }

  if(brcmf_load_firmware(func, bin_name, &code) < 0 ||
     brcmf_load_firmware(func, txt_name, &nvram) < 0){
    printf("brcmfmac: %s requires BIN and TXT on bootfs\n", chip);
    return -1;
  }
  have_clm = fat32openroot(clm_name, &clm) == 0;
  printf("brcmfmac: selected %s code=%d nvram=%d",
         chip, code.size, nvram.size);
  if(have_clm)
    printf(" clm=%d", clm.size);
  else
    printf(" clm=none");
  printf(" bytes\n");
  printf("brcmfmac: backplane access ready; starting firmware loader\n");

  if(brcmf_scan_cores(func, &bc) < 0){
    printf("brcmfmac: AI core enumeration failed\n");
    return -1;
  }
  if(brcmf_get_raminfo(func, &bc) < 0){
    printf("brcmfmac: CR4 RAM discovery failed\n");
    return -1;
  }
  if(code.size + BRCMF_NVRAM_MAX > bc.ramsize){
    printf("brcmfmac: firmware does not fit TCM RAM\n");
    return -1;
  }
  if(brcmf_set_passive(func, &bc) < 0){
    printf("brcmfmac: cannot halt/reset CR4 and D11 cores\n");
    return -1;
  }
  if(brcmf_write_firmware(func, &bc, &code, &reset_vector) < 0){
    printf("brcmfmac: firmware RAM download failed\n");
    return -1;
  }
  if(brcmf_write_nvram(func, &bc, &nvram) < 0){
    printf("brcmfmac: NVRAM download failed\n");
    return -1;
  }
  if(brcmf_set_active(func, &bc, reset_vector) < 0){
    printf("brcmfmac: cannot release CR4\n");
    return -1;
  }
  if(brcmf_start_f2(func, &bc) < 0)
    return -1;
  printf("brcmfmac: firmware running; SDPCM transport ready\n");
  bus = brcmf_alloc_bus();
  if(bus == 0){
    printf("brcmfmac: no free per-device state\n");
    return -1;
  }
  func->dev.driver_data = bus;
  if(brcmf_control_plane_start(bus, func, &bc, &clm, have_clm) < 0){
    func->dev.driver_data = 0;
    brcmf_free_bus(bus);
    return -1;
  }
  return 0;
}

static void
brcmf_remove(struct sdio_func *func)
{
  struct brcmf_bus_state *bus = func ? func->dev.driver_data : 0;
  if(bus == 0)
    return;
  brcmf_bus.ready = 0;
  sdio_release_irq(func);
  if(brcmf_bus.netdev.running)
    unregister_netdev(&brcmf_bus.netdev);
  func->dev.driver_data = 0;
  brcmf_free_bus(bus);
}

static struct sdio_driver brcmf_driver = {
  .driver = {
    .name = "brcmfmac",
  },
  .id_table = brcmf_ids,
  .probe = brcmf_probe,
  .remove = brcmf_remove,
};

void
brcmfmac_driver_init(void)
{
  memset(brcmf_devices, 0, sizeof(brcmf_devices));
  for(int i = 0; i < BRCMF_MAX_DEVICES; i++)
    init_workqueue(&brcmf_wq[i], brcmf_wq_names[i], 1);
  sdio_register_driver(&brcmf_driver);
}

void
brcmfmac_driver_exit(void)
{
  sdio_unregister_driver(&brcmf_driver);
}
