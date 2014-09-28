/*
 * AES SIV (RFC5297)
 *
 * Copyright (c) 2013 Cozybit, Inc.
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#include "includes.h"

#include "common.h"
#include "aes.h"
#include "aes_wrap.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a)/sizeof(a[0]))
#endif

static const u8 zero[AES_BLOCK_SIZE];

static void dbl(u8 *pad)
{
	int i, carry;

	carry = pad[0] & 0x80;
	for (i = 0; i < AES_BLOCK_SIZE - 1; i++)
		pad[i] = (pad[i] << 1) | (pad[i + 1] >> 7);
	pad[AES_BLOCK_SIZE - 1] <<= 1;
	if (carry)
		pad[AES_BLOCK_SIZE - 1] ^= 0x87;
}

static void xor(u8 *a, u8 *b)
{
	int i;
	for (i = 0; i < AES_BLOCK_SIZE; i++)
		*a++ ^= *b++;
}

static void xorend(u8 *a, int alen, u8 *b, int blen)
{
	int i;

	if (alen < blen)
		return;

	for (i = 0; i < blen; i++)
		a[alen - blen + i] ^= b[i];
}

static void pad(u8 *pad, const u8 *addr, size_t len)
{
	os_memset(pad, 0, AES_BLOCK_SIZE);
	memcpy(pad, addr, len);

	if (len < AES_BLOCK_SIZE)
		pad[len] = 0x80;
}

int aes_s2v(const u8 *key, size_t num_elem, const u8 *addr[],
	    size_t *len, u8 *mac)
{
	u8 tmp[AES_BLOCK_SIZE], tmp2[AES_BLOCK_SIZE];
	u8 *buf = NULL;
	int ret;
	int i;

	if (!num_elem) {
		memcpy(tmp, zero, sizeof(zero));
		tmp[AES_BLOCK_SIZE - 1] = 1;
		return omac1_aes_128(key, tmp, sizeof(tmp), mac);
	}

	ret = omac1_aes_128(key, zero, sizeof(zero), tmp);
	if (ret)
		return ret;

	for (i = 0; i < num_elem - 1; i++) {

		ret = omac1_aes_128(key, addr[i], len[i], tmp2);
		if (ret)
			return ret;

		dbl(tmp);
		xor(tmp, tmp2);
	}
	if (len[i] >= AES_BLOCK_SIZE) {
		buf = os_malloc(len[i]);
		if (!buf)
			return -ENOMEM;

		memcpy(buf, addr[i], len[i]);
		xorend(buf, len[i], tmp, AES_BLOCK_SIZE);
		ret = omac1_aes_128(key, buf, len[i], mac);
		os_free(buf);
		return ret;
	}

	dbl(tmp);
	pad(tmp2, addr[i], len[i]);
	xor(tmp, tmp2);

	return omac1_aes_128(key, tmp, sizeof(tmp), mac);
}

int aes_siv_encrypt(const u8 *key, const u8 *pw,
		    size_t pwlen, size_t num_elem,
		    const u8 *addr[], const size_t *len, u8 *out)
{
	const u8 *_addr[6];
	size_t _len[6];
	const u8 *k1 = key, *k2 = key + 16;
	u8 v[AES_BLOCK_SIZE];
	int i;
	u8 *iv, *crypt_pw;

	if (num_elem > ARRAY_SIZE(_addr) - 1)
		return -1;

	for (i = 0; i < num_elem; i++) {
		_addr[i] = addr[i];
		_len[i] = len[i];
	}
	_addr[num_elem] = pw;
	_len[num_elem] = pwlen;

	aes_s2v(k1, num_elem + 1, _addr, _len, v);

	iv = out;
	crypt_pw = out + AES_BLOCK_SIZE;

	memcpy(iv, v, AES_BLOCK_SIZE);
	memcpy(crypt_pw, pw, pwlen);

	/* zero out 63rd and 31st bits of ctr (from right) */
	v[8] &= 0x7f;
	v[12] &= 0x7f;
	return aes_128_ctr_encrypt(k2, v, crypt_pw, pwlen);
}

int aes_siv_decrypt(const u8 *key, const u8 *iv_crypt, size_t iv_c_len,
		    int num_elem, const u8 *addr[], const size_t *len,
		    u8 *out)
{
	const u8 *_addr[6];
	size_t _len[6];
	const u8 *k1 = key, *k2 = key + 16;
	size_t crypt_len = iv_c_len - 16;
	int i, ret;

	u8 iv[16];
	u8 check[16];

	if (num_elem > ARRAY_SIZE(_addr) - 1)
		return -1;

	for (i = 0; i < num_elem; i++) {
		_addr[i] = addr[i];
		_len[i] = len[i];
	}
	_addr[num_elem] = out;
	_len[num_elem] = crypt_len;

	memcpy(iv, iv_crypt, 16);
	memcpy(out, iv_crypt + 16, crypt_len);

	iv[8] &= 0x7f;
	iv[12] &= 0x7f;

	ret = aes_128_ctr_encrypt(k2, iv, out, crypt_len);
	if (ret)
		return ret;

	aes_s2v(k1, num_elem + 1, _addr, _len, check);
	if (os_memcmp(check, iv_crypt, 16) == 0)
		return 0;

	return -1;
}
