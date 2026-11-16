// MediaTek MT7601U USB attachment and MCU firmware loader.

#include "kernel/types.h"
#include "kernel/aarch64.h"
#include "kernel/defs.h"
#include "kernel/spinlock.h"
#include "kernel/device.h"
#include "kernel/usb.h"
#include "kernel/firmware.h"
#include "kernel/module.h"

#ifndef MT7601U_CONFIG_TRACE
#define MT7601U_CONFIG_TRACE 0
#endif
#ifndef MT7601U_DATA_TRACE
#define MT7601U_DATA_TRACE 0
#endif
#if MT7601U_CONFIG_TRACE
#define MT_CFG_LOG(...) printf("wifi-config: " __VA_ARGS__)
#else
#define MT_CFG_LOG(...) do { } while(0)
#endif
#if MT7601U_CONFIG_TRACE >= 2
#define MT_REG_LOG(...) printf("wifi-reg: " __VA_ARGS__)
#else
#define MT_REG_LOG(...) do { } while(0)
#endif
#if MT7601U_DATA_TRACE
#define MT_DATA_LOG(...) printf("wifi-data: " __VA_ARGS__)
#else
#define MT_DATA_LOG(...) do { } while(0)
#endif
#if MT7601U_DATA_TRACE >= 2
#define MT_FRAME_LOG(...) printf("wifi-frame: " __VA_ARGS__)
#else
#define MT_FRAME_LOG(...) do { } while(0)
#endif

// Firmware comes from request_firmware() (linked into the kernel) instead of
// the RPi's FAT32 boot partition; fw_file keeps the old pread interface.
struct fw_file { const struct firmware *fw; uint32 size; };
static int
fw_open(const char *name, struct fw_file *f)
{
  if(request_firmware(&f->fw, name) < 0)
    return -1;
  f->size = f->fw->size;
  return 0;
}
static int
fw_pread(struct fw_file *f, uint32 off, void *buf, uint32 len)
{
  if(off > f->size) return -1;
  if(len > f->size - off) len = f->size - off;
  memmove(buf, f->fw->data + off, len);
  return len;
}
#include "kernel/net.h"
#include "kernel/workqueue.h"

#define MT_VEND_MULTI_READ 7
#define MT_VEND_DEV_MODE   1
#define MT_VEND_WRITE      2
#define MT_VEND_WRITE_FCE  0x42
#define MT_ASIC_VERSION    0x0000
#define MT_MAC_CSR0        0x1000
#define MT_MCU_IVB_SIZE    0x40
#define MT_MCU_DLM_OFFSET  0x80000
#define MT_MCU_COM_REG0    0x0730
#define MT_MCU_COM_REG1    0x0734
#define MT_FCE_DMA_ADDR    0x0230
#define MT_FCE_DMA_LEN     0x0234
#define MT_USB_DMA_CFG     0x0238
#define MT_PBF_CFG         0x0404
#define MT_FCE_PSE_CTRL    0x0800
#define MT_TX_CPU_FROM_FCE_BASE_PTR 0x09a0
#define MT_TX_CPU_FROM_FCE_MAX_COUNT 0x09a4
#define MT_TX_CPU_FROM_FCE_CPU_DESC_IDX 0x09a8
#define MT_FCE_PDMA_GLOBAL_CONF 0x09c4
#define MT_FCE_SKIP_FS     0x0a6c
#define MT_USB_DMA_CFG_RX_BULK_EN (1U << 22)
#define MT_USB_DMA_CFG_TX_BULK_EN (1U << 23)
#define MT_USB_DMA_CFG_TX_CLR     (1U << 19)
#define MT_PBF_CFG_TXQ_EN         0xf
#define MT_FW_CHUNK               0x3800
#define MT_DMA_HDR_LEN            4
#define MT_EFUSE_CTRL             0x0024
#define MT_EFUSE_DATA_BASE        0x0028
#define MT_EFUSE_CTRL_AOUT        0x3f
#define MT_EFUSE_CTRL_MODE        (3U << 6)
#define MT_EFUSE_CTRL_AIN         (0x3ffU << 16)
#define MT_EFUSE_CTRL_KICK        (1U << 30)
#define MT7601U_EEPROM_SIZE       256
#define MT_EE_VERSION_FAE         0x02
#define MT_EE_VERSION_EE          0x03
#define MT_EE_MAC_ADDR            0x04
#define MT_EE_COUNTRY_REGION      0x39
#define MT_EE_FREQ_OFFSET         0x3a
#define MT_EE_LNA_GAIN            0x44
#define MT_EE_RSSI_OFFSET         0x46
#define MT_EE_REF_TEMP            0xd1
#define MT_WLAN_FUN_CTRL          0x0080
#define MT_WLAN_FUN_CTRL_EN       3
#define MT_CMB_CTRL               0x0020
#define MT_CMB_CTRL_READY         ((1U << 22) | (1U << 23))
#define MT_MAC_SYS_CTRL           0x1004
#define MT_MAC_ADDR_DW0           0x1008
#define MT_MAC_ADDR_DW1           0x100c
#define MT_BBP_CSR_CFG            0x101c
#define MT_RF_CSR_CFG             0x0500
#define MT_RF_CSR_KICK            (1U << 31)
#define MT_RF_CSR_WRITE           (1U << 30)
#define MT_MCU_CMD_FUN_SET        1
#define MT_MCU_CMD_CALIBRATE      31
#define MT_MCU_FUN_Q_SELECT       1
#define MT_BBP_BUSY               (1U << 17)
#define MT_BBP_WRITE              (1U << 19)
#define MT_RX_BUF_SIZE            16384
#define MT_RXWI_SIZE              28
#define MT_RXINFO_CRCERR          (1U << 8)
#define MT_RXINFO_ICVERR          (1U << 9)
#define MT_RXINFO_MICERR          (1U << 10)
#define MT_SCAN_MAX               32
#define MT_TXWI_SIZE              20
#define MT_TX_BUF_SIZE            2048
#define MT_RSN_IE_MAX             66
#define MT_NET_FRAME_MAX          1600
#define MT_WCID_AP                1
#define MT_N_WCIDS                128

#define MT_TXD_PKT_INFO_80211     (1U << 19)
#define MT_TXD_PKT_INFO_WIV       (1U << 24)
#define MT_TXD_INFO_QSEL_SHIFT    25
#define MT_QSEL_MGMT              0
#define MT_QSEL_EDCA              2

// TX path experiment.  1: send 802.11 frames on bulk OUT EP4 (AC_BE) with
// TXINFO QSEL=EDCA, as Linux mt7601u does for data and management frames.
// 0: the previous path, OUT EP8 (the in-band MCU command endpoint) with
// QSEL=MGMT.  Build with MT7601U_TX_EDCA_EP4=0 to compare.
// A/B switch for commit 3977211 (clear the WCID address/attribute and
// shared-key mode tables after the BBP table): 0 skips the clear.
#ifndef MT7601U_WCID_CLEAR
#define MT7601U_WCID_CLEAR 1
#endif
#ifndef MT7601U_TX_EDCA_EP4
#define MT7601U_TX_EDCA_EP4 1
#endif

enum mt7601u_link_state {
  MT_LINK_SCAN,
  MT_LINK_AUTH_PENDING,
  MT_LINK_AUTH_SENT,
  MT_LINK_ASSOC_PENDING,
  MT_LINK_ASSOC_SENT,
  MT_LINK_ASSOCIATED,
};

struct mt_reg_pair { uint16 reg; uint32 value; };
struct mt_bbp_pair { uint8 reg; uint8 value; };
struct mt_rf_pair { uint8 bank; uint8 reg; uint8 value; };

struct mt7601u_scan_entry {
  int valid;
  uint8 bssid[6];
  char ssid[33];
  uint8 ssid_len;
  uint8 channel;
  uint8 security;
  uint16 capability;
  uint8 rsn_ie[MT_RSN_IE_MAX];
  uint8 rsn_ie_len;
};

struct mt7601u_txwi {
  uint16 flags;
  uint16 rate_ctl;
  uint8 ack_ctl;
  uint8 wcid;
  uint16 len_ctl;
  uint32 iv;
  uint32 eiv;
  uint8 aid;
  uint8 txstream;
  uint16 ctl;
} __attribute__((packed, aligned(4)));

#define MT_SCAN_OPEN 0
#define MT_SCAN_WEP  1
#define MT_SCAN_WPA  2
#define MT_SCAN_WPA2 3

static const struct mt_reg_pair mt_mac_init[] = {
  {0x1408,0x0000013f}, {0x140c,0x00008003}, {0x1004,0},
  {0x1400,0x00017f97}, {0x1104,0x00000209}, {0x1330,0},
  {0x1334,0x00080606}, {0x1350,0x00001020}, {0x1348,0x000a2090},
  {0x1018,0x00003fff}, {0x0408,0x1fbf1f1f}, {0x040c,0x0000009f},
  {0x134c,0x47d01f0f}, {0x1404,0x00000013}, {0x1364,0x05740003},
  {0x1368,0x05740003}, {0x1370,0x03f44084}, {0x1374,0x01744004},
  {0x1378,0x03f44084}, {0x136c,0x01744004}, {0x1340,0x0000583f},
  {0x1344,0x01092b20}, {0x1380,0x002400ca}, {0x1608,2},
  {0x1100,0x33a41010}, {0x1204,0}, {0x150c,1},
  {0x0250,0x00006050}, {0x041c,0x18100800}, {0x0420,0x38302820},
  {0x0400,0x00080c00}, {0x0404,0x7f723c1f}, {0x0800,1},
  {0x0a38,0}, {0x13a0,0x003b0005}, {0x13a8,0x00006900},
  {0x13c0,0x00000400}, {0x13c8,0x00060006}, {0x1330,0x00000402},
  {0x1334,0}, {0x1338,0}, {0x0260,0}, {0x0808,0x0000030f},
  {0x0804,0x00256f0f},
};

static const struct mt_bbp_pair mt_bbp_init[] = {
  {65,0x2c},{66,0x38},{68,0x0b},{69,0x12},{70,0x0a},{73,0x10},
  {81,0x37},{82,0x62},{83,0x6a},{84,0x99},{86,0},{91,4},{92,0},
  {103,0},{105,5},{106,0x35},{1,4},{4,0x40},{20,6},{31,8},
  {178,0xff},{66,0x14},{68,0x8b},{69,0x12},{70,9},{73,0x11},
  {75,0x60},{76,0x44},{84,0x9a},{86,0x38},{91,7},{92,2},
  {99,0x50},{101,0},{103,0xc0},{104,0x92},{105,0x3c},{106,3},
  {128,0x12},{142,4},{143,0x37},{142,3},{143,0x99},
  {160,0xeb},{161,0xc4},{162,0x77},{163,0xf9},{164,0x88},
  {165,0x80},{166,0xff},{167,0xe4},{47,0x80},{60,0x80},
  {150,0xd2},{151,0x32},{152,0x23},{153,0x41},{154,0},{155,0x4f},
};

static const struct mt_rf_pair mt_rf_init[] = {
  {0,0,2},{0,1,1},{0,2,0x11},{0,3,0xff},{0,4,0x0a},{0,5,0x20},
  {0,6,0},{0,7,0},{0,8,0},{0,9,0},{0,10,0},{0,11,0x21},
  {0,13,0},{0,14,0x7c},{0,15,0x22},{0,16,0x80},{0,17,0x99},
  {0,18,0x99},{0,19,9},{0,20,0x50},{0,21,0xb0},{0,22,0},
  {0,23,0xc5},{0,24,0xfc},{0,25,0x40},{0,26,0x4d},{0,27,2},
  {0,28,0x72},{0,29,1},{0,30,0},{0,31,0},{0,32,0},{0,33,0},
  {0,34,0x23},{0,35,1},{0,36,0},{0,37,0},{0,38,0},{0,39,0x20},
  {0,40,0},{0,41,0xd0},{0,42,0x1b},{0,43,2},{0,44,0},
  {4,0,1},{4,1,0},{4,2,0},{4,3,0},{4,4,0},{4,5,8},{4,6,0},
  {4,7,0x5b},{4,8,0x52},{4,9,0xb6},{4,10,0x57},{4,11,0x33},
  {4,12,0x22},{4,13,0x3d},{4,14,0x3e},{4,15,0x13},{4,16,0x22},
  {4,17,0x23},{4,18,2},{4,19,0xa4},{4,20,1},{4,21,0x12},
  {4,22,0x80},{4,23,0xb3},{4,28,0x18},{4,29,0xee},{4,30,0x6b},
  {4,31,0x31},{4,32,0x5d},{4,34,0x96},{4,35,0x55},{4,36,8},
  {4,37,0xbb},{4,38,0xb3},{4,39,0xb3},{4,40,3},{4,43,0xc5},
  {4,44,0xc5},{4,45,0xc5},{4,46,7},{4,47,0xa8},{4,48,0xef},
  {4,49,0x1a},{4,54,7},{4,55,0xa7},{4,56,0xcc},{4,57,0x14},
  {4,58,7},{4,59,0xa8},{4,60,0xd7},{4,61,0x10},{4,62,0x1c},
  {5,0,0x47},{5,1,0},{5,2,0},{5,3,8},{5,4,4},{5,5,0x20},
  {5,6,0x3a},{5,7,0x3a},{5,8,0},{5,9,0},{5,10,0x10},
  {5,11,0x10},{5,12,0x10},{5,13,0x10},{5,14,0x10},{5,15,0x20},
  {5,16,0x22},{5,17,0x7c},{5,18,0},{5,19,0},{5,20,0},
  {5,21,0xf1},{5,22,0x11},{5,23,2},{5,24,0x41},{5,25,0x20},
  {5,26,0},{5,27,0xd7},{5,28,0xa2},{5,29,0x20},{5,30,0x49},
  {5,31,0x20},{5,32,4},{5,33,0xf1},{5,34,0xa1},{5,35,1},
  {5,58,0x31},{5,59,0x31},{5,60,0x0a},{5,61,2},{5,62,0},{5,63,0},
};

