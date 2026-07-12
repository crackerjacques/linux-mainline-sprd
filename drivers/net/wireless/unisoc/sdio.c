// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDIO transport for the external Unisoc SC2355 (Marlin3-Lite) WiFi/BT
 * combo chip, on top of the BSP sprdwcn bus (sdiohal).
 *
 * The channel layout, 4-byte public header and power sequencing are based
 * on the Unisoc BSP WLAN module (modules/wcn/wlan/wlan_combo/sc2355).
 * Copyright (C) 2021-2022 Unisoc (Shanghai) Technologies Co. Ltd
 */

#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/skbuff.h>
#include <linux/slab.h>

#include <misc/marlin_platform.h>
#include <misc/wcn_bus.h>

#include "sc23xx.h"

/*
 * sdiohal channel ("port") numbers. These must match the sdiohal
 * channel<->hwtype mapping in the BSP bus layer (subtype == port).
 *
 *  SDIO TX (host -> CP2, WiFi type):  cmd port 8, data port 10
 *  SDIO RX (CP2 -> host, WiFi type):  cmd port 22, log port 23, data port 24
 */
#define SC23XX_SDIO_TX_CMD_PORT		8
#define SC23XX_SDIO_TX_DATA_PORT	10
#define SC23XX_SDIO_RX_CMD_PORT		22
#define SC23XX_SDIO_RX_PKT_LOG_PORT	23
#define SC23XX_SDIO_RX_DATA_PORT	24

/*
 * sdiohal reserves SDIOHAL_PUB_HEAD_RSV bytes at the front of every buffer
 * for its public header (struct bus_puh_t). It fills the header itself on
 * TX from the channel number, and it is present in front of the payload on
 * RX, so the driver only ever touches the payload at buf + this offset.
 */
#define SC23XX_SDIO_PUH_LEN		SDIOHAL_PUB_HEAD_RSV

#define SC23XX_SDIO_MAX_CMD_LEN		1600
#define SC23XX_SDIO_MAX_DATA_LEN	1676

/*
 * Upper bound on data frames queued to the TX thread. ndo_start_xmit runs in
 * softirq and cannot do the (sleeping) direct SDIO write itself, so frames are
 * handed to a kthread; cap the backlog so a stalled thread can't consume
 * unbounded memory.
 */
#define SC23XX_SDIO_DATA_TXQ_MAX	512

struct sc23xx_sdio {
	struct sc23xx_dev sdev; /* must be first */
	struct platform_device *pdev;

	/*
	 * Data TX is deferred to a kthread: the SC2355 data port must be
	 * written with the "direct" sdiohal path (it wakes the CP before the
	 * transfer), which sleeps and so cannot run from the softirq xmit path.
	 */
	struct sk_buff_head data_txq;
	wait_queue_head_t data_tx_wait;
	struct task_struct *data_tx_thread;
};

/*
 * The mchn callbacks are plain C functions without a context pointer, so
 * like the vendor driver we keep a single global back-pointer. There is
 * only ever one WCN combo chip.
 */
static struct sc23xx_sdio *sc23xx_sdio_priv;

static int sc23xx_sdio_tx(int chn, const void *data, u16 len, bool direct)
{
	struct mbuf_t *head = NULL, *tail = NULL;
	unsigned char *buf;
	int num = 1;
	int ret;

	buf = kmalloc(SC23XX_SDIO_PUH_LEN + len, GFP_ATOMIC);
	if (!buf)
		return -ENOMEM;

	/*
	 * sdiohal fills the public header (type/subtype/len/eof/pad) from the
	 * channel, but it does NOT touch the check_sum bit, so zero the header
	 * region first. A stray check_sum=1 makes the chip treat the last two
	 * payload bytes as a transport checksum and silently drop the frame.
	 */
	memset(buf, 0, SC23XX_SDIO_PUH_LEN);
	memcpy(buf + SC23XX_SDIO_PUH_LEN, data, len);

	ret = sprdwcn_bus_list_alloc(chn, &head, &tail, &num);
	if (ret || !head || !tail) {
		kfree(buf);
		return -ENOMEM;
	}

	head->buf = buf;
	head->len = len;
	head->next = NULL;

	ret = direct ? sprdwcn_bus_push_list_direct(chn, head, tail, num) :
		       sprdwcn_bus_push_list(chn, head, tail, num);
	if (ret) {
		head->buf = NULL;
		kfree(buf);
		sprdwcn_bus_list_free(chn, head, tail, num);
	}

	return ret;
}

