// SPDX-License-Identifier: GPL-2.0+
// Debug logs for the ChromeOS EC
//
// Copyright (C) 2015 Google, Inc.

#include <linux/circ_buf.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <fwk_ec_commands.h>
#include <fwk_ec_proto.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/wait.h>

#define DRV_NAME "fwk-ec-debugfs"

#define LOG_SHIFT		14
#define LOG_SIZE		(1 << LOG_SHIFT)
#define LOG_POLL_SEC		10

#define CIRC_ADD(idx, size, value)	(((idx) + (value)) & ((size) - 1))

/* waitqueue for log readers */
static DECLARE_WAIT_QUEUE_HEAD(fwk_ec_debugfs_log_wq);

/**
 * struct fwk_ec_debugfs - EC debugging information.
 *
 * @ec: EC device this debugfs information belongs to
 * @dir: dentry for debugfs files
 * @log_buffer: circular buffer for console log information
 * @read_msg: preallocated EC command and buffer to read console log
 * @log_mutex: mutex to protect circular buffer
 * @log_poll_work: recurring task to poll EC for new console log data
 * @panicinfo_blob: panicinfo debugfs blob
 * @notifier_panic: notifier_block to let kernel to flush buffered log
 *                  when EC panic
 */
struct fwk_ec_debugfs {
	struct fwk_ec_dev *ec;
	struct dentry *dir;
	/* EC log */
	struct circ_buf log_buffer;
	struct fwk_ec_command *read_msg;
	struct mutex log_mutex;
	struct delayed_work log_poll_work;
	/* EC panicinfo */
	struct debugfs_blob_wrapper panicinfo_blob;
	struct notifier_block notifier_panic;
};

/*
 * We need to make sure that the EC log buffer on the UART is large enough,
 * so that it is unlikely enough to overlow within LOG_POLL_SEC.
 */
static void fwk_ec_console_log_work(struct work_struct *__work)
{
	struct fwk_ec_debugfs *debug_info =
		container_of(to_delayed_work(__work),
			     struct fwk_ec_debugfs,
			     log_poll_work);
	struct fwk_ec_dev *ec = debug_info->ec;
	struct circ_buf *cb = &debug_info->log_buffer;
	struct fwk_ec_command snapshot_msg = {
		.command = EC_CMD_CONSOLE_SNAPSHOT + ec->cmd_offset,
	};

	struct ec_params_console_read_v1 *read_params =
		(struct ec_params_console_read_v1 *)debug_info->read_msg->data;
	uint8_t *ec_buffer = (uint8_t *)debug_info->read_msg->data;
	int idx;
	int buf_space;
	int ret;

	ret = fwk_ec_cmd_xfer_status(ec->ec_dev, &snapshot_msg);
	if (ret < 0)
		goto resched;

	/* Loop until we have read everything, or there's an error. */
	mutex_lock(&debug_info->log_mutex);
	buf_space = CIRC_SPACE(cb->head, cb->tail, LOG_SIZE);

	while (1) {
		if (!buf_space) {
			dev_info_once(ec->dev,
				      "Some logs may have been dropped...\n");
			break;
		}

		memset(read_params, '\0', sizeof(*read_params));
		read_params->subcmd = CONSOLE_READ_RECENT;
		ret = fwk_ec_cmd_xfer_status(ec->ec_dev,
					      debug_info->read_msg);
		if (ret < 0)
			break;

		/* If the buffer is empty, we're done here. */
		if (ret == 0 || ec_buffer[0] == '\0')
			break;

		idx = 0;
		while (idx < ret && ec_buffer[idx] != '\0' && buf_space > 0) {
			cb->buf[cb->head] = ec_buffer[idx];
			cb->head = CIRC_ADD(cb->head, LOG_SIZE, 1);
			idx++;
			buf_space--;
		}

		wake_up(&fwk_ec_debugfs_log_wq);
	}

	mutex_unlock(&debug_info->log_mutex);

resched:
	schedule_delayed_work(&debug_info->log_poll_work,
			      msecs_to_jiffies(LOG_POLL_SEC * 1000));
}

static int fwk_ec_console_log_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;

	return stream_open(inode, file);
}