struct mt7601u_fw_header {
  uint32 ilm_len;
  uint32 dlm_len;
  uint16 build_ver;
  uint16 fw_ver;
  uint8 pad[4];
  char build_time[16];
} __attribute__((packed));

struct mt7601u_device {
  struct spinlock lock;
  int used;
  struct usb_device *udev;
  uint32 asic_rev;
  uint32 mac_rev;
  uint32 firmware_size;
  int mcu_running;
  uint8 mac[6];
  uint8 eeprom[MT7601U_EEPROM_SIZE];
  uint8 country;
  uint8 freq_offset;
  int rssi_offset[2];
  int ref_temp;
  int lna_gain;
  uint8 mcu_seq;
  int endpoints_ready;
  int ready;               // probe finished: RX ring armed, state machine queued
  uint32 rx_frames;
  uint32 rx_mgmt, rx_datafr, rx_urbs;   // bring-up counters
  uint32 rx_usb_err;                    // EP4 transfers that ended in an error
  uint32 rx_ucast, rx_ucast_enc, rx_ucast_busy;  // data frames to our MAC: all,
                                        // still encrypted, dropped (slot full)
  uint32 rx_bad;
  uint32 mgmt_frames;
  uint32 scan_seen;
  uint32 scan_rounds;      // full 1..11 sweeps, for the join-timeout report
  uint32 scan_rx_mark;     // rx_urbs / mgmt_frames at the start of this dwell
  uint32 scan_mgmt_mark;
  uint32 scan_dwell_logs;  // per-dwell report budget, reset by mt7601u_connect
  uint8 scan_channel;
  uint8 target_channel;    // from the AP's DS Parameter Set IE
  int target_found;
  char want_ssid[33];       // set by mt7601u_connect(); empty: scan only
  int want_ssid_len;
  uint8 target_bssid[6];
  uint16 target_capability;
  uint8 target_rsn_ie[MT_RSN_IE_MAX];
  uint8 target_rsn_ie_len;
  uint16 tx_sequence;
  uint8 link_state;
  uint8 link_retries;
  uint64 state_deadline;
  uint64 scan_deadline;
  uint8 pmk[32];
  uint8 anonce[32];
  uint8 snonce[32];
  uint8 ptk[64];
  uint8 gtk[16];
  uint8 gtk_index;
  uint8 m3_replay[8];
  uint8 m3_desc_type;
  uint8 m3_desc_version;
  int handshake_started;
  int handshake_done;
  struct net_device netdev;
  int net_registered;
  uint8 net_rx[MT_NET_FRAME_MAX];
  int net_rx_len;
  int paused;
  struct napi_struct napi;
  struct delayed_work state_work;
  struct mt7601u_scan_entry scan[MT_SCAN_MAX];
};

static struct mt7601u_device mt7601u;
static struct workqueue mt7601u_wq;
static uint8 mt_fw_buf[MT_FW_CHUNK + 12] __attribute__((aligned(64)));
static uint8 mt_mcu_buf[1024] __attribute__((aligned(64)));
static uint8 mt_mcu_resp[1024] __attribute__((aligned(64)));
#define MT_RX_REQUESTS 4
static uint8 mt_rx_buf[MT_RX_REQUESTS][MT_RX_BUF_SIZE]
  __attribute__((aligned(64)));
static uint8 mt_tx_buf[MT_TX_BUF_SIZE] __attribute__((aligned(64)));
static uint8 mt_data_buf[MT_NET_FRAME_MAX + 64] __attribute__((aligned(64)));
static uint8 mt_eapol_buf[256] __attribute__((aligned(64)));

static int mt7601u_net_xmit(struct net_device*, void*, int);
static void mt7601u_net_poll(struct net_device*);
static int mt7601u_napi_poll(struct napi_struct*, int);
static void mt7601u_state_worker(struct work_struct*);

#define MT_STATE_SOON_JIFFIES       1
#define MT_SCAN_DWELL_JIFFIES      50
#ifndef MT7601U_SCAN_CHAN
#define MT7601U_SCAN_CHAN 0        // nonzero: stay on this channel only
#endif
#define MT_RESPONSE_TIMEOUT_JIFFIES 100
#define MT_WPA_TIMEOUT_JIFFIES     500

static const struct net_device_ops mt7601u_netdev_ops = {
  .start_xmit = mt7601u_net_xmit,
  .poll = mt7601u_net_poll,
};

static const struct usb_device_id mt7601u_ids[] = {
  { 0x0b05, 0x17d3 }, { 0x0e8d, 0x760a }, { 0x0e8d, 0x760b },
  { 0x13d3, 0x3431 }, { 0x13d3, 0x3434 }, { 0x148f, 0x7601 },
  { 0x148f, 0x760a }, { 0x148f, 0x760b }, { 0x148f, 0x760c },
  { 0x148f, 0x760d }, { 0x2001, 0x3d04 }, { 0x2717, 0x4106 },
  { 0x2955, 0x0001 }, { 0x2955, 0x1001 }, { 0x2955, 0x1003 },
  { 0x2a5f, 0x1000 }, { 0x7392, 0x7710 }, { 0, 0 },
};

static int
mt7601u_read32(struct usb_device *udev, uint16 reg, uint32 *value)
{
  uint8 data[4];
  int r = udev->ops->control(udev, 0xc0, MT_VEND_MULTI_READ,
                             0, reg, data, sizeof(data));
  if(r < 0){
    MT_REG_LOG("read reg=%x failed=%d\n", reg, r);
    return -1;
  }
  *value = (uint32)data[0] | ((uint32)data[1] << 8) |
           ((uint32)data[2] << 16) | ((uint32)data[3] << 24);
  MT_REG_LOG("read reg=%x value=%x\n", reg, *value);
  return 0;
}

static void
mt7601u_delay_ms(uint32 ms)
{
  uint64 end = r_cntvct_el0() +
               ((uint64)r_cntfrq_el0() * ms + 999) / 1000;
  while(r_cntvct_el0() < end)
    ;
}

static int
mt7601u_write32_req(struct usb_device *udev, uint8 request,
                    uint16 reg, uint32 value)
{
  if(udev->ops->control(udev, 0x40, request, value & 0xffff,
                        reg, 0, 0) < 0 ||
     udev->ops->control(udev, 0x40, request, value >> 16,
                        reg + 2, 0, 0) < 0){
    MT_REG_LOG("write request=%x reg=%x value=%x failed\n",
               request, reg, value);
    return -1;
  }
  MT_REG_LOG("write request=%x reg=%x value=%x\n", request, reg, value);
  return 0;
}

static int
mt7601u_write32(struct usb_device *udev, uint16 reg, uint32 value)
{
  return mt7601u_write32_req(udev, MT_VEND_WRITE, reg, value);
}

static int
mt7601u_write_bytes(struct usb_device *udev, uint16 reg, uint8 *data, int len)
{
  if((len & 3) != 0)
    return -1;
  for(int i = 0; i < len; i += 4){
    uint32 value = (uint32)data[i] | ((uint32)data[i + 1] << 8) |
      ((uint32)data[i + 2] << 16) | ((uint32)data[i + 3] << 24);
    if(mt7601u_write32(udev, reg + i, value) < 0)
      return -1;
  }
  return 0;
}

static int
mt7601u_rmw(struct usb_device *udev, uint16 reg, uint32 clear, uint32 set)
{
  uint32 value;
  if(mt7601u_read32(udev, reg, &value) < 0)
    return -1;
  return mt7601u_write32(udev, reg, (value & ~clear) | set);
}

static int
mt7601u_dma_fw_chunk(struct usb_device *udev, struct fw_file *file,
                     uint32 file_offset, uint32 length, uint32 destination)
{
  uint32 info, value, padded = (length + 3) & ~3U;
  int r;

  MT_CFG_LOG("firmware chunk file-off=%d len=%d destination=%x padded=%d ep=%d\n",
             file_offset, length, destination, padded, udev->bulk_out_ep);
  memset(mt_fw_buf, 0, padded + 8);
  if(fw_pread(file, file_offset, mt_fw_buf + MT_DMA_HDR_LEN,
                length) != (int)length)
    return -1;
  // TXINFO: DMA packet, destination CPU_TX_PORT (2), payload length.
  info = (2U << 27) | length;
  mt_fw_buf[0] = info;
  mt_fw_buf[1] = info >> 8;
  mt_fw_buf[2] = info >> 16;
  mt_fw_buf[3] = info >> 24;
  if(mt7601u_write32_req(udev, MT_VEND_WRITE_FCE,
                         MT_FCE_DMA_ADDR, destination) < 0 ||
     mt7601u_write32_req(udev, MT_VEND_WRITE_FCE,
                         MT_FCE_DMA_LEN, padded << 16) < 0)
    return -1;
  r = udev->ops->bulk(udev, udev->bulk_out_ep, 0, mt_fw_buf,
                      MT_DMA_HDR_LEN + padded + 4);
  if(r != (int)(MT_DMA_HDR_LEN + padded + 4))
    return -1;
  if(mt7601u_read32(udev, MT_TX_CPU_FROM_FCE_CPU_DESC_IDX, &value) < 0 ||
     mt7601u_write32(udev, MT_TX_CPU_FROM_FCE_CPU_DESC_IDX,
                     value + 1) < 0)
    return -1;
  for(int i = 0; i < 500; i++){
    if(mt7601u_read32(udev, MT_MCU_COM_REG1, &value) == 0 &&
       (value & (1U << 31)))
      return 0;
    mt7601u_delay_ms(1);
  }
  return -1;
}

static int
mt7601u_dma_fw_range(struct usb_device *udev, struct fw_file *file,
                     uint32 file_offset, uint32 length, uint32 destination,
                     char *name)
{
  uint32 done = 0;
  while(done < length){
    uint32 n = length - done;
    if(n > MT_FW_CHUNK)
      n = MT_FW_CHUNK;
    if(mt7601u_dma_fw_chunk(udev, file, file_offset + done, n,
                            destination + done) < 0){
      printf("mt7601u: %s firmware upload failed at %d/%d\n",
             name, done, length);
      return -1;
    }
    done += n;
    printf("mt7601u: %s firmware upload %d/%d\n", name, done, length);
  }
  return 0;
}

static int
mt7601u_load_firmware(struct usb_device *udev, struct fw_file *file)
{
  struct mt7601u_fw_header header;
  uint8 ivb[MT_MCU_IVB_SIZE];
  uint32 value, ilm_payload;

  if(fw_pread(file, 0, &header, sizeof(header)) != sizeof(header) ||
     fw_pread(file, sizeof(header), ivb, sizeof(ivb)) != sizeof(ivb))
    return -1;
  MT_CFG_LOG("firmware begin ilm=%d dlm=%d ivb=%d bulk-out=EP%d\n",
             header.ilm_len, header.dlm_len, MT_MCU_IVB_SIZE,
             udev->bulk_out_ep);
  if(mt7601u_write32(udev, MT_USB_DMA_CFG,
                     MT_USB_DMA_CFG_RX_BULK_EN |
                     MT_USB_DMA_CFG_TX_BULK_EN) < 0)
    return -1;
  if(mt7601u_read32(udev, MT_MCU_COM_REG0, &value) == 0 && value == 1){
    printf("mt7601u: MCU firmware already running\n");
    return 0;
  }

  mt7601u_write32(udev, 0x094c, 0);
  mt7601u_write32(udev, MT_FCE_PSE_CTRL, 0);
  if(udev->ops->control(udev, 0x40, MT_VEND_DEV_MODE, 1, 0, 0, 0) < 0)
    return -1;
  mt7601u_delay_ms(5);
  if(mt7601u_write32(udev, 0x0a44, 0) < 0 ||
     mt7601u_write32(udev, 0x0230, 0x84210) < 0 ||
     mt7601u_write32(udev, 0x0400, 0x80c00) < 0 ||
     mt7601u_write32(udev, 0x0800, 1) < 0 ||
     mt7601u_rmw(udev, MT_PBF_CFG, 0, MT_PBF_CFG_TXQ_EN) < 0 ||
     mt7601u_write32(udev, MT_FCE_PSE_CTRL, 1) < 0 ||
     mt7601u_write32(udev, MT_USB_DMA_CFG,
                     MT_USB_DMA_CFG_RX_BULK_EN |
                     MT_USB_DMA_CFG_TX_BULK_EN) < 0 ||
     mt7601u_rmw(udev, MT_USB_DMA_CFG, 0, MT_USB_DMA_CFG_TX_CLR) < 0 ||
     mt7601u_rmw(udev, MT_USB_DMA_CFG, MT_USB_DMA_CFG_TX_CLR, 0) < 0 ||
     mt7601u_write32(udev, MT_TX_CPU_FROM_FCE_BASE_PTR, 0x400230) < 0 ||
     mt7601u_write32(udev, MT_TX_CPU_FROM_FCE_MAX_COUNT, 1) < 0 ||
     mt7601u_write32(udev, MT_FCE_PDMA_GLOBAL_CONF, 0x44) < 0 ||
     mt7601u_write32(udev, MT_FCE_SKIP_FS, 3) < 0)
    return -1;

  ilm_payload = header.ilm_len - MT_MCU_IVB_SIZE;
  if(mt7601u_dma_fw_range(udev, file,
                          sizeof(header) + MT_MCU_IVB_SIZE,
                          ilm_payload, MT_MCU_IVB_SIZE, "ILM") < 0 ||
     mt7601u_dma_fw_range(udev, file,
                          sizeof(header) + header.ilm_len,
                          header.dlm_len, MT_MCU_DLM_OFFSET, "DLM") < 0)
    return -1;
  if(udev->ops->control(udev, 0x40, MT_VEND_DEV_MODE, 0x12, 0,
                        ivb, sizeof(ivb)) < 0)
    return -1;
  for(int i = 0; i < 100; i++){
    mt7601u_delay_ms(10);
    if(mt7601u_read32(udev, MT_MCU_COM_REG0, &value) == 0 && value == 1){
      printf("mt7601u: MCU firmware running\n");
      MT_CFG_LOG("firmware end COM_REG0=%x\n", value);
      return 0;
    }
  }
  printf("mt7601u: MCU firmware start timeout com0=%x\n", value);
  return -1;
}

