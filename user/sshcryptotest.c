#include "kernel/types.h"
#include "user/user.h"
#include "user/ssh_crypto.h"

static int
hexval(char c)
{
  if(c >= '0' && c <= '9') return c - '0';
  if(c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static void
decode(uchar *out, char *hex, int n)
{
  for(int i = 0; i < n; i++)
    out[i] = (hexval(hex[2*i]) << 4) | hexval(hex[2*i+1]);
}

static int
same(uchar *value, char *expected, int n)
{
  uchar decoded[64];
  decode(decoded, expected, n);
  return memcmp(value, decoded, n) == 0;
}

int
main(void)
{
  uchar scalar[32], peer[32], value[64], seed[32], public_key[32];
  uchar secret_key[64], signature[64];
  struct ssh_aes128_ctr aes;

  ssh_sha256(value, (uchar *)"abc", 3);
  if(!same(value, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", 32))
    goto fail;

  decode(scalar, "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", 32);
  if(ssh_x25519_public(value, scalar) < 0 ||
     !same(value, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", 32))
    goto fail;
  decode(peer, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", 32);
  if(ssh_x25519(value, scalar, peer) < 0 ||
     !same(value, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", 32))
    goto fail;

  decode(seed, "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", 32);
  if(ssh_ed25519_keypair(public_key, secret_key, seed) < 0 ||
     !same(public_key, "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", 32) ||
     ssh_ed25519_sign(signature, seed, 0, secret_key) < 0 ||
     !same(signature, "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
                      "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b", 64))
    goto fail;

  uchar aes_key[16], aes_iv[16], aes_data[16];
  decode(aes_key, "2b7e151628aed2a6abf7158809cf4f3c", 16);
  decode(aes_iv, "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", 16);
  decode(aes_data, "6bc1bee22e409f96e93d7e117393172a", 16);
  ssh_aes128_ctr_init(&aes, aes_key, aes_iv);
  ssh_aes128_ctr_xor(&aes, aes_data, 16);
  if(!same(aes_data, "874d6191b620e3261bef6864990db6ce", 16))
    goto fail;

  uchar hmac_key[20];
  memset(hmac_key, 0x0b, sizeof(hmac_key));
  if(ssh_hmac_sha256(value, hmac_key, sizeof(hmac_key),
                     (uchar *)"Hi There", 8) < 0 ||
     !same(value, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", 32))
    goto fail;

  printf("sshcryptotest: SHA-256, HMAC, AES-CTR, X25519, Ed25519 ok\n");
  exit(0);
fail:
  printf("sshcryptotest: FAILED\n");
  exit(1);
}
