// USB CDC-ECM network-device layer.
//
// DWC2 controller programming, DMA and USB enumeration are implemented in
// dwc2.c.  This file only adapts that transport to xv6's struct net_device.

#include "types.h"
#include "param.h"
#include "aarch64.h"
#include "spinlock.h"
#include "defs.h"
#include "device.h"
#include "net.h"
#include "usb.h"

#define USB_GET_DESCRIPTOR     6
#define USB_SET_CONFIG         9
#define USB_SET_INTERFACE      11
#define USB_DT_CONFIG          2
#define CDC_SET_PACKET_FILTER  0x43
#define USBNET_CTRL_BUFSIZE    512

static struct net_device usb_netdev;
static int usbnet_ready;
static uchar usbnet_ctrl_buf[USBNET_CTRL_BUFSIZE] __attribute__((aligned(64)));

static int
usbnet_xmit(struct net_device *netdev, void *packet, int len)
{
  (void)netdev;
  if(!usbnet_ready)
    return -1;
  return dwc2_cdc_xmit(packet, len);
}

static void
usbnet_poll_device(struct net_device *netdev)
{
  (void)netdev;
  if(usbnet_ready)
    dwc2_cdc_poll();
}

static const struct net_device_ops usbnet_ops = {
  .start_xmit = usbnet_xmit,
  .poll = usbnet_poll_device,
};

int
usbnet_attach(struct usb_device *udev)
{
  int cfg = -1, idx, total, off;

  if(usbnet_ready)
    return 0;
  if(udev == 0 || udev->ops == 0 || udev->ops->control == 0)
    return -1;

  // Locate a CDC Ethernet control interface instead of depending on the
  // ordering of the device's configurations.
  for(idx = 0; idx < 2 && cfg < 0; idx++){
    memset(usbnet_ctrl_buf, 0, sizeof(usbnet_ctrl_buf));
    if(udev->ops->control(udev, 0x80, USB_GET_DESCRIPTOR,
                          (USB_DT_CONFIG << 8) | idx, 0,
                          usbnet_ctrl_buf, 9) < 0)
      continue;
    total = usbnet_ctrl_buf[2] | ((uint16)usbnet_ctrl_buf[3] << 8);
    if(total < 9 || total > (int)sizeof(usbnet_ctrl_buf))
      continue;
    if(udev->ops->control(udev, 0x80, USB_GET_DESCRIPTOR,
                          (USB_DT_CONFIG << 8) | idx, 0,
                          usbnet_ctrl_buf, total) < 0)
      continue;
    for(off = 0; off + 2 <= total && usbnet_ctrl_buf[off] >= 2;
        off += usbnet_ctrl_buf[off]){
      if(usbnet_ctrl_buf[off + 1] == 4 && usbnet_ctrl_buf[off] >= 9 &&
         usbnet_ctrl_buf[off + 5] == 2 && usbnet_ctrl_buf[off + 6] == 6){
        cfg = usbnet_ctrl_buf[5];
        break;
      }
    }
  }
  if(cfg < 0){
    printf("usbnet: no CDC-ECM configuration\n");
    return -1;
  }
  if(udev->ops->control(udev, 0x00, USB_SET_CONFIG, cfg, 0,
                        usbnet_ctrl_buf, 0) < 0){
    printf("usbnet: CDC SET_CONFIG failed\n");
    return -1;
  }
  // QEMU usb-net exposes its data interface as interface 1, alternate 1.
  if(udev->ops->control(udev, 0x01, USB_SET_INTERFACE, 1, 1,
                        usbnet_ctrl_buf, 0) < 0){
    printf("usbnet: CDC SET_INTERFACE failed\n");
    return -1;
  }
  if(udev->ops->control(udev, 0x21, CDC_SET_PACKET_FILTER, 0x000f, 0,
                        usbnet_ctrl_buf, 0) < 0){
    printf("usbnet: CDC SET_PACKET_FILTER failed\n");
    return -1;
  }

  memset(&usb_netdev, 0, sizeof(usb_netdev));
  usb_netdev.dev.name = "usb0";
  usb_netdev.dev.parent = udev->dev.parent;
  usb_netdev.ops = &usbnet_ops;
  usb_netdev.mtu = NET_MTU;
  // QEMU's CDC-ECM test device uses this locally administered address.
  usb_netdev.mac[0] = 0x52; usb_netdev.mac[1] = 0x54;
  usb_netdev.mac[2] = 0x00; usb_netdev.mac[3] = 0x12;
  usb_netdev.mac[4] = 0x34; usb_netdev.mac[5] = 0x56;
  if(register_netdev(&usb_netdev) < 0)
    return -1;
  usbnet_ready = 1;
  printf("usbnet: CDC-ECM ready addr=%d cfg=%d vid=%x pid=%x\n",
         udev->address, cfg, udev->vendor, udev->product);
  return 0;
}

void
usbnet_detach(void)
{
  if(!usbnet_ready)
    return;
  usbnet_ready = 0;
  unregister_netdev(&usb_netdev);
}

void
usbnet_rx(void *packet, int len)
{
  if(usbnet_ready && len > 0)
    net_rx_dev(&usb_netdev, packet, len);
}