static int sc23xx_sdio_tx_cmd(struct sc23xx_dev *sdev, struct sk_buff *skb)
{
	return sc23xx_sdio_tx(SC23XX_SDIO_TX_CMD_PORT, skb->data, skb->len,
			      false);
}

/*
 * Stamp and transmit one data frame. Runs in the TX-thread (process) context:
 * the data port must be written with the "direct" sdiohal path, which wakes
 * the CP before the transfer and sleeps, so it cannot run from softirq.
 */
static void sc23xx_sdio_send_data_frame(struct sc23xx_dev *sdev,
					struct sk_buff *skb)
{
	struct sc23xx_tx_data_hdr *hdr = (void *)skb->data;

	/*
	 * SC2355 gates data TX on per-color credits granted by the firmware
	 * (EVT_SDIO_FLOWCON). Every frame must carry a color that has credit
	 * plus a running sequence number in the data header's seq_info field;
	 * sending an unstamped (color 0 / seq 0, no credit) frame asserts the
	 * CP. Reserve a credit here and stamp the frame. If no credit is
	 * available, drop the frame rather than crash the firmware.
	 */
	if (sdev->credit_capa == SC23XX_TX_WITH_CREDIT) {
		int color = sc23xx_tx_reserve_credit(sdev);

		if (color < 0) {
			wiphy_warn_ratelimited(sdev->wiphy,
					       "no TX credit, dropping data frame\n");
			dev_kfree_skb_any(skb);
			return;
		}

		put_unaligned_le16(sc23xx_tx_seq_info(sdev, color),
				   &hdr->seq_info);
	}

	if (net_ratelimit())
		wiphy_info(sdev->wiphy,
			   "tx data (direct): type=%u lut=%u seq_info=0x%04x len=%u cred[%d %d %d %d]\n",
			   hdr->common.type, hdr->sta_lut_index,
			   get_unaligned_le16(&hdr->seq_info), skb->len,
			   atomic_read(&sdev->tx_credit[0]),
			   atomic_read(&sdev->tx_credit[1]),
			   atomic_read(&sdev->tx_credit[2]),
			   atomic_read(&sdev->tx_credit[3]));

	sc23xx_sdio_tx(SC23XX_SDIO_TX_DATA_PORT, skb->data, skb->len, true);
	dev_kfree_skb_any(skb);
}

static int sc23xx_sdio_data_tx_thread(void *data)
{
	struct sc23xx_sdio *priv = data;
	struct sc23xx_dev *sdev = &priv->sdev;
	struct sk_buff *skb;

	while (!kthread_should_stop()) {
		/*
		 * Declare the host idle while we have nothing to send, so a
		 * firmware power-down request (EVT_FW_PWR_DOWN) is acked and the
		 * CP is allowed to suspend its WiFi data subsystem.
		 */
		mutex_lock(&sdev->pwr_state_lock);
		sdev->pwr_state = SC23XX_PWR_HOST_IDLE;
		mutex_unlock(&sdev->pwr_state_lock);

		wait_event_interruptible(priv->data_tx_wait,
					 !skb_queue_empty(&priv->data_txq) ||
					 kthread_should_stop());
		if (kthread_should_stop())
			break;

		/*
		 * Wake the firmware's data subsystem before writing to the data
		 * port. If we acked a power-down (pwr_state SUSPENDED), the CP has
		 * torn down its port-10 receive path, so a data CMD53 hangs and
		 * times out (-110). HOST_WAKEUP_FW brings it back and blocks until
		 * the CP acks. Mirrors the SIPC transport's wake in sc23xx_sipc.
		 */
		mutex_lock(&sdev->pwr_state_lock);
		if (sdev->pwr_state == SC23XX_PWR_SUSPENDED)
			sc23xx_wakeup_fw(sdev);
		sdev->pwr_state = SC23XX_PWR_ACTIVE;
		mutex_unlock(&sdev->pwr_state_lock);

		while ((skb = skb_dequeue(&priv->data_txq)))
			sc23xx_sdio_send_data_frame(sdev, skb);
	}

	/* Drain anything still queued at teardown. */
	while ((skb = skb_dequeue(&priv->data_txq)))
		dev_kfree_skb_any(skb);

	return 0;
}

/* Called from ndo_start_xmit (softirqs disabled): hand the frame to the TX
 * thread, which does the sleeping direct-write. */