static int
mt7601u_poll32(struct usb_device *udev, uint16 reg, uint32 mask,
               uint32 wanted, int loops)
{
  uint32 value = 0;
  while(loops-- > 0){
    if(mt7601u_read32(udev, reg, &value) == 0 &&
       (value & mask) == wanted)
      return 0;
    mt7601u_delay_ms(1);
  }
  return -1;
}

static int
mt7601u_efuse_read_block(struct usb_device *udev, uint16 address, uint8 *data)
{
  uint32 value;
  if(mt7601u_read32(udev, MT_EFUSE_CTRL, &value) < 0)
    return -1;
  value &= ~(MT_EFUSE_CTRL_AIN | MT_EFUSE_CTRL_MODE);
  value |= ((uint32)(address & ~0xf) << 16) | MT_EFUSE_CTRL_KICK;
  if(mt7601u_write32(udev, MT_EFUSE_CTRL, value) < 0 ||
     mt7601u_poll32(udev, MT_EFUSE_CTRL, MT_EFUSE_CTRL_KICK, 0, 1000) < 0)
    return -1;
  if(mt7601u_read32(udev, MT_EFUSE_CTRL, &value) < 0)
    return -1;
  if((value & MT_EFUSE_CTRL_AOUT) == MT_EFUSE_CTRL_AOUT){
    memset(data, 0xff, 16);
    return 0;
  }
  for(int i = 0; i < 4; i++){
    if(mt7601u_read32(udev, MT_EFUSE_DATA_BASE + i * 4, &value) < 0)
      return -1;
    data[i * 4] = value;
    data[i * 4 + 1] = value >> 8;
    data[i * 4 + 2] = value >> 16;
    data[i * 4 + 3] = value >> 24;
  }
  return 0;
}

static int
mt7601u_read_eeprom(struct mt7601u_device *dev)
{
  MT_CFG_LOG("eFuse read begin bytes=%d blocks=%d\n",
             MT7601U_EEPROM_SIZE, MT7601U_EEPROM_SIZE / 16);
  for(int offset = 0; offset < MT7601U_EEPROM_SIZE; offset += 16)
    if(mt7601u_efuse_read_block(dev->udev, offset,
                                dev->eeprom + offset) < 0){
      printf("mt7601u: eFuse read failed at %x\n", offset);
      return -1;
    }
  memmove(dev->mac, dev->eeprom + MT_EE_MAC_ADDR, sizeof(dev->mac));
  if((dev->mac[0] & 1) ||
     (dev->mac[0] == 0xff && dev->mac[1] == 0xff)){
    printf("mt7601u: invalid permanent MAC in eFuse\n");
    return -1;
  }
  dev->country = dev->eeprom[MT_EE_COUNTRY_REGION];
  dev->freq_offset = dev->eeprom[MT_EE_FREQ_OFFSET] == 0xff ?
                     0 : dev->eeprom[MT_EE_FREQ_OFFSET];
  dev->rssi_offset[0] = dev->eeprom[MT_EE_RSSI_OFFSET] == 0xff ?
                        0 : dev->eeprom[MT_EE_RSSI_OFFSET];
  dev->rssi_offset[1] = dev->eeprom[MT_EE_RSSI_OFFSET + 1] == 0xff ?
                        0 : dev->eeprom[MT_EE_RSSI_OFFSET + 1];
  dev->ref_temp = dev->eeprom[MT_EE_REF_TEMP] == 0xff ?
                  0 : dev->eeprom[MT_EE_REF_TEMP];
  dev->lna_gain = dev->eeprom[MT_EE_LNA_GAIN] == 0xff ?
                  0 : dev->eeprom[MT_EE_LNA_GAIN];
  if(dev->rssi_offset[0] & 0x80) dev->rssi_offset[0] -= 0x100;
  if(dev->rssi_offset[1] & 0x80) dev->rssi_offset[1] -= 0x100;
  if(dev->ref_temp & 0x80) dev->ref_temp -= 0x100;
  if(dev->lna_gain & 0x80) dev->lna_gain -= 0x100;
  printf("mt7601u: EEPROM ver=%x fae=%x country=%x freq=%d "
         "rssi=%d,%d temp=%d lna=%d\n",
         dev->eeprom[MT_EE_VERSION_EE], dev->eeprom[MT_EE_VERSION_FAE],
         dev->country, dev->freq_offset, dev->rssi_offset[0],
         dev->rssi_offset[1], dev->ref_temp, dev->lna_gain);
  printf("mt7601u: permanent mac=%x:%x:%x:%x:%x:%x\n",
         dev->mac[0], dev->mac[1], dev->mac[2], dev->mac[3],
         dev->mac[4], dev->mac[5]);
  MT_CFG_LOG("eFuse read end country=%x freq-offset=%d\n",
             dev->country, dev->freq_offset);
  return 0;
}

static int
mt7601u_mcu_command(struct mt7601u_device *dev, uint8 command,
                    void *payload, int length, int wait_response)
{
  uint32 info, response;
  int padded = (length + 3) & ~3;
  int total = MT_DMA_HDR_LEN + padded + 4;
  uint8 seq = 0;
  if(total > (int)sizeof(mt_mcu_buf))
    return -1;
  if(wait_response){
    seq = ++dev->mcu_seq & 0xf;
    if(seq == 0)
      seq = ++dev->mcu_seq & 0xf;
  }
  MT_CFG_LOG("MCU command begin cmd=%d seq=%d payload=%d wait=%d tx=EP%d rx=EP%d\n",
             command, seq, length, wait_response,
             dev->udev->bulk_out_ep, dev->udev->bulk_in_ep2);
  memset(mt_mcu_buf, 0, total);
  memmove(mt_mcu_buf + MT_DMA_HDR_LEN, payload, length);
  info = (1U << 30) | (2U << 27) | ((uint32)command << 20) |
         ((uint32)seq << 16) | padded;
  mt_mcu_buf[0] = info;
  mt_mcu_buf[1] = info >> 8;
  mt_mcu_buf[2] = info >> 16;
  mt_mcu_buf[3] = info >> 24;
  if(dev->udev->ops->bulk(dev->udev, dev->udev->bulk_out_ep, 0,
                          mt_mcu_buf, total) != total)
    return -1;
  if(!wait_response){
    MT_CFG_LOG("MCU command end cmd=%d no-response\n", command);
    return 0;
  }
  memset(mt_mcu_resp, 0, sizeof(mt_mcu_resp));
  int got = dev->udev->ops->bulk(dev->udev, dev->udev->bulk_in_ep2, 1,
                                 mt_mcu_resp, sizeof(mt_mcu_resp));
  if(got < 4)
    return -1;
  response = (uint32)mt_mcu_resp[0] | ((uint32)mt_mcu_resp[1] << 8) |
             ((uint32)mt_mcu_resp[2] << 16) |
             ((uint32)mt_mcu_resp[3] << 24);
  if(((response >> 16) & 0xf) != seq || ((response >> 20) & 0xf) != 0)
    return -1;
  MT_CFG_LOG("MCU command end cmd=%d seq=%d response=%x bytes=%d\n",
             command, seq, response, got);
  return 0;
}

static int
mt7601u_mcu_channel_init(struct mt7601u_device *dev)
{
  uint32 msg[2] = { MT_MCU_FUN_Q_SELECT, 1 };
  if(dev->udev->bulk_in_ep2 == 0 || dev->udev->bulk_out_ep == 0)
    return -1;
  if(mt7601u_mcu_command(dev, MT_MCU_CMD_FUN_SET,
                         msg, sizeof(msg), 0) < 0)
    return -1;
  printf("mt7601u: MCU command channel ready tx-ep=%d resp-ep=%d\n",
         dev->udev->bulk_out_ep, dev->udev->bulk_in_ep2);
  return 0;
}

static int
mt7601u_bbp_write(struct usb_device *udev, uint8 reg, uint8 value)
{
  if(mt7601u_poll32(udev, MT_BBP_CSR_CFG, MT_BBP_BUSY, 0, 1000) < 0)
    return -1;
  if(mt7601u_write32(udev, MT_BBP_CSR_CFG,
                     value | ((uint32)reg << 8) |
                     MT_BBP_WRITE | MT_BBP_BUSY) < 0)
    return -1;
  return mt7601u_poll32(udev, MT_BBP_CSR_CFG,
                        MT_BBP_BUSY, 0, 1000);
}

static int
mt7601u_rf_write(struct usb_device *udev, uint8 bank,
                 uint8 reg, uint8 value)
{
  if(mt7601u_poll32(udev, MT_RF_CSR_CFG, MT_RF_CSR_KICK, 0, 100) < 0)
    return -1;
  if(mt7601u_write32(udev, MT_RF_CSR_CFG,
                     value | ((uint32)reg << 8) |
                     ((uint32)bank << 14) |
                     MT_RF_CSR_WRITE | MT_RF_CSR_KICK) < 0)
    return -1;
  return mt7601u_poll32(udev, MT_RF_CSR_CFG,
                        MT_RF_CSR_KICK, 0, 100);
}

static int
mt7601u_calibrate(struct mt7601u_device *dev, uint32 id, uint32 value)
{
  uint32 msg[2] = { id, value };
  if(mt7601u_mcu_command(dev, MT_MCU_CMD_CALIBRATE,
                         msg, sizeof(msg), 1) < 0){
    printf("mt7601u: calibration %d failed\n", id);
    return -1;
  }
  return 0;
}

static int
mt7601u_full_hw_init(struct mt7601u_device *dev)
{
  struct usb_device *udev = dev->udev;
  uint32 mac_status;

  MT_CFG_LOG("full init begin MAC=%d BBP=%d RF=%d WCID=%d\n",
             sizeof(mt_mac_init) / sizeof(mt_mac_init[0]),
             sizeof(mt_bbp_init) / sizeof(mt_bbp_init[0]),
             sizeof(mt_rf_init) / sizeof(mt_rf_init[0]), MT_N_WCIDS);

  for(uint i = 0; i < sizeof(mt_mac_init) / sizeof(mt_mac_init[0]); i++)
    if(mt7601u_write32(udev, mt_mac_init[i].reg,
                       mt_mac_init[i].value) < 0){
      printf("mt7601u: MAC table failed at %d reg=%x\n",
             i, mt_mac_init[i].reg);
      return -1;
    }
  if(mt7601u_read32(udev, 0x1200, &mac_status) < 0 ||
     (mac_status & 3) != 0){
    printf("mt7601u: MAC did not become idle status=%x\n", mac_status);
    return -1;
  }
  printf("mt7601u: MAC register table initialized (%d entries)\n",
         sizeof(mt_mac_init) / sizeof(mt_mac_init[0]));

  for(uint i = 0; i < sizeof(mt_bbp_init) / sizeof(mt_bbp_init[0]); i++)
    if(mt7601u_bbp_write(udev, mt_bbp_init[i].reg,
                         mt_bbp_init[i].value) < 0){
      printf("mt7601u: BBP table failed at %d reg=%d\n",
             i, mt_bbp_init[i].reg);
      return -1;
    }
  // Linux mt7601u_init_hardware() (and the rpi3-BCM43430 driver): after the
  // BBP table, clear the WCID address table (all ff), the shared-key mode
  // registers, and set every WCID attribute to 1 (pairwise, no cipher,
  // BSS 0). Power-up contents are undefined; a stray BSS index or key mode
  // in these tables makes the MAC pick the wrong key or drop data frames,
  // which is why some boots got no data at all after the handshake.
#if MT7601U_WCID_CLEAR
  for(int i = 0; i < MT_N_WCIDS; i++)
    if(mt7601u_write32(udev, 0x1800 + i * 8, 0xffffffff) < 0 ||
       mt7601u_write32(udev, 0x1800 + i * 8 + 4, 0x00ffffff) < 0)
      return -1;
  for(int i = 0; i < 4; i++)
    if(mt7601u_write32(udev, 0xb000 + i * 4, 0) < 0)
      return -1;
  for(int i = 0; i < MT_N_WCIDS * 2; i++)
    if(mt7601u_write32(udev, 0xa800 + i * 4, 1) < 0)
      return -1;
#else
  printf("mt7601u: WCID/key table clear skipped (MT7601U_WCID_CLEAR=0)\n");
#endif
  printf("mt7601u: BBP register table initialized (%d entries)%s\n",
         sizeof(mt_bbp_init) / sizeof(mt_bbp_init[0]),
         MT7601U_WCID_CLEAR ? "; WCID/key tables cleared" : "");

  for(uint i = 0; i < sizeof(mt_rf_init) / sizeof(mt_rf_init[0]); i++)
    if(mt7601u_rf_write(udev, mt_rf_init[i].bank,
                        mt_rf_init[i].reg, mt_rf_init[i].value) < 0){
      printf("mt7601u: RF table failed at %d bank=%d reg=%d\n",
             i, mt_rf_init[i].bank, mt_rf_init[i].reg);
      return -1;
    }
  // EEPROM frequency trim supersedes the table's neutral bank-0/R12 value.
  if(mt7601u_rf_write(udev, 0, 12, dev->freq_offset) < 0)
    return -1;
  printf("mt7601u: RF register table initialized (%d entries)\n",
         sizeof(mt_rf_init) / sizeof(mt_rf_init[0]));

  // Linux's bring-up performs these MCU calibrations before enabling queues.
  if(mt7601u_calibrate(dev, 1, 0) < 0)         // R calibration
    return -1;
  // Linux raises RF bank 0/R4 bit 7 before TXDCOC calibration.
  if(mt7601u_rf_write(udev, 0, 4, 0x8a) < 0 ||
     mt7601u_calibrate(dev, 9, 0) < 0 ||      // TXDCOC
     mt7601u_calibrate(dev, 4, 0) < 0 ||      // LOFT
     mt7601u_calibrate(dev, 5, 0) < 0 ||      // TXIQ
     mt7601u_calibrate(dev, 8, 0) < 0 ||      // RXIQ
     mt7601u_calibrate(dev, 7, 0) < 0)        // DPD
    return -1;

  dev->endpoints_ready = 1;
  // Start MAC receive.  Association-specific filtering is installed later;
  // the broad filter lets the SoftMAC observe beacons and probe responses.
  if(mt7601u_write32(udev, 0x1400, 0x00017f97) < 0 ||
     mt7601u_write32(udev, MT_MAC_SYS_CTRL, 0x0c) < 0)
    return -1;
  printf("mt7601u: RF calibration complete; bulk endpoints ready\n");
  MT_CFG_LOG("full init end MAC_SYS_CTRL=0c endpoints-ready=%d\n",
             dev->endpoints_ready);
  return 0;
}

