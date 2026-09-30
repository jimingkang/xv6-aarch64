#ifndef XV6_SSH_CRYPTO_H
#define XV6_SSH_CRYPTO_H

int ssh_random(void *, int);
int ssh_sha256(unsigned char out[32], const unsigned char *, unsigned long long);
int ssh_x25519_public(unsigned char out[32], const unsigned char secret[32]);
int ssh_x25519(unsigned char out[32], const unsigned char secret[32],
               const unsigned char peer[32]);
int ssh_ed25519_keypair(unsigned char public_key[32],
                        unsigned char secret_key[64],
                        const unsigned char seed[32]);
int ssh_ed25519_sign(unsigned char signature[64], const unsigned char *message,
                     unsigned long long length,
                     const unsigned char secret_key[64]);
int ssh_hmac_sha256(unsigned char out[32], const unsigned char *key, int keylen,
                    const unsigned char *message, int length);

struct ssh_aes128_ctr {
  unsigned char round_key[176];
  unsigned char counter[16];
  unsigned char stream[16];
  int available;
};

void ssh_aes128_ctr_init(struct ssh_aes128_ctr *, const unsigned char key[16],
                         const unsigned char iv[16]);
void ssh_aes128_ctr_xor(struct ssh_aes128_ctr *, unsigned char *, int);

#endif