static ssize_t fwk_ec_console_log_read(struct file *file, char __user *buf,
					size_t count, loff_t *ppos)
{
	struct fwk_ec_debugfs *debug_info = file->private_data;
	struct circ_buf *cb = &debug_info->log_buffer;
	ssize_t ret;

	mutex_lock(&debug_info->log_mutex);

	while (!CIRC_CNT(cb->head, cb->tail, LOG_SIZE)) {
		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto error;
		}

		mutex_unlock(&debug_info->log_mutex);

		ret = wait_event_interruptible(fwk_ec_debugfs_log_wq,
					CIRC_CNT(cb->head, cb->tail, LOG_SIZE));
		if (ret < 0)
			return ret;

		mutex_lock(&debug_info->log_mutex);
	}

	/* Only copy until the end of the circular buffer, and let userspace
	 * retry to get the rest of the data.
	 */
	ret = min_t(size_t, CIRC_CNT_TO_END(cb->head, cb->tail, LOG_SIZE),
		    count);

	if (copy_to_user(buf, cb->buf + cb->tail, ret)) {
		ret = -EFAULT;
		goto error;
	}

	cb->tail = CIRC_ADD(cb->tail, LOG_SIZE, ret);

error:
	mutex_unlock(&debug_info->log_mutex);
	return ret;
}

static __poll_t fwk_ec_console_log_poll(struct file *file,
					     poll_table *wait)
{
	struct fwk_ec_debugfs *debug_info = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &fwk_ec_debugfs_log_wq, wait);

	mutex_lock(&debug_info->log_mutex);
	if (CIRC_CNT(debug_info->log_buffer.head,
		     debug_info->log_buffer.tail,
		     LOG_SIZE))
		mask |= EPOLLIN | EPOLLRDNORM;
	mutex_unlock(&debug_info->log_mutex);

	return mask;
}

static int fwk_ec_console_log_release(struct inode *inode, struct file *file)
{
	return 0;
}

static ssize_t fwk_ec_pdinfo_read(struct file *file,
				   char __user *user_buf,
				   size_t count,
				   loff_t *ppos)
{
	char read_buf[EC_USB_PD_MAX_PORTS * 40], *p = read_buf;
	struct fwk_ec_debugfs *debug_info = file->private_data;
	struct fwk_ec_device *ec_dev = debug_info->ec->ec_dev;
	struct {
		struct fwk_ec_command msg;
		union {
			struct ec_response_usb_pd_control_v1 resp;
			struct ec_params_usb_pd_control params;
		};
	} __packed ec_buf;
	struct fwk_ec_command *msg;
	struct ec_response_usb_pd_control_v1 *resp;
	struct ec_params_usb_pd_control *params;
	int i;

	msg = &ec_buf.msg;
	params = (struct ec_params_usb_pd_control *)msg->data;
	resp = (struct ec_response_usb_pd_control_v1 *)msg->data;

	msg->command = EC_CMD_USB_PD_CONTROL;
	msg->version = 1;
	msg->insize = sizeof(*resp);
	msg->outsize = sizeof(*params);

	/*
	 * Read status from all PD ports until failure, typically caused
	 * by attempting to read status on a port that doesn't exist.
	 */
	for (i = 0; i < EC_USB_PD_MAX_PORTS; ++i) {
		params->port = i;
		params->role = 0;
		params->mux = 0;
		params->swap = 0;

		if (fwk_ec_cmd_xfer_status(ec_dev, msg) < 0)
			break;

		p += scnprintf(p, sizeof(read_buf) + read_buf - p,
			       "p%d: %s en:%.2x role:%.2x pol:%.2x\n", i,
			       resp->state, resp->enabled, resp->role,
			       resp->polarity);
	}

	return simple_read_from_buffer(user_buf, count, ppos,
				       read_buf, p - read_buf);
}

static bool fwk_ec_uptime_is_supported(struct fwk_ec_device *ec_dev)
{
	struct {
		struct fwk_ec_command cmd;
		struct ec_response_uptime_info resp;
	} __packed msg = {};
	int ret;

	msg.cmd.command = EC_CMD_GET_UPTIME_INFO;
	msg.cmd.insize = sizeof(msg.resp);

	ret = fwk_ec_cmd_xfer_status(ec_dev, &msg.cmd);
	if (ret == -EPROTO && msg.cmd.result == EC_RES_INVALID_COMMAND)
		return false;

	/* Other errors maybe a transient error, do not rule about support. */
	return true;
}

static ssize_t fwk_ec_uptime_read(struct file *file, char __user *user_buf,
				   size_t count, loff_t *ppos)
{
	struct fwk_ec_debugfs *debug_info = file->private_data;
	struct fwk_ec_device *ec_dev = debug_info->ec->ec_dev;
	struct {
		struct fwk_ec_command cmd;
		struct ec_response_uptime_info resp;
	} __packed msg = {};
	struct ec_response_uptime_info *resp;
	char read_buf[32];
	int ret;

	resp = (struct ec_response_uptime_info *)&msg.resp;

	msg.cmd.command = EC_CMD_GET_UPTIME_INFO;
	msg.cmd.insize = sizeof(*resp);

	ret = fwk_ec_cmd_xfer_status(ec_dev, &msg.cmd);
	if (ret < 0)
		return ret;

	ret = scnprintf(read_buf, sizeof(read_buf), "%u\n",
			resp->time_since_ec_boot_ms);

	return simple_read_from_buffer(user_buf, count, ppos, read_buf, ret);
}