static uint16
mt_get16(uint8 *p)
{
  return (uint16)p[0] | ((uint16)p[1] << 8);
}

static uint32
mt_get32(uint8 *p)
{
  return (uint32)p[0] | ((uint32)p[1] << 8) |
         ((uint32)p[2] << 16) | ((uint32)p[3] << 24);
}

static void
mt_put16(uint8 *p, uint16 value)
{
  p[0] = value;
  p[1] = value >> 8;
}

static void
mt_put32(uint8 *p, uint32 value)
{
  p[0] = value;
  p[1] = value >> 8;
  p[2] = value >> 16;
  p[3] = value >> 24;
}

static int
mt_mac_equal(uint8 *a, uint8 *b)
{
  for(int i = 0; i < 6; i++)
    if(a[i] != b[i])
      return 0;
  return 1;
}

// Submit one raw 802.11 frame through the MT7601U packet-DMA format:
// TXINFO, TXWI, MPDU, four-byte alignment padding, and a trailing zero word.
// Read and discard TX status entries (Linux mt7601u_tx_stat: TX_STAT_FIFO_EXT
// then TX_STAT_FIFO until the valid bit clears). Left unread, the FIFO fills
// after a handful of frames and the MAC stops taking new ones.
#define MT_TX_STAT_FIFO     0x1718
#define MT_TX_STAT_FIFO_EXT 0x1798
static int tx_stat_logged;
static void
mt7601u_tx_stat_drain(struct mt7601u_device *dev)
{
  uint32 ext, stat;
  int n = 0;
  for(; n < 64; n++){
    if(mt7601u_read32(dev->udev, MT_TX_STAT_FIFO_EXT, &ext) < 0 ||
       mt7601u_read32(dev->udev, MT_TX_STAT_FIFO, &stat) < 0 ||
       !(stat & 1))
      break;
    if(MT7601U_DATA_TRACE && tx_stat_logged < 24){
      tx_stat_logged++;
      MT_DATA_LOG("tx-status=%x ext=%x result=%s\n", stat, ext,
                  (stat >> 5) & 1 ? "acked" : "not-acked");
    }
  }
}

// Bulk OUT endpoint for WLAN frames (see MT7601U_TX_EDCA_EP4).
static int
mt7601u_wlan_tx_ep(struct mt7601u_device *dev)
{
#if MT7601U_TX_EDCA_EP4
  for(int i = 0; i < dev->udev->n_bulk_out; i++)
    if(dev->udev->bulk_out_eps[i] == 4)
      return 4;
#endif
  return dev->udev->bulk_out_ep;
}

// When the chip NAKs a WLAN frame, show which part of its TX path is off.
// TX_STAT_FIFO is read last: reading it pops an entry.
static int tx_fail_dumps;
static void
mt7601u_tx_fail_dump(struct mt7601u_device *dev, int ep, uint32 info)
{
  static const struct { uint16 reg; char *name; } regs[] = {
    { 0x0208, "WPDMA_GLO_CFG" },
    { 0x0404, "PBF_CFG" },
    { 0x0238, "USB_DMA_CFG" },
    { 0x1004, "MAC_SYS_CTRL" },
    { 0x0730, "MCU_COM_REG0" },
    { 0x1798, "TX_STAT_FIFO_EXT" },
    { 0x1718, "TX_STAT_FIFO" },
  };
  uint32 value;
  if(tx_fail_dumps >= 3)
    return;
  tx_fail_dumps++;
  printf("mt7601u: TX stall dump #%d: EP%d txinfo=%x\n",
         tx_fail_dumps, ep, info);
  for(int i = 0; i < (int)(sizeof(regs) / sizeof(regs[0])); i++){
    if(mt7601u_read32(dev->udev, regs[i].reg, &value) == 0)
      printf("mt7601u:   %s(%x)=%x\n", regs[i].name, regs[i].reg, value);
    else
      printf("mt7601u:   %s(%x) read failed\n", regs[i].name, regs[i].reg);
  }
}

static int tx_path_logged;

static int
mt7601u_tx_raw(struct mt7601u_device *dev, uint8 *frame, int frame_len,
               uint8 wcid, int hardware_iv)
{
  struct mt7601u_txwi *txwi;
  uint32 info;
  int payload_len, padded, total, result;

  if(frame_len <= 0 || frame_len > MT_TX_BUF_SIZE - 32)
    return -1;
  payload_len = MT_TXWI_SIZE + frame_len;
  padded = (payload_len + 3) & ~3;
  total = MT_DMA_HDR_LEN + padded + 4;
  memset(mt_tx_buf, 0, total);

  // WLAN port, DMA packet, management queue, raw 802.11 MPDU, TXWI IV.
  info = padded | MT_TXD_PKT_INFO_80211;
  if(!hardware_iv)
    info |= MT_TXD_PKT_INFO_WIV;
  mt_put32(mt_tx_buf, info);
  txwi = (struct mt7601u_txwi *)(mt_tx_buf + MT_DMA_HDR_LEN);
  txwi->rate_ctl = 0;             // CCK, 1 Mbit/s basic rate
  txwi->ack_ctl = 1;              // request receiver ACK
  txwi->wcid = wcid;
  txwi->len_ctl = frame_len;      // MPDU byte count, packet id zero
  memmove(mt_tx_buf + MT_DMA_HDR_LEN + MT_TXWI_SIZE, frame, frame_len);

  // OUT #0 (0x08), as the RPi version used; OUT #5 (0x09) NAKed from the
  // first frame on the Tiny210. After each frame the TX status FIFO is
  // drained (mt7601u_tx_stat_drain): the MAC stops transmitting, and the
  // endpoint NAKs for good, once that FIFO is full.
  int ep = mt7601u_wlan_tx_ep(dev);
  int qsel = ep != dev->udev->bulk_out_ep ? MT_QSEL_EDCA : MT_QSEL_MGMT;
  info |= (uint32)qsel << MT_TXD_INFO_QSEL_SHIFT;
  mt_put32(mt_tx_buf, info);
  if(!tx_path_logged){
    tx_path_logged = 1;
    printf("mt7601u: WLAN TX path EP%d qsel=%s\n", ep,
           qsel == MT_QSEL_EDCA ? "EDCA" : "MGMT");
  }
  MT_FRAME_LOG("tx 802.11 fc=%x mpdu=%d wcid=%d hw-iv=%d usb=EP%d bytes=%d\n",
               frame_len >= 2 ? mt_get16(frame) : 0, frame_len, wcid,
               hardware_iv, ep, total);
  result = dev->udev->ops->bulk(dev->udev, ep, 0, mt_tx_buf, total);
  if(result == total)
    mt7601u_tx_stat_drain(dev);
  if(result != total){
    printf("mt7601u: TX bulk failed result=%d expected=%d mpdu=%d wcid=%d hwiv=%d\n",
           result, total, frame_len, wcid, hardware_iv);
    mt7601u_tx_fail_dump(dev, ep, info);
    // A full status FIFO also stops the MAC; drain it on failure too, or
    // one stall would block every later frame.
    mt7601u_tx_stat_drain(dev);
    return -1;
  }
  MT_DATA_LOG("tx-complete EP%d usb-bytes=%d mpdu=%d\n",
              ep, total, frame_len);
  return 0;
}

static int
mt7601u_tx_mgmt(struct mt7601u_device *dev, uint8 *frame, int frame_len)
{
  return mt7601u_tx_raw(dev, frame, frame_len, 0xff, 0);
}

static int
mt7601u_build_mgmt_header(struct mt7601u_device *dev, uint8 *frame,
                          uint16 frame_control)
{
  memset(frame, 0, 24);
  mt_put16(frame, frame_control);
  memmove(frame + 4, dev->target_bssid, 6);  // receiver
  memmove(frame + 10, dev->mac, 6);         // transmitter
  memmove(frame + 16, dev->target_bssid, 6);// BSSID
  mt_put16(frame + 22, (dev->tx_sequence++ & 0xfff) << 4);
  return 24;
}

static int
mt7601u_send_auth(struct mt7601u_device *dev)
{
  uint8 frame[32];
  int n = mt7601u_build_mgmt_header(dev, frame, 0x00b0);
  mt_put16(frame + n, 0); n += 2; // Open System algorithm
  mt_put16(frame + n, 1); n += 2; // transaction 1
  mt_put16(frame + n, 0); n += 2;
  if(mt7601u_tx_mgmt(dev, frame, n) < 0)
    return -1;
  printf("mt7601u: authentication request sent\n");
  return 0;
}

static int
mt7601u_send_assoc(struct mt7601u_device *dev)
{
  static const uint8 rates[] = { 0x82, 0x84, 0x8b, 0x96,
                                  0x0c, 0x12, 0x18, 0x24 };
  uint8 frame[192];
  int n = mt7601u_build_mgmt_header(dev, frame, 0x0000);
  uint16 capability = dev->target_capability & 0x0531;
  capability |= 0x0001;           // ESS
  mt_put16(frame + n, capability); n += 2;
  mt_put16(frame + n, 10); n += 2; // listen interval
  frame[n++] = 0;                  // SSID element: the network wifi_connect() asked for
  frame[n++] = dev->want_ssid_len;
  memmove(frame + n, dev->want_ssid, dev->want_ssid_len); n += dev->want_ssid_len;
  frame[n++] = 1;
  frame[n++] = sizeof(rates);
  memmove(frame + n, rates, sizeof(rates)); n += sizeof(rates);
  if(dev->target_rsn_ie_len){
    memmove(frame + n, dev->target_rsn_ie, dev->target_rsn_ie_len);
    n += dev->target_rsn_ie_len;
  }
  if(mt7601u_tx_mgmt(dev, frame, n) < 0)
    return -1;
  printf("mt7601u: association request sent rsn=%d bytes\n",
         dev->target_rsn_ie_len);
  return 0;
}

static int
mt7601u_send_llc(struct mt7601u_device *dev, uint8 *destination,
                 uint16 ethertype, uint8 *payload, int length, int encrypted)
{
  uint8 *frame = mt_data_buf;
  int n;
  if(length < 0 || length > MT_NET_FRAME_MAX)
    return -1;
  memset(frame, 0, 24 + 8);
  mt_put16(frame, 0x0108 | (encrypted ? 0x4000 : 0)); // data, ToDS
  memmove(frame + 4, dev->target_bssid, 6);
  memmove(frame + 10, dev->mac, 6);
  memmove(frame + 16, destination, 6);
  mt_put16(frame + 22, (dev->tx_sequence++ & 0xfff) << 4);
  n = 24;
  frame[n++] = 0xaa; frame[n++] = 0xaa; frame[n++] = 0x03;
  frame[n++] = 0; frame[n++] = 0; frame[n++] = 0;
  frame[n++] = ethertype >> 8; frame[n++] = ethertype;
  memmove(frame + n, payload, length);
  n += length;
  // Once associated, even unencrypted EAPOL frames use the AP station WCID.
  // WIV remains set until a hardware key exists, so the MPDU itself is not
  // encrypted and carries no hardware-generated CCMP IV.
  return mt7601u_tx_raw(dev, frame, n, MT_WCID_AP, encrypted);
}

