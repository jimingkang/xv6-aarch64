#ifndef XV6_DWC2_H
#define XV6_DWC2_H
// Board hooks for the DWC2 (s3c-hsotg) host driver.
struct dwc2_platdata {
  void (*phy_init)(void);   // power and reset the OTG PHY, select its clock
};
void dwc2_irq(void);
#endif
