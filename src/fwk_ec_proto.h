/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ChromeOS Embedded Controller protocol interface.
 *
 * Copyright (C) 2012 Google, Inc
 */

#ifndef __LINUX_FWK_EC_PROTO_H
#define __LINUX_FWK_EC_PROTO_H

#include <linux/device.h>
#include <linux/lockdep_types.h>
#include <linux/mutex.h>
#include <linux/notifier.h>

#include <fwk_ec_commands.h>

#include <linux/acpi.h>

#define FWK_EC_DEV_NAME	"cros_ec"
#define FWK_EC_DEV_FP_NAME	"fwk_fp"
#define FWK_EC_DEV_ISH_NAME	"fwk_ish"
#define FWK_EC_DEV_PD_NAME	"fwk_pd"
#define FWK_EC_DEV_SCP_NAME	"fwk_scp"
#define FWK_EC_DEV_TP_NAME	"fwk_tp"

#define FWK_EC_DEV_EC_INDEX 0
#define FWK_EC_DEV_PD_INDEX 1

/*
 * The EC is unresponsive for a time after a reboot command.  Add a
 * simple delay to make sure that the bus stays locked.
 */
#define EC_REBOOT_DELAY_MS		50

/*
 * Max bus-specific overhead incurred by request/responses.
 * I2C requires 1 additional byte for requests.
 * I2C requires 2 additional bytes for responses.
 * SPI requires up to 32 additional bytes for responses.
 */
#define EC_PROTO_VERSION_UNKNOWN	0
#define EC_MAX_REQUEST_OVERHEAD		1
#define EC_MAX_RESPONSE_OVERHEAD	32

/*
 * EC panic is not covered by the standard (0-F) ACPI notify values.
 * Arbitrarily choosing B0 to notify ec panic, which is in the 84-BF
 * device specific ACPI notify range.
 */
#define ACPI_NOTIFY_FWK_EC_PANIC 0xB0

/*
 * Command interface between EC and AP, for LPC, I2C and SPI interfaces.
 */
enum {
	EC_MSG_TX_HEADER_BYTES	= 3,
	EC_MSG_TX_TRAILER_BYTES	= 1,
	EC_MSG_TX_PROTO_BYTES	= EC_MSG_TX_HEADER_BYTES +
				  EC_MSG_TX_TRAILER_BYTES,
	EC_MSG_RX_PROTO_BYTES	= 3,

	/* Max length of messages for proto 2*/
	EC_PROTO2_MSG_BYTES	= EC_PROTO2_MAX_PARAM_SIZE +
				  EC_MSG_TX_PROTO_BYTES,

	EC_MAX_MSG_BYTES	= 64 * 1024,
};

/**
 * struct fwk_ec_command - Information about a ChromeOS EC command.
 * @version: Command version number (often 0).
 * @command: Command to send (EC_CMD_...).
 * @outsize: Outgoing length in bytes.
 * @insize: Max number of bytes to accept from the EC.
 * @result: EC's response to the command (separate from communication failure).
 * @data: Where to put the incoming data from EC and outgoing data to EC.
 */
struct fwk_ec_command {
	uint32_t version;
	uint32_t command;
	uint32_t outsize;
	uint32_t insize;
	uint32_t result;
	uint8_t data[];
};

/**
 * struct fwk_ec_device - Information about a ChromeOS EC device.
 * @phys_name: Name of physical comms layer (e.g. 'i2c-4').
 * @dev: Device pointer for physical comms device
 * @fwk_class: The class structure for this device.
 * @cmd_readmem: Direct read of the EC memory-mapped region, if supported.
 *     @offset: Is within EC_LPC_ADDR_MEMMAP region.
 *     @bytes: Number of bytes to read. zero means "read a string" (including
 *             the trailing '\0'). At most only EC_MEMMAP_SIZE bytes can be
 *             read. Caller must ensure that the buffer is large enough for the
 *             result when reading a string.
 * @max_request: Max size of message requested.
 * @max_response: Max size of message response.
 * @max_passthru: Max sice of passthru message.
 * @proto_version: The protocol version used for this device.
 * @priv: Private data.
 * @irq: Interrupt to use.
 * @id: Device id.
 * @din: Input buffer (for data from EC). This buffer will always be
 *       dword-aligned and include enough space for up to 7 word-alignment
 *       bytes also, so we can ensure that the body of the message is always
 *       dword-aligned (64-bit). We use this alignment to keep ARM and x86
 *       happy. Probably word alignment would be OK, there might be a small
 *       performance advantage to using dword.
 * @dout: Output buffer (for data to EC). This buffer will always be
 *        dword-aligned and include enough space for up to 7 word-alignment
 *        bytes also, so we can ensure that the body of the message is always
 *        dword-aligned (64-bit). We use this alignment to keep ARM and x86
 *        happy. Probably word alignment would be OK, there might be a small
 *        performance advantage to using dword.
 * @din_size: Size of din buffer to allocate (zero to use static din).
 * @dout_size: Size of dout buffer to allocate (zero to use static dout).
 * @wake_enabled: True if this device can wake the system from sleep.
 * @suspended: True if this device had been suspended.
 * @cmd_xfer: Send command to EC and get response.
 *            Returns the number of bytes received if the communication
 *            succeeded, but that doesn't mean the EC was happy with the
 *            command. The caller should check msg.result for the EC's result
 *            code.
 * @pkt_xfer: Send packet to EC and get response.
 * @lockdep_key: Lockdep class for each instance. Unused if CONFIG_LOCKDEP is
 *		 not enabled.
 * @lock: One transaction at a time.
 * @mkbp_event_supported: 0 if MKBP not supported. Otherwise its value is
 *                        the maximum supported version of the MKBP host event
 *                        command + 1.
 * @host_sleep_v1: True if this EC supports the sleep v1 command.
 * @event_notifier: Interrupt event notifier for transport devices.
 * @event_data: Raw payload transferred with the MKBP event.
 * @event_size: Size in bytes of the event data.
 * @host_event_wake_mask: Mask of host events that cause wake from suspend.
 * @suspend_timeout_ms: The timeout in milliseconds between when sleep event
 *                      is received and when the EC will declare sleep
 *                      transition failure if the sleep signal is not
 *                      asserted.  See also struct
 *                      ec_params_host_sleep_event_v1 in fwk_ec_commands.h.
 * @last_resume_result: The number of sleep power signal transitions that
 *                      occurred since the suspend message. The high bit
 *                      indicates a timeout occurred.  See also struct
 *                      ec_response_host_sleep_event_v1 in fwk_ec_commands.h.
 * @last_event_time: exact time from the hard irq when we got notified of
 *     a new event.
 * @notifier_ready: The notifier_block to let the kernel re-query EC
 *		    communication protocol when the EC sends
 *		    EC_HOST_EVENT_INTERFACE_READY.
 * @ec: The platform_device used by the mfd driver to interface with the
 *      main EC.
 * @pd: The platform_device used by the mfd driver to interface with the
 *      PD behind an EC.
 * @panic_notifier: EC panic notifier.
 */