static int
mt7601u_send_m2(struct mt7601u_device *dev, uint8 *m1, int m1len)
{
  uint8 *eapol = mt_eapol_buf, *key;
  int rsn_len = dev->target_rsn_ie_len;
  int body_len = 95 + rsn_len;
  if(m1len < 99 || rsn_len == 0 || 4 + body_len > (int)sizeof(mt_eapol_buf)){
    printf("mt7601u: M2 build rejected m1len=%d rsn=%d body=%d capacity=%d\n",
           m1len, rsn_len, body_len, sizeof(mt_eapol_buf));
    return -1;
  }
  memset(eapol, 0, 4 + body_len);
  eapol[0] = m1[0]; eapol[1] = 3;
  eapol[2] = body_len >> 8; eapol[3] = body_len;
  key = eapol + 4;
  key[0] = m1[4];
  key[1] = 0x01;
  key[2] = 0x08 | (m1[6] & 7); // pairwise + MIC + descriptor version
  memmove(key + 5, m1 + 9, 8);
  memmove(key + 13, dev->snonce, 32);
  key[93] = rsn_len >> 8; key[94] = rsn_len;
  memmove(key + 95, dev->target_rsn_ie, rsn_len);
  wpa_eapol_mic(dev->ptk, eapol, 4 + body_len, key + 77);
  if(mt7601u_send_llc(dev, dev->target_bssid, 0x888e, eapol,
                      4 + body_len, 0) < 0)
    return -1;
  printf("mt7601u: sent WPA2 EAPOL M2 replay=%x%x%x%x\n",
         key[9], key[10], key[11], key[12]);
  return 0;
}

static int
mt7601u_install_ccmp_keys(struct mt7601u_device *dev)
{
  uint8 key[32], iv[8];
  uint32 attr, mode;
  uint16 shared = 0xac00 + dev->gtk_index * 32;
  memset(key, 0, sizeof(key));
  memmove(key, dev->ptk + 32, 16);
  if(mt7601u_write_bytes(dev->udev, 0x8000 + MT_WCID_AP * 32,
                         key, sizeof(key)) < 0)
    return -1;
  memset(iv, 0, sizeof(iv));
  iv[0] = 1; iv[3] = 0x20;
  if(mt7601u_write_bytes(dev->udev, 0xa000 + MT_WCID_AP * 8,
                         iv, sizeof(iv)) < 0 ||
     mt7601u_read32(dev->udev, 0xa800 + MT_WCID_AP * 4, &attr) < 0)
    return -1;
  // AES-CCMP cipher mode 4 and pairwise-key flag, BSS index 0. Written
  // absolutely, not read-modify-write: a stale register read (seen on the
  // RPi as 0x11009) must not leak unrelated bits into the attribute.
  attr = (4U << 1) | 1U;
  if(mt7601u_write32(dev->udev, 0xa800 + MT_WCID_AP * 4, attr) < 0)
    return -1;

  memset(key, 0, sizeof(key));
  memmove(key, dev->gtk, 16);
  if(mt7601u_write_bytes(dev->udev, shared, key, sizeof(key)) < 0 ||
     mt7601u_read32(dev->udev, 0xb000, &mode) < 0)
    return -1;
  // BSS 0 owns the low 16 bits (4 key slots x 4 bits); only the current
  // GTK slot is valid.
  mode = 4U << (4 * dev->gtk_index);
  printf("mt7601u: keys installed: PTK wcid=%d attr=%x, GTK idx=%d skey-mode=%x\n",
         MT_WCID_AP, attr, dev->gtk_index, mode);
  return mt7601u_write32(dev->udev, 0xb000, mode);
}

static int
mt7601u_send_m4(struct mt7601u_device *dev)
{
  uint8 *eapol = mt_eapol_buf, *key;
  int body_len = 95;
  memset(eapol, 0, 4 + body_len);
  eapol[0] = 2; eapol[1] = 3;
  eapol[2] = body_len >> 8; eapol[3] = body_len;
  key = eapol + 4;
  key[0] = dev->m3_desc_type;
  key[1] = 0x03;
  key[2] = 0x08 | dev->m3_desc_version;
  memmove(key + 5, dev->m3_replay, 8);
  wpa_eapol_mic(dev->ptk, eapol, 4 + body_len, key + 77);
  if(mt7601u_send_llc(dev, dev->target_bssid, 0x888e, eapol,
                      4 + body_len, 1) < 0)
    return -1;
  printf("mt7601u: sent WPA2 EAPOL M4\n");
  return 0;
}

static void
mt7601u_rx_eapol(struct mt7601u_device *dev, uint8 *eapol, int length)
{
  uint8 *key;
  uint16 body_len, info;
  if(length < 4 + 95 || eapol[1] != 3)
    return;
  body_len = ((uint16)eapol[2] << 8) | eapol[3];
  if(body_len < 95 || 4 + body_len > length)
    return;
  // Association and the WPA2 handshake share MT_LINK_ASSOCIATED.  Refresh
  // its watchdog for every structurally valid EAPOL-Key frame so an M1/M3
  // exchange in progress is not mistaken for a permanently lost M1.
  dev->state_deadline = workqueue_now() + MT_WPA_TIMEOUT_JIFFIES;
  mod_delayed_work(&mt7601u_wq, &dev->state_work,
                   MT_WPA_TIMEOUT_JIFFIES);
  key = eapol + 4;
  info = ((uint16)key[1] << 8) | key[2];
  if((info & 0x0088) == 0x0088 && (info & 0x0100) == 0){
    if(!dev->handshake_started || memcmp(dev->anonce, key + 13, 32) != 0){
      memmove(dev->anonce, key + 13, 32);
      wpa_make_snonce(dev->pmk, dev->anonce, r_cntvct_el0(), dev->snonce);
      dev->handshake_started = 1;
    }
    wpa_derive_ptk(dev->pmk, dev->target_bssid, dev->mac,
                   dev->anonce, dev->snonce, dev->ptk);
    printf("mt7601u: received WPA2 EAPOL M1 key-info=%x\n", info);
    if(mt7601u_send_m2(dev, eapol, 4 + body_len) < 0)
      printf("mt7601u: failed to transmit WPA2 EAPOL M2\n");
  } else if((info & 0x0188) == 0x0188){
    uint8 received[16], calculated[16], plain[96];
    int data_len = ((int)key[93] << 8) | key[94];
    int plain_len, found = 0;
    memmove(received, key + 77, 16);
    memset(key + 77, 0, 16);
    wpa_eapol_mic(dev->ptk, eapol, 4 + body_len, calculated);
    memmove(key + 77, received, 16);
    if(memcmp(received, calculated, 16) != 0){
      printf("mt7601u: EAPOL M3 MIC verification failed\n");
      return;
    }
    if(dev->handshake_done){
      mt7601u_send_m4(dev);
      return;
    }
    if((info & 0x1000) == 0 || data_len > (int)sizeof(plain) + 8 ||
       95 + data_len > body_len)
      return;
    plain_len = wpa_aes_unwrap(dev->ptk + 16, key + 95, data_len, plain);
    if(plain_len < 0)
      return;
    for(int pos = 0; pos + 2 <= plain_len; pos += 2 + plain[pos + 1]){
      int n = 2 + plain[pos + 1];
      if(pos + n > plain_len)
        break;
      if(plain[pos] == 0xdd && n >= 24 && plain[pos+2] == 0 &&
         plain[pos+3] == 0x0f && plain[pos+4] == 0xac &&
         plain[pos+5] == 1){
        dev->gtk_index = plain[pos+6] & 3;
        memmove(dev->gtk, plain + pos + 8, 16);
        found = 1;
        break;
      }
    }
    if(!found){
      printf("mt7601u: EAPOL M3 contains no GTK\n");
      return;
    }
    memmove(dev->m3_replay, key + 5, 8);
    dev->m3_desc_type = key[0];
    dev->m3_desc_version = info & 7;
    if(mt7601u_install_ccmp_keys(dev) < 0 || mt7601u_send_m4(dev) < 0){
      printf("mt7601u: WPA2 key installation/M4 failed\n");
      return;
    }
    dev->handshake_done = 1;
    printf("mt7601u: WPA2 four-way handshake complete\n");
  }
}

static int
mt7601u_set_channel(struct mt7601u_device *dev, int channel)
{
  // Linux mt7601u 20 MHz frequency plan: RF bank 0 registers 17..20.
  static const uint8 plan[14][4] = {
    {0x99,0x99,0x09,0x50}, {0x46,0x44,0x0a,0x50},
    {0xec,0xee,0x0a,0x50}, {0x99,0x99,0x0b,0x50},
    {0x46,0x44,0x08,0x51}, {0xec,0xee,0x08,0x51},
    {0x99,0x99,0x09,0x51}, {0x46,0x44,0x0a,0x51},
    {0xec,0xee,0x0a,0x51}, {0x99,0x99,0x0b,0x51},
    {0x46,0x44,0x08,0x52}, {0xec,0xee,0x08,0x52},
    {0x99,0x99,0x09,0x52}, {0x33,0x33,0x0b,0x52},
  };
  uint8 gain = 0x37 - dev->lna_gain;
  MT_CFG_LOG("set-channel begin old=%d new=%d gain=%d\n",
             dev->scan_channel, channel, gain);
  if(channel < 1 || channel > 14)
    return -1;
  for(int i = 0; i < 4; i++)
    if(mt7601u_rf_write(dev->udev, 0, 17 + i,
                        plan[channel - 1][i]) < 0)
      return -1;
  if(mt7601u_bbp_write(dev->udev, 62, gain) < 0 ||
     mt7601u_bbp_write(dev->udev, 63, gain) < 0 ||
     mt7601u_bbp_write(dev->udev, 64, gain) < 0)
    return -1;
  // Trigger VCO calibration after changing the synthesizer frequency.
  if(mt7601u_rf_write(dev->udev, 0, 4, 0x0a) < 0 ||
     mt7601u_rf_write(dev->udev, 0, 5, 0x20) < 0 ||
     mt7601u_rf_write(dev->udev, 0, 4, 0x8a) < 0)
    return -1;
  mt7601u_delay_ms(2);
  dev->scan_channel = channel;
  MT_CFG_LOG("set-channel end channel=%d\n", channel);
  return 0;
}