static void sc23xx_sdio_tx_data(struct sc23xx_dev *sdev, struct sk_buff *skb)
{
	struct sc23xx_sdio *priv = container_of(sdev, struct sc23xx_sdio, sdev);

	if (skb_queue_len(&priv->data_txq) >= SC23XX_SDIO_DATA_TXQ_MAX) {
		wiphy_warn_ratelimited(sdev->wiphy,
				       "data TX backlog full, dropping frame\n");
		dev_kfree_skb_any(skb);
		return;
	}

	skb_queue_tail(&priv->data_txq, skb);
	wake_up(&priv->data_tx_wait);
}

static const struct sc23xx_bus_ops sc23xx_sdio_bus_ops = {
	.tx_cmd = sc23xx_sdio_tx_cmd,
	.tx_data = sc23xx_sdio_tx_data,
};

/* TX completion: sdiohal is done with the buffers, free them. */
static int sc23xx_sdio_tx_pop(int chn, struct mbuf_t *head,
			      struct mbuf_t *tail, int num)
{
	struct mbuf_t *pos = head;
	int i;

	for (i = 0; i < num && pos; i++, pos = pos->next) {
		kfree(pos->buf);
		pos->buf = NULL;
	}

	sprdwcn_bus_list_free(chn, head, tail, num);
	return 0;
}

/*
 * RX: sdiohal hands us a list of buffers, each holding one packet prefixed
 * by the public header. We copy the payload into an skb, hand it to the
 * core, then return the buffers to sdiohal.
 */
static int sc23xx_sdio_rx(int chn, struct mbuf_t *head,
			  struct mbuf_t *tail, int num)
{
	struct sc23xx_sdio *priv = sc23xx_sdio_priv;
	enum sc23xx_msg_type type;
	struct mbuf_t *pos = head;
	struct bus_puh_t *puh;
	struct sk_buff *skb;
	u16 len;
	int i;

	if (!priv)
		goto out;

	type = (chn == SC23XX_SDIO_RX_CMD_PORT) ? SC23XX_MSG_TYPE_CMD :
						  SC23XX_MSG_TYPE_DATA;

	for (i = 0; i < num && pos; i++, pos = pos->next) {
		/* the packet-log channel carries firmware logs, not frames */
		if (chn == SC23XX_SDIO_RX_PKT_LOG_PORT || !pos->buf)
			continue;

		puh = (struct bus_puh_t *)pos->buf;
		len = puh->len;
		/* sdiohal folds a trailing 2-byte checksum into puh->len */
		if (puh->check_sum && len >= 2)
			len -= 2;
		if (!len)
			continue;

		skb = alloc_skb(len, GFP_ATOMIC);
		if (!skb)
			continue;

		skb_put_data(skb, pos->buf + SC23XX_SDIO_PUH_LEN, len);
		sc23xx_rx_msg(&priv->sdev, type, skb);
	}

out:
	/* give the buffers back to sdiohal */
	sprdwcn_bus_push_list(chn, head, tail, num);
	return 0;
}

static struct mchn_ops_t sc23xx_sdio_chn_ops[] = {
	{
		.hif_type = HW_TYPE_SDIO,
		.channel = SC23XX_SDIO_RX_CMD_PORT,
		.inout = 0,
		.pool_size = 1,
		.buf_size = SC23XX_SDIO_MAX_CMD_LEN,
		.pop_link = sc23xx_sdio_rx,
	},
	{
		.hif_type = HW_TYPE_SDIO,
		.channel = SC23XX_SDIO_RX_PKT_LOG_PORT,
		.inout = 0,
		.pool_size = 1,
		.buf_size = SC23XX_SDIO_MAX_DATA_LEN,
		.pop_link = sc23xx_sdio_rx,
	},
	{
		.hif_type = HW_TYPE_SDIO,
		.channel = SC23XX_SDIO_RX_DATA_PORT,
		.inout = 0,
		.pool_size = 1,
		.buf_size = SC23XX_SDIO_MAX_DATA_LEN,
		.pop_link = sc23xx_sdio_rx,
	},
	{
		.hif_type = HW_TYPE_SDIO,
		.channel = SC23XX_SDIO_TX_CMD_PORT,
		.inout = 1,
		.pool_size = 10,
		.buf_size = SC23XX_SDIO_MAX_CMD_LEN,
		.pop_link = sc23xx_sdio_tx_pop,
	},
	{
		.hif_type = HW_TYPE_SDIO,
		.channel = SC23XX_SDIO_TX_DATA_PORT,
		.inout = 1,
		.pool_size = 600,
		.buf_size = SC23XX_SDIO_MAX_DATA_LEN,
		.pop_link = sc23xx_sdio_tx_pop,
	},
};