struct fwk_ec_device {
	/* These are used by other drivers that want to talk to the EC */
	const char *phys_name;
	struct device *dev;
	struct class *fwk_class;
	int (*cmd_readmem)(struct fwk_ec_device *ec, unsigned int offset,
			   unsigned int bytes, void *dest);

	/* These are used to implement the platform-specific interface */
	u16 max_request;
	u16 max_response;
	u16 max_passthru;
	u16 proto_version;
	void *priv;
	int irq;
	u8 *din;
	u8 *dout;
	int din_size;
	int dout_size;
	bool wake_enabled;
	bool suspended;
	int (*cmd_xfer)(struct fwk_ec_device *ec,
			struct fwk_ec_command *msg);
	int (*pkt_xfer)(struct fwk_ec_device *ec,
			struct fwk_ec_command *msg);
	int (*ec_mutex_lock)(struct fwk_ec_device *ec);
	int (*ec_mutex_unlock)(struct fwk_ec_device *ec);
	struct lock_class_key lockdep_key;
	struct mutex lock;
	acpi_handle aml_mutex;
	u8 mkbp_event_supported;
	bool host_sleep_v1;
	struct blocking_notifier_head event_notifier;

	struct ec_response_get_next_event_v1 event_data;
	int event_size;
	u32 host_event_wake_mask;
	u32 last_resume_result;
	u16 suspend_timeout_ms;
	ktime_t last_event_time;
	struct notifier_block notifier_ready;

	/* The platform devices used by the mfd driver */
	struct platform_device *ec;
	struct platform_device *pd;

	struct blocking_notifier_head panic_notifier;
};

/**
 * struct fwk_ec_platform - ChromeOS EC platform information.
 * @ec_name: Name of EC device (e.g. 'fwk-ec', 'fwk-pd', ...)
 *           used in /dev/ and sysfs.
 * @cmd_offset: Offset to apply for each command. Set when
 *              registering a device behind another one.
 */
struct fwk_ec_platform {
	const char *ec_name;
	u16 cmd_offset;
};

/**
 * struct fwk_ec_dev - ChromeOS EC device entry point.
 * @class_dev: Device structure used in sysfs.
 * @ec_dev: fwk_ec_device structure to talk to the physical device.
 * @dev: Pointer to the platform device.
 * @debug_info: fwk_ec_debugfs structure for debugging information.
 * @has_kb_wake_angle: True if at least 2 accelerometer are connected to the EC.
 * @cmd_offset: Offset to apply for each command.
 * @features: Features supported by the EC.
 */
struct fwk_ec_dev {
	struct device class_dev;
	struct fwk_ec_device *ec_dev;
	struct device *dev;
	struct fwk_ec_debugfs *debug_info;
	bool has_kb_wake_angle;
	u16 cmd_offset;
	struct ec_response_get_features features;
};

#define to_fwk_ec_dev(dev)  container_of(dev, struct fwk_ec_dev, class_dev)

int fwk_ec_prepare_tx(struct fwk_ec_device *ec_dev,
		       struct fwk_ec_command *msg);

int fwk_ec_check_result(struct fwk_ec_device *ec_dev,
			 struct fwk_ec_command *msg);

int fwk_ec_cmd_xfer(struct fwk_ec_device *ec_dev,
		     struct fwk_ec_command *msg);

int fwk_ec_cmd_xfer_status(struct fwk_ec_device *ec_dev,
			    struct fwk_ec_command *msg);

int fwk_ec_query_all(struct fwk_ec_device *ec_dev);

int fwk_ec_get_next_event(struct fwk_ec_device *ec_dev,
			   bool *wake_event,
			   bool *has_more_events);

u32 fwk_ec_get_host_event(struct fwk_ec_device *ec_dev);

bool fwk_ec_check_features(struct fwk_ec_dev *ec, int feature);

int fwk_ec_get_sensor_count(struct fwk_ec_dev *ec);

int fwk_ec_cmd(struct fwk_ec_device *ec_dev, unsigned int version, int command, const void *outdata,
		    size_t outsize, void *indata, size_t insize);

/**
 * fwk_ec_get_time_ns() - Return time in ns.
 *
 * This is the function used to record the time for last_event_time in struct
 * fwk_ec_device during the hard irq.
 *
 * Return: ktime_t format since boot.
 */
static inline ktime_t fwk_ec_get_time_ns(void)
{
	return ktime_get_boottime_ns();
}

#endif /* __LINUX_FWK_EC_PROTO_H */