static void
mt7601u_rx_mgmt(struct mt7601u_device *dev, uint8 *frame, int length)
{
  uint16 fc;
  uint16 capability;
  uint8 ssid[32], bssid[6];
  int subtype, pos, ssid_len = 0, channel = 0;
  int security, slot = -1, empty = -1;
  if(length < 24)
    return;
  fc = mt_get16(frame);
  if(((fc >> 2) & 3) != 0)
    return;
  subtype = (fc >> 4) & 0xf;
  dev->mgmt_frames++;
  // Non-beacon management frames addressed to us: shows auth/assoc
  // responses, deauth reasons and retransmissions (retry bit = the AP
  // did not see our ACK for the previous copy).
  if(subtype != 8 && mt_mac_equal(frame + 4, dev->mac))
    MT_DATA_LOG("rx-mgmt subtype=%d retry=%d seq=%d from=%x:%x:%x len=%d body=%x %x %x\n",
                subtype, !!(fc & 0x0800), mt_get16(frame + 22) >> 4,
                frame[13], frame[14], frame[15], length,
                length >= 26 ? mt_get16(frame + 24) : 0,
                length >= 28 ? mt_get16(frame + 26) : 0,
                length >= 30 ? mt_get16(frame + 28) : 0);

  // Deauthentication (12) / disassociation (10) from our AP (from the
  // rpi3-BCM43430 driver): the AP dropped our state (reason 4 inactivity,
  // 6/7 class 2/3 frame, 15 four-way timeout). Re-authenticate with the
  // same BSS; wlan0 stays registered, data waits for the new handshake.
  if((subtype == 12 || subtype == 10) && length >= 26 &&
     mt_mac_equal(frame + 4, dev->mac) &&
     mt_mac_equal(frame + 10, dev->target_bssid)){
    printf("mt7601u: %s from AP reason=%d (link_state=%d handshake=%d)\n",
           subtype == 12 ? "deauthentication" : "disassociation",
           mt_get16(frame + 24), dev->link_state, dev->handshake_done);
    if(dev->link_state >= MT_LINK_AUTH_SENT){
      dev->handshake_done = 0;
      dev->handshake_started = 0;
      memset(dev->anonce, 0, sizeof(dev->anonce));
      memset(dev->snonce, 0, sizeof(dev->snonce));
      memset(dev->ptk, 0, sizeof(dev->ptk));
      dev->link_state = MT_LINK_AUTH_PENDING;
      dev->link_retries = 0;
      dev->state_deadline = workqueue_now() + MT_STATE_SOON_JIFFIES;
      mod_delayed_work(&mt7601u_wq, &dev->state_work, MT_STATE_SOON_JIFFIES);
      printf("mt7601u: reconnecting\n");
    }
    return;
  }

  // Open-System Authentication response (transaction 2).
  if(subtype == 11 && length >= 30 &&
     mt_mac_equal(frame + 4, dev->mac) &&
     mt_mac_equal(frame + 10, dev->target_bssid) &&
     mt_get16(frame + 24) == 0 && mt_get16(frame + 26) == 2){
    uint16 status = mt_get16(frame + 28);
    if(status == 0 && dev->link_state == MT_LINK_AUTH_SENT){
      dev->link_state = MT_LINK_ASSOC_PENDING;
      dev->link_retries = 0;
      dev->state_deadline = workqueue_now() + MT_STATE_SOON_JIFFIES;
      mod_delayed_work(&mt7601u_wq, &dev->state_work,
                       MT_STATE_SOON_JIFFIES);
      printf("mt7601u: Open-System authentication accepted\n");
    } else if(status != 0) {
      printf("mt7601u: authentication rejected status=%d\n", status);
      dev->link_state = MT_LINK_AUTH_PENDING;
      dev->state_deadline = workqueue_now() + MT_STATE_SOON_JIFFIES;
      mod_delayed_work(&mt7601u_wq, &dev->state_work,
                       MT_STATE_SOON_JIFFIES);
    }
    return;
  }

  // Association response body: capability, status and association id.
  if(subtype == 1 && length >= 30 &&
     mt_mac_equal(frame + 4, dev->mac) &&
     mt_mac_equal(frame + 10, dev->target_bssid)){
    uint16 status = mt_get16(frame + 26);
    if(status == 0 && dev->link_state == MT_LINK_ASSOC_SENT){
      uint16 aid = mt_get16(frame + 28) & 0x3fff;
      dev->link_state = MT_LINK_ASSOCIATED;
      dev->state_deadline = workqueue_now() + MT_WPA_TIMEOUT_JIFFIES;
      mod_delayed_work(&mt7601u_wq, &dev->state_work,
                       MT_WPA_TIMEOUT_JIFFIES);
      printf("mt7601u: 802.11 associated aid=%d; WPA2 handshake pending\n",
             aid);
    } else if(status != 0) {
      printf("mt7601u: association rejected status=%d\n", status);
      dev->link_state = MT_LINK_AUTH_PENDING;
      dev->state_deadline = workqueue_now() + MT_STATE_SOON_JIFFIES;
      mod_delayed_work(&mt7601u_wq, &dev->state_work,
                       MT_STATE_SOON_JIFFIES);
    }
    return;
  }

  // Beacon and probe-response bodies start with 12 fixed bytes.
  if((subtype != 8 && subtype != 5) || length < 36)
    return;
  memset(ssid, 0, sizeof(ssid));
  memmove(bssid, frame + 16, sizeof(bssid));
  capability = mt_get16(frame + 24 + 10);
  security = (capability & 0x10) ? MT_SCAN_WEP : MT_SCAN_OPEN;
  pos = 24 + 12;
  while(pos + 2 <= length){
    int id = frame[pos], n = frame[pos + 1];
    pos += 2;
    if(pos + n > length)
      break;
    if(id == 0 && n <= 32){
      ssid_len = n;
      memmove(ssid, frame + pos, n);
    } else if(id == 3 && n == 1){
      channel = frame[pos];
    } else if(id == 48){
      security = MT_SCAN_WPA2;
    } else if(id == 221 && n >= 4 &&
              frame[pos] == 0x00 && frame[pos + 1] == 0x50 &&
              frame[pos + 2] == 0xf2 && frame[pos + 3] == 1 &&
              security != MT_SCAN_WPA2){
      security = MT_SCAN_WPA;
    }
    pos += n;
  }

  for(int i = 0; i < MT_SCAN_MAX; i++){
    if(!dev->scan[i].valid){
      if(empty < 0)
        empty = i;
      continue;
    }
    int same = 1;
    for(int j = 0; j < 6; j++)
      if(dev->scan[i].bssid[j] != bssid[j]){
        same = 0;
        break;
      }
    if(same){
      slot = i;
      break;
    }
  }
  if(slot < 0)
    slot = empty;
  if(slot < 0)
    return;

  struct mt7601u_scan_entry *ap = &dev->scan[slot];
  static char *security_name[] = { "open", "WEP", "WPA", "WPA2" };
  int is_new = !ap->valid;
  int revealed = ap->valid && ap->ssid_len == 0 && ssid_len != 0;
  ap->valid = 1;
  memmove(ap->bssid, bssid, sizeof(ap->bssid));
  if(ssid_len != 0 || ap->ssid_len == 0){
    memmove(ap->ssid, ssid, ssid_len);
    ap->ssid[ssid_len] = 0;
    ap->ssid_len = ssid_len;
  }
  if(channel)
    ap->channel = channel;
  ap->security = security;
  ap->capability = capability;
  pos = 24 + 12;
  while(pos + 2 <= length){
    int id = frame[pos], n = frame[pos + 1];
    if(pos + 2 + n > length)
      break;
    if(id == 48 && n + 2 <= MT_RSN_IE_MAX){
      ap->rsn_ie_len = n + 2;
      memmove(ap->rsn_ie, frame + pos, n + 2);
      break;
    }
    pos += n + 2;
  }
  if(is_new || revealed){
    printf("mt7601u: AP ssid=%s bssid=%x:%x:%x:%x:%x:%x channel=%d %s\n",
           ap->ssid_len ? ap->ssid : "<hidden>",
           ap->bssid[0], ap->bssid[1], ap->bssid[2], ap->bssid[3],
           ap->bssid[4], ap->bssid[5], ap->channel,
           security_name[ap->security]);
    dev->scan_seen++;
  }
  if(dev->want_ssid_len && !dev->target_found && ssid_len == dev->want_ssid_len){
    int match = 1;
    for(int i = 0; i < ssid_len; i++)
      if(ssid[i] != (uint8)dev->want_ssid[i]){
        match = 0;
        break;
      }
    if(match){
      uint32 bssid0 = (uint32)ap->bssid[0] |
        ((uint32)ap->bssid[1] << 8) | ((uint32)ap->bssid[2] << 16) |
        ((uint32)ap->bssid[3] << 24);
      uint32 bssid1 = (uint32)ap->bssid[4] |
        ((uint32)ap->bssid[5] << 8);
      dev->target_found = 1;
      dev->target_channel = ap->channel;
      memmove(dev->target_bssid, ap->bssid, 6);
      dev->target_capability = ap->capability;
      dev->target_rsn_ie_len = ap->rsn_ie_len;
      if(ap->rsn_ie_len)
        memmove(dev->target_rsn_ie, ap->rsn_ie, ap->rsn_ie_len);
      dev->link_state = MT_LINK_AUTH_PENDING;
      dev->link_retries = 0;
      dev->state_deadline = workqueue_now() + MT_STATE_SOON_JIFFIES;
      mod_delayed_work(&mt7601u_wq, &dev->state_work,
                       MT_STATE_SOON_JIFFIES);
      // Enable hardware address matching/automatic ACK for this BSS.  The
      // receive filter remains broad until association has completed.
      if(mt7601u_write32(dev->udev, 0x1010, bssid0) < 0 ||
         mt7601u_write32(dev->udev, 0x1014, bssid1) < 0 ||
         mt7601u_write32(dev->udev, 0x1800 + MT_WCID_AP * 8,
                         bssid0) < 0 ||
         mt7601u_write32(dev->udev, 0x1804 + MT_WCID_AP * 8,
                         bssid1) < 0 ||
         mt7601u_write32(dev->udev, 0xa800 + MT_WCID_AP * 4, 0) < 0)
        printf("mt7601u: warning: cannot program target BSSID\n");
      printf("mt7601u: target %s found bssid=%x:%x:%x:%x:%x:%x channel=%d %s (radio on %d)\n",
             dev->want_ssid, ap->bssid[0], ap->bssid[1], ap->bssid[2], ap->bssid[3],
             ap->bssid[4], ap->bssid[5], ap->channel,
             security_name[ap->security], dev->scan_channel);
    }
  }
}

static void
mt7601u_rx_data(struct mt7601u_device *dev, uint8 *frame, int length,
                uint32 rxinfo)
{
  uint16 fc, type;
  uint8 *llc, *source;
  int header = 24;
  if(length < 32)
    return;
  fc = mt_get16(frame);
  if(((fc >> 2) & 3) != 2)
    return;
  int to_me = mt_mac_equal(frame + 4, dev->mac);
  if(to_me && dev->handshake_done){
    dev->rx_ucast++;
    MT_FRAME_LOG("rx unicast fc=%x len=%d rxinfo=%x src=%x:%x:%x:%x:%x:%x\n",
                 fc, length, rxinfo, frame[16], frame[17], frame[18],
                 frame[19], frame[20], frame[21]);
  }
  if(dev->handshake_done)
    MT_FRAME_LOG("rx data fc=%x len=%d rxinfo=%x a1=%x:%x:%x:%x:%x:%x\n",
                 fc, length, rxinfo, frame[4], frame[5], frame[6],
                 frame[7], frame[8], frame[9]);
  if(((fc >> 4) & 8) != 0)
    header += 2; // QoS control
  if(rxinfo & (1U << 14))
    header += 2; // hardware L2 padding
  // Hardware-decrypted CCMP frames (RXINFO DECRYPT) keep the Protected bit
  // and the 8-byte CCMP header (PN, key id) in front of the LLC/SNAP header;
  // the MIC is already removed. Skip it, as Linux mt76 does.
  if(fc & 0x4000){
    if(!(rxinfo & (1U << 16))){
      if(to_me)
        dev->rx_ucast_enc++;
      return;      // still encrypted: no key for it
    }
    header += 8;
  }
  if(header + 8 > length)
    return;
  llc = frame + header;
  if(llc[0] != 0xaa || llc[1] != 0xaa || llc[2] != 3 ||
     llc[3] != 0 || llc[4] != 0 || llc[5] != 0)
    return;
  type = ((uint16)llc[6] << 8) | llc[7];
  if(type == 0x888e){
    mt7601u_rx_eapol(dev, llc + 8, length - header - 8);
    return;
  }
  if(dev->handshake_done && dev->net_rx_len != 0 && to_me)
    dev->rx_ucast_busy++;
  if(!dev->handshake_done || dev->net_rx_len != 0 ||
     length - header > MT_NET_FRAME_MAX - 6)
    return;
  // For frames from the distribution system addr3 is the Ethernet source;
  // addr1 is the local/multicast Ethernet destination.
  source = (fc & 0x0200) ? frame + 16 : frame + 10;
  memmove(dev->net_rx, frame + 4, 6);
  memmove(dev->net_rx + 6, source, 6);
  dev->net_rx[12] = type >> 8;
  dev->net_rx[13] = type;
  memmove(dev->net_rx + 14, llc + 8, length - header - 8);
  dev->net_rx_len = 14 + length - header - 8;
  MT_DATA_LOG("rx-decapsulate ethertype=%x ethernet-bytes=%d to-me=%d\n",
              type, dev->net_rx_len, to_me);
}

static void
mt7601u_rx_parse(struct mt7601u_device *dev, uint8 *data, int length)
{
  int offset = 0;
  MT_FRAME_LOG("rx-aggregate usb-bytes=%d\n", length);
  while(offset + 8 + MT_RXWI_SIZE + 4 <= length){
    uint8 *segment = data + offset;
    uint16 dma_len = mt_get16(segment);
    int segment_len = dma_len + 8;
    uint8 *rxwi, *frame;
    uint32 rxinfo, ctl;
    int mpdu_len, available;
    if(dma_len == 0 || (dma_len & 3) || segment_len > length - offset){
      dev->rx_bad++;
      break;
    }
    rxwi = segment + 4;
    rxinfo = mt_get32(rxwi);
    ctl = mt_get32(rxwi + 4);
    mpdu_len = (ctl >> 16) & 0xfff;
    frame = rxwi + MT_RXWI_SIZE;
    available = segment_len - 4 - MT_RXWI_SIZE - 4;
    MT_FRAME_LOG("rx-segment off=%d dma=%d mpdu=%d rxinfo=%x available=%d\n",
                 offset, dma_len, mpdu_len, rxinfo, available);
    if(mpdu_len <= 0 || mpdu_len > available ||
       (rxinfo & (MT_RXINFO_CRCERR | MT_RXINFO_ICVERR |
                  MT_RXINFO_MICERR))){
      dev->rx_bad++;
      MT_DATA_LOG("rx-drop rxinfo=%x crc=%d icv=%d mic=%d mpdu=%d avail=%d fc=%x\n",
                  rxinfo, !!(rxinfo & MT_RXINFO_CRCERR),
                  !!(rxinfo & MT_RXINFO_ICVERR),
                  !!(rxinfo & MT_RXINFO_MICERR), mpdu_len, available,
                  mpdu_len > 1 && mpdu_len <= available ? mt_get16(frame) : 0);
    } else {
      dev->rx_frames++;
      // Who sent it and with which sequence number: tells real traffic
      // (sequence numbers advance) from a stale buffer replayed by the
      // host side (same sequence number again and again).
      if(MT7601U_DATA_TRACE && mpdu_len >= 24 &&
         (dev->rx_frames <= 30 || (dev->rx_frames & 63) == 0))
        MT_DATA_LOG("rx-frame #%d fc=%x from=%x:%x:%x seq=%d len=%d\n",
                    dev->rx_frames, mt_get16(frame), frame[13], frame[14],
                    frame[15], mt_get16(frame + 22) >> 4, mpdu_len);
      if(((mt_get16(frame) >> 2) & 3) == 0){
        // RXWI PHY fields (Linux struct mt7601u_rxwi): snr, gain (LNA in
        // bits 7:6, VGA in 5:0) and the measured carrier frequency offset
        // of frames from the AP we are joining.  Linux uses freq_off to
        // trim the crystal (mt7601u_phy_freq_cal); this driver does not.
        static int phy_logged;
        if(MT7601U_DATA_TRACE && phy_logged < 40 && mpdu_len >= 16 &&
           mt_mac_equal(frame + 10, dev->target_bssid)){
          phy_logged++;
          MT_DATA_LOG("rx-phy from-ap subtype=%d snr=%d gain=%x lna=%d vga=%d freq_off=%d rate=%x\n",
                      (mt_get16(frame) >> 4) & 0xf, rxwi[16], rxwi[18],
                      rxwi[18] >> 6, rxwi[18] & 0x3f, (int)(signed char)rxwi[19],
                      mt_get16(rxwi + 10));
        }
        dev->rx_mgmt++;
        mt7601u_rx_mgmt(dev, frame, mpdu_len);
      } else {
        dev->rx_datafr++;
        mt7601u_rx_data(dev, frame, mpdu_len, rxinfo);
      }
    }
    offset += segment_len;
  }
}

