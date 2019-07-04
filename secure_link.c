// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019, Silicon Laboratories, Inc.
 */

#include <linux/random.h>
#include <crypto/sha.h>
#include <mbedtls/md.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ccm.h>
#include <mbedtls/sha256.h>
#include <mbedtls/sha512.h>

#include "wfx.h"
#include "wsm_rx.h"
#include "secure_link.h"

static int mbedtls_random(void *data, unsigned char *output, size_t len)
{
	get_random_bytes(output, len);

	return 0;
}

static int memzcmp(void *src, unsigned int size)
{
	if (!size)
		return 0;
	if (*(unsigned char *) src)
		return 1;
	return memcmp(src, src + 1, size - 1);
}

static void memreverse(uint8_t *src, uint8_t length)
{
	uint8_t *lo = src;
	uint8_t *hi = src + length - 1;
	uint8_t swap;

	while (lo < hi) {
		swap = *lo;
		*lo++ = *hi;
		*hi-- = swap;
	}
}

int wfx_is_secure_command(struct wfx_dev *wdev, int cmd_id)
{
	return test_bit(cmd_id, wdev->sl.commands);
}

int wfx_sl_decode(struct wfx_dev *wdev, struct sl_wmsg *m)
{
	int ret;
	size_t clear_len = le16_to_cpu(m->len);
	size_t payload_len = round_up(clear_len - sizeof(m->len), 16);
	uint8_t *tag = m->payload + payload_len;
	uint8_t *output = (uint8_t *) m;
	uint32_t nonce[3] = { };

	WARN(m->encrypted != 0x02, "packet is not encrypted");

	// Other bytes of nonce are 0
	nonce[1] = m->seqnum;
	if (wdev->sl.rx_seqnum != m->seqnum)
		dev_warn(wdev->dev, "wrong encrypted message sequence: %d != %d\n",
				m->seqnum, wdev->sl.rx_seqnum);
	wdev->sl.rx_seqnum = m->seqnum + 1;
	if (wdev->sl.rx_seqnum == SECURE_LINK_NONCE_COUNTER_MAX)
		  schedule_work(&wdev->sl.key_renew_work);

	memcpy(output, &m->len, sizeof(m->len));
	ret = mbedtls_ccm_auth_decrypt(&wdev->sl.ccm_ctxt, payload_len,
			(uint8_t *) nonce, sizeof(nonce), NULL, 0,
			m->payload, output + sizeof(m->len),
			tag, sizeof(struct sl_tag));
	if (ret) {
		dev_err(wdev->dev, "mbedtls error: %08x\n", ret);
		return -EIO;
	}
	if (memzcmp(output + clear_len, payload_len + sizeof(m->len) - clear_len))
		dev_warn(wdev->dev, "padding is not 0\n");
	return 0;
}

int wfx_sl_encode(struct wfx_dev *wdev, struct wmsg *input, struct sl_wmsg *output)
{
	int payload_len = round_up(input->len - sizeof(input->len), 16);
	uint8_t *tag = output->payload + payload_len;
	uint32_t nonce[3] = { };
	int ret;

	output->encrypted = 0x1;
	output->len = input->len;
	output->seqnum = wdev->sl.tx_seqnum;
	// Other bytes of nonce are 0
	nonce[2] = wdev->sl.tx_seqnum;
	wdev->sl.tx_seqnum++;
	if (wdev->sl.tx_seqnum == SECURE_LINK_NONCE_COUNTER_MAX)
		  schedule_work(&wdev->sl.key_renew_work);

	ret = mbedtls_ccm_encrypt_and_tag(&wdev->sl.ccm_ctxt, payload_len,
			(uint8_t *) nonce, sizeof(nonce), NULL, 0,
			(uint8_t *) input + sizeof(input->len), output->payload,
			tag, sizeof(struct sl_tag));
	if (ret) {
		dev_err(wdev->dev, "mbedtls error: %08x\n", ret);
		return -EIO;
	}
	return 0;
}

static int wfx_sl_get_pubkey_mac(struct wfx_dev *wdev, uint8_t *pubkey, uint8_t *mac)
{
	return mbedtls_md_hmac(
			mbedtls_md_info_from_type(MBEDTLS_MD_SHA512),
			wdev->pdata.sec_link_key, sizeof(wdev->pdata.sec_link_key),
			pubkey, API_HOST_PUB_KEY_SIZE,
			mac);
}

int wfx_sl_check_pubkey(struct wfx_dev *wdev, uint8_t *pubkey, uint8_t *mac)
{
	int ret;
	size_t olen;
	uint8_t secret[API_HOST_PUB_KEY_SIZE];
	uint8_t secret_digest[SHA256_DIGEST_SIZE];
	uint8_t expected_mac[SHA512_DIGEST_SIZE];

	ret = wfx_sl_get_pubkey_mac(wdev, pubkey, expected_mac);
	if (ret)
		goto end;
	ret = memcmp(expected_mac, mac, sizeof(expected_mac));
	if (ret)
		goto end;

	// FIXME: save Y or (reset it), concat it with ncp_public_key and use mbedtls_ecdh_read_public.
	memreverse(pubkey, API_NCP_PUB_KEY_SIZE);
	ret = mbedtls_mpi_read_binary(&wdev->sl.edch_ctxt.Qp.X, pubkey, API_NCP_PUB_KEY_SIZE);
	if (ret)
		goto end;
	ret = mbedtls_mpi_lset(&wdev->sl.edch_ctxt.Qp.Z, 1);
	if (ret)
		goto end;

	ret = mbedtls_ecdh_calc_secret(&wdev->sl.edch_ctxt, &olen,
			secret, sizeof(secret),
			mbedtls_random, NULL);
	if (ret)
		goto end;

	memreverse(secret, sizeof(secret));
	ret = mbedtls_sha256_ret(secret, sizeof(secret), secret_digest, 0);
	if (ret)
		goto end;

	// Use the lower 16 bytes of the sha256 of the secret for AES key
	ret = mbedtls_ccm_setkey(&wdev->sl.ccm_ctxt, MBEDTLS_CIPHER_ID_AES,
			secret_digest, 16 * BITS_PER_BYTE);

end:
	complete(&wdev->sl.key_renew_done);
	return 0;
}