static int sc23xx_sdio_chn_init(void)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(sc23xx_sdio_chn_ops); i++) {
		ret = sprdwcn_bus_chn_init(&sc23xx_sdio_chn_ops[i]);
		if (ret)
			goto err;
	}

	return 0;
err:
	while (--i >= 0)
		sprdwcn_bus_chn_deinit(&sc23xx_sdio_chn_ops[i]);
	return ret;
}

static void sc23xx_sdio_chn_deinit(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(sc23xx_sdio_chn_ops); i++)
		sprdwcn_bus_chn_deinit(&sc23xx_sdio_chn_ops[i]);
}

static int sc23xx_sdio_probe(struct platform_device *pdev)
{
	struct sc23xx_sdio *priv;
	int ret;

	priv = sc23xx_alloc_device(&pdev->dev, sizeof(*priv),
				   &sc23xx_sdio_bus_ops);
	if (IS_ERR(priv))
		return PTR_ERR(priv);

	priv->pdev = pdev;
	priv->sdev.use_dma = false;
	platform_set_drvdata(pdev, priv);
	sc23xx_sdio_priv = priv;

	skb_queue_head_init(&priv->data_txq);
	init_waitqueue_head(&priv->data_tx_wait);
	priv->data_tx_thread = kthread_run(sc23xx_sdio_data_tx_thread, priv,
					   "sc23xx-tx");
	if (IS_ERR(priv->data_tx_thread)) {
		ret = PTR_ERR(priv->data_tx_thread);
		priv->data_tx_thread = NULL;
		dev_err(&pdev->dev, "failed to start TX thread: %d\n", ret);
		goto err_free;
	}

	ret = sc23xx_sdio_chn_init();
	if (ret) {
		dev_err(&pdev->dev, "failed to init SDIO channels: %d\n", ret);
		goto err_free;
	}

	/*
	 * Power the chip on and download the firmware. Unlike the one-shot
	 * WCN_AUTO cali cycle, this wlan client holds the chip powered for
	 * its whole lifetime (marlin power_cnt stays >= 1).
	 */
	ret = start_marlin(MARLIN_WIFI);
	if (ret) {
		dev_err(&pdev->dev, "failed to power on WCN: %d\n", ret);
		ret = -EPROBE_DEFER;
		goto err_chn_deinit;
	}

	/*
	 * Version-handshake first: the SC2355 firmware negotiates its API
	 * version map before it will service any other command (mirrors the
	 * vendor sprd_hif_power_on: sync_version -> download_hw_param).
	 */
	ret = sc23xx_sync_version(&priv->sdev);
	if (ret) {
		dev_err(&pdev->dev, "version handshake failed: %d\n", ret);
		goto err_stop;
	}

	/* Download the board config/NVM (INI), CRC-appended per SC2355 fw. */
	ret = sc23xx_load_hw_param(&priv->sdev, "sprd/wifi_board_config.ini");
	if (ret) {
		dev_err(&pdev->dev, "failed to download config: %d\n", ret);
		goto err_stop;
	}

	ret = sc23xx_register_device(&priv->sdev);
	if (ret)
		goto err_stop;

	return 0;

err_stop:
	stop_marlin(MARLIN_WIFI);
err_chn_deinit:
	sc23xx_sdio_chn_deinit();
err_free:
	if (priv->data_tx_thread)
		kthread_stop(priv->data_tx_thread);
	sc23xx_sdio_priv = NULL;
	sc23xx_free_device(&priv->sdev);
	return ret;
}

static void sc23xx_sdio_remove(struct platform_device *pdev)
{
	struct sc23xx_sdio *priv = platform_get_drvdata(pdev);

	sc23xx_unregister_device(&priv->sdev);
	stop_marlin(MARLIN_WIFI);
	sc23xx_sdio_chn_deinit();
	kthread_stop(priv->data_tx_thread);
	sc23xx_sdio_priv = NULL;
	sc23xx_free_device(&priv->sdev);
}

static const struct of_device_id sc23xx_sdio_of_match[] = {
	{ .compatible = "sprd,sc2355-sdio-wifi" },
	{ }
};
MODULE_DEVICE_TABLE(of, sc23xx_sdio_of_match);

static struct platform_driver sc23xx_sdio_driver = {
	.probe = sc23xx_sdio_probe,
	.remove = sc23xx_sdio_remove,
	.driver = {
		.name = "sc23xx-wlan-sdio",
		.of_match_table = sc23xx_sdio_of_match,
	},
};

module_platform_driver(sc23xx_sdio_driver);

MODULE_DESCRIPTION("Unisoc SC2355 SDIO wireless transport");
MODULE_LICENSE("GPL");
