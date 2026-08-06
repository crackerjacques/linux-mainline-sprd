// SPDX-License-Identifier: GPL-2.0-only
/*
 * Raw smsg "DFS" control channel to the PM sys (SP/CM4) firmware.
 *
 * The vendor DDR-DVFS driver talks to the pm_sys DFS task over bare smsg
 * on channel 22 (SMSG_CH_PM_CTRL): requests are SMSG_TYPE_DFS with the
 * command in the flag field, replies come back as SMSG_TYPE_DFS_RSP.
 * There is no shared-memory ring behind this channel - the 8-byte mailbox
 * message is the whole protocol.
 *
 * This implementation exposes the channel through debugfs for probing:
 *   echo "<cmd> [value]" > /sys/kernel/debug/sprd-sipc-dfs/send   (hex)
 *   cat /sys/kernel/debug/sprd-sipc-dfs/status
 * Responses are also logged to the kernel log.
 */

#include <linux/debugfs.h>
#include <linux/slab.h>

#include "sprd_sipc.h"

struct sipc_dfs {
	struct sipc_channel *channel;
	struct dentry *debugfs;
	spinlock_t lock;
	bool have_rsp;
	u8 rsp_type;
	u16 rsp_cmd;
	u32 rsp_value;
	struct completion rsp_done;
};

static void sipc_dfs_rx(struct sipc_channel *channel, u8 type, u16 cmd, u32 value)
{
	struct sipc_dfs *dfs = channel->priv;
	unsigned long flags;

	dev_info(channel->parent, "dfs rx: type=0x%02x flag=0x%04x value=0x%08x\n",
		 type, cmd, value);

	spin_lock_irqsave(&dfs->lock, flags);
	dfs->have_rsp = true;
	dfs->rsp_type = type;
	dfs->rsp_cmd = cmd;
	dfs->rsp_value = value;
	spin_unlock_irqrestore(&dfs->lock, flags);
	complete(&dfs->rsp_done);
}

static int sipc_dfs_open(struct sipc_channel *channel)
{
	dev_info(channel->parent, "dfs channel %d: remote opened\n", channel->id);
	return 0;
}

static void sipc_dfs_close(struct sipc_channel *channel)
{
	dev_info(channel->parent, "dfs channel %d: closed\n", channel->id);
}

static void sipc_dfs_free(struct sipc_channel *channel)
{
	struct sipc_dfs *dfs = channel->priv;

	debugfs_remove_recursive(dfs->debugfs);
	kfree(dfs);
}

static ssize_t sipc_dfs_send_write(struct file *file, const char __user *buf,
				   size_t count, loff_t *ppos)
{
	struct sipc_dfs *dfs = file->private_data;
	char kbuf[32];
	unsigned int cmd, value = 0;
	int n, ret;

	if (count >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;
	kbuf[count] = '\0';

	n = sscanf(kbuf, "%x %x", &cmd, &value);
	if (n < 1 || cmd > 0xffff)
		return -EINVAL;

	reinit_completion(&dfs->rsp_done);
	ret = sipc_send(dfs->channel, SMSG_TYPE_DFS, cmd, value);
	if (ret)
		return ret;

	if (!wait_for_completion_timeout(&dfs->rsp_done, msecs_to_jiffies(2000)))
		dev_warn(dfs->channel->parent, "dfs cmd 0x%04x: no response\n", cmd);

	return count;
}

static const struct file_operations sipc_dfs_send_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = sipc_dfs_send_write,
};

static int sipc_dfs_status_show(struct seq_file *s, void *unused)
{
	struct sipc_dfs *dfs = s->private;
	struct sipc_channel *channel = dfs->channel;
	unsigned long flags;

	seq_printf(s, "remote_open: %d\n",
		   !!test_bit(SIPC_REMOTE_OPEN, &channel->state));
	spin_lock_irqsave(&dfs->lock, flags);
	if (dfs->have_rsp)
		seq_printf(s, "last_rsp: type=0x%02x flag=0x%04x value=0x%08x\n",
			   dfs->rsp_type, dfs->rsp_cmd, dfs->rsp_value);
	else
		seq_puts(s, "last_rsp: none\n");
	spin_unlock_irqrestore(&dfs->lock, flags);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sipc_dfs_status);

static struct sipc_channel_ops sipc_dfs_ops = {
	.rx = sipc_dfs_rx,
	.open = sipc_dfs_open,
	.close = sipc_dfs_close,
	.free = sipc_dfs_free,
};

int sipc_dfs_init(struct sipc_channel *channel)
{
	struct sipc_dfs *dfs;

	dfs = kzalloc_obj(*dfs, GFP_KERNEL);
	if (!dfs)
		return -ENOMEM;

	dfs->channel = channel;
	spin_lock_init(&dfs->lock);
	init_completion(&dfs->rsp_done);

	dfs->debugfs = debugfs_create_dir("sprd-sipc-dfs", NULL);
	debugfs_create_file("send", 0200, dfs->debugfs, dfs, &sipc_dfs_send_fops);
	debugfs_create_file("status", 0444, dfs->debugfs, dfs, &sipc_dfs_status_fops);

	channel->priv = dfs;
	channel->ops = &sipc_dfs_ops;

	return 0;
}