static const struct file_operations fwk_ec_console_log_fops = {
	.owner = THIS_MODULE,
	.open = fwk_ec_console_log_open,
	.read = fwk_ec_console_log_read,
	.llseek = no_llseek,
	.poll = fwk_ec_console_log_poll,
	.release = fwk_ec_console_log_release,
};

static const struct file_operations fwk_ec_pdinfo_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = fwk_ec_pdinfo_read,
	.llseek = default_llseek,
};

static const struct file_operations fwk_ec_uptime_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = fwk_ec_uptime_read,
	.llseek = default_llseek,
};

static int ec_read_version_supported(struct fwk_ec_dev *ec)
{
	struct ec_params_get_cmd_versions_v1 *params;
	struct ec_response_get_cmd_versions *response;
	int ret;

	struct fwk_ec_command *msg;

	msg = kzalloc(sizeof(*msg) + max(sizeof(*params), sizeof(*response)),
		GFP_KERNEL);
	if (!msg)
		return 0;

	msg->command = EC_CMD_GET_CMD_VERSIONS + ec->cmd_offset;
	msg->outsize = sizeof(*params);
	msg->insize = sizeof(*response);

	params = (struct ec_params_get_cmd_versions_v1 *)msg->data;
	params->cmd = EC_CMD_CONSOLE_READ;
	response = (struct ec_response_get_cmd_versions *)msg->data;

	ret = fwk_ec_cmd_xfer_status(ec->ec_dev, msg) >= 0 &&
	      response->version_mask & EC_VER_MASK(1);

	kfree(msg);

	return ret;
}