static int
mt7601u_service(int service_rx, int service_state)
{
  int got, deliver = 0, register_needed = 0, rx_done = 0;
  uint64 now = workqueue_now();
  void *rx_data = 0;
  struct mt7601u_device *dev = &mt7601u;
  if(!dev->used || !dev->mcu_running || !dev->endpoints_ready || dev->paused ||
     dev->udev == 0)
    return 0;
  acquire(&dev->lock);
  if(service_rx && dev->udev->ops->bulk_rx_arm &&
     dev->udev->ops->bulk_rx_complete){
    got = dev->udev->ops->bulk_rx_complete(dev->udev,
                                            dev->udev->bulk_in_ep,
                                            &rx_data);
    if(got >= 0){
      dev->rx_urbs++;
      rx_done = 1;
      MT_FRAME_LOG("rx-completion EP%d bytes=%d buffer=%x urb-count=%d\n",
                  dev->udev->bulk_in_ep, got, (uint)rx_data, dev->rx_urbs);
      if(got > 0)
        mt7601u_rx_parse(dev, rx_data, got);
      dev->udev->ops->bulk_rx_arm(dev->udev, dev->udev->bulk_in_ep,
                                  rx_data, MT_RX_BUF_SIZE);
    } else if(got == -1 && rx_data) {
      rx_done = 1;
      dev->rx_usb_err++;
      MT_DATA_LOG("rx-completion EP%d error buffer=%x errors=%d\n",
                  dev->udev->bulk_in_ep, (uint)rx_data, dev->rx_usb_err);
      // Return a failed request's buffer to the ring; the remaining queued
      // buffers kept the endpoint live while this completion was handled.
      dev->udev->ops->bulk_rx_arm(dev->udev, dev->udev->bulk_in_ep,
                                  rx_data, MT_RX_BUF_SIZE);
    }
  } else if(service_rx) {
    got = dev->udev->ops->bulk(dev->udev, dev->udev->bulk_in_ep, 1,
                               mt_rx_buf[0], MT_RX_BUF_SIZE);
    if(got > 0){
      rx_done = 1;
      mt7601u_rx_parse(dev, mt_rx_buf[0], got);
    }
  }

  if(service_state && dev->link_state == MT_LINK_AUTH_PENDING &&
     dev->target_channel >= 1 && dev->target_channel <= 14 &&
     dev->scan_channel != dev->target_channel){
    // A beacon from a neighbouring channel can be heard while the radio is
    // tuned 1-2 channels away (from the rpi3-BCM43430 driver). Associating
    // there "works" but the link is weak: few frames, DHCP offers lost.
    // Tune to the AP's own channel first, then authenticate.
    printf("mt7601u: tuning to AP channel %d (radio on %d)\n",
           dev->target_channel, dev->scan_channel);
    if(mt7601u_set_channel(dev, dev->target_channel) < 0){
      printf("mt7601u: channel switch to %d failed\n", dev->target_channel);
      dev->target_channel = 0;      // authenticate where we are
    }
    dev->state_deadline = now + MT_STATE_SOON_JIFFIES;
  } else if(service_state && dev->link_state == MT_LINK_AUTH_PENDING){
    if(dev->link_retries >= 3){
      printf("mt7601u: authentication timeout\n");
      dev->link_state = MT_LINK_SCAN;
      dev->target_found = 0;
      dev->scan_deadline = now + MT_SCAN_DWELL_JIFFIES;
    } else {
      int sent = mt7601u_send_auth(dev);
      dev->link_retries++;
      if(sent == 0){
        dev->link_state = MT_LINK_AUTH_SENT;
        dev->state_deadline = now + MT_RESPONSE_TIMEOUT_JIFFIES;
      } else {
        dev->state_deadline = now + MT_STATE_SOON_JIFFIES;
      }
    }
  } else if(service_state && dev->link_state == MT_LINK_AUTH_SENT){
    if((long)(now - dev->state_deadline) >= 0){
      // The frame has been on the air for a while now: report whether the
      // AP acknowledged it (the drain right after the USB write is too early).
      mt7601u_tx_stat_drain(dev);
      dev->link_state = MT_LINK_AUTH_PENDING;
      dev->state_deadline = now + MT_STATE_SOON_JIFFIES;
    }
  } else if(service_state && dev->link_state == MT_LINK_ASSOC_PENDING){
    if(dev->link_retries >= 3){
      printf("mt7601u: association timeout\n");
      dev->link_state = MT_LINK_AUTH_PENDING;
      dev->link_retries = 0;
      dev->state_deadline = now + MT_STATE_SOON_JIFFIES;
    } else {
      int sent = mt7601u_send_assoc(dev);
      dev->link_retries++;
      if(sent == 0){
        dev->link_state = MT_LINK_ASSOC_SENT;
        dev->state_deadline = now + MT_RESPONSE_TIMEOUT_JIFFIES;
      } else {
        dev->state_deadline = now + MT_STATE_SOON_JIFFIES;
      }
    }
  } else if(service_state && dev->link_state == MT_LINK_ASSOC_SENT){
    if((long)(now - dev->state_deadline) >= 0){
      // The frame has been on the air for a while now: report whether the
      // AP acknowledged it (the drain right after the USB write is too early).
      mt7601u_tx_stat_drain(dev);
      dev->link_state = MT_LINK_ASSOC_PENDING;
      dev->state_deadline = now + MT_STATE_SOON_JIFFIES;
    }
  } else if(service_state && dev->link_state == MT_LINK_ASSOCIATED &&
            !dev->handshake_done){
    // EP4 completion is IRQ-driven, but retain a five-second protocol
    // watchdog in case the AP never sends M1 or a USB channel hard-fails.
    if((long)(now - dev->state_deadline) >= 0){
      printf("mt7601u: WPA2 M1 timeout; reassociating\n");
      dev->link_state = MT_LINK_ASSOC_PENDING;
      dev->link_retries = 0;
      dev->state_deadline = now + MT_STATE_SOON_JIFFIES;
      dev->handshake_started = 0;
      memset(dev->anonce, 0, sizeof(dev->anonce));
      memset(dev->snonce, 0, sizeof(dev->snonce));
      memset(dev->ptk, 0, sizeof(dev->ptk));
    }
  }
  // Dwell until the real scan deadline rather than waking every 10 ms merely
  // to increment a counter.
  if(service_state && !dev->target_found &&
     (long)(now - dev->scan_deadline) >= 0){
    int next = MT7601U_SCAN_CHAN ? MT7601U_SCAN_CHAN :
               (dev->scan_channel >= 11 ? 1 : dev->scan_channel + 1);
    if(next == 1)
      dev->scan_rounds++;
    // Per-dwell receive report: a deaf receiver after re-tuning shows up as
    // rx=0 on every channel, a missing AP as rx>0 but no beacon from it.
    if(dev->scan_dwell_logs < 33){
      dev->scan_dwell_logs++;
      printf("mt7601u: scan ch%d dwell usb-rx=%d mgmt=%d aps=%d\n",
             dev->scan_channel, dev->rx_urbs - dev->scan_rx_mark,
             dev->mgmt_frames - dev->scan_mgmt_mark, dev->scan_seen);
    }
    dev->scan_rx_mark = dev->rx_urbs;
    dev->scan_mgmt_mark = dev->mgmt_frames;
    // MT7601U_SCAN_CHAN: already there; re-tuning (VCO calibration) every
    // dwell would only make the radio deaf for a moment.
    if(next != dev->scan_channel && mt7601u_set_channel(dev, next) < 0)
      printf("mt7601u: channel switch to %d failed\n", next);
    dev->scan_deadline = now + MT_SCAN_DWELL_JIFFIES;
  }
  if(dev->handshake_done && !dev->net_registered)
    register_needed = 1;
  if(dev->handshake_done && dev->net_registered && dev->net_rx_len){
    deliver = dev->net_rx_len;
    dev->net_rx_len = 0;
  }
  release(&dev->lock);

  if(register_needed){
    if(register_netdev(&dev->netdev) == 0){
      dev->net_registered = 1;
      printf("mt7601u: wlan0 registered mac=%x:%x:%x:%x:%x:%x\n",
             dev->mac[0], dev->mac[1], dev->mac[2], dev->mac[3],
             dev->mac[4], dev->mac[5]);
    } else {
      printf("mt7601u: cannot register wlan1\n");
    }
  }
  if(deliver){
    MT_DATA_LOG("deliver ethernet bytes=%d -> net_rx_dev(wlan0)\n", deliver);
    net_rx_dev(&dev->netdev, dev->net_rx, deliver);
  }
  return rx_done;
}

void
mt7601u_poll(void)
{
  mt7601u_service(1, 1);
}

static int
mt7601u_napi_poll(struct napi_struct *napi, int budget)
{
  int done = 0;
  MT_FRAME_LOG("NAPI poll begin budget=%d\n", budget);
  while(done < budget){
    int one = mt7601u_service(1, 0);
    if(one == 0)
      break;
    done += one;
  }
  if(done < budget)
    napi_complete_done(napi, done);
  MT_FRAME_LOG("NAPI poll end done=%d completed=%d\n", done, done < budget);
  return done;
}

static void
mt7601u_state_worker(struct work_struct *work)
{
  uint64 now, deadline = 0;
  (void)work;
  mt7601u_service(0, 1);
  // IRQ-driven RX remains independent.  Re-arm at the next real scan,
  // response or WPA2 watchdog deadline instead of polling every 10 ms.
  acquire(&mt7601u.lock);
  if(mt7601u.used && !mt7601u.handshake_done){
    now = workqueue_now();
    switch(mt7601u.link_state){
    case MT_LINK_SCAN:
      deadline = mt7601u.target_found ?
        now + MT_STATE_SOON_JIFFIES : mt7601u.scan_deadline;
      break;
    case MT_LINK_AUTH_PENDING:
    case MT_LINK_ASSOC_PENDING:
      deadline = now + MT_STATE_SOON_JIFFIES;
      break;
    case MT_LINK_AUTH_SENT:
    case MT_LINK_ASSOC_SENT:
    case MT_LINK_ASSOCIATED:
      deadline = mt7601u.state_deadline;
      break;
    }
    if(deadline){
      uint64 delay = (long)(deadline - now) > 0 ? deadline - now : 1;
      queue_delayed_work(&mt7601u_wq, &mt7601u.state_work, delay);
    }
  }
  release(&mt7601u.lock);
}

void
mt7601u_rx_irq(void)
{
  // Before the probe has finished, every EHCI IRQ is one of the probe's own
  // control transfers and NAPI is not enabled yet: nothing to schedule.
  static uint32 irq_count;
  if(mt7601u.used && mt7601u.ready){
    // One line per IRQ floods the console; log the first few, then one
    // summary line every 256 interrupts.
    irq_count++;
    if(irq_count <= 4 || (irq_count & 255) == 0)
      MT_DATA_LOG("EHCI IRQ -> napi_schedule (#%d)\n", irq_count);
    napi_schedule(&mt7601u.napi);
  }
}

void
mt7601u_pause(int paused)
{
  struct mt7601u_device *dev = &mt7601u;
  if(!dev->used)
    return;
  acquire(&dev->lock);
  dev->paused = paused != 0;
  release(&dev->lock);
}

static int
mt7601u_net_xmit(struct net_device *netdev, void *packet, int length)
{
  struct mt7601u_device *dev = netdev ? netdev->priv : 0;
  uint8 *ether = packet;
  uint16 type;
  int result;
  if(dev == 0 || !dev->handshake_done || length < 14 ||
     length > MT_NET_FRAME_MAX)
    return -1;
  type = ((uint16)ether[12] << 8) | ether[13];
  acquire(&dev->lock);
  result = mt7601u_send_llc(dev, ether, type, ether + 14,
                            length - 14, 1);
  release(&dev->lock);
  MT_DATA_LOG("net-xmit ethertype=%x len=%d dst=%x:%x:%x:%x:%x:%x result=%d\n",
              type, length, ether[0], ether[1], ether[2], ether[3],
              ether[4], ether[5], result);
  return result;
}

static void
mt7601u_net_poll(struct net_device *netdev)
{
  // RX completion and protocol timers own separate private work items.  Keep
  // the net_device hook for API compatibility without re-running both paths
  // from the global network poller.
  (void)netdev;
}