static int wfx_sl_key_exchange(struct wfx_dev *wdev)
{
	int ret;
	size_t olen;
	uint8_t mac[SHA512_DIGEST_SIZE];
	uint8_t pubkey[API_HOST_PUB_KEY_SIZE + 2];

	wdev->sl.rx_seqnum = 0;
	wdev->sl.tx_seqnum = 0;
	mbedtls_ccm_free(&wdev->sl.ccm_ctxt);

	mbedtls_ecdh_init(&wdev->sl.edch_ctxt);
	ret = mbedtls_ecdh_setup(&wdev->sl.edch_ctxt, MBEDTLS_ECP_DP_CURVE25519);
	if (ret)
		goto err;
	wdev->sl.edch_ctxt.point_format = MBEDTLS_ECP_PF_COMPRESSED;
	ret = mbedtls_ecdh_make_public(&wdev->sl.edch_ctxt, &olen,
				       pubkey, sizeof(pubkey),
				       mbedtls_random, NULL);
	if (ret || olen != sizeof(pubkey))
		goto err;
	memreverse(pubkey + 2, sizeof(pubkey) - 2);
	ret = wfx_sl_get_pubkey_mac(wdev, pubkey + 2, mac);
	if (ret)
		goto err;
	ret = wsm_send_pub_keys(wdev, pubkey + 2, mac);
	if (ret)
		goto err;
	if (!wait_for_completion_timeout(&wdev->sl.key_renew_done, msecs_to_jiffies(500)))
		goto err;
	if (!memzcmp(&wdev->sl.ccm_ctxt, sizeof(wdev->sl.ccm_ctxt)))
		goto err;

	mbedtls_ecdh_free(&wdev->sl.edch_ctxt);
	return 0;
err:
	mbedtls_ecdh_free(&wdev->sl.edch_ctxt);
	dev_err(wdev->dev, "key negociation error\n");
	return -EIO;
}

static void wfx_sl_renew_key(struct work_struct *work)
{
	struct wfx_dev *wdev = container_of(work, struct wfx_dev, sl.key_renew_work);

	wsm_tx_lock_flush(wdev);
	mutex_lock(&wdev->wsm_cmd.key_renew_lock);
	wfx_sl_key_exchange(wdev);
	mutex_unlock(&wdev->wsm_cmd.key_renew_lock);
	wsm_tx_unlock(wdev);
}

static void wfx_sl_init_cfg(struct wfx_dev *wdev)
{
	DECLARE_BITMAP(sl_commands, 256);

	bitmap_fill(sl_commands, 256);
	clear_bit(HI_SET_SL_MAC_KEY_REQ_ID, sl_commands);
	clear_bit(HI_SL_EXCHANGE_PUB_KEYS_REQ_ID, sl_commands);
	clear_bit(HI_SL_EXCHANGE_PUB_KEYS_IND_ID, sl_commands);
	clear_bit(HI_EXCEPTION_IND_ID, sl_commands);
	clear_bit(HI_ERROR_IND_ID, sl_commands);
	wsm_sl_config(wdev, sl_commands);
	bitmap_copy(wdev->sl.commands, sl_commands, 256);
}

int wfx_sl_init(struct wfx_dev *wdev)
{
	int link_mode = wdev->wsm_caps.Capabilities.LinkMode;

	INIT_WORK(&wdev->sl.key_renew_work, wfx_sl_renew_key);
	init_completion(&wdev->sl.key_renew_done);
	if (!memzcmp(wdev->pdata.sec_link_key, sizeof(wdev->pdata.sec_link_key)))
		return -EIO;
	if (link_mode == SECURE_LINK_TRUSTED_ACTIVE_ENFORCED) {
		bitmap_set(wdev->sl.commands, HI_SL_CONFIGURE_REQ_ID, 1);
		if (wfx_sl_key_exchange(wdev))
			return -EIO;
		wfx_sl_init_cfg(wdev);
	} else if (link_mode == SECURE_LINK_TRUSTED_MODE) {
		if (wsm_set_mac_key(wdev, wdev->pdata.sec_link_key, SL_MAC_KEY_DEST_RAM))
			return -EIO;
		if (wfx_sl_key_exchange(wdev))
			return -EIO;
		wfx_sl_init_cfg(wdev);
	} else {
		dev_info(wdev->dev, "ignoring provided secure link key since chip does not support it\n");
	}
	return 0;
}

void wfx_sl_deinit(struct wfx_dev *wdev)
{
	mbedtls_ccm_free(&wdev->sl.ccm_ctxt);
}

