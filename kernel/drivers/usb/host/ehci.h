#ifndef XV6_EHCI_H
#define XV6_EHCI_H
// Board hooks for the EHCI host driver.
struct ehci_platdata {
  void (*phy_init)(void);   // power and reset the host PHY, select its clock
};
#endif
