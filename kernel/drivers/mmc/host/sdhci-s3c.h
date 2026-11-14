#ifndef XV6_SDHCI_S3C_H
#define XV6_SDHCI_S3C_H
// Board hook for the S5PV210 HSMMC driver: pins, clock gate and SCLK_MMC.
struct sdhci_s3c_platdata {
  void (*setup)(void);
};
int sd_ready(void);
#endif
