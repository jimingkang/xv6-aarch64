#include "types.h"
#include "aarch64.h"
#include "defs.h"

struct sha1_ctx {
  uint32 h[5];
  uint64 bytes;
  uint8 block[64];
  int used;
};

static uint32
rol(uint32 x, int n)
{
  return (x << n) | (x >> (32 - n));
}

static void
sha1_transform(struct sha1_ctx *ctx, const uint8 *p)
{
  uint32 w[80], a, b, c, d, e, f, k, t;
  for(int i = 0; i < 16; i++)
    w[i] = ((uint32)p[i*4] << 24) | ((uint32)p[i*4+1] << 16) |
           ((uint32)p[i*4+2] << 8) | p[i*4+3];
  for(int i = 16; i < 80; i++)
    w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
  a = ctx->h[0]; b = ctx->h[1]; c = ctx->h[2];
  d = ctx->h[3]; e = ctx->h[4];
  for(int i = 0; i < 80; i++){
    if(i < 20){ f = (b & c) | ((~b) & d); k = 0x5a827999; }
    else if(i < 40){ f = b ^ c ^ d; k = 0x6ed9eba1; }
    else if(i < 60){ f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
    else { f = b ^ c ^ d; k = 0xca62c1d6; }
    t = rol(a, 5) + f + e + k + w[i];
    e = d; d = c; c = rol(b, 30); b = a; a = t;
  }
  ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c;
  ctx->h[3] += d; ctx->h[4] += e;
}

static void
sha1_init(struct sha1_ctx *ctx)
{
  ctx->h[0] = 0x67452301; ctx->h[1] = 0xefcdab89;
  ctx->h[2] = 0x98badcfe; ctx->h[3] = 0x10325476;
  ctx->h[4] = 0xc3d2e1f0;
  ctx->bytes = 0;
  ctx->used = 0;
}

static void
sha1_update(struct sha1_ctx *ctx, const void *data, int length)
{
  const uint8 *p = data;
  ctx->bytes += length;
  while(length){
    int n = 64 - ctx->used;
    if(n > length)
      n = length;
    memmove(ctx->block + ctx->used, p, n);
    ctx->used += n;
    p += n;
    length -= n;
    if(ctx->used == 64){
      sha1_transform(ctx, ctx->block);
      ctx->used = 0;
    }
  }
}

static void
sha1_final(struct sha1_ctx *ctx, uint8 out[20])
{
  uint64 bits = ctx->bytes * 8;
  uint8 pad[72];
  int n;
  memset(pad, 0, sizeof(pad));
  pad[0] = 0x80;
  n = ctx->used < 56 ? 56 - ctx->used : 120 - ctx->used;
  sha1_update(ctx, pad, n);
  for(int i = 0; i < 8; i++)
    pad[i] = bits >> (56 - i * 8);
  sha1_update(ctx, pad, 8);
  for(int i = 0; i < 5; i++){
    out[i*4] = ctx->h[i] >> 24;
    out[i*4+1] = ctx->h[i] >> 16;
    out[i*4+2] = ctx->h[i] >> 8;
    out[i*4+3] = ctx->h[i];
  }
}

static void
hmac_sha1(const uint8 *key, int keylen, const void *a, int alen,
          const void *b, int blen, uint8 out[20])
{
  struct sha1_ctx ctx;
  uint8 inner[20], ipad[64], opad[64];
  memset(ipad, 0x36, sizeof(ipad));
  memset(opad, 0x5c, sizeof(opad));
  for(int i = 0; i < keylen; i++){
    ipad[i] ^= key[i];
    opad[i] ^= key[i];
  }
  sha1_init(&ctx);
  sha1_update(&ctx, ipad, sizeof(ipad));
  sha1_update(&ctx, a, alen);
  if(b && blen)
    sha1_update(&ctx, b, blen);
  sha1_final(&ctx, inner);
  sha1_init(&ctx);
  sha1_update(&ctx, opad, sizeof(opad));
  sha1_update(&ctx, inner, sizeof(inner));
  sha1_final(&ctx, out);
}

void
wpa_pbkdf2(char *passphrase, char *ssid, uint8 *pmk)
{
  uint8 u[20], digest[20], counter[4];
  int plen = strlen(passphrase), slen = strlen(ssid), done = 0;

  for(uint32 block = 1; done < 32; block++){
    counter[0] = block >> 24; counter[1] = block >> 16;
    counter[2] = block >> 8; counter[3] = block;
    hmac_sha1((uint8*)passphrase, plen, ssid, slen,
              counter, sizeof(counter), u);
    memmove(digest, u, sizeof(digest));
    for(int iteration = 1; iteration < 4096; iteration++){
      hmac_sha1((uint8*)passphrase, plen, u, sizeof(u), 0, 0, u);
      for(int i = 0; i < 20; i++)
        digest[i] ^= u[i];
    }
    int n = 32 - done;
    if(n > 20)
      n = 20;
    memmove(pmk + done, digest, n);
    done += n;
  }
}

static int
bytes_less(uint8 *a, uint8 *b, int n)
{
  for(int i = 0; i < n; i++){
    if(a[i] < b[i]) return 1;
    if(a[i] > b[i]) return 0;
  }
  return 0;
}

void
wpa_make_snonce(uint8 *pmk, uint8 *anonce, uint64 seed, uint8 *snonce)
{
  uint8 material[40], digest[20];
  memmove(material, anonce, 32);
  for(int i = 0; i < 8; i++)
    material[32+i] = seed >> (i * 8);
  hmac_sha1(pmk, 32, material, sizeof(material), 0, 0, digest);
  memmove(snonce, digest, 20);
  material[39] ^= 0xa5;
  hmac_sha1(pmk, 32, material, sizeof(material), 0, 0, digest);
  memmove(snonce + 20, digest, 12);
}

void
wpa_derive_ptk(uint8 *pmk, uint8 *aa, uint8 *spa,
               uint8 *anonce, uint8 *snonce, uint8 *ptk)
{
  static char label[] = "Pairwise key expansion";
  uint8 data[76], digest[20], counter;
  uint8 *mac1 = bytes_less(aa, spa, 6) ? aa : spa;
  uint8 *mac2 = mac1 == aa ? spa : aa;
  uint8 *nonce1 = bytes_less(anonce, snonce, 32) ? anonce : snonce;
  uint8 *nonce2 = nonce1 == anonce ? snonce : anonce;
  int done = 0;

  memmove(data, mac1, 6);
  memmove(data + 6, mac2, 6);
  memmove(data + 12, nonce1, 32);
  memmove(data + 44, nonce2, 32);
  for(counter = 0; done < 64; counter++){
    // WPA PRF: HMAC(PMK, label || NUL || context || counter).
    struct sha1_ctx ctx;
    uint8 ipad[64], opad[64], inner[20];
    memset(ipad, 0x36, sizeof(ipad));
    memset(opad, 0x5c, sizeof(opad));
    for(int i = 0; i < 32; i++){
      ipad[i] ^= pmk[i];
      opad[i] ^= pmk[i];
    }
    sha1_init(&ctx); sha1_update(&ctx, ipad, 64);
    sha1_update(&ctx, label, sizeof(label));
    sha1_update(&ctx, data, sizeof(data));
    sha1_update(&ctx, &counter, 1); sha1_final(&ctx, inner);
    sha1_init(&ctx); sha1_update(&ctx, opad, 64);
    sha1_update(&ctx, inner, 20); sha1_final(&ctx, digest);
    int n = 64 - done;
    if(n > 20) n = 20;
    memmove(ptk + done, digest, n);
    done += n;
  }
}

void
wpa_eapol_mic(uint8 *kck, void *eapol, int length, uint8 *mic)
{
  uint8 digest[20];
  hmac_sha1(kck, 16, eapol, length, 0, 0, digest);
  memmove(mic, digest, 16);
}

static uint8
aes_xtime(uint8 x)
{
  return (x << 1) ^ ((x & 0x80) ? 0x1b : 0);
}

static uint8
aes_mul(uint8 a, uint8 b)
{
  uint8 r = 0;
  while(b){
    if(b & 1) r ^= a;
    a = aes_xtime(a);
    b >>= 1;
  }
  return r;
}

static uint8
aes_sbox(uint8 x)
{
  uint8 y = 1, p = x, inv, r;
  if(x == 0)
    inv = 0;
  else {
    // x^254 in GF(2^8).
    for(int e = 254; e; e >>= 1){
      if(e & 1) y = aes_mul(y, p);
      p = aes_mul(p, p);
    }
    inv = y;
  }
  r = inv;
  for(int i = 1; i <= 4; i++)
    r ^= (uint8)((inv << i) | (inv >> (8 - i)));
  return r ^ 0x63;
}

static void
aes128_expand(uint8 *key, uint8 *round)
{
  uint8 t[4], rcon = 1;
  memmove(round, key, 16);
  for(int bytes = 16; bytes < 176;){
    memmove(t, round + bytes - 4, 4);
    if((bytes & 15) == 0){
      uint8 q = t[0];
      t[0] = aes_sbox(t[1]) ^ rcon;
      t[1] = aes_sbox(t[2]);
      t[2] = aes_sbox(t[3]);
      t[3] = aes_sbox(q);
      rcon = aes_xtime(rcon);
    }
    for(int i = 0; i < 4; i++, bytes++)
      round[bytes] = round[bytes - 16] ^ t[i];
  }
}

static void
aes128_decrypt(uint8 *key, uint8 in[16], uint8 out[16])
{
  uint8 round[176], inv[256], s[16], t[16];
  aes128_expand(key, round);
  for(int i = 0; i < 256; i++)
    inv[aes_sbox(i)] = i;
  for(int i = 0; i < 16; i++)
    s[i] = in[i] ^ round[160 + i];
  for(int rnd = 9; rnd >= 0; rnd--){
    // Inverse ShiftRows followed by inverse SubBytes.
    t[0]=s[0]; t[1]=s[13]; t[2]=s[10]; t[3]=s[7];
    t[4]=s[4]; t[5]=s[1];  t[6]=s[14]; t[7]=s[11];
    t[8]=s[8]; t[9]=s[5];  t[10]=s[2]; t[11]=s[15];
    t[12]=s[12]; t[13]=s[9]; t[14]=s[6]; t[15]=s[3];
    for(int i = 0; i < 16; i++)
      s[i] = inv[t[i]] ^ round[rnd * 16 + i];
    if(rnd != 0){
      for(int c = 0; c < 4; c++){
        uint8 *a = s + c * 4;
        t[c*4]   = aes_mul(a[0],14)^aes_mul(a[1],11)^aes_mul(a[2],13)^aes_mul(a[3],9);
        t[c*4+1] = aes_mul(a[0],9)^aes_mul(a[1],14)^aes_mul(a[2],11)^aes_mul(a[3],13);
        t[c*4+2] = aes_mul(a[0],13)^aes_mul(a[1],9)^aes_mul(a[2],14)^aes_mul(a[3],11);
        t[c*4+3] = aes_mul(a[0],11)^aes_mul(a[1],13)^aes_mul(a[2],9)^aes_mul(a[3],14);
      }
      memmove(s, t, 16);
    }
  }
  memmove(out, s, 16);
}

// RFC 3394 AES key unwrap.  Input contains A || R[1]..R[n], while output
// receives only the unwrapped R blocks.  Returns their byte length.
int
wpa_aes_unwrap(uint8 *kek, uint8 *cipher, int cipher_len, uint8 *plain)
{
  uint8 a[8], block[16], dec[16];
  int n;
  if(cipher_len < 24 || (cipher_len & 7))
    return -1;
  n = cipher_len / 8 - 1;
  memmove(a, cipher, 8);
  memmove(plain, cipher + 8, n * 8);
  for(int j = 5; j >= 0; j--){
    for(int i = n; i >= 1; i--){
      uint64 v = (uint64)(n * j + i);
      memmove(block, a, 8);
      for(int k = 0; k < 8; k++)
        block[7-k] ^= v >> (k * 8);
      memmove(block + 8, plain + (i - 1) * 8, 8);
      aes128_decrypt(kek, block, dec);
      memmove(a, dec, 8);
      memmove(plain + (i - 1) * 8, dec + 8, 8);
    }
  }
  for(int i = 0; i < 8; i++)
    if(a[i] != 0xa6)
      return -1;
  return n * 8;
}