static int fwk_ec_create_console_log(struct fwk_ec_debugfs *debug_info)
{
	struct fwk_ec_dev *ec = debug_info->ec;
	char *buf;
	int read_params_size;
	int read_response_size;

	/*
	 * If the console log feature is not supported return silently and
	 * don't create the console_log entry.
	 */
	if (!ec_read_version_supported(ec))
		return 0;

	buf = devm_kzalloc(ec->dev, LOG_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	read_params_size = sizeof(struct ec_params_console_read_v1);
	read_response_size = ec->ec_dev->max_response;
	debug_info->read_msg = devm_kzalloc(ec->dev,
		sizeof(*debug_info->read_msg) +
			max(read_params_size, read_response_size), GFP_KERNEL);
	if (!debug_info->read_msg)
		return -ENOMEM;

	debug_info->read_msg->version = 1;
	debug_info->read_msg->command = EC_CMD_CONSOLE_READ + ec->cmd_offset;
	debug_info->read_msg->outsize = read_params_size;
	debug_info->read_msg->insize = read_response_size;

	debug_info->log_buffer.buf = buf;
	debug_info->log_buffer.head = 0;
	debug_info->log_buffer.tail = 0;

	mutex_init(&debug_info->log_mutex);

	debugfs_create_file("console_log", S_IFREG | 0444, debug_info->dir,
			    debug_info, &fwk_ec_console_log_fops);

	INIT_DELAYED_WORK(&debug_info->log_poll_work,
			  fwk_ec_console_log_work);
	schedule_delayed_work(&debug_info->log_poll_work, 0);

	return 0;
}

static void fwk_ec_cleanup_console_log(struct fwk_ec_debugfs *debug_info)
{
	if (debug_info->log_buffer.buf) {
		cancel_delayed_work_sync(&debug_info->log_poll_work);
		mutex_destroy(&debug_info->log_mutex);
	}
}

/*
 * Returns the size of the panicinfo data fetched from the EC
 */
static int fwk_ec_get_panicinfo(struct fwk_ec_device *ec_dev, uint8_t *data,
				 int data_size)
{
	int ret;
	struct fwk_ec_command *msg;

	if (!data || data_size <= 0 || data_size > ec_dev->max_response)
		return -EINVAL;

	msg = kzalloc(sizeof(*msg) + data_size, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	msg->command = EC_CMD_GET_PANIC_INFO;
	msg->insize = data_size;

	ret = fwk_ec_cmd_xfer_status(ec_dev, msg);
	if (ret < 0)
		goto free;

	memcpy(data, msg->data, data_size);

free:
	kfree(msg);
	return ret;
}

static int fwk_ec_create_panicinfo(struct fwk_ec_debugfs *debug_info)
{
	struct fwk_ec_device *ec_dev = debug_info->ec->ec_dev;
	int ret;
	void *data;

	data = devm_kzalloc(debug_info->ec->dev, ec_dev->max_response,
			    GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	ret = fwk_ec_get_panicinfo(ec_dev, data, ec_dev->max_response);
	if (ret < 0) {
		ret = 0;
		goto free;
	}

	/* No panic data */
	if (ret == 0)
		goto free;

	debug_info->panicinfo_blob.data = data;
	debug_info->panicinfo_blob.size = ret;

	debugfs_create_blob("panicinfo", S_IFREG | 0444, debug_info->dir,
			    &debug_info->panicinfo_blob);

	return 0;

free:
	devm_kfree(debug_info->ec->dev, data);
	return ret;
}

static int fwk_ec_debugfs_panic_event(struct notifier_block *nb,
				       unsigned long queued_during_suspend, void *_notify)
{
	struct fwk_ec_debugfs *debug_info =
		container_of(nb, struct fwk_ec_debugfs, notifier_panic);

	if (debug_info->log_buffer.buf) {
		/* Force log poll work to run immediately */
		mod_delayed_work(debug_info->log_poll_work.wq, &debug_info->log_poll_work, 0);
		/* Block until log poll work finishes */
		flush_delayed_work(&debug_info->log_poll_work);
	}

	return NOTIFY_DONE;
}

static int fwk_ec_debugfs_probe(struct platform_device *pd)
{
	struct fwk_ec_dev *ec = dev_get_drvdata(pd->dev.parent);
	struct fwk_ec_platform *ec_platform = dev_get_platdata(ec->dev);
	const char *name = ec_platform->ec_name;
	struct fwk_ec_debugfs *debug_info;
	int ret;

	debug_info = devm_kzalloc(ec->dev, sizeof(*debug_info), GFP_KERNEL);
	if (!debug_info)
		return -ENOMEM;

	debug_info->ec = ec;
	debug_info->dir = debugfs_create_dir(name, NULL);

	ret = fwk_ec_create_panicinfo(debug_info);
	if (ret)
		goto remove_debugfs;

	ret = fwk_ec_create_console_log(debug_info);
	if (ret)
		goto remove_debugfs;

	debugfs_create_file("pdinfo", 0444, debug_info->dir, debug_info,
			    &fwk_ec_pdinfo_fops);

	if (fwk_ec_uptime_is_supported(ec->ec_dev))
		debugfs_create_file("uptime", 0444, debug_info->dir, debug_info,
				    &fwk_ec_uptime_fops);

	debugfs_create_x32("last_resume_result", 0444, debug_info->dir,
			   &ec->ec_dev->last_resume_result);

	debugfs_create_u16("suspend_timeout_ms", 0664, debug_info->dir,
			   &ec->ec_dev->suspend_timeout_ms);

	debug_info->notifier_panic.notifier_call = fwk_ec_debugfs_panic_event;
	ret = blocking_notifier_chain_register(&ec->ec_dev->panic_notifier,
					       &debug_info->notifier_panic);
	if (ret)
		goto remove_debugfs;

	ec->debug_info = debug_info;

	dev_set_drvdata(&pd->dev, ec);

	return 0;

remove_debugfs:
	debugfs_remove_recursive(debug_info->dir);
	return ret;
}

static void fwk_ec_debugfs_remove(struct platform_device *pd)
{
	struct fwk_ec_dev *ec = dev_get_drvdata(pd->dev.parent);

	debugfs_remove_recursive(ec->debug_info->dir);
	fwk_ec_cleanup_console_log(ec->debug_info);
}

static int __maybe_unused fwk_ec_debugfs_suspend(struct device *dev)
{
	struct fwk_ec_dev *ec = dev_get_drvdata(dev);

	if (ec->debug_info->log_buffer.buf)
		cancel_delayed_work_sync(&ec->debug_info->log_poll_work);

	return 0;
}

static int __maybe_unused fwk_ec_debugfs_resume(struct device *dev)
{
	struct fwk_ec_dev *ec = dev_get_drvdata(dev);

	if (ec->debug_info->log_buffer.buf)
		schedule_delayed_work(&ec->debug_info->log_poll_work, 0);

	return 0;
}

static SIMPLE_DEV_PM_OPS(fwk_ec_debugfs_pm_ops,
			 fwk_ec_debugfs_suspend, fwk_ec_debugfs_resume);

static struct platform_driver fwk_ec_debugfs_driver = {
	.driver = {
		.name = DRV_NAME,
		.pm = &fwk_ec_debugfs_pm_ops,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = fwk_ec_debugfs_probe,
	.remove_new = fwk_ec_debugfs_remove,
};

module_platform_driver(fwk_ec_debugfs_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Debug logs for ChromeOS EC");
MODULE_ALIAS("platform:" DRV_NAME);