static int
mt7601u_basic_hw_init(struct mt7601u_device *dev)
{
  struct usb_device *udev = dev->udev;
  uint32 value, mac0, mac1;
  MT_CFG_LOG("basic init begin: enable WLAN clock/PLL and USB bulk DMA\n");
  if(mt7601u_read32(udev, MT_WLAN_FUN_CTRL, &value) < 0 ||
     mt7601u_write32(udev, MT_WLAN_FUN_CTRL,
                     value | MT_WLAN_FUN_CTRL_EN) < 0 ||
     mt7601u_poll32(udev, MT_CMB_CTRL, MT_CMB_CTRL_READY,
                    MT_CMB_CTRL_READY, 200) < 0){
    printf("mt7601u: WLAN clock/PLL not ready\n");
    return -1;
  }
  // Reset CSR/BBP, then enable high-speed RX aggregation and bulk DMA.
  mt7601u_write32(udev, MT_MAC_SYS_CTRL, 3);
  mt7601u_write32(udev, MT_USB_DMA_CFG, 0);
  mt7601u_delay_ms(1);
  mt7601u_write32(udev, MT_MAC_SYS_CTRL, 0);
  if(mt7601u_write32(udev, MT_USB_DMA_CFG,
                     MT_USB_DMA_CFG_RX_BULK_EN |
                     MT_USB_DMA_CFG_TX_BULK_EN | (1U << 21) |
                     (0x80U << 8) | 0x20) < 0)
    return -1;
  mac0 = (uint32)dev->mac[0] | ((uint32)dev->mac[1] << 8) |
         ((uint32)dev->mac[2] << 16) | ((uint32)dev->mac[3] << 24);
  // Bits 23:16 = UNICAST_TO_ME mask, 0xff as Linux mt7601u_set_macaddr().
  mac1 = (uint32)dev->mac[4] | ((uint32)dev->mac[5] << 8) | (0xffU << 16);
  if(mt7601u_write32(udev, MT_MAC_ADDR_DW0, mac0) < 0 ||
     mt7601u_write32(udev, MT_MAC_ADDR_DW1, mac1) < 0)
    return -1;
  // Verify that the BBP indirect port is idle before later table loading.
  if(mt7601u_poll32(udev, MT_BBP_CSR_CFG, 1U << 17, 0, 1000) < 0){
    printf("mt7601u: BBP interface busy\n");
    return -1;
  }
  // Program the crystal frequency offset into RF bank 0 register 12.
  if(mt7601u_poll32(udev, MT_RF_CSR_CFG, MT_RF_CSR_KICK, 0, 100) < 0 ||
     mt7601u_write32(udev, MT_RF_CSR_CFG,
                     dev->freq_offset | (12U << 8) |
                     MT_RF_CSR_WRITE | MT_RF_CSR_KICK) < 0 ||
     mt7601u_poll32(udev, MT_RF_CSR_CFG, MT_RF_CSR_KICK, 0, 100) < 0){
    printf("mt7601u: RF bootstrap failed\n");
    return -1;
  }
  printf("mt7601u: MAC/BBP/RF bootstrap ready; rx-ep=%d data, %d MCU\n",
         udev->bulk_in_ep, udev->bulk_in_ep2);
  MT_CFG_LOG("basic init end USB_DMA_CFG=%x MAC=%x:%x\n",
             MT_USB_DMA_CFG_RX_BULK_EN | MT_USB_DMA_CFG_TX_BULK_EN |
             (1U << 21) | (0x80U << 8) | 0x20, mac1, mac0);
  return 0;
}

static int
mt7601u_validate_firmware(struct fw_file *file)
{
  struct mt7601u_fw_header header;
  uint32 expected;
  if(fw_open("mt7601u.bin", file) < 0){
    printf("mt7601u: firmware mt7601u.bin missing\n");
    return -1;
  }
  if(file->size < sizeof(header) ||
     fw_pread(file, 0, &header, sizeof(header)) != sizeof(header))
    return -1;
  expected = sizeof(header) + header.ilm_len + header.dlm_len;
  if(header.ilm_len <= MT_MCU_IVB_SIZE || expected != file->size){
    printf("mt7601u: invalid firmware size=%d ilm=%d dlm=%d\n",
           file->size, header.ilm_len, header.dlm_len);
    return -1;
  }
  printf("mt7601u: firmware v%d.%d.%d build=%x size=%d validated\n",
         (header.fw_ver >> 12) & 0xf, (header.fw_ver >> 8) & 0xf,
         header.fw_ver & 0xff, header.build_ver, file->size);
  return 0;
}

static int
mt7601u_probe(struct usb_device *udev)
{
  struct fw_file firmware;
  if(mt7601u.used)
    return -1;
  memset(&mt7601u, 0, sizeof(mt7601u));
  initlock(&mt7601u.lock, "mt7601u");
  MT_CFG_LOG("probe begin addr=%d vid:pid=%x:%x bulk-in=%d,%d bulk-out=%d mps=%d\n",
             udev->address, udev->vendor, udev->product,
             udev->bulk_in_ep, udev->bulk_in_ep2, udev->bulk_out_ep,
             udev->bulk_out_max_packet);
  if(mt7601u_read32(udev, MT_ASIC_VERSION, &mt7601u.asic_rev) < 0 ||
     mt7601u_read32(udev, MT_MAC_CSR0, &mt7601u.mac_rev) < 0){
    printf("mt7601u: USB vendor register read failed\n");
    return -1;
  }
  if((mt7601u.asic_rev >> 16) != 0x7601){
    printf("mt7601u: ASIC mismatch asic=%x mac=%x\n",
           mt7601u.asic_rev, mt7601u.mac_rev);
    return -1;
  }
  printf("mt7601u: USB device %x:%x ASIC=%x MAC=%x\n",
         udev->vendor, udev->product, mt7601u.asic_rev, mt7601u.mac_rev);
  printf("mt7601u: driver WPA2-v4 (M1 timeout recovery)\n");
  if(mt7601u_validate_firmware(&firmware) < 0)
    return -1;
  MT_CFG_LOG("probe phase=firmware-load\n");
  if(mt7601u_load_firmware(udev, &firmware) < 0){
    printf("mt7601u: MCU firmware load failed\n");
    return -1;
  }
  netif_napi_add(&mt7601u.netdev, &mt7601u.napi, &mt7601u_wq,
                 mt7601u_napi_poll, MT_RX_REQUESTS);
  init_delayed_work(&mt7601u.state_work, mt7601u_state_worker);
  mt7601u.used = 1;
  mt7601u.udev = udev;
  mt7601u.firmware_size = firmware.size;
  mt7601u.mcu_running = 1;
  MT_CFG_LOG("probe phase=eFuse/basic/MCU/full-hardware\n");
  if(mt7601u_read_eeprom(&mt7601u) < 0 ||
     mt7601u_basic_hw_init(&mt7601u) < 0 ||
     mt7601u_mcu_channel_init(&mt7601u) < 0 ||
     mt7601u_full_hw_init(&mt7601u) < 0){
    printf("mt7601u: post-firmware hardware initialization failed\n");
    memset(&mt7601u, 0, sizeof(mt7601u));
    return -1;
  }
  if(mt7601u_set_channel(&mt7601u, 1) < 0){
    printf("mt7601u: initial channel setup failed\n");
    memset(&mt7601u, 0, sizeof(mt7601u));
    return -1;
  }
  mt7601u.scan_deadline = workqueue_now() + MT_SCAN_DWELL_JIFFIES;
  // The network to join comes from wifi_connect() (mt7601u_connect); until
  // then the state machine only scans and lists access points.
  mt7601u.netdev.dev.name = "wlan0";
  mt7601u.netdev.dev.parent = &udev->dev;
  mt7601u.netdev.ops = &mt7601u_netdev_ops;
  mt7601u.netdev.priv = &mt7601u;
  mt7601u.netdev.mtu = NET_MTU;
  memmove(mt7601u.netdev.mac, mt7601u.mac, 6);
  napi_enable(&mt7601u.napi);
  udev->dev.driver_data = &mt7601u;
  MT_CFG_LOG("probe phase=rx-ring EP%d requests=%d bytes-each=%d\n",
             udev->bulk_in_ep, MT_RX_REQUESTS, MT_RX_BUF_SIZE);
  if(udev->ops->bulk_rx_arm && udev->ops->bulk_rx_complete)
    for(int i = 0; i < MT_RX_REQUESTS; i++)
      if(udev->ops->bulk_rx_arm(udev, udev->bulk_in_ep,
                               mt_rx_buf[i], MT_RX_BUF_SIZE) < 0)
        printf("mt7601u: RX request %d submission failed\n", i);
  if(udev->ops->bulk_rx_arm && udev->ops->bulk_rx_complete)
    printf("mt7601u: EP%d receive uses USB host IRQ with %d buffers\n",
           udev->bulk_in_ep, MT_RX_REQUESTS);
  queue_delayed_work(&mt7601u_wq, &mt7601u.state_work,
                     MT_SCAN_DWELL_JIFFIES);
  __sync_synchronize();
  mt7601u.ready = 1;
  printf("mt7601u: transport and MCU ready; SoftMAC initialization pending\n");
  MT_CFG_LOG("probe end initial-channel=%d state-work-delay=%d\n",
             mt7601u.scan_channel, MT_SCAN_DWELL_JIFFIES);
  return 0;
}

static void
mt7601u_remove(struct usb_device *udev)
{
  // Stop IRQ/timer producers before draining private works.
  acquire(&mt7601u.lock);
  mt7601u.used = 0;
  release(&mt7601u.lock);
  __sync_synchronize();
  napi_disable(&mt7601u.napi);
  cancel_delayed_work_sync(&mt7601u.state_work);
  if(mt7601u.net_registered)
    unregister_netdev(&mt7601u.netdev);
  if(udev)
    udev->dev.driver_data = 0;
  memset(&mt7601u, 0, sizeof(mt7601u));
}

static struct usb_driver mt7601u_driver = {
  .driver = {
    .name = "mt7601u",
  },
  .id_table = mt7601u_ids,
  .probe = mt7601u_probe,
  .remove = mt7601u_remove,
};

static void
mt7601u_register(void)
{
  mt7601u_driver_init();
}
module_init_ord("mt7601u", mt7601u_register, 1);

void
mt7601u_driver_init(void)
{
  memset(&mt7601u, 0, sizeof(mt7601u));
  init_workqueue(&mt7601u_wq, "mt7601u_wq", 1);
  usb_register_driver(&mt7601u_driver);
}

void
mt7601u_driver_exit(void)
{
  usb_unregister_driver(&mt7601u_driver);
}

// wifi_connect(ssid, passphrase): derive the PMK here (PBKDF2, 4096 rounds,
// too slow for interrupt context), aim the scan state machine at `ssid`,
// and wait up to `timeout_ms` for the 4-way handshake. 0 when associated
// and keyed, -1 otherwise (no such network, wrong passphrase, no device).
int
mt7601u_connect(const char *ssid, const char *passphrase, int timeout_ms)
{
  struct mt7601u_device *dev = &mt7601u;
  int n = strlen(ssid);
  if(n == 0 || n > 32)
    return -1;
  // The probe sets `used` before eFuse and the MAC/BBP/RF/WCID tables, which
  // take tens of seconds with traces on.  Start the join timeout only once
  // the probe has finished, instead of spending it on initialization.
  {
    uint64 freq = (uint64)r_cntfrq_el0();
    uint64 wait_end = r_cntvct_el0() + freq * 60;
    if(dev->used && !dev->ready)
      printf("mt7601u: adapter still initializing; join waits for it\n");
    while(dev->used && !dev->ready && r_cntvct_el0() < wait_end){
      acquire(&tickslock);
      sleep(&ticks, &tickslock);
      release(&tickslock);
    }
  }
  if(!dev->used || !dev->ready)
    return -1;
  // A retry for the network we are already joining must not restart the
  // join: the state machine keeps running after a timed-out wifi_connect(),
  // and resetting it here threw away a link that was about to complete.
  acquire(&dev->lock);
  int same = dev->want_ssid_len == n && memcmp(dev->want_ssid, ssid, n) == 0;
  if(same && dev->handshake_done){
    release(&dev->lock);
    printf("mt7601u: already associated with %s\n", dev->want_ssid);
    return 0;
  }
  int in_progress = same && dev->target_found;
  release(&dev->lock);
  if(!in_progress){
    uint8 pmk[32];
    wpa_pbkdf2((char*)passphrase, (char*)ssid, pmk);
    acquire(&dev->lock);
    memmove(dev->pmk, pmk, sizeof(pmk));
    memmove(dev->want_ssid, ssid, n);
    dev->want_ssid[n] = 0;
    dev->want_ssid_len = n;
    dev->target_found = 0;
    dev->handshake_done = 0;
    dev->link_state = MT_LINK_SCAN;
    dev->scan_rounds = 0;
    dev->scan_dwell_logs = 0;
    release(&dev->lock);
    mod_delayed_work(&mt7601u_wq, &dev->state_work, MT_STATE_SOON_JIFFIES);
    printf("mt7601u: joining %s\n", dev->want_ssid);
  } else {
    printf("mt7601u: %s: join already in progress, waiting\n", dev->want_ssid);
  }
  uint64 hz = (uint64)r_cntfrq_el0();
  uint64 end = r_cntvct_el0() + hz * timeout_ms / 1000;
  int extended = 0;
  for(;;){
    if(dev->handshake_done)
      return 0;
    uint64 now = r_cntvct_el0();
    if(now >= end){
      // AP found late in the window: give auth/assoc/WPA2 15 s more, once.
      if(dev->target_found && !extended){
        extended = 1;
        end = now + hz * 15;
        printf("mt7601u: %s found late; waiting 15 s more for the handshake\n",
               dev->want_ssid);
        continue;
      }
      break;
    }
    acquire(&tickslock);
    sleep(&ticks, &tickslock);     // let the workqueue and RX run
    release(&tickslock);
  }
  printf("mt7601u: %s: no handshake within %d ms (state %d, found %d, channel %d, "
         "sweeps %d, mgmt frames %d, usb rx %d)\n", dev->want_ssid, timeout_ms,
         dev->link_state, dev->target_found, dev->scan_channel, dev->scan_rounds,
         dev->mgmt_frames, dev->rx_urbs);
  return -1;
}
