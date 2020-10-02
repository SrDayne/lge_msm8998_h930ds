/* Copyright (c) 2016-2017, Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/ipc_logging.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/power_supply.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/extcon.h>
#include <linux/usb/class-dual-role.h>
#include <linux/usb/usbpd.h>
#include "usbpd.h"
#if defined(CONFIG_LGE_USB_DEBUGGER) || defined(CONFIG_LGE_USB_MOISTURE_DETECTION)
#include <linux/of_gpio.h>
#include <linux/gpio/consumer.h>
#endif
#if defined(CONFIG_LGE_USB_DEBUGGER) || defined(CONFIG_LGE_USB_FACTORY)
#include <soc/qcom/lge/power/lge_power_class.h>
#include <soc/qcom/lge/power/lge_cable_detect.h>
#endif
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
#include <linux/qpnp/qpnp-adc.h>
#include <soc/qcom/lge/power/lge_board_revision.h>
#include <linux/time.h>
#endif
#if defined(CONFIG_LGE_USB_FACTORY) || defined(CONFIG_LGE_USB_MOISTURE_DETECTION)
#include <soc/qcom/lge/board_lge.h>
#endif

/* To start USB stack for USB3.1 complaince testing */
static bool usb_compliance_mode;
module_param(usb_compliance_mode, bool, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(usb_compliance_mode, "Start USB stack for USB3.1 compliance testing");

static bool disable_usb_pd;
module_param(disable_usb_pd, bool, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(disable_usb_pd, "Disable USB PD for USB3.1 compliance testing");

enum usbpd_state {
	PE_UNKNOWN,
	PE_ERROR_RECOVERY,
#ifdef CONFIG_LGE_USB
	PE_FORCED_PR_SWAP,
#endif
	PE_SRC_DISABLED,
	PE_SRC_STARTUP,
	PE_SRC_SEND_CAPABILITIES,
	PE_SRC_SEND_CAPABILITIES_WAIT, /* substate to wait for Request */
	PE_SRC_NEGOTIATE_CAPABILITY,
	PE_SRC_TRANSITION_SUPPLY,
	PE_SRC_READY,
	PE_SRC_HARD_RESET,
	PE_SRC_SOFT_RESET,
	PE_SRC_SEND_SOFT_RESET,
	PE_SRC_DISCOVERY,
#ifdef CONFIG_LGE_USB_COMPLIANCE_TEST
	PE_SRC_DISCOVERY_WAIT, /* substate to wait for Response */
#endif
	PE_SRC_TRANSITION_TO_DEFAULT,
	PE_SNK_STARTUP,
	PE_SNK_DISCOVERY,
	PE_SNK_WAIT_FOR_CAPABILITIES,
	PE_SNK_EVALUATE_CAPABILITY,
	PE_SNK_SELECT_CAPABILITY,
	PE_SNK_TRANSITION_SINK,
	PE_SNK_READY,
	PE_SNK_HARD_RESET,
	PE_SNK_SOFT_RESET,
	PE_SNK_SEND_SOFT_RESET,
	PE_SNK_TRANSITION_TO_DEFAULT,
	PE_DRS_SEND_DR_SWAP,
	PE_PRS_SNK_SRC_SEND_SWAP,
	PE_PRS_SNK_SRC_TRANSITION_TO_OFF,
	PE_PRS_SNK_SRC_SOURCE_ON,
	PE_PRS_SRC_SNK_SEND_SWAP,
	PE_PRS_SRC_SNK_TRANSITION_TO_OFF,
	PE_PRS_SRC_SNK_WAIT_SOURCE_ON,
	PE_VCS_WAIT_FOR_VCONN,
};

static const char * const usbpd_state_strings[] = {
	"UNKNOWN",
	"ERROR_RECOVERY",
#ifdef CONFIG_LGE_USB
	"FORCED_PR_SWAP",
#endif
	"SRC_Disabled",
	"SRC_Startup",
	"SRC_Send_Capabilities",
	"SRC_Send_Capabilities (Wait for Request)",
	"SRC_Negotiate_Capability",
	"SRC_Transition_Supply",
	"SRC_Ready",
	"SRC_Hard_Reset",
	"SRC_Soft_Reset",
	"SRC_Send_Soft_Reset",
	"SRC_Discovery",
#ifdef CONFIG_LGE_USB_COMPLIANCE_TEST
	"SRC_Discovery (Wait for Response)",
#endif
	"SRC_Transition_to_default",
	"SNK_Startup",
	"SNK_Discovery",
	"SNK_Wait_for_Capabilities",
	"SNK_Evaluate_Capability",
	"SNK_Select_Capability",
	"SNK_Transition_Sink",
	"SNK_Ready",
	"SNK_Hard_Reset",
	"SNK_Soft_Reset",
	"SNK_Send_Soft_Reset",
	"SNK_Transition_to_default",
	"DRS_Send_DR_Swap",
	"PRS_SNK_SRC_Send_Swap",
	"PRS_SNK_SRC_Transition_to_off",
	"PRS_SNK_SRC_Source_on",
	"PRS_SRC_SNK_Send_Swap",
	"PRS_SRC_SNK_Transition_to_off",
	"PRS_SRC_SNK_Wait_Source_on",
	"VCS_Wait_for_VCONN",
};

enum usbpd_control_msg_type {
	MSG_RESERVED = 0,
	MSG_GOODCRC,
	MSG_GOTOMIN,
	MSG_ACCEPT,
	MSG_REJECT,
	MSG_PING,
	MSG_PS_RDY,
	MSG_GET_SOURCE_CAP,
	MSG_GET_SINK_CAP,
	MSG_DR_SWAP,
	MSG_PR_SWAP,
	MSG_VCONN_SWAP,
	MSG_WAIT,
	MSG_SOFT_RESET,
};

enum usbpd_data_msg_type {
	MSG_SOURCE_CAPABILITIES = 1,
	MSG_REQUEST,
	MSG_BIST,
	MSG_SINK_CAPABILITIES,
	MSG_VDM = 0xF,
};

enum vdm_state {
	VDM_NONE,
#ifdef CONFIG_LGE_USB
	DISCOVERED_NAK,
#endif
	DISCOVERED_ID,
	DISCOVERED_SVIDS,
	DISCOVERED_MODES,
	MODE_ENTERED,
	MODE_EXITED,
};

#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
/* ADC threshold values */
static int adc_low_threshold = 1280000;
module_param(adc_low_threshold, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(adc_low_threshold, "ADC Low voltage threshold");

static int adc_high_threshold = 1520000;
module_param(adc_high_threshold, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(adc_high_threshold, "ADC High voltage threshold");

static int adc_edge_low_threshold = 1280000;
module_param(adc_edge_low_threshold, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(adc_edge_low_threshold, "ADC Low voltage threshold");

static int adc_edge_high_threshold = 1350000;
module_param(adc_edge_high_threshold, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(adc_edge_high_threshold, "ADC High voltage threshold");

static int adc_gnd_low_threshold = 35000; // 1.8V: 90000 1V: 50000 (10K ohm)
module_param(adc_gnd_low_threshold, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(adc_gnd_low_threshold, "ADC GND Low voltage threshold");

static int adc_gnd_high_threshold = 110000; // 1.8V: 110000 1V: 61000 (13K ohm)
module_param(adc_gnd_high_threshold, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(adc_gnd_high_threshold, "ADC GND High voltage threshold");

static int adc_meas_interval = ADC_MEAS1_INTERVAL_1S;
module_param(adc_meas_interval, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(adc_meas_interval, "ADC polling period");

enum pd_adc_state {
	ADC_STATE_DRY = 0,
	ADC_STATE_WDT, //Wet DeTection
	ADC_STATE_WFD, //Wait For Dry
	ADC_STATE_WET,
	ADC_STATE_GND,
};

enum hw_pullup_volt {
	HW_PULLUP_NONE = 0,
	HW_PULLUP_1V8,
	HW_PULLUP_1V,
	HW_PULLUP_MAX,
};
#endif

static void *usbpd_ipc_log;
#define usbpd_dbg(dev, fmt, ...) do { \
	ipc_log_string(usbpd_ipc_log, "%s: %s: " fmt, dev_name(dev), __func__, \
			##__VA_ARGS__); \
	dev_dbg(dev, fmt, ##__VA_ARGS__); \
	} while (0)

#define usbpd_info(dev, fmt, ...) do { \
	ipc_log_string(usbpd_ipc_log, "%s: %s: " fmt, dev_name(dev), __func__, \
			##__VA_ARGS__); \
	dev_info(dev, fmt, ##__VA_ARGS__); \
	} while (0)

#define usbpd_warn(dev, fmt, ...) do { \
	ipc_log_string(usbpd_ipc_log, "%s: %s: " fmt, dev_name(dev), __func__, \
			##__VA_ARGS__); \
	dev_warn(dev, fmt, ##__VA_ARGS__); \
	} while (0)

#define usbpd_err(dev, fmt, ...) do { \
	ipc_log_string(usbpd_ipc_log, "%s: %s: " fmt, dev_name(dev), __func__, \
			##__VA_ARGS__); \
	dev_err(dev, fmt, ##__VA_ARGS__); \
	} while (0)

#define NUM_LOG_PAGES		10

/* Timeouts (in ms) */
#define ERROR_RECOVERY_TIME	25
#ifdef CONFIG_LGE_USB
#define SENDER_RESPONSE_TIME	30	/* 24 - 30 ms */
#else
#define SENDER_RESPONSE_TIME	26
#endif
#define SINK_WAIT_CAP_TIME	500
#define PS_TRANSITION_TIME	450
#define SRC_CAP_TIME		120
#define SRC_TRANSITION_TIME	25
#define SRC_RECOVER_TIME	750
#define PS_HARD_RESET_TIME	25
#ifdef CONFIG_LGE_USB
#define PS_SOURCE_ON		480
#else
#define PS_SOURCE_ON		400
#endif
#define PS_SOURCE_OFF		750
#define FIRST_SOURCE_CAP_TIME	200
#define VDM_BUSY_TIME		50
#define VCONN_ON_TIME		100
#ifdef CONFIG_LGE_USB
#define VCONN_STABLE_TIME	50
#define DISCOVERY_IDENTITY_TIME 45
#define DRP_TRY_WAIT_TIME	800
#define FORCED_PR_SWAP_TIME	(DRP_TRY_WAIT_TIME * 3)
#endif

/* tPSHardReset + tSafe0V */
#define SNK_HARD_RESET_VBUS_OFF_TIME	(35 + 650)

/* tSrcRecover + tSrcTurnOn */
#define SNK_HARD_RESET_VBUS_ON_TIME	(1000 + 275)

#ifdef CONFIG_LGE_USB_COMPLIANCE_TEST
/* 20 per PD3.0 spec, but using 3 to avoid violating Sink Wait Cap timeout */
#define PD_DISCOVER_IDENTITY_COUNT 3
#endif

#define PD_CAPS_COUNT		50

#define PD_MAX_MSG_ID		7

#define PD_MAX_DATA_OBJ		7

#define PD_MSG_HDR(type, dr, pr, id, cnt, rev) \
	(((type) & 0xF) | ((dr) << 5) | (rev << 6) | \
	 ((pr) << 8) | ((id) << 9) | ((cnt) << 12))
#define PD_MSG_HDR_COUNT(hdr) (((hdr) >> 12) & 7)
#define PD_MSG_HDR_TYPE(hdr) ((hdr) & 0xF)
#define PD_MSG_HDR_ID(hdr) (((hdr) >> 9) & 7)
#define PD_MSG_HDR_REV(hdr) (((hdr) >> 6) & 3)

#define PD_RDO_FIXED(obj, gb, mismatch, usb_comm, no_usb_susp, curr1, curr2) \
		(((obj) << 28) | ((gb) << 27) | ((mismatch) << 26) | \
		 ((usb_comm) << 25) | ((no_usb_susp) << 24) | \
		 ((curr1) << 10) | (curr2))

#define PD_RDO_AUGMENTED(obj, mismatch, usb_comm, no_usb_susp, volt, curr) \
		(((obj) << 28) | ((mismatch) << 26) | ((usb_comm) << 25) | \
		 ((no_usb_susp) << 24) | ((volt) << 9) | (curr))

#define PD_RDO_OBJ_POS(rdo)		((rdo) >> 28 & 7)
#define PD_RDO_GIVEBACK(rdo)		((rdo) >> 27 & 1)
#define PD_RDO_MISMATCH(rdo)		((rdo) >> 26 & 1)
#define PD_RDO_USB_COMM(rdo)		((rdo) >> 25 & 1)
#define PD_RDO_NO_USB_SUSP(rdo)		((rdo) >> 24 & 1)
#define PD_RDO_FIXED_CURR(rdo)		((rdo) >> 10 & 0x3FF)
#define PD_RDO_FIXED_CURR_MINMAX(rdo)	((rdo) & 0x3FF)
#define PD_RDO_PROG_VOLTAGE(rdo)	((rdo) >> 9 & 0x7FF)
#define PD_RDO_PROG_CURR(rdo)		((rdo) & 0x7F)

#define PD_SRC_PDO_TYPE(pdo)		(((pdo) >> 30) & 3)
#define PD_SRC_PDO_TYPE_FIXED		0
#define PD_SRC_PDO_TYPE_BATTERY		1
#define PD_SRC_PDO_TYPE_VARIABLE	2
#define PD_SRC_PDO_TYPE_AUGMENTED	3

#define PD_SRC_PDO_FIXED_PR_SWAP(pdo)		(((pdo) >> 29) & 1)
#define PD_SRC_PDO_FIXED_USB_SUSP(pdo)		(((pdo) >> 28) & 1)
#define PD_SRC_PDO_FIXED_EXT_POWERED(pdo)	(((pdo) >> 27) & 1)
#define PD_SRC_PDO_FIXED_USB_COMM(pdo)		(((pdo) >> 26) & 1)
#define PD_SRC_PDO_FIXED_DR_SWAP(pdo)		(((pdo) >> 25) & 1)
#define PD_SRC_PDO_FIXED_PEAK_CURR(pdo)		(((pdo) >> 20) & 3)
#define PD_SRC_PDO_FIXED_VOLTAGE(pdo)		(((pdo) >> 10) & 0x3FF)
#define PD_SRC_PDO_FIXED_MAX_CURR(pdo)		((pdo) & 0x3FF)

#define PD_SRC_PDO_VAR_BATT_MAX_VOLT(pdo)	(((pdo) >> 20) & 0x3FF)
#define PD_SRC_PDO_VAR_BATT_MIN_VOLT(pdo)	(((pdo) >> 10) & 0x3FF)
#define PD_SRC_PDO_VAR_BATT_MAX(pdo)		((pdo) & 0x3FF)

#define PD_APDO_PPS(pdo)			(((pdo) >> 28) & 3)
#define PD_APDO_MAX_VOLT(pdo)			(((pdo) >> 17) & 0xFF)
#define PD_APDO_MIN_VOLT(pdo)			(((pdo) >> 8) & 0xFF)
#define PD_APDO_MAX_CURR(pdo)			((pdo) & 0x7F)

/* Vendor Defined Messages */
#define MAX_CRC_RECEIVE_TIME	9 /* ~(2 * tReceive_max(1.1ms) * # retry 4) */
#define MAX_VDM_RESPONSE_TIME	60 /* 2 * tVDMSenderResponse_max(30ms) */
#define MAX_VDM_BUSY_TIME	100 /* 2 * tVDMBusy (50ms) */

#define PD_SNK_PDO_FIXED(prs, hc, uc, usb_comm, drs, volt, curr) \
	(((prs) << 29) | ((hc) << 28) | ((uc) << 27) | ((usb_comm) << 26) | \
	 ((drs) << 25) | ((volt) << 10) | (curr))

/* VDM header is the first 32-bit object following the 16-bit PD header */
#define VDM_HDR_SVID(hdr)	((hdr) >> 16)
#define VDM_IS_SVDM(hdr)	((hdr) & 0x8000)
#define SVDM_HDR_OBJ_POS(hdr)	(((hdr) >> 8) & 0x7)
#define SVDM_HDR_CMD_TYPE(hdr)	(((hdr) >> 6) & 0x3)
#define SVDM_HDR_CMD(hdr)	((hdr) & 0x1f)

#define SVDM_HDR(svid, ver, obj, cmd_type, cmd) \
	(((svid) << 16) | (1 << 15) | ((ver) << 13) \
	| ((obj) << 8) | ((cmd_type) << 6) | (cmd))

/* discover id response vdo bit fields */
#define ID_HDR_USB_HOST		BIT(31)
#define ID_HDR_USB_DEVICE	BIT(30)
#define ID_HDR_MODAL_OPR	BIT(26)
#define ID_HDR_PRODUCT_TYPE(n)	((n) >> 27)
#define ID_HDR_PRODUCT_PER_MASK	(2 << 27)
#define ID_HDR_PRODUCT_HUB	1
#define ID_HDR_PRODUCT_PER	2
#define ID_HDR_PRODUCT_AMA	5
#ifdef CONFIG_LGE_USB
#define ID_HDR_VID		0x1004	/* LG Electronics Inc. */
#else
#define ID_HDR_VID		0x05c6 /* qcom */
#endif
#define PROD_VDO_PID		0x0a00 /* TBD */

static bool check_vsafe0v = true;
module_param(check_vsafe0v, bool, S_IRUSR | S_IWUSR);

static int min_sink_current = 900;
module_param(min_sink_current, int, S_IRUSR | S_IWUSR);

#ifdef CONFIG_LGE_USB
static bool eval_src_caps = false;
module_param(eval_src_caps, bool, S_IRUSR | S_IWUSR);
#endif

#ifdef CONFIG_LGE_USB_COMPLIANCE_TEST
static const u32 default_src_caps[] = { 0x36019032 };  /* VSafe5V @ 500mA */
#else
static const u32 default_src_caps[] = { 0x36019096 };	/* VSafe5V @ 1.5A */
#endif
static const u32 default_snk_caps[] = { 0x2601912C };	/* VSafe5V @ 3A */

static inline const char* typec_to_string(int mode);
struct vdm_tx {
	u32			data[7];
	int			size;
};

struct rx_msg {
	u8			type;
	u8			len;
	u32			payload[7];
	struct list_head	entry;
};

#define IS_DATA(m, t) ((m) && ((m)->len) && ((m)->type == (t)))
#define IS_CTRL(m, t) ((m) && !((m)->len) && ((m)->type == (t)))

struct usbpd {
	struct device		dev;
	struct workqueue_struct	*wq;
	struct work_struct	sm_work;
	struct hrtimer		timer;
	bool			sm_queued;

	struct extcon_dev	*extcon;

	enum usbpd_state	current_state;
	bool			hard_reset_recvd;
	struct list_head	rx_q;
	spinlock_t		rx_lock;

	u32			received_pdos[PD_MAX_DATA_OBJ];
	u16			src_cap_id;
	u8			selected_pdo;
	u8			requested_pdo;
	u32			rdo;	/* can be either source or sink */
	int			current_voltage;	/* uV */
	int			requested_voltage;	/* uV */
	int			requested_current;	/* mA */
	bool			pd_connected;
	bool			in_explicit_contract;
	bool			peer_usb_comm;
	bool			peer_pr_swap;
	bool			peer_dr_swap;

	u32			sink_caps[7];
	int			num_sink_caps;

	struct power_supply	*usb_psy;
	struct notifier_block	psy_nb;

	enum power_supply_typec_mode typec_mode;
	enum power_supply_type	psy_type;
	enum power_supply_typec_power_role forced_pr;
	bool			vbus_present;

	enum pd_spec_rev	spec_rev;
	enum data_role		current_dr;
	enum power_role		current_pr;
	bool			in_pr_swap;
	bool			pd_phy_opened;
	bool			send_request;
	struct completion	is_ready;

	struct mutex		swap_lock;
	struct dual_role_phy_instance	*dual_role;
	struct dual_role_phy_desc	dr_desc;
	bool			send_pr_swap;
	bool			send_dr_swap;

	struct regulator	*vbus;
	struct regulator	*vconn;
	bool			vbus_enabled;
	bool			vconn_enabled;
	bool			vconn_is_external;

	u8			tx_msgid;
	u8			rx_msgid;
	int			caps_count;
	int			hard_reset_count;
#ifdef CONFIG_LGE_USB_COMPLIANCE_TEST
	bool			discovered_identity;
	int			discover_identity_count;
#endif

	enum vdm_state		vdm_state;
	u16			*discovered_svids;
	int			num_svids;
	struct vdm_tx		*vdm_tx;
	struct vdm_tx		*vdm_tx_retry;
	struct list_head	svid_handlers;

	struct list_head	instance;
#ifdef CONFIG_LGE_USB_FACTORY
	struct lge_power *lge_power_cd;
#endif
#ifdef CONFIG_LGE_USB_DEBUGGER
	bool is_debug_accessory;
	struct work_struct usb_debugger_work;
	struct gpio_desc *uart_sbu_sel_gpio;
#endif
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
	struct hrtimer		edge_timer;
	struct hrtimer		sbu_timer;
	struct delayed_work	init_edge_adc_work;
	struct delayed_work	init_sbu_adc_work;
	struct delayed_work	edge_adc_work;
	struct delayed_work	sbu_adc_work;
	struct delayed_work	sbu_ov_adc_work;
	struct qpnp_vadc_chip	*vadc_dev;
	struct qpnp_adc_tm_chip	*adc_tm_dev;
	struct qpnp_adc_tm_btm_param	edge_adc_param;
	struct qpnp_adc_tm_btm_param	sbu_adc_param;
	struct gpio_desc *edge_sel;
	struct gpio_desc *sbu_sel;
	struct gpio_desc *sbu_oe;
	struct mutex		moisture_lock;
	struct timespec		sbu_mtime;
	struct timespec		edge_mtime;
	enum qpnp_tm_state	edge_tm_state;
	enum qpnp_tm_state	sbu_tm_state;
	enum pd_adc_state	edge_adc_state;
	enum pd_adc_state	sbu_adc_state;
	enum dual_role_prop_moisture_en	prop_moisture_en;
	enum dual_role_prop_moisture	prop_moisture;
	enum hw_pullup_volt	pullup_volt;
	int			edge_moisture;
	int			sbu_moisture;
	bool			edge_lock;
	bool			sbu_lock;
	bool			cc_disabled;
	bool			adc_initialized;
	bool			edge_run_work;
	bool			sbu_run_work;
#endif
};

#ifdef CONFIG_LGE_USB_DEBUGGER
extern int msm_serial_set_uart_console(int enable);
extern int msm_serial_get_uart_console_status(void);
#endif
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
static int pd_set_input_suspend(struct usbpd *pd, bool enable);
static int pd_set_cc_disable(struct usbpd *pd, bool enable);
#endif

static LIST_HEAD(_usbpd);	/* useful for debugging */

static const unsigned int usbpd_extcon_cable[] = {
	EXTCON_USB,
	EXTCON_USB_HOST,
	EXTCON_USB_CC,
	EXTCON_USB_SPEED,
	EXTCON_NONE,
};

/* EXTCON_USB and EXTCON_USB_HOST are mutually exclusive */
static const u32 usbpd_extcon_exclusive[] = {0x3, 0};

enum plug_orientation usbpd_get_plug_orientation(struct usbpd *pd)
{
	int ret;
	union power_supply_propval val;

	ret = power_supply_get_property(pd->usb_psy,
		POWER_SUPPLY_PROP_TYPEC_CC_ORIENTATION, &val);
	if (ret)
		return ORIENTATION_NONE;

	return val.intval;
}
EXPORT_SYMBOL(usbpd_get_plug_orientation);

static inline void stop_usb_host(struct usbpd *pd)
{
	extcon_set_cable_state_(pd->extcon, EXTCON_USB_HOST, 0);
}

static inline void start_usb_host(struct usbpd *pd, bool ss)
{
	enum plug_orientation cc = usbpd_get_plug_orientation(pd);

	extcon_set_cable_state_(pd->extcon, EXTCON_USB_CC,
			cc == ORIENTATION_CC2);
	extcon_set_cable_state_(pd->extcon, EXTCON_USB_SPEED, ss);
	extcon_set_cable_state_(pd->extcon, EXTCON_USB_HOST, 1);
}

static inline void stop_usb_peripheral(struct usbpd *pd)
{
	extcon_set_cable_state_(pd->extcon, EXTCON_USB, 0);
}

static inline void start_usb_peripheral(struct usbpd *pd)
{
	enum plug_orientation cc = usbpd_get_plug_orientation(pd);

#ifdef CONFIG_LGE_USB_FACTORY
	if (pd->typec_mode == POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY)
		cc = ORIENTATION_CC2;
#endif

	usbpd_info(&pd->dev, "%s : entered into\n", __func__);
	extcon_set_cable_state_(pd->extcon, EXTCON_USB_CC,
			cc == ORIENTATION_CC2);
	extcon_set_cable_state_(pd->extcon, EXTCON_USB_SPEED, 1);
	extcon_set_cable_state_(pd->extcon, EXTCON_USB, 1);
}

static int set_power_role(struct usbpd *pd, enum power_role pr)
{
	union power_supply_propval val = {0};

	switch (pr) {
	case PR_NONE:
		val.intval = POWER_SUPPLY_TYPEC_PR_NONE;
		break;
	case PR_SINK:
		val.intval = POWER_SUPPLY_TYPEC_PR_SINK;
		break;
	case PR_SRC:
		val.intval = POWER_SUPPLY_TYPEC_PR_SOURCE;
		break;
	}

	return power_supply_set_property(pd->usb_psy,
			POWER_SUPPLY_PROP_TYPEC_POWER_ROLE, &val);
}

static struct usbpd_svid_handler *find_svid_handler(struct usbpd *pd, u16 svid)
{
	struct usbpd_svid_handler *handler;

	list_for_each_entry(handler, &pd->svid_handlers, entry)
		if (svid == handler->svid)
			return handler;

	return NULL;
}

/* Reset protocol layer */
static inline void pd_reset_protocol(struct usbpd *pd)
{
	/*
	 * first Rx ID should be 0; set this to a sentinel of -1 so that in
	 * phy_msg_received() we can check if we had seen it before.
	 */
	pd->rx_msgid = -1;
	pd->tx_msgid = 0;
	pd->send_request = false;
	pd->send_pr_swap = false;
	pd->send_dr_swap = false;
}

static int pd_send_msg(struct usbpd *pd, u8 msg_type, const u32 *data,
		size_t num_data, enum pd_sop_type sop)
{
	int ret;
	u16 hdr;

#ifdef CONFIG_LGE_USB_COMPLIANCE_TEST
	if (sop == SOPI_MSG)
		hdr = PD_MSG_HDR(msg_type, 0, 1,
				 pd->tx_msgid, num_data, pd->spec_rev);
	else
#endif
	hdr = PD_MSG_HDR(msg_type, pd->current_dr, pd->current_pr,
			pd->tx_msgid, num_data, pd->spec_rev);
	ret = pd_phy_write(hdr, (u8 *)data, num_data * sizeof(u32), sop, 15);
	/* TODO figure out timeout. based on tReceive=1.1ms x nRetryCount? */

	if (ret < 0)
		return ret;
	else if (ret != num_data * sizeof(u32))
		return -EIO;

	pd->tx_msgid = (pd->tx_msgid + 1) & PD_MAX_MSG_ID;
	return 0;
}

static int pd_select_pdo(struct usbpd *pd, int pdo_pos, int uv, int ua)
{
	int curr;
	int max_current;
	bool mismatch = false;
	u8 type;
	u32 pdo = pd->received_pdos[pdo_pos - 1];

	type = PD_SRC_PDO_TYPE(pdo);
	if (type == PD_SRC_PDO_TYPE_FIXED) {
		curr = max_current = PD_SRC_PDO_FIXED_MAX_CURR(pdo) * 10;

		/*
		 * Check if the PDO has enough current, otherwise set the
		 * Capability Mismatch flag
		 */
		if (curr < min_sink_current) {
			mismatch = true;
			max_current = min_sink_current;
		}

		pd->requested_voltage =
			PD_SRC_PDO_FIXED_VOLTAGE(pdo) * 50 * 1000;
		pd->rdo = PD_RDO_FIXED(pdo_pos, 0, mismatch, 1, 1, curr / 10,
				max_current / 10);
	} else if (type == PD_SRC_PDO_TYPE_AUGMENTED) {
		if ((uv / 100000) > PD_APDO_MAX_VOLT(pdo) ||
			(uv / 100000) < PD_APDO_MIN_VOLT(pdo) ||
			(ua / 50000) > PD_APDO_MAX_CURR(pdo) || (ua < 0)) {
			usbpd_err(&pd->dev, "uv (%d) and ua (%d) out of range of APDO\n",
					uv, ua);
			return -EINVAL;
		}

		curr = ua / 1000;
		pd->requested_voltage = uv;
		pd->rdo = PD_RDO_AUGMENTED(pdo_pos, mismatch, 1, 1,
				uv / 20000, ua / 50000);
	} else {
		usbpd_err(&pd->dev, "Only Fixed or Programmable PDOs supported\n");
		return -ENOTSUPP;
	}

	/* Can't sink more than 5V if VCONN is sourced from the VBUS input */
	if (pd->vconn_enabled && !pd->vconn_is_external &&
			pd->requested_voltage > 5000000)
		return -ENOTSUPP;

	pd->requested_current = curr;
	pd->requested_pdo = pdo_pos;

	return 0;
}

static int pd_eval_src_caps(struct usbpd *pd)
{
	int obj_cnt;
	union power_supply_propval val;
	u32 first_pdo = pd->received_pdos[0];

	if (PD_SRC_PDO_TYPE(first_pdo) != PD_SRC_PDO_TYPE_FIXED) {
		usbpd_err(&pd->dev, "First src_cap invalid! %08x\n", first_pdo);
		return -EINVAL;
	}

	pd->peer_usb_comm = PD_SRC_PDO_FIXED_USB_COMM(first_pdo);
	pd->peer_pr_swap = PD_SRC_PDO_FIXED_PR_SWAP(first_pdo);
	pd->peer_dr_swap = PD_SRC_PDO_FIXED_DR_SWAP(first_pdo);

	val.intval = PD_SRC_PDO_FIXED_USB_SUSP(first_pdo);
	power_supply_set_property(pd->usb_psy,
			POWER_SUPPLY_PROP_PD_USB_SUSPEND_SUPPORTED, &val);

#ifdef CONFIG_LGE_USB
	if (eval_src_caps) {
		u8 type;
		int i, j;
		int pos = 1;

		for (i = 0; i < ARRAY_SIZE(pd->received_pdos); i++) {
			u32 pdo = pd->received_pdos[i];

			if (pdo == 0)
				break;

			type = PD_SRC_PDO_TYPE(pdo);
			if (type != PD_SRC_PDO_TYPE_FIXED)
				continue;

			for (j = 0; j < pd->num_sink_caps; j++) {
				if (PD_SRC_PDO_FIXED_VOLTAGE(pdo) ==
				    PD_SRC_PDO_FIXED_VOLTAGE(pd->sink_caps[j])) {
				    pos = i + 1;
				    break;
				}
			}
		}

		pd_select_pdo(pd, pos, 0, 0);

		return 0;
	}
#endif
	for (obj_cnt = 1; obj_cnt < PD_MAX_DATA_OBJ; obj_cnt++) {
		if ((PD_SRC_PDO_TYPE(pd->received_pdos[obj_cnt]) ==
					PD_SRC_PDO_TYPE_AUGMENTED) &&
				!PD_APDO_PPS(pd->received_pdos[obj_cnt]))
			pd->spec_rev = USBPD_REV_30;
	}

	/* Select the first PDO (vSafe5V) immediately. */
	pd_select_pdo(pd, 1, 0, 0);

	return 0;
}

static void pd_send_hard_reset(struct usbpd *pd)
{
	union power_supply_propval val = {0};

	usbpd_dbg(&pd->dev, "send hard reset");

	/* Force CC logic to source/sink to keep Rp/Rd unchanged */
	set_power_role(pd, pd->current_pr);
	pd->hard_reset_count++;
	pd_phy_signal(HARD_RESET_SIG, 5); /* tHardResetComplete */
	pd->in_pr_swap = false;
	power_supply_set_property(pd->usb_psy, POWER_SUPPLY_PROP_PR_SWAP, &val);
}

static void kick_sm(struct usbpd *pd, int ms)
{
	pm_stay_awake(&pd->dev);
	pd->sm_queued = true;

	if (ms)
		hrtimer_start(&pd->timer, ms_to_ktime(ms), HRTIMER_MODE_REL);
	else
#ifdef CONFIG_LGE_USB
		queue_work_on(0, system_highpri_wq, &pd->sm_work);
#else
		queue_work(pd->wq, &pd->sm_work);
#endif
}

static void phy_sig_received(struct usbpd *pd, enum pd_sig_type sig)
{
	union power_supply_propval val = {1};

	if (sig != HARD_RESET_SIG) {
		usbpd_err(&pd->dev, "invalid signal (%d) received\n", sig);
		return;
	}

	usbpd_dbg(&pd->dev, "hard reset received\n");

	/* Force CC logic to source/sink to keep Rp/Rd unchanged */
	set_power_role(pd, pd->current_pr);
	pd->hard_reset_recvd = true;
	power_supply_set_property(pd->usb_psy,
			POWER_SUPPLY_PROP_PD_IN_HARD_RESET, &val);

	kick_sm(pd, 0);
}

static void phy_msg_received(struct usbpd *pd, enum pd_sop_type sop,
		u8 *buf, size_t len)
{
	struct rx_msg *rx_msg;
	unsigned long flags;
	u16 header;

#ifdef CONFIG_LGE_USB_COMPLIANCE_TEST
	if (sop != SOP_MSG &&
	    !(sop == SOPI_MSG && pd->current_state == PE_SRC_DISCOVERY_WAIT)) {
		usbpd_err(&pd->dev, "invalid msg type (%d) received; only SOP/SOPI supported\n",
				sop);
		return;
	}
#else
	if (sop != SOP_MSG) {
		usbpd_err(&pd->dev, "invalid msg type (%d) received; only SOP supported\n",
				sop);
		return;
	}
#endif

	if (len < 2) {
		usbpd_err(&pd->dev, "invalid message received, len=%zd\n", len);
		return;
	}

	header = *((u16 *)buf);
	buf += sizeof(u16);
	len -= sizeof(u16);

	if (len % 4 != 0) {
		usbpd_err(&pd->dev, "len=%zd not multiple of 4\n", len);
		return;
	}

	/* if MSGID already seen, discard */
	if (PD_MSG_HDR_ID(header) == pd->rx_msgid &&
			PD_MSG_HDR_TYPE(header) != MSG_SOFT_RESET) {
		usbpd_dbg(&pd->dev, "MessageID already seen, discarding\n");
		return;
	}

	pd->rx_msgid = PD_MSG_HDR_ID(header);

	/* discard Pings */
	if (PD_MSG_HDR_TYPE(header) == MSG_PING && !len)
		return;

	/* check header's count field to see if it matches len */
	if (PD_MSG_HDR_COUNT(header) != (len / 4)) {
		usbpd_err(&pd->dev, "header count (%d) mismatch, len=%zd\n",
				PD_MSG_HDR_COUNT(header), len);
		return;
	}

#ifdef CONFIG_LGE_USB
	rx_msg = kzalloc(sizeof(*rx_msg), GFP_ATOMIC);
#else
	rx_msg = kzalloc(sizeof(*rx_msg), GFP_KERNEL);
#endif
	if (!rx_msg)
		return;

	rx_msg->type = PD_MSG_HDR_TYPE(header);
	rx_msg->len = PD_MSG_HDR_COUNT(header);
	memcpy(&rx_msg->payload, buf, min(len, sizeof(rx_msg->payload)));

	spin_lock_irqsave(&pd->rx_lock, flags);
	list_add_tail(&rx_msg->entry, &pd->rx_q);
	spin_unlock_irqrestore(&pd->rx_lock, flags);

	usbpd_dbg(&pd->dev, "received message: type(%d) len(%d)\n",
			rx_msg->type, rx_msg->len);

	kick_sm(pd, 0);
}

static void phy_shutdown(struct usbpd *pd)
{
	usbpd_dbg(&pd->dev, "shutdown");
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
	/* W/A reduce power-off delay in qpnp_adc_tm_shutdown */
	if (pd->edge_sel) {
		qpnp_adc_tm_disable_chan_meas(pd->adc_tm_dev, &pd->edge_adc_param);
	}
	if (pd->sbu_sel) {
		qpnp_adc_tm_disable_chan_meas(pd->adc_tm_dev, &pd->sbu_adc_param);
	}
#endif
}

static enum hrtimer_restart pd_timeout(struct hrtimer *timer)
{
	struct usbpd *pd = container_of(timer, struct usbpd, timer);

	usbpd_dbg(&pd->dev, "timeout");
#ifdef CONFIG_LGE_USB
	queue_work_on(0, system_highpri_wq, &pd->sm_work);
#else
	queue_work(pd->wq, &pd->sm_work);
#endif

	return HRTIMER_NORESTART;
}

/* Enters new state and executes actions on entry */
static void usbpd_set_state(struct usbpd *pd, enum usbpd_state next_state)
{
	struct pd_phy_params phy_params = {
		.signal_cb		= phy_sig_received,
		.msg_rx_cb		= phy_msg_received,
		.shutdown_cb		= phy_shutdown,
		.frame_filter_val	= FRAME_FILTER_EN_SOP |
					  FRAME_FILTER_EN_HARD_RESET,
	};
	union power_supply_propval val = {0};
	unsigned long flags;
	int ret;

	usbpd_dbg(&pd->dev, "%s -> %s\n",
			usbpd_state_strings[pd->current_state],
			usbpd_state_strings[next_state]);

	pd->current_state = next_state;

	switch (next_state) {
	case PE_ERROR_RECOVERY: /* perform hard disconnect/reconnect */
#ifdef CONFIG_LGE_USB
		pd->in_pr_swap = true;
#else
		pd->in_pr_swap = false;
#endif
		pd->current_pr = PR_NONE;
		set_power_role(pd, PR_NONE);
		pd->typec_mode = POWER_SUPPLY_TYPEC_NONE;
		kick_sm(pd, 0);
		break;
#ifdef CONFIG_LGE_USB
	case PE_FORCED_PR_SWAP:
		if (!pd->in_pr_swap) {
			pd->in_pr_swap = true;
			pd->current_pr = PR_NONE;
			set_power_role(pd, PR_NONE);
			kick_sm(pd, 0);
		}
		break;
#endif

	/* Source states */
	case PE_SRC_DISABLED:
		/* are we still connected? */
		if (pd->typec_mode == POWER_SUPPLY_TYPEC_NONE) {
			pd->current_pr = PR_NONE;
			kick_sm(pd, 0);
		}

		break;

	case PE_SRC_STARTUP:
		if (pd->current_dr == DR_NONE) {
			pd->current_dr = DR_DFP;
			/*
			 * Defer starting USB host mode until PE_SRC_READY or
			 * when PE_SRC_SEND_CAPABILITIES fails
			 */
		}

		dual_role_instance_changed(pd->dual_role);

		/* Set CC back to DRP toggle for the next disconnect */
		val.intval = POWER_SUPPLY_TYPEC_PR_DUAL;
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_TYPEC_POWER_ROLE, &val);

		pd_reset_protocol(pd);

		if (!pd->in_pr_swap) {
			if (pd->pd_phy_opened) {
				pd_phy_close();
				pd->pd_phy_opened = false;
			}

			phy_params.data_role = pd->current_dr;
			phy_params.power_role = pd->current_pr;
#ifdef CONFIG_LGE_USB_COMPLIANCE_TEST
			phy_params.frame_filter_val |= FRAME_FILTER_EN_SOPI;
#endif

			ret = pd_phy_open(&phy_params);
			if (ret) {
				WARN_ON_ONCE(1);
				usbpd_err(&pd->dev, "error opening PD PHY %d\n",
						ret);
				pd->current_state = PE_UNKNOWN;
				return;
			}

			pd->pd_phy_opened = true;

#ifdef CONFIG_LGE_USB_COMPLIANCE_TEST
			if (pd->vconn_enabled && !pd->discovered_identity) {
				pd->discovered_identity = true;
				pd->current_state = PE_SRC_DISCOVERY;
				kick_sm(pd, VCONN_STABLE_TIME);
				break;
			}
#endif
		}

		if (pd->in_pr_swap) {
			pd->in_pr_swap = false;
			val.intval = 0;
			power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PR_SWAP, &val);
		}

		/*
		 * A sink might remove its terminations (during some Type-C
		 * compliance tests or a sink attempting to do Try.SRC)
		 * at this point just after we enabled VBUS. Sending PD
		 * messages now would delay detecting the detach beyond the
		 * required timing. Instead, delay sending out the first
		 * source capabilities to allow for the other side to
		 * completely settle CC debounce and allow HW to detect detach
		 * sooner in the meantime. PD spec allows up to
		 * tFirstSourceCap (250ms).
		 */
		pd->current_state = PE_SRC_SEND_CAPABILITIES;
		kick_sm(pd, FIRST_SOURCE_CAP_TIME);
		break;

	case PE_SRC_SEND_CAPABILITIES:
		kick_sm(pd, 0);
		break;

	case PE_SRC_NEGOTIATE_CAPABILITY:
#ifdef CONFIG_LGE_USB
		if (PD_RDO_OBJ_POS(pd->rdo) != 1 ||
			PD_RDO_FIXED_CURR(pd->rdo) >
				PD_SRC_PDO_FIXED_MAX_CURR(*default_src_caps)) {
#else
		if (PD_RDO_OBJ_POS(pd->rdo) != 1 ||
			PD_RDO_FIXED_CURR(pd->rdo) >
				PD_SRC_PDO_FIXED_MAX_CURR(*default_src_caps) ||
			PD_RDO_FIXED_CURR_MINMAX(pd->rdo) >
				PD_SRC_PDO_FIXED_MAX_CURR(*default_src_caps)) {
#endif
			/* send Reject */
			ret = pd_send_msg(pd, MSG_REJECT, NULL, 0, SOP_MSG);
			if (ret) {
				usbpd_err(&pd->dev, "Error sending Reject\n");
				usbpd_set_state(pd, PE_SRC_SEND_SOFT_RESET);
				break;
			}

			usbpd_err(&pd->dev, "Invalid request: %08x\n", pd->rdo);

			if (pd->in_explicit_contract)
				usbpd_set_state(pd, PE_SRC_READY);
			else
				/*
				 * bypass PE_SRC_Capability_Response and
				 * PE_SRC_Wait_New_Capabilities in this
				 * implementation for simplicity.
				 */
				usbpd_set_state(pd, PE_SRC_SEND_CAPABILITIES);
			break;
		}

		/* PE_SRC_TRANSITION_SUPPLY pseudo-state */
		ret = pd_send_msg(pd, MSG_ACCEPT, NULL, 0, SOP_MSG);
		if (ret) {
			usbpd_err(&pd->dev, "Error sending Accept\n");
			usbpd_set_state(pd, PE_SRC_SEND_SOFT_RESET);
			break;
		}

		/* tSrcTransition required after ACCEPT */
		usleep_range(SRC_TRANSITION_TIME * USEC_PER_MSEC,
				(SRC_TRANSITION_TIME + 5) * USEC_PER_MSEC);

		/*
		 * Normally a voltage change should occur within tSrcReady
		 * but since we only support VSafe5V there is nothing more to
		 * prepare from the power supply so send PS_RDY right away.
		 */
		ret = pd_send_msg(pd, MSG_PS_RDY, NULL, 0, SOP_MSG);
		if (ret) {
			usbpd_err(&pd->dev, "Error sending PS_RDY\n");
			usbpd_set_state(pd, PE_SRC_SEND_SOFT_RESET);
			break;
		}

		usbpd_set_state(pd, PE_SRC_READY);
		break;

	case PE_SRC_READY:
		pd->in_explicit_contract = true;
		if (pd->current_dr == DR_DFP) {
			/* don't start USB host until after SVDM discovery */
			if (pd->vdm_state == VDM_NONE)
				usbpd_send_svdm(pd, USBPD_SID,
						USBPD_SVDM_DISCOVER_IDENTITY,
						SVDM_CMD_TYPE_INITIATOR, 0,
						NULL, 0);
		}

		kobject_uevent(&pd->dev.kobj, KOBJ_CHANGE);
		complete(&pd->is_ready);
		dual_role_instance_changed(pd->dual_role);
		break;

	case PE_SRC_HARD_RESET:
	case PE_SNK_HARD_RESET:
		/* are we still connected? */
		if (pd->typec_mode == POWER_SUPPLY_TYPEC_NONE)
			pd->current_pr = PR_NONE;

		/* hard reset may sleep; handle it in the workqueue */
		kick_sm(pd, 0);
		break;

	case PE_SRC_SEND_SOFT_RESET:
	case PE_SNK_SEND_SOFT_RESET:
		pd_reset_protocol(pd);

		ret = pd_send_msg(pd, MSG_SOFT_RESET, NULL, 0, SOP_MSG);
		if (ret) {
			usbpd_err(&pd->dev, "Error sending Soft Reset, do Hard Reset\n");
			usbpd_set_state(pd, pd->current_pr == PR_SRC ?
					PE_SRC_HARD_RESET : PE_SNK_HARD_RESET);
			break;
		}

		/* wait for ACCEPT */
		kick_sm(pd, SENDER_RESPONSE_TIME);
		break;

	/* Sink states */
	case PE_SNK_STARTUP:
		if (pd->current_dr == DR_NONE || pd->current_dr == DR_UFP) {
			pd->current_dr = DR_UFP;

#ifdef CONFIG_LGE_USB
			if(pd->psy_type == POWER_SUPPLY_TYPE_UNKNOWN)
			{
				usbpd_info(&pd->dev, "APSD is not yet completed, wait 800ms\n");
				msleep(800);
				ret = power_supply_get_property(pd->usb_psy,
					POWER_SUPPLY_PROP_REAL_TYPE, &val);
				if (ret) {
					usbpd_err(&pd->dev, "Unable to read USB TYPE: %d\n", ret);
				} else {
					pd->psy_type = val.intval;
				}
			}
#endif
			if (pd->psy_type == POWER_SUPPLY_TYPE_USB ||
				pd->psy_type == POWER_SUPPLY_TYPE_USB_CDP ||
				pd->psy_type == POWER_SUPPLY_TYPE_USB_FLOAT ||
				usb_compliance_mode)
				start_usb_peripheral(pd);
		}

		dual_role_instance_changed(pd->dual_role);

		ret = power_supply_get_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PD_ALLOWED, &val);
		if (ret) {
			usbpd_err(&pd->dev, "Unable to read USB PROP_PD_ALLOWED: %d\n",
					ret);
			break;
		}

		if (!val.intval || disable_usb_pd)
			break;

		pd_reset_protocol(pd);

		if (!pd->in_pr_swap) {
			if (pd->pd_phy_opened) {
				pd_phy_close();
				pd->pd_phy_opened = false;
			}

#ifdef CONFIG_LGE_USB
			if (pd->typec_mode == POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE ||
					pd->typec_mode == POWER_SUPPLY_TYPEC_SINK ||
					pd->typec_mode == POWER_SUPPLY_TYPEC_NONE)
				break;
#endif

			phy_params.data_role = pd->current_dr;
			phy_params.power_role = pd->current_pr;

			ret = pd_phy_open(&phy_params);
			if (ret) {
				WARN_ON_ONCE(1);
				usbpd_err(&pd->dev, "error opening PD PHY %d\n",
						ret);
				pd->current_state = PE_UNKNOWN;
				return;
			}

			pd->pd_phy_opened = true;
		}

		pd->current_voltage = pd->requested_voltage = 5000000;
		val.intval = pd->requested_voltage; /* set max range to 5V */
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PD_VOLTAGE_MAX, &val);

		if (!pd->vbus_present) {
			pd->current_state = PE_SNK_DISCOVERY;
			/* max time for hard reset to turn vbus back on */
			kick_sm(pd, SNK_HARD_RESET_VBUS_ON_TIME);
			break;
		}

		pd->current_state = PE_SNK_WAIT_FOR_CAPABILITIES;
		/* fall-through */

	case PE_SNK_WAIT_FOR_CAPABILITIES:
		spin_lock_irqsave(&pd->rx_lock, flags);
		if (list_empty(&pd->rx_q))
			kick_sm(pd, SINK_WAIT_CAP_TIME);
		spin_unlock_irqrestore(&pd->rx_lock, flags);
		break;

	case PE_SNK_EVALUATE_CAPABILITY:
		pd->pd_connected = true; /* we know peer is PD capable */
		pd->hard_reset_count = 0;

		/* evaluate PDOs and select one */
		ret = pd_eval_src_caps(pd);
		if (ret < 0) {
			usbpd_err(&pd->dev, "Invalid src_caps received. Skipping request\n");
			break;
		}
		pd->current_state = PE_SNK_SELECT_CAPABILITY;
		/* fall-through */

	case PE_SNK_SELECT_CAPABILITY:
		ret = pd_send_msg(pd, MSG_REQUEST, &pd->rdo, 1, SOP_MSG);
		if (ret) {
			usbpd_err(&pd->dev, "Error sending Request\n");
			usbpd_set_state(pd, PE_SNK_SEND_SOFT_RESET);
			break;
		}

		/* wait for ACCEPT */
		kick_sm(pd, SENDER_RESPONSE_TIME);
		break;

	case PE_SNK_TRANSITION_SINK:
		/* wait for PS_RDY */
		kick_sm(pd, PS_TRANSITION_TIME);
		break;

	case PE_SNK_READY:
		pd->in_explicit_contract = true;
		kobject_uevent(&pd->dev.kobj, KOBJ_CHANGE);
		complete(&pd->is_ready);
		dual_role_instance_changed(pd->dual_role);
		break;

	case PE_SNK_TRANSITION_TO_DEFAULT:
		if (pd->current_dr != DR_UFP) {
			stop_usb_host(pd);
			start_usb_peripheral(pd);
			pd->current_dr = DR_UFP;
			pd_phy_update_roles(pd->current_dr, pd->current_pr);
		}
		if (pd->vconn_enabled) {
			regulator_disable(pd->vconn);
			pd->vconn_enabled = false;
		}

		/* max time for hard reset to turn vbus off */
		kick_sm(pd, SNK_HARD_RESET_VBUS_OFF_TIME);
		break;

	case PE_PRS_SNK_SRC_TRANSITION_TO_OFF:
		val.intval = pd->requested_current = 0; /* suspend charging */
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PD_CURRENT_MAX, &val);

		pd->in_explicit_contract = false;

		/*
		 * need to update PR bit in message header so that
		 * proper GoodCRC is sent when receiving next PS_RDY
		 */
		pd_phy_update_roles(pd->current_dr, PR_SRC);

		/* wait for PS_RDY */
		kick_sm(pd, PS_SOURCE_OFF);
		break;

	default:
		usbpd_dbg(&pd->dev, "No action for state %s\n",
				usbpd_state_strings[pd->current_state]);
		break;
	}
}

int usbpd_register_svid(struct usbpd *pd, struct usbpd_svid_handler *hdlr)
{
	if (find_svid_handler(pd, hdlr->svid)) {
		usbpd_err(&pd->dev, "SVID 0x%04x already registered\n",
				hdlr->svid);
		return -EINVAL;
	}

	/* require connect/disconnect callbacks be implemented */
	if (!hdlr->connect || !hdlr->disconnect) {
		usbpd_err(&pd->dev, "SVID 0x%04x connect/disconnect must be non-NULL\n",
				hdlr->svid);
		return -EINVAL;
	}

	usbpd_dbg(&pd->dev, "registered handler for SVID 0x%04x\n", hdlr->svid);

	list_add_tail(&hdlr->entry, &pd->svid_handlers);

	/* already connected with this SVID discovered? */
	if (pd->vdm_state >= DISCOVERED_SVIDS) {
		int i;

		for (i = 0; i < pd->num_svids; i++) {
			if (pd->discovered_svids[i] == hdlr->svid) {
				hdlr->connect(hdlr);
				hdlr->discovered = true;
				break;
			}
		}
	}

	return 0;
}
EXPORT_SYMBOL(usbpd_register_svid);

void usbpd_unregister_svid(struct usbpd *pd, struct usbpd_svid_handler *hdlr)
{
	list_del_init(&hdlr->entry);
}
EXPORT_SYMBOL(usbpd_unregister_svid);

int usbpd_send_vdm(struct usbpd *pd, u32 vdm_hdr, const u32 *vdos, int num_vdos)
{
	struct vdm_tx *vdm_tx;

	if (!pd->in_explicit_contract || pd->vdm_tx)
		return -EBUSY;

	vdm_tx = kzalloc(sizeof(*vdm_tx), GFP_KERNEL);
	if (!vdm_tx)
		return -ENOMEM;

	vdm_tx->data[0] = vdm_hdr;
	if (vdos && num_vdos)
		memcpy(&vdm_tx->data[1], vdos, num_vdos * sizeof(u32));
	vdm_tx->size = num_vdos + 1; /* include the header */

	/* VDM will get sent in PE_SRC/SNK_READY state handling */
	pd->vdm_tx = vdm_tx;

	/* slight delay before queuing to prioritize handling of incoming VDM */
#ifdef CONFIG_LGE_USB
	kick_sm(pd, SENDER_RESPONSE_TIME);
#else
	kick_sm(pd, 2);
#endif

	return 0;
}
EXPORT_SYMBOL(usbpd_send_vdm);

int usbpd_send_svdm(struct usbpd *pd, u16 svid, u8 cmd,
		enum usbpd_svdm_cmd_type cmd_type, int obj_pos,
		const u32 *vdos, int num_vdos)
{
	u32 svdm_hdr = SVDM_HDR(svid, 0, obj_pos, cmd_type, cmd);

	usbpd_dbg(&pd->dev, "VDM tx: svid:%x cmd:%x cmd_type:%x svdm_hdr:%x\n",
			svid, cmd, cmd_type, svdm_hdr);

	return usbpd_send_vdm(pd, svdm_hdr, vdos, num_vdos);
}
EXPORT_SYMBOL(usbpd_send_svdm);

static void handle_vdm_rx(struct usbpd *pd, struct rx_msg *rx_msg)
{
	u32 vdm_hdr = rx_msg->payload[0];
	u32 *vdos = &rx_msg->payload[1];
	u16 svid = VDM_HDR_SVID(vdm_hdr);
	u16 *psvid;
	u8 i, num_vdos = rx_msg->len - 1;	/* num objects minus header */
	u8 cmd = SVDM_HDR_CMD(vdm_hdr);
	u8 cmd_type = SVDM_HDR_CMD_TYPE(vdm_hdr);
	bool has_dp = false;
	struct usbpd_svid_handler *handler;

	usbpd_dbg(&pd->dev, "VDM rx: svid:%x cmd:%x cmd_type:%x vdm_hdr:%x\n",
			svid, cmd, cmd_type, vdm_hdr);

	/* if it's a supported SVID, pass the message to the handler */
	handler = find_svid_handler(pd, svid);

	/* Unstructured VDM */
	if (!VDM_IS_SVDM(vdm_hdr)) {
		if (handler && handler->vdm_received)
			handler->vdm_received(handler, vdm_hdr, vdos, num_vdos);
		return;
	}

	/* if this interrupts a previous exchange, abort queued response */
	if (cmd_type == SVDM_CMD_TYPE_INITIATOR && pd->vdm_tx) {
		usbpd_dbg(&pd->dev, "Discarding previously queued SVDM tx (SVID:0x%04x)\n",
				VDM_HDR_SVID(pd->vdm_tx->data[0]));

		kfree(pd->vdm_tx);
		pd->vdm_tx = NULL;
	}

	if (handler && handler->svdm_received) {
		handler->svdm_received(handler, cmd, cmd_type, vdos, num_vdos);
		return;
	}

	/* Standard Discovery or unhandled messages go here */
	switch (cmd_type) {
	case SVDM_CMD_TYPE_INITIATOR:
		if (svid == USBPD_SID && cmd == USBPD_SVDM_DISCOVER_IDENTITY) {
			u32 tx_vdos[3] = {
				ID_HDR_USB_HOST | ID_HDR_USB_DEVICE |
					ID_HDR_PRODUCT_PER_MASK | ID_HDR_VID,
				0x0, /* TBD: Cert Stat VDO */
				(PROD_VDO_PID << 16),
				/* TBD: Get these from gadget */
			};

			usbpd_send_svdm(pd, USBPD_SID, cmd,
					SVDM_CMD_TYPE_RESP_ACK, 0, tx_vdos, 3);
		} else if (cmd != USBPD_SVDM_ATTENTION) {
			usbpd_send_svdm(pd, svid, cmd, SVDM_CMD_TYPE_RESP_NAK,
					SVDM_HDR_OBJ_POS(vdm_hdr), NULL, 0);
		}
		break;

	case SVDM_CMD_TYPE_RESP_ACK:
		if (svid != USBPD_SID) {
			usbpd_err(&pd->dev, "unhandled ACK for SVID:0x%x\n",
					svid);
			break;
		}

		switch (cmd) {
		case USBPD_SVDM_DISCOVER_IDENTITY:
			kfree(pd->vdm_tx_retry);
			pd->vdm_tx_retry = NULL;

			pd->vdm_state = DISCOVERED_ID;
			usbpd_send_svdm(pd, USBPD_SID,
					USBPD_SVDM_DISCOVER_SVIDS,
					SVDM_CMD_TYPE_INITIATOR, 0, NULL, 0);
			break;

		case USBPD_SVDM_DISCOVER_SVIDS:
			pd->vdm_state = DISCOVERED_SVIDS;

			kfree(pd->vdm_tx_retry);
			pd->vdm_tx_retry = NULL;

			if (!pd->discovered_svids) {
				pd->num_svids = 2 * num_vdos;
				pd->discovered_svids = kcalloc(pd->num_svids,
								sizeof(u16),
								GFP_KERNEL);
				if (!pd->discovered_svids) {
					usbpd_err(&pd->dev, "unable to allocate SVIDs\n");
					break;
				}

				psvid = pd->discovered_svids;
			} else { /* handle > 12 SVIDs */
				void *ptr;
				size_t oldsize = pd->num_svids * sizeof(u16);
				size_t newsize = oldsize +
						(2 * num_vdos * sizeof(u16));

				ptr = krealloc(pd->discovered_svids, newsize,
						GFP_KERNEL);
				if (!ptr) {
					usbpd_err(&pd->dev, "unable to realloc SVIDs\n");
					break;
				}

				pd->discovered_svids = ptr;
				psvid = pd->discovered_svids + pd->num_svids;
				memset(psvid, 0, (2 * num_vdos));
				pd->num_svids += 2 * num_vdos;
			}

			/* convert 32-bit VDOs to list of 16-bit SVIDs */
			for (i = 0; i < num_vdos * 2; i++) {
				/*
				 * Within each 32-bit VDO,
				 *    SVID[i]: upper 16-bits
				 *    SVID[i+1]: lower 16-bits
				 * where i is even.
				 */
				if (!(i & 1))
					svid = vdos[i >> 1] >> 16;
				else
					svid = vdos[i >> 1] & 0xFFFF;

				/*
				 * There are some devices that incorrectly
				 * swap the order of SVIDs within a VDO. So in
				 * case of an odd-number of SVIDs it could end
				 * up with SVID[i] as 0 while SVID[i+1] is
				 * non-zero. Just skip over the zero ones.
				 */
				if (svid) {
					usbpd_dbg(&pd->dev, "Discovered SVID: 0x%04x\n",
							svid);
					*psvid++ = svid;
				}
			}

			/* if more than 12 SVIDs, resend the request */
			if (num_vdos == 6 && vdos[5] != 0) {
				usbpd_send_svdm(pd, USBPD_SID,
						USBPD_SVDM_DISCOVER_SVIDS,
						SVDM_CMD_TYPE_INITIATOR, 0,
						NULL, 0);
				break;
			}

			/* now that all SVIDs are discovered, notify handlers */
			for (i = 0; i < pd->num_svids; i++) {
				svid = pd->discovered_svids[i];
				if (svid) {
					handler = find_svid_handler(pd, svid);
					if (handler) {
						handler->connect(handler);
						handler->discovered = true;
					}
				}

				if (svid == 0xFF01)
					has_dp = true;
			}

			/*
			 * Finally start USB host now that we have determined
			 * if DisplayPort mode is present or not and limit USB
			 * to HS-only mode if so.
			 */
			start_usb_host(pd, !has_dp);

			break;

		default:
			usbpd_dbg(&pd->dev, "unhandled ACK for command:0x%x\n",
					cmd);
			break;
		}
		break;

	case SVDM_CMD_TYPE_RESP_NAK:
		usbpd_info(&pd->dev, "VDM NAK received for SVID:0x%04x command:0x%x\n",
				svid, cmd);

		switch (cmd) {
		case USBPD_SVDM_DISCOVER_IDENTITY:
		case USBPD_SVDM_DISCOVER_SVIDS:
			start_usb_host(pd, true);
#ifdef CONFIG_LGE_USB
			pd->vdm_state = DISCOVERED_NAK;
#endif
			break;
		default:
			break;
		}

		break;

	case SVDM_CMD_TYPE_RESP_BUSY:
		switch (cmd) {
		case USBPD_SVDM_DISCOVER_IDENTITY:
		case USBPD_SVDM_DISCOVER_SVIDS:
			if (!pd->vdm_tx_retry) {
				usbpd_err(&pd->dev, "Discover command %d VDM was unexpectedly freed\n",
						cmd);
				break;
			}

			/* wait tVDMBusy, then retry */
			pd->vdm_tx = pd->vdm_tx_retry;
			pd->vdm_tx_retry = NULL;
			kick_sm(pd, VDM_BUSY_TIME);
			break;
		default:
			break;
		}
		break;
	}
}

static void handle_vdm_tx(struct usbpd *pd)
{
	int ret;
	unsigned long flags;

	/* only send one VDM at a time */
	if (pd->vdm_tx) {
		u32 vdm_hdr = pd->vdm_tx->data[0];

		/* bail out and try again later if a message just arrived */
		spin_lock_irqsave(&pd->rx_lock, flags);
		if (!list_empty(&pd->rx_q)) {
			spin_unlock_irqrestore(&pd->rx_lock, flags);
			return;
		}
		spin_unlock_irqrestore(&pd->rx_lock, flags);

		ret = pd_send_msg(pd, MSG_VDM, pd->vdm_tx->data,
				pd->vdm_tx->size, SOP_MSG);
		if (ret) {
			usbpd_err(&pd->dev, "Error (%d) sending VDM command %d\n",
					ret, SVDM_HDR_CMD(pd->vdm_tx->data[0]));

			/* retry when hitting PE_SRC/SNK_Ready again */
			if (ret != -EBUSY)
				usbpd_set_state(pd, pd->current_pr == PR_SRC ?
					PE_SRC_SEND_SOFT_RESET :
					PE_SNK_SEND_SOFT_RESET);

			return;
		}

		/*
		 * special case: keep initiated Discover ID/SVIDs
		 * around in case we need to re-try when receiving BUSY
		 */
		if (VDM_IS_SVDM(vdm_hdr) &&
			SVDM_HDR_CMD_TYPE(vdm_hdr) == SVDM_CMD_TYPE_INITIATOR &&
			SVDM_HDR_CMD(vdm_hdr) <= USBPD_SVDM_DISCOVER_SVIDS) {
			if (pd->vdm_tx_retry) {
				usbpd_dbg(&pd->dev, "Previous Discover VDM command %d not ACKed/NAKed\n",
					SVDM_HDR_CMD(
						pd->vdm_tx_retry->data[0]));
				kfree(pd->vdm_tx_retry);
			}
			pd->vdm_tx_retry = pd->vdm_tx;
		} else {
			kfree(pd->vdm_tx);
		}

		pd->vdm_tx = NULL;
	}
}

static void reset_vdm_state(struct usbpd *pd)
{
	struct usbpd_svid_handler *handler;

	list_for_each_entry(handler, &pd->svid_handlers, entry) {
		if (handler->discovered) {
			handler->disconnect(handler);
			handler->discovered = false;
		}
	}

	pd->vdm_state = VDM_NONE;
	kfree(pd->vdm_tx_retry);
	pd->vdm_tx_retry = NULL;
	kfree(pd->discovered_svids);
	pd->discovered_svids = NULL;
	pd->num_svids = 0;
	kfree(pd->vdm_tx);
	pd->vdm_tx = NULL;
}

static void dr_swap(struct usbpd *pd)
{
#ifdef CONFIG_LGE_USB
	if (pd->current_dr == DR_DFP) {
		pd_phy_update_roles(DR_UFP, pd->current_pr);
	} else if (pd->current_dr == DR_UFP) {
		pd_phy_update_roles(DR_DFP, pd->current_pr);
	}
#endif

	reset_vdm_state(pd);

	if (pd->current_dr == DR_DFP) {
		stop_usb_host(pd);
		start_usb_peripheral(pd);
		pd->current_dr = DR_UFP;
	} else if (pd->current_dr == DR_UFP) {
		stop_usb_peripheral(pd);
		pd->current_dr = DR_DFP;

		/* don't start USB host until after SVDM discovery */
		usbpd_send_svdm(pd, USBPD_SID, USBPD_SVDM_DISCOVER_IDENTITY,
				SVDM_CMD_TYPE_INITIATOR, 0, NULL, 0);
	}

#ifndef CONFIG_LGE_USB
	pd_phy_update_roles(pd->current_dr, pd->current_pr);
#endif
	dual_role_instance_changed(pd->dual_role);
}


static void vconn_swap(struct usbpd *pd)
{
	int ret;

	if (pd->vconn_enabled) {
		pd->current_state = PE_VCS_WAIT_FOR_VCONN;
		kick_sm(pd, VCONN_ON_TIME);
	} else {
		ret = regulator_enable(pd->vconn);
		if (ret) {
			usbpd_err(&pd->dev, "Unable to enable vconn\n");
			return;
		}

		pd->vconn_enabled = true;

		/*
		 * Small delay to ensure Vconn has ramped up. This is well
		 * below tVCONNSourceOn (100ms) so we still send PS_RDY within
		 * the allowed time.
		 */
		usleep_range(5000, 10000);

		ret = pd_send_msg(pd, MSG_PS_RDY, NULL, 0, SOP_MSG);
		if (ret) {
			usbpd_err(&pd->dev, "Error sending PS_RDY\n");
			usbpd_set_state(pd, pd->current_pr == PR_SRC ?
					PE_SRC_SEND_SOFT_RESET :
					PE_SNK_SEND_SOFT_RESET);
			return;
		}
	}
}

static int enable_vbus(struct usbpd *pd)
{
	union power_supply_propval val = {0};
	int count = 100;
	int ret;

	if (!check_vsafe0v)
		goto enable_reg;

	/*
	 * Check to make sure there's no lingering charge on
	 * VBUS before enabling it as a source. If so poll here
	 * until it goes below VSafe0V (0.8V) before proceeding.
	 */
	while (count--) {
		ret = power_supply_get_property(pd->usb_psy,
				POWER_SUPPLY_PROP_VOLTAGE_NOW, &val);
		if (ret || val.intval <= 800000)
			break;
		usleep_range(20000, 30000);
	}

	if (count < 99)
		msleep(100);	/* need to wait an additional tCCDebounce */

enable_reg:
	ret = regulator_enable(pd->vbus);
	if (ret)
		usbpd_err(&pd->dev, "Unable to enable vbus (%d)\n", ret);
	else
		pd->vbus_enabled = true;

	return ret;
}

static inline void rx_msg_cleanup(struct usbpd *pd)
{
	struct rx_msg *msg, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&pd->rx_lock, flags);
	list_for_each_entry_safe(msg, tmp, &pd->rx_q, entry) {
		list_del(&msg->entry);
		kfree(msg);
	}
	spin_unlock_irqrestore(&pd->rx_lock, flags);
}

/* For PD 3.0, check SinkTxOk before allowing initiating AMS */
static inline bool is_sink_tx_ok(struct usbpd *pd)
{
	if (pd->spec_rev == USBPD_REV_30)
		return pd->typec_mode == POWER_SUPPLY_TYPEC_SOURCE_HIGH;

	return true;
}

/* Handles current state and determines transitions */
static void usbpd_sm(struct work_struct *w)
{
	struct usbpd *pd = container_of(w, struct usbpd, sm_work);
	union power_supply_propval val = {0};
	int ret;
	struct rx_msg *rx_msg = NULL;
	unsigned long flags;
#ifdef CONFIG_LGE_USB_COMPLIANCE_TEST
	u32 svdm_hdr;
#endif

	usbpd_dbg(&pd->dev, "handle state %s\n",
			usbpd_state_strings[pd->current_state]);

	hrtimer_cancel(&pd->timer);
	pd->sm_queued = false;

	spin_lock_irqsave(&pd->rx_lock, flags);
	if (!list_empty(&pd->rx_q)) {
		rx_msg = list_first_entry(&pd->rx_q, struct rx_msg, entry);
		list_del(&rx_msg->entry);
	}
	spin_unlock_irqrestore(&pd->rx_lock, flags);

	/* Disconnect? */
	if (pd->current_pr == PR_NONE) {
		if (pd->current_state == PE_UNKNOWN)
			goto sm_done;

		if (pd->vconn_enabled) {
			regulator_disable(pd->vconn);
			pd->vconn_enabled = false;
		}

		usbpd_info(&pd->dev, "USB Type-C disconnect\n");

#ifdef CONFIG_LGE_USB
		if (pd->vconn_enabled) {
			regulator_disable(pd->vconn);
			pd->vconn_enabled = false;
		}

		if (pd->vbus_enabled) {
			regulator_disable(pd->vbus);
			pd->vbus_enabled = false;
		}
#endif

		if (pd->pd_phy_opened) {
			pd_phy_close();
			pd->pd_phy_opened = false;
		}
#ifdef CONFIG_LGE_USB
		if (pd->current_state != PE_FORCED_PR_SWAP)
			pd->in_pr_swap = false;
#else
		pd->in_pr_swap = false;
#endif
		pd->pd_connected = false;
		pd->in_explicit_contract = false;
		pd->hard_reset_recvd = false;
		pd->caps_count = 0;
		pd->hard_reset_count = 0;
		pd->requested_voltage = 0;
		pd->requested_current = 0;
		pd->selected_pdo = pd->requested_pdo = 0;
#ifdef CONFIG_LGE_USB
#ifdef CONFIG_LGE_USB_COMPLIANCE_TEST
		pd->discovered_identity = false;
		pd->discover_identity_count = 0;
#endif
		pd->rdo = 0;
#endif
		memset(&pd->received_pdos, 0, sizeof(pd->received_pdos));
		rx_msg_cleanup(pd);

		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PD_IN_HARD_RESET, &val);

		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PD_USB_SUSPEND_SUPPORTED,
				&val);

		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PD_ACTIVE, &val);

#ifndef CONFIG_LGE_USB
		if (pd->vbus_enabled) {
			regulator_disable(pd->vbus);
			pd->vbus_enabled = false;
		}

#endif

		if (pd->current_dr == DR_UFP)
			stop_usb_peripheral(pd);
		else if (pd->current_dr == DR_DFP)
			stop_usb_host(pd);

		pd->current_dr = DR_NONE;

		reset_vdm_state(pd);

#ifdef CONFIG_LGE_USB
		if (pd->current_state == PE_ERROR_RECOVERY ||
		    pd->current_state == PE_FORCED_PR_SWAP)
#else
		if (pd->current_state == PE_ERROR_RECOVERY)
#endif
			/* forced disconnect, wait before resetting to DRP */
			usleep_range(ERROR_RECOVERY_TIME * USEC_PER_MSEC,
				(ERROR_RECOVERY_TIME + 5) * USEC_PER_MSEC);

		val.intval = 0;
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PR_SWAP, &val);

		/* set due to dual_role class "mode" change */
		if (pd->forced_pr != POWER_SUPPLY_TYPEC_PR_NONE)
			val.intval = pd->forced_pr;
		else
			/* Set CC back to DRP toggle */
			val.intval = POWER_SUPPLY_TYPEC_PR_DUAL;

		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_TYPEC_POWER_ROLE, &val);
#ifdef CONFIG_LGE_USB
		if (pd->current_state != PE_FORCED_PR_SWAP)
#endif
		pd->forced_pr = POWER_SUPPLY_TYPEC_PR_NONE;

#ifdef CONFIG_LGE_USB
		if (pd->forced_pr != POWER_SUPPLY_TYPEC_PR_NONE) {
			switch (pd->forced_pr) {
			case POWER_SUPPLY_TYPEC_PR_SINK:
				pd->current_pr = PR_SRC;
				kick_sm(pd, FORCED_PR_SWAP_TIME);
				break;
			case POWER_SUPPLY_TYPEC_PR_SOURCE:
				pd->current_pr = PR_SINK;
				kick_sm(pd, FORCED_PR_SWAP_TIME);
				break;
			default:
				usbpd_err(&pd->dev, "Unknown forced PR: %d\n",
					  pd->forced_pr);
				pd->forced_pr = POWER_SUPPLY_TYPEC_PR_NONE;
				usbpd_set_state(pd, PE_ERROR_RECOVERY);
				break;
			}
		}
		else
#endif
		pd->current_state = PE_UNKNOWN;

		kobject_uevent(&pd->dev.kobj, KOBJ_CHANGE);
		dual_role_instance_changed(pd->dual_role);

		goto sm_done;
	}

	/* Hard reset? */
	if (pd->hard_reset_recvd) {
		pd->hard_reset_recvd = false;

		if (pd->requested_current) {
			val.intval = pd->requested_current = 0;
			power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PD_CURRENT_MAX, &val);
		}

		pd->requested_voltage = 5000000;
		val.intval = pd->requested_voltage;
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PD_VOLTAGE_MIN, &val);

		pd->in_pr_swap = false;
		val.intval = 0;
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PR_SWAP, &val);

		pd->in_explicit_contract = false;
		pd->selected_pdo = pd->requested_pdo = 0;
		pd->rdo = 0;
		rx_msg_cleanup(pd);
		reset_vdm_state(pd);
		kobject_uevent(&pd->dev.kobj, KOBJ_CHANGE);

		if (pd->current_pr == PR_SINK) {
#ifdef CONFIG_LGE_USB
			pd->requested_voltage = 5000000;

			if (pd->requested_current) {
				val.intval = pd->requested_current = 0;
				power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PD_CURRENT_MAX, &val);
			}

			val.intval = pd->requested_voltage;
			power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_VOLTAGE_MIN, &val);
#endif
			usbpd_set_state(pd, PE_SNK_TRANSITION_TO_DEFAULT);
		} else {
			pd->current_state = PE_SRC_TRANSITION_TO_DEFAULT;
			kick_sm(pd, PS_HARD_RESET_TIME);
		}

		goto sm_done;
	}

	/* Soft reset? */
	if (IS_CTRL(rx_msg, MSG_SOFT_RESET)) {
		usbpd_dbg(&pd->dev, "Handle soft reset\n");

		if (pd->current_pr == PR_SRC)
			pd->current_state = PE_SRC_SOFT_RESET;
		else if (pd->current_pr == PR_SINK)
			pd->current_state = PE_SNK_SOFT_RESET;
	}

	switch (pd->current_state) {
	case PE_UNKNOWN:
		if (pd->current_pr == PR_SINK) {
			usbpd_set_state(pd, PE_SNK_STARTUP);
		} else if (pd->current_pr == PR_SRC) {
			if (!pd->vconn_enabled &&
					pd->typec_mode ==
					POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE) {
				ret = regulator_enable(pd->vconn);
				if (ret)
					usbpd_err(&pd->dev, "Unable to enable vconn\n");
				else
					pd->vconn_enabled = true;
			}
			enable_vbus(pd);

			usbpd_set_state(pd, PE_SRC_STARTUP);
		}
		break;

#ifdef CONFIG_LGE_USB
	case PE_FORCED_PR_SWAP:
		pd->in_pr_swap = false;
		pd->forced_pr = POWER_SUPPLY_TYPEC_PR_NONE;

		if (pd->typec_mode == POWER_SUPPLY_TYPEC_NONE) {
			usbpd_set_state(pd, PE_ERROR_RECOVERY);
		} else {
			pd->current_state = PE_UNKNOWN;
			kick_sm(pd, 0);
		}
		break;
#endif

	case PE_SRC_STARTUP:
		usbpd_set_state(pd, PE_SRC_STARTUP);
		break;

#ifdef CONFIG_LGE_USB_COMPLIANCE_TEST
	case PE_SRC_DISCOVERY:
		svdm_hdr = SVDM_HDR(USBPD_SID, 0, 0, SVDM_CMD_TYPE_INITIATOR,
				    USBPD_SVDM_DISCOVER_IDENTITY);

		ret = pd_send_msg(pd, MSG_VDM, &svdm_hdr, 1, SOPI_MSG);
		if (ret) {
			pd->discover_identity_count++;

			if (pd->discover_identity_count >=
			    PD_DISCOVER_IDENTITY_COUNT) {
				usbpd_dbg(&pd->dev, "DiscoverIDCounter exceeded\n");
				usbpd_set_state(pd, PE_SRC_SEND_CAPABILITIES);
				break;
			}

			kick_sm(pd, DISCOVERY_IDENTITY_TIME);
			break;
		}

		/* transmit was successful */
		pd->discover_identity_count = 0;

		/* wait for RESPONSE */
		pd->current_state = PE_SRC_DISCOVERY_WAIT;
		kick_sm(pd, SENDER_RESPONSE_TIME);

		val.intval = 1;
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PD_ACTIVE, &val);
		break;

	case PE_SRC_DISCOVERY_WAIT:
		if (IS_DATA(rx_msg, MSG_VDM)) {
			/* TODO: Handle Discovery Identity response from cable */
		}

		pd_reset_protocol(pd);
		usbpd_set_state(pd, PE_SRC_SEND_CAPABILITIES);
		break;
#endif

	case PE_SRC_SEND_CAPABILITIES:
		ret = pd_send_msg(pd, MSG_SOURCE_CAPABILITIES, default_src_caps,
				ARRAY_SIZE(default_src_caps), SOP_MSG);
		if (ret) {
#ifdef CONFIG_LGE_USB
			usbpd_dbg(&pd->dev, "pd_send_msg(Src Caps) return %d\n", ret);
			if (ret != -EFAULT) {
				if (rx_msg) {
					usbpd_err(&pd->dev, "Unexpected message received\n");
					usbpd_set_state(pd, PE_SRC_SEND_SOFT_RESET);
				} else {
					usbpd_set_state(pd, PE_SRC_HARD_RESET);
				}
				break;
			}

			/*
			 * When sending Src_Caps, do not increase the MessageID
			 * even if a Tx error occurs.
			 */
			pd->tx_msgid = 0;
#endif
			pd->caps_count++;

#ifdef CONFIG_LGE_USB
			if (pd->caps_count == 8 && pd->current_dr == DR_DFP) {
#else
			if (pd->caps_count == 10 && pd->current_dr == DR_DFP) {
#endif
				/* Likely not PD-capable, start host now */
				start_usb_host(pd, true);
			} else if (pd->caps_count >= PD_CAPS_COUNT) {
				usbpd_dbg(&pd->dev, "Src CapsCounter exceeded, disabling PD\n");
				usbpd_set_state(pd, PE_SRC_DISABLED);

				val.intval = 0;
				power_supply_set_property(pd->usb_psy,
						POWER_SUPPLY_PROP_PD_ACTIVE,
						&val);
				break;
			}

			kick_sm(pd, SRC_CAP_TIME);
			break;
		}

		/* transmit was successful if GoodCRC was received */
		pd->caps_count = 0;
		pd->hard_reset_count = 0;
		pd->pd_connected = true; /* we know peer is PD capable */

		/* wait for REQUEST */
		pd->current_state = PE_SRC_SEND_CAPABILITIES_WAIT;
		kick_sm(pd, SENDER_RESPONSE_TIME);

		val.intval = 1;
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PD_ACTIVE, &val);
		break;

	case PE_SRC_SEND_CAPABILITIES_WAIT:
		if (IS_DATA(rx_msg, MSG_REQUEST)) {
			pd->rdo = rx_msg->payload[0];
			usbpd_set_state(pd, PE_SRC_NEGOTIATE_CAPABILITY);
		} else if (rx_msg) {
			usbpd_err(&pd->dev, "Unexpected message received\n");
			usbpd_set_state(pd, PE_SRC_SEND_SOFT_RESET);
		} else {
			usbpd_set_state(pd, PE_SRC_HARD_RESET);
		}
		break;

	case PE_SRC_READY:
		if (IS_CTRL(rx_msg, MSG_GET_SOURCE_CAP)) {
			pd->current_state = PE_SRC_SEND_CAPABILITIES;
			kick_sm(pd, 0);
		} else if (IS_CTRL(rx_msg, MSG_GET_SINK_CAP)) {
			ret = pd_send_msg(pd, MSG_SINK_CAPABILITIES,
					pd->sink_caps, pd->num_sink_caps,
					SOP_MSG);
			if (ret) {
				usbpd_err(&pd->dev, "Error sending Sink Caps\n");
				usbpd_set_state(pd, PE_SRC_SEND_SOFT_RESET);
			}
		} else if (IS_DATA(rx_msg, MSG_REQUEST)) {
			pd->rdo = rx_msg->payload[0];
			usbpd_set_state(pd, PE_SRC_NEGOTIATE_CAPABILITY);
		} else if (IS_CTRL(rx_msg, MSG_DR_SWAP)) {
			if (pd->vdm_state == MODE_ENTERED) {
				usbpd_set_state(pd, PE_SRC_HARD_RESET);
				break;
			}

			ret = pd_send_msg(pd, MSG_ACCEPT, NULL, 0, SOP_MSG);
			if (ret) {
				usbpd_err(&pd->dev, "Error sending Accept\n");
				usbpd_set_state(pd, PE_SRC_SEND_SOFT_RESET);
				break;
			}

			dr_swap(pd);
		} else if (IS_CTRL(rx_msg, MSG_PR_SWAP)) {
			/* lock in current mode */
			set_power_role(pd, pd->current_pr);

			/* we'll happily accept Src->Sink requests anytime */
			ret = pd_send_msg(pd, MSG_ACCEPT, NULL, 0, SOP_MSG);
			if (ret) {
				usbpd_err(&pd->dev, "Error sending Accept\n");
				usbpd_set_state(pd, PE_SRC_SEND_SOFT_RESET);
				break;
			}

			pd->current_state = PE_PRS_SRC_SNK_TRANSITION_TO_OFF;
			kick_sm(pd, SRC_TRANSITION_TIME);
			break;
		} else if (IS_CTRL(rx_msg, MSG_VCONN_SWAP)) {
			ret = pd_send_msg(pd, MSG_ACCEPT, NULL, 0, SOP_MSG);
			if (ret) {
				usbpd_err(&pd->dev, "Error sending Accept\n");
				usbpd_set_state(pd, PE_SRC_SEND_SOFT_RESET);
				break;
			}

			vconn_swap(pd);
		} else if (IS_DATA(rx_msg, MSG_VDM)) {
			handle_vdm_rx(pd, rx_msg);
		} else if (pd->send_pr_swap) {
			pd->send_pr_swap = false;
			ret = pd_send_msg(pd, MSG_PR_SWAP, NULL, 0, SOP_MSG);
			if (ret) {
				dev_err(&pd->dev, "Error sending PR Swap\n");
				usbpd_set_state(pd, PE_SRC_SEND_SOFT_RESET);
				break;
			}

			pd->current_state = PE_PRS_SRC_SNK_SEND_SWAP;
			kick_sm(pd, SENDER_RESPONSE_TIME);
		} else if (pd->send_dr_swap) {
			pd->send_dr_swap = false;
			ret = pd_send_msg(pd, MSG_DR_SWAP, NULL, 0, SOP_MSG);
			if (ret) {
				dev_err(&pd->dev, "Error sending DR Swap\n");
				usbpd_set_state(pd, PE_SRC_SEND_SOFT_RESET);
				break;
			}

			pd->current_state = PE_DRS_SEND_DR_SWAP;
			kick_sm(pd, SENDER_RESPONSE_TIME);
		} else {
#ifdef CONFIG_LGE_USB
			if (pd->vdm_tx) {
				handle_vdm_tx(pd);
				kick_sm(pd, SENDER_RESPONSE_TIME);
			} else {
				if (pd->current_dr == DR_DFP &&
				    !extcon_get_cable_state_(pd->extcon,
							     EXTCON_USB_HOST))
					start_usb_host(pd, true);
			}
#else
			handle_vdm_tx(pd);
#endif
		}
#ifdef CONFIG_LGE_USB
		if (!pd->sm_queued && pd->vdm_tx)
			kick_sm(pd, SENDER_RESPONSE_TIME);
#endif
		break;

	case PE_SRC_TRANSITION_TO_DEFAULT:
		if (pd->vconn_enabled)
			regulator_disable(pd->vconn);
		if (pd->vbus_enabled)
			regulator_disable(pd->vbus);

		if (pd->current_dr != DR_DFP) {
			extcon_set_cable_state_(pd->extcon, EXTCON_USB, 0);
			pd->current_dr = DR_DFP;
			pd_phy_update_roles(pd->current_dr, pd->current_pr);
		}

		msleep(SRC_RECOVER_TIME);

		pd->vbus_enabled = false;
		enable_vbus(pd);

		if (pd->vconn_enabled) {
			ret = regulator_enable(pd->vconn);
			if (ret) {
				usbpd_err(&pd->dev, "Unable to enable vconn\n");
				pd->vconn_enabled = false;
			}
		}

		val.intval = 0;
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PD_IN_HARD_RESET, &val);

		usbpd_set_state(pd, PE_SRC_STARTUP);
		break;

	case PE_SRC_HARD_RESET:
		val.intval = 1;
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PD_IN_HARD_RESET, &val);

		pd_send_hard_reset(pd);
		pd->in_explicit_contract = false;
		pd->rdo = 0;
		rx_msg_cleanup(pd);
		reset_vdm_state(pd);
		kobject_uevent(&pd->dev.kobj, KOBJ_CHANGE);

		pd->current_state = PE_SRC_TRANSITION_TO_DEFAULT;
		kick_sm(pd, PS_HARD_RESET_TIME);
		break;

	case PE_SNK_STARTUP:
		usbpd_set_state(pd, PE_SNK_STARTUP);
		break;

	case PE_SNK_DISCOVERY:
		if (!rx_msg) {
			if (pd->vbus_present)
				usbpd_set_state(pd,
						PE_SNK_WAIT_FOR_CAPABILITIES);

			/*
			 * Handle disconnection in the middle of PR_Swap.
			 * Since in psy_changed() if pd->in_pr_swap is true
			 * we ignore the typec_mode==NONE change since that is
			 * expected to happen. However if the cable really did
			 * get disconnected we need to check for it here after
			 * waiting for VBUS presence times out.
			 */
#ifdef CONFIG_LGE_USB
			if (!pd->typec_mode || !pd->vbus_present) {
#else
			if (!pd->typec_mode) {
#endif
				pd->current_pr = PR_NONE;
				kick_sm(pd, 0);
			}

			break;
		}
		/* else fall-through */

	case PE_SNK_WAIT_FOR_CAPABILITIES:
		pd->in_pr_swap = false;
		val.intval = 0;
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PR_SWAP, &val);

		if (IS_DATA(rx_msg, MSG_SOURCE_CAPABILITIES)) {
			val.intval = 0;
			power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PD_IN_HARD_RESET,
					&val);

			/* save the PDOs so userspace can further evaluate */
			memcpy(&pd->received_pdos, rx_msg->payload,
					sizeof(pd->received_pdos));
			pd->src_cap_id++;

			usbpd_set_state(pd, PE_SNK_EVALUATE_CAPABILITY);

			val.intval = 1;
			power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PD_ACTIVE, &val);
		} else if (pd->hard_reset_count < 3) {
			usbpd_set_state(pd, PE_SNK_HARD_RESET);
		} else {
			usbpd_dbg(&pd->dev, "Sink hard reset count exceeded, disabling PD\n");

			val.intval = 0;
			power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PD_IN_HARD_RESET,
					&val);

			val.intval = 0;
			power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PD_ACTIVE, &val);

			pd_phy_close();
			pd->pd_phy_opened = false;
		}
		break;

	case PE_SNK_SELECT_CAPABILITY:
		if (IS_CTRL(rx_msg, MSG_ACCEPT)) {
			u32 pdo = pd->received_pdos[pd->requested_pdo - 1];
			bool same_pps = (pd->selected_pdo == pd->requested_pdo)
				&& (PD_SRC_PDO_TYPE(pdo) ==
						PD_SRC_PDO_TYPE_AUGMENTED);

			usbpd_set_state(pd, PE_SNK_TRANSITION_SINK);

			/* prepare for voltage increase/decrease */
			val.intval = pd->requested_voltage;
			power_supply_set_property(pd->usb_psy,
				pd->requested_voltage >= pd->current_voltage ?
					POWER_SUPPLY_PROP_PD_VOLTAGE_MAX :
					POWER_SUPPLY_PROP_PD_VOLTAGE_MIN,
					&val);

			/*
			 * if changing voltages (not within the same PPS PDO),
			 * we must lower input current to pSnkStdby (2.5W).
			 * Calculate it and set PD_CURRENT_MAX accordingly.
			 */
			if (!same_pps &&
				pd->requested_voltage != pd->current_voltage) {
				int mv = max(pd->requested_voltage,
						pd->current_voltage) / 1000;
				val.intval = (2500000 / mv) * 1000;
				power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PD_CURRENT_MAX, &val);
			} else {
				/* decreasing current? */
				ret = power_supply_get_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PD_CURRENT_MAX, &val);
				if (!ret &&
					pd->requested_current < val.intval) {
					val.intval =
						pd->requested_current * 1000;
					power_supply_set_property(pd->usb_psy,
					     POWER_SUPPLY_PROP_PD_CURRENT_MAX,
					     &val);
				}
			}

			pd->selected_pdo = pd->requested_pdo;
		} else if (IS_CTRL(rx_msg, MSG_REJECT) ||
				IS_CTRL(rx_msg, MSG_WAIT)) {
			if (pd->in_explicit_contract)
				usbpd_set_state(pd, PE_SNK_READY);
			else
				usbpd_set_state(pd,
						PE_SNK_WAIT_FOR_CAPABILITIES);
		} else if (rx_msg) {
			usbpd_err(&pd->dev, "Invalid response to sink request\n");
			usbpd_set_state(pd, PE_SNK_SEND_SOFT_RESET);
		} else {
			/* timed out; go to hard reset */
			usbpd_set_state(pd, PE_SNK_HARD_RESET);
		}
		break;

	case PE_SNK_TRANSITION_SINK:
		if (IS_CTRL(rx_msg, MSG_PS_RDY)) {
			val.intval = pd->requested_voltage;
			power_supply_set_property(pd->usb_psy,
				pd->requested_voltage >= pd->current_voltage ?
					POWER_SUPPLY_PROP_PD_VOLTAGE_MIN :
					POWER_SUPPLY_PROP_PD_VOLTAGE_MAX, &val);
			pd->current_voltage = pd->requested_voltage;

			/* resume charging */
			val.intval = pd->requested_current * 1000; /* mA->uA */
			power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PD_CURRENT_MAX, &val);

			usbpd_set_state(pd, PE_SNK_READY);
		} else {
			/* timed out; go to hard reset */
			usbpd_set_state(pd, PE_SNK_HARD_RESET);
		}
		break;

	case PE_SNK_READY:
		if (IS_DATA(rx_msg, MSG_SOURCE_CAPABILITIES)) {
			/* save the PDOs so userspace can further evaluate */
			memcpy(&pd->received_pdos, rx_msg->payload,
					sizeof(pd->received_pdos));
			pd->src_cap_id++;

			usbpd_set_state(pd, PE_SNK_EVALUATE_CAPABILITY);
		} else if (IS_CTRL(rx_msg, MSG_GET_SINK_CAP)) {
			ret = pd_send_msg(pd, MSG_SINK_CAPABILITIES,
					pd->sink_caps, pd->num_sink_caps,
					SOP_MSG);
#ifdef CONFIG_LGE_USB
			if (ret) {
				usbpd_err(&pd->dev, "Error sending Sink Caps\n");
				usbpd_set_state(pd, PE_SNK_SEND_SOFT_RESET);
				break;
			}
			kick_sm(pd, SENDER_RESPONSE_TIME);
#else
			if (ret) {
				usbpd_err(&pd->dev, "Error sending Sink Caps\n");
				usbpd_set_state(pd, PE_SNK_SEND_SOFT_RESET);
			}
#endif
		} else if (IS_CTRL(rx_msg, MSG_GET_SOURCE_CAP)) {
			ret = pd_send_msg(pd, MSG_SOURCE_CAPABILITIES,
					default_src_caps,
					ARRAY_SIZE(default_src_caps), SOP_MSG);
			if (ret) {
				usbpd_err(&pd->dev, "Error sending SRC CAPs\n");
				usbpd_set_state(pd, PE_SNK_SEND_SOFT_RESET);
				break;
			}
#ifdef CONFIG_LGE_USB
			kick_sm(pd, SENDER_RESPONSE_TIME);
#endif
		} else if (IS_CTRL(rx_msg, MSG_DR_SWAP)) {
			if (pd->vdm_state == MODE_ENTERED) {
				usbpd_set_state(pd, PE_SNK_HARD_RESET);
				break;
			}

			ret = pd_send_msg(pd, MSG_ACCEPT, NULL, 0, SOP_MSG);
			if (ret) {
				usbpd_err(&pd->dev, "Error sending Accept\n");
				usbpd_set_state(pd, PE_SRC_SEND_SOFT_RESET);
				break;
			}

			dr_swap(pd);
		} else if (IS_CTRL(rx_msg, MSG_PR_SWAP)) {
			/* lock in current mode */
			set_power_role(pd, pd->current_pr);

			/* TODO: should we Reject in certain circumstances? */
			ret = pd_send_msg(pd, MSG_ACCEPT, NULL, 0, SOP_MSG);
			if (ret) {
				usbpd_err(&pd->dev, "Error sending Accept\n");
				usbpd_set_state(pd, PE_SNK_SEND_SOFT_RESET);
				break;
			}

			pd->in_pr_swap = true;
			val.intval = 1;
			power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PR_SWAP, &val);
			usbpd_set_state(pd, PE_PRS_SNK_SRC_TRANSITION_TO_OFF);
			break;
		} else if (IS_CTRL(rx_msg, MSG_VCONN_SWAP)) {
			/*
			 * if VCONN is connected to VBUS, make sure we are
			 * not in high voltage contract, otherwise reject.
			 */
			if (!pd->vconn_is_external &&
					(pd->requested_voltage > 5000000)) {
				ret = pd_send_msg(pd, MSG_REJECT, NULL, 0,
						SOP_MSG);
				if (ret) {
					usbpd_err(&pd->dev, "Error sending Reject\n");
					usbpd_set_state(pd,
							PE_SNK_SEND_SOFT_RESET);
				}

				break;
			}

			ret = pd_send_msg(pd, MSG_ACCEPT, NULL, 0, SOP_MSG);
			if (ret) {
				usbpd_err(&pd->dev, "Error sending Accept\n");
				usbpd_set_state(pd, PE_SNK_SEND_SOFT_RESET);
				break;
			}

			vconn_swap(pd);
		} else if (IS_DATA(rx_msg, MSG_VDM)) {
			handle_vdm_rx(pd, rx_msg);
		} else if (pd->send_request) {
			pd->send_request = false;
			usbpd_set_state(pd, PE_SNK_SELECT_CAPABILITY);
		} else if (pd->send_pr_swap && is_sink_tx_ok(pd)) {
			pd->send_pr_swap = false;
			ret = pd_send_msg(pd, MSG_PR_SWAP, NULL, 0, SOP_MSG);
			if (ret) {
				dev_err(&pd->dev, "Error sending PR Swap\n");
				usbpd_set_state(pd, PE_SNK_SEND_SOFT_RESET);
				break;
			}

			pd->current_state = PE_PRS_SNK_SRC_SEND_SWAP;
			kick_sm(pd, SENDER_RESPONSE_TIME);
		} else if (pd->send_dr_swap && is_sink_tx_ok(pd)) {
			pd->send_dr_swap = false;
			ret = pd_send_msg(pd, MSG_DR_SWAP, NULL, 0, SOP_MSG);
			if (ret) {
				dev_err(&pd->dev, "Error sending DR Swap\n");
				usbpd_set_state(pd, PE_SNK_SEND_SOFT_RESET);
				break;
			}

			pd->current_state = PE_DRS_SEND_DR_SWAP;
			kick_sm(pd, SENDER_RESPONSE_TIME);
		} else if (is_sink_tx_ok(pd)) {
#ifdef CONFIG_LGE_USB
			if (pd->vdm_tx) {
				handle_vdm_tx(pd);
				kick_sm(pd, SENDER_RESPONSE_TIME);
			}
#else
			handle_vdm_tx(pd);
#endif
		}
#ifdef CONFIG_LGE_USB
		if (!pd->sm_queued && pd->vdm_tx)
			kick_sm(pd, SENDER_RESPONSE_TIME);
#endif
		break;

	case PE_SNK_TRANSITION_TO_DEFAULT:
		usbpd_set_state(pd, PE_SNK_STARTUP);
		break;

	case PE_SRC_SOFT_RESET:
	case PE_SNK_SOFT_RESET:
		pd_reset_protocol(pd);

		ret = pd_send_msg(pd, MSG_ACCEPT, NULL, 0, SOP_MSG);
		if (ret) {
			usbpd_err(&pd->dev, "%s: Error sending Accept, do Hard Reset\n",
					usbpd_state_strings[pd->current_state]);
			usbpd_set_state(pd, pd->current_pr == PR_SRC ?
					PE_SRC_HARD_RESET : PE_SNK_HARD_RESET);
			break;
		}

		usbpd_set_state(pd, pd->current_pr == PR_SRC ?
				PE_SRC_SEND_CAPABILITIES :
				PE_SNK_WAIT_FOR_CAPABILITIES);
		break;

	case PE_SRC_SEND_SOFT_RESET:
	case PE_SNK_SEND_SOFT_RESET:
		if (IS_CTRL(rx_msg, MSG_ACCEPT)) {
			usbpd_set_state(pd, pd->current_pr == PR_SRC ?
					PE_SRC_SEND_CAPABILITIES :
					PE_SNK_WAIT_FOR_CAPABILITIES);
		} else {
			usbpd_err(&pd->dev, "%s: Did not see Accept, do Hard Reset\n",
					usbpd_state_strings[pd->current_state]);
			usbpd_set_state(pd, pd->current_pr == PR_SRC ?
					PE_SRC_HARD_RESET : PE_SNK_HARD_RESET);
		}
		break;

	case PE_SNK_HARD_RESET:
		/* prepare charger for VBUS change */
		val.intval = 1;
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PD_IN_HARD_RESET, &val);

		pd->requested_voltage = 5000000;

		if (pd->requested_current) {
			val.intval = pd->requested_current = 0;
			power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PD_CURRENT_MAX, &val);
		}

		val.intval = pd->requested_voltage;
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PD_VOLTAGE_MIN, &val);

		pd_send_hard_reset(pd);
		pd->in_explicit_contract = false;
		pd->selected_pdo = pd->requested_pdo = 0;
		pd->rdo = 0;
		reset_vdm_state(pd);
		kobject_uevent(&pd->dev.kobj, KOBJ_CHANGE);
		usbpd_set_state(pd, PE_SNK_TRANSITION_TO_DEFAULT);
		break;

	case PE_DRS_SEND_DR_SWAP:
		if (IS_CTRL(rx_msg, MSG_ACCEPT))
			dr_swap(pd);

		usbpd_set_state(pd, pd->current_pr == PR_SRC ?
				PE_SRC_READY : PE_SNK_READY);
		break;

	case PE_PRS_SRC_SNK_SEND_SWAP:
		if (!IS_CTRL(rx_msg, MSG_ACCEPT)) {
			pd->current_state = PE_SRC_READY;
			break;
		}

		pd->current_state = PE_PRS_SRC_SNK_TRANSITION_TO_OFF;
		kick_sm(pd, SRC_TRANSITION_TIME);
		break;

	case PE_PRS_SRC_SNK_TRANSITION_TO_OFF:
		pd->in_pr_swap = true;
		val.intval = 1;
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PR_SWAP, &val);
		pd->in_explicit_contract = false;

		if (pd->vbus_enabled) {
			regulator_disable(pd->vbus);
			pd->vbus_enabled = false;
		}

		/* PE_PRS_SRC_SNK_Assert_Rd */
		pd->current_pr = PR_SINK;
		set_power_role(pd, pd->current_pr);
		pd_phy_update_roles(pd->current_dr, pd->current_pr);

		/* allow time for Vbus discharge, must be < tSrcSwapStdby */
		msleep(500);

		ret = pd_send_msg(pd, MSG_PS_RDY, NULL, 0, SOP_MSG);
		if (ret) {
			usbpd_err(&pd->dev, "Error sending PS_RDY\n");
			usbpd_set_state(pd, PE_ERROR_RECOVERY);
			break;
		}

		pd->current_state = PE_PRS_SRC_SNK_WAIT_SOURCE_ON;
		kick_sm(pd, PS_SOURCE_ON);
		break;

	case PE_PRS_SRC_SNK_WAIT_SOURCE_ON:
		if (IS_CTRL(rx_msg, MSG_PS_RDY))
			usbpd_set_state(pd, PE_SNK_STARTUP);
		else
			usbpd_set_state(pd, PE_ERROR_RECOVERY);
		break;

	case PE_PRS_SNK_SRC_SEND_SWAP:
		if (!IS_CTRL(rx_msg, MSG_ACCEPT)) {
			pd->current_state = PE_SNK_READY;
			break;
		}

		pd->in_pr_swap = true;
		val.intval = 1;
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PR_SWAP, &val);
		usbpd_set_state(pd, PE_PRS_SNK_SRC_TRANSITION_TO_OFF);
		break;

	case PE_PRS_SNK_SRC_TRANSITION_TO_OFF:
		if (!IS_CTRL(rx_msg, MSG_PS_RDY)) {
			usbpd_set_state(pd, PE_ERROR_RECOVERY);
			break;
		}

		/* PE_PRS_SNK_SRC_Assert_Rp */
		pd->current_pr = PR_SRC;
		set_power_role(pd, pd->current_pr);
		pd->current_state = PE_PRS_SNK_SRC_SOURCE_ON;

		/* fall-through */

	case PE_PRS_SNK_SRC_SOURCE_ON:
		enable_vbus(pd);
#ifdef CONFIG_LGE_USB
		msleep(100); /* allow time VBUS ramp-up, must be < tNewSrc */
#else
		msleep(200); /* allow time VBUS ramp-up, must be < tNewSrc */
#endif

		ret = pd_send_msg(pd, MSG_PS_RDY, NULL, 0, SOP_MSG);
		if (ret) {
			usbpd_err(&pd->dev, "Error sending PS_RDY\n");
			usbpd_set_state(pd, PE_ERROR_RECOVERY);
			break;
		}

		usbpd_set_state(pd, PE_SRC_STARTUP);
		break;

	case PE_VCS_WAIT_FOR_VCONN:
		if (IS_CTRL(rx_msg, MSG_PS_RDY)) {
			/*
			 * hopefully redundant check but in case not enabled
			 * avoids unbalanced regulator disable count
			 */
			if (pd->vconn_enabled)
				regulator_disable(pd->vconn);
			pd->vconn_enabled = false;

			pd->current_state = pd->current_pr == PR_SRC ?
				PE_SRC_READY : PE_SNK_READY;
		} else {
			/* timed out; go to hard reset */
			usbpd_set_state(pd, pd->current_pr == PR_SRC ?
					PE_SRC_HARD_RESET : PE_SNK_HARD_RESET);
		}

		break;

	default:
		usbpd_err(&pd->dev, "Unhandled state %s\n",
				usbpd_state_strings[pd->current_state]);
		break;
	}

sm_done:
	kfree(rx_msg);

#ifdef CONFIG_LGE_USB
	if (!pd->sm_queued) {
		spin_lock_irqsave(&pd->rx_lock, flags);
		if (!list_empty(&pd->rx_q))
			kick_sm(pd, 0);
		spin_unlock_irqrestore(&pd->rx_lock, flags);
	}
#endif
	spin_lock_irqsave(&pd->rx_lock, flags);
	ret = list_empty(&pd->rx_q);
	spin_unlock_irqrestore(&pd->rx_lock, flags);

	/* requeue if there are any new/pending RX messages */
	if (!ret)
		kick_sm(pd, 0);

	if (!pd->sm_queued)
#ifdef CONFIG_LGE_USB
		pm_wakeup_event(&pd->dev, 2000);
#else
		pm_relax(&pd->dev);
#endif
}

static inline const char *src_current(enum power_supply_typec_mode typec_mode)
{
	switch (typec_mode) {
	case POWER_SUPPLY_TYPEC_SOURCE_DEFAULT:
		return "default";
	case POWER_SUPPLY_TYPEC_SOURCE_MEDIUM:
		return "medium - 1.5A";
	case POWER_SUPPLY_TYPEC_SOURCE_HIGH:
		return "high - 3.0A";
	default:
		return "";
	}
}

static inline const char* typec_to_string(int mode){
	switch (mode){
	case POWER_SUPPLY_TYPEC_NONE:
		return "TYPEC_NONE";
	case POWER_SUPPLY_TYPEC_SINK:
		return "TYPEC_SINK(Rd Only)";
	case POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE:
		return "TYPEC_SINK_POWERED_CABLE(Rd/Ra)";
	case POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY:
		return "TYPEC_SINK_DEBUG_ACCESSORY(Rd/Rd)";
	case POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER:
		return "TYPEC_SINK_AUDIO_ADAPTER(Ra/Ra)";
	case POWER_SUPPLY_TYPEC_POWERED_CABLE_ONLY:
		return "TYPEC_POWERED_CABLE_ONLY(Ra Only)";
	case POWER_SUPPLY_TYPEC_SOURCE_DEFAULT:
		return "TYPEC_SOURCE_DEFAULT(Rp56k)";
	case POWER_SUPPLY_TYPEC_SOURCE_MEDIUM:
		return "TYPEC_SOURCE_MEDIUM(Rp22k)";
	case POWER_SUPPLY_TYPEC_SOURCE_HIGH:
		return "TYPEC_SOURCE_HIGH(Rp10k)";
	case POWER_SUPPLY_TYPEC_NON_COMPLIANT:
		return "TYPEC_NON_COMPLIANT";
	default:
		return "Unknown mode";
	}
}

static int psy_changed(struct notifier_block *nb, unsigned long evt, void *ptr)
{
	struct usbpd *pd = container_of(nb, struct usbpd, psy_nb);
	union power_supply_propval val;
	enum power_supply_typec_mode typec_mode;
	int ret;
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
	int vbus_present;
#endif

	if (ptr != pd->usb_psy || evt != PSY_EVENT_PROP_CHANGED)
		return 0;

	ret = power_supply_get_property(pd->usb_psy,
			POWER_SUPPLY_PROP_TYPEC_MODE, &val);
	if (ret) {
		usbpd_err(&pd->dev, "Unable to read USB TYPEC_MODE: %d\n", ret);
		return ret;
	}

	typec_mode = val.intval;

	ret = power_supply_get_property(pd->usb_psy,
			POWER_SUPPLY_PROP_PE_START, &val);
	if (ret) {
		usbpd_err(&pd->dev, "Unable to read USB PROP_PE_START: %d\n",
				ret);
		return ret;
	}

	/* Don't proceed if PE_START=0 as other props may still change */
	if (!val.intval && !pd->pd_connected &&
			typec_mode != POWER_SUPPLY_TYPEC_NONE)
		return 0;

	ret = power_supply_get_property(pd->usb_psy,
			POWER_SUPPLY_PROP_PRESENT, &val);
	if (ret) {
		usbpd_err(&pd->dev, "Unable to read USB PRESENT: %d\n", ret);
		return ret;
	}

	pd->vbus_present = val.intval;
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
	if(!pd->vbus_present)
		vbus_present = 0;
#endif

	ret = power_supply_get_property(pd->usb_psy,
			POWER_SUPPLY_PROP_REAL_TYPE, &val);
	if (ret) {
		usbpd_err(&pd->dev, "Unable to read USB TYPE: %d\n", ret);
		return ret;
	}

	pd->psy_type = val.intval;

	/*
	 * For sink hard reset, state machine needs to know when VBUS changes
	 *   - when in PE_SNK_TRANSITION_TO_DEFAULT, notify when VBUS falls
	 *   - when in PE_SNK_DISCOVERY, notify when VBUS rises
	 */
	if (typec_mode && ((!pd->vbus_present &&
			pd->current_state == PE_SNK_TRANSITION_TO_DEFAULT) ||
		(pd->vbus_present && pd->current_state == PE_SNK_DISCOVERY))) {
		usbpd_dbg(&pd->dev, "hard reset: typec mode:%d present:%d\n",
			typec_mode, pd->vbus_present);
		pd->typec_mode = typec_mode;
		kick_sm(pd, 0);
		return 0;
	}

	usbpd_info(&pd->dev,"pd->typec_mode=%d typec_mode=%d\n",pd->typec_mode, typec_mode);
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
	if (pd->adc_initialized) {
		if (pd->sbu_sel && !pd->sbu_moisture) {
			if (pd->sbu_run_work) {
				pd->sbu_run_work = false;
				pm_relax(&pd->dev);
			}
			cancel_delayed_work(&pd->sbu_adc_work);
			cancel_delayed_work(&pd->init_sbu_adc_work);
			pd->sbu_lock = true;
			usbpd_dbg(&pd->dev, "[moisture] pd->in_pr_swap: %d, pd->current_state: %d\n",
					pd->in_pr_swap, pd->current_state);
			if (!pd->vbus_present && typec_mode == POWER_SUPPLY_TYPEC_NONE && !pd->in_pr_swap &&
				!(pd->current_state == PE_ERROR_RECOVERY || pd->current_state == PE_FORCED_PR_SWAP)) {
				schedule_delayed_work(&pd->init_sbu_adc_work, (1500*HZ/1000));
			} else {
				usbpd_dbg(&pd->dev, "[moisture] sbu switch off\n");
				gpiod_direction_output(pd->sbu_sel, 0);
				if (lge_get_boot_mode() == LGE_BOOT_MODE_CHARGERLOGO &&
						!vbus_present && pd->vbus_present && pd->current_dr != DR_DFP) {
					schedule_delayed_work(&pd->sbu_ov_adc_work, msecs_to_jiffies(1000));
					vbus_present = pd->vbus_present;
				}
			}
		}
		if (pd->edge_sel && !pd->edge_moisture) {
			if (pd->edge_run_work) {
				pd->edge_run_work = false;
				pm_relax(&pd->dev);
			}
			cancel_delayed_work(&pd->edge_adc_work);
			cancel_delayed_work(&pd->init_edge_adc_work);
			pd->edge_lock = true;
			if (!pd->vbus_present && typec_mode == POWER_SUPPLY_TYPEC_NONE && !pd->in_pr_swap &&
				!(pd->current_state == PE_ERROR_RECOVERY || pd->current_state == PE_FORCED_PR_SWAP))
				schedule_delayed_work(&pd->init_edge_adc_work, (1500*HZ/1000));
		}

		if (pd->edge_sel && pd->edge_moisture && !pd->vbus_present) {
			if (pd->edge_run_work) {
				pd->edge_run_work = false;
				pm_relax(&pd->dev);
			}
			cancel_delayed_work(&pd->edge_adc_work);
			schedule_delayed_work(&pd->edge_adc_work, 0);
		}
		if (pd->sbu_sel && pd->sbu_moisture && !pd->vbus_present) {
			if (pd->sbu_run_work) {
				pd->sbu_run_work = false;
				pm_relax(&pd->dev);
			}
			cancel_delayed_work(&pd->sbu_adc_work);
			schedule_delayed_work(&pd->sbu_adc_work, 0);
		}
	}

	if (pd->sbu_moisture) {
		usbpd_info(&pd->dev, "[moisture] moisture is detected, skip set power role\n");
		typec_mode = POWER_SUPPLY_TYPEC_NONE;
		pd->current_pr = PR_NONE;
		pd->edge_mtime = pd->sbu_mtime = CURRENT_TIME;
		return 0;
	}
#endif

	if (pd->typec_mode == typec_mode) {
#ifdef CONFIG_LGE_USB
		if (pd->in_pr_swap) {
			return 0;
#if defined(CONFIG_LGE_USB_DEBUGGER) || defined(CONFIG_LGE_USB_FACTORY)
		} else if (typec_mode == POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY) {//only debug accessory cable (pif cable, usb debugger)
			if (!pd->vbus_present) {		//case of vbus remove only
				usbpd_info(&pd->dev,"->TYPEC_NONE - VBUS OFF ONLY\n");
				pd->current_pr = PR_NONE;
				kick_sm(pd, 0);
				return 0;
			} else {
				usbpd_info(&pd->dev,"->PR_SINK !! - VBUS ON ONLY\n");
				pd->psy_type = POWER_SUPPLY_TYPE_USB;
				pd->current_pr = PR_SINK;
				pd->in_pr_swap = false;
				kick_sm(pd, 0);
				return 0;
			}
#endif
		} else if (typec_mode == POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE ||
		    typec_mode == POWER_SUPPLY_TYPEC_SINK) {
			if (pd->current_pr == PR_SINK) {
				if (!pd->vbus_present)
				pd->current_pr = PR_NONE;
				kick_sm(pd, 0);
				return 0;
			}
		} else if (typec_mode == POWER_SUPPLY_TYPEC_NONE) {
			if (pd->vbus_present) {
				if (pd->psy_type == POWER_SUPPLY_TYPE_USB ||
				    pd->psy_type == POWER_SUPPLY_TYPE_USB_CDP) {
					if (pd->current_pr == PR_SINK)
						return 0;

					pd->current_pr = PR_SINK;
					kick_sm(pd, 0);
					return 0;
				}
			} else {
				if (pd->current_pr == PR_SINK) {
					pd->current_pr = PR_NONE;
					kick_sm(pd, 0);
					return 0;
				}
			}
		}
#endif
		return 0;
	}

	pd->typec_mode = typec_mode;
	usbpd_info(&pd->dev, "typec_mode = %s  present:%d, type:%d, orientation:%d\n",
			typec_to_string(typec_mode), pd->vbus_present, pd->psy_type,
			usbpd_get_plug_orientation(pd));

	switch (typec_mode) {
	/* Disconnect */
	case POWER_SUPPLY_TYPEC_NONE:
#ifdef CONFIG_LGE_USB
		if (pd->current_state == PE_FORCED_PR_SWAP) {
			usbpd_dbg(&pd->dev, "Ignoring disconnect due to forced PR swap\n");
			return 0;
		}
#endif
		if (pd->in_pr_swap) {
			usbpd_dbg(&pd->dev, "Ignoring disconnect due to PR swap\n");
			return 0;
		}
		usbpd_info(&pd->dev,"TYPEC_NONE - DISCONNECT CABLE\n");
#ifdef CONFIG_LGE_USB_DEBUGGER
		if(pd->is_debug_accessory) {
			pd->is_debug_accessory = false;
			schedule_work(&pd->usb_debugger_work);
		}
#endif
		pd->current_pr = PR_NONE;
		break;

	/* Sink states */
	case POWER_SUPPLY_TYPEC_SOURCE_DEFAULT:
	case POWER_SUPPLY_TYPEC_SOURCE_MEDIUM:
	case POWER_SUPPLY_TYPEC_SOURCE_HIGH:
		usbpd_info(&pd->dev, "Type-C Source (%s) connected\n",
				src_current(typec_mode));

		/* if waiting for SinkTxOk to start an AMS */
		if (pd->spec_rev == USBPD_REV_30 &&
			typec_mode == POWER_SUPPLY_TYPEC_SOURCE_HIGH &&
			(pd->send_pr_swap || pd->send_dr_swap || pd->vdm_tx))
			break;

		if (pd->current_pr == PR_SINK)
			return 0;

		/*
		 * Unexpected if not in PR swap; need to force disconnect from
		 * source so we can turn off VBUS, Vconn, PD PHY etc.
		 */
		if (pd->current_pr == PR_SRC) {
			usbpd_info(&pd->dev, "Forcing disconnect from source mode\n");
			pd->current_pr = PR_NONE;
			break;
		}

		pd->current_pr = PR_SINK;
		break;

	/* Source states */
	case POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE:
	case POWER_SUPPLY_TYPEC_SINK:
#ifdef CONFIG_LGE_USB
		if (pd->vbus_present) {
			usbpd_info(&pd->dev, "Type-C Sink%s connected with VBUS\n",
				   typec_mode == POWER_SUPPLY_TYPEC_SINK ?
				   "" : " (powered)");

			if (pd->current_pr == PR_SINK)
				return 0;

			pd->current_pr = PR_SINK;
			break;
		}
#endif

		usbpd_info(&pd->dev, "Type-C Sink%s connected\n",
				typec_mode == POWER_SUPPLY_TYPEC_SINK ?
					"" : " (powered)");

		if (pd->current_pr == PR_SRC)
			return 0;

		pd->current_pr = PR_SRC;
		break;

	case POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY:
		usbpd_info(&pd->dev, "Type-C Debug Accessory connected\n");
#ifdef CONFIG_LGE_USB_DEBUGGER
		pd->is_debug_accessory = true;
		schedule_work(&pd->usb_debugger_work);
#endif
#ifdef CONFIG_LGE_USB_FACTORY
		usbpd_info(&pd->dev,"pd->vbus_present:%d\n",pd->vbus_present);
		if(pd->vbus_present){
			pd->psy_type = POWER_SUPPLY_TYPE_USB;
			pd->current_pr = PR_SINK;
			pd->in_pr_swap = false;
		}
#endif
		break;
	case POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER:
		usbpd_info(&pd->dev, "Type-C Analog Audio Adapter connected\n");
		break;
	default:
		usbpd_warn(&pd->dev, "Unsupported typec mode:%d\n",
				typec_mode);
		break;
	}

	/* queue state machine due to CC state change */
	kick_sm(pd, 0);
	return 0;
}

static enum dual_role_property usbpd_dr_properties[] = {
	DUAL_ROLE_PROP_SUPPORTED_MODES,
	DUAL_ROLE_PROP_MODE,
	DUAL_ROLE_PROP_PR,
	DUAL_ROLE_PROP_DR,
#ifdef CONFIG_LGE_USB
	DUAL_ROLE_PROP_VCONN_SUPPLY,
	DUAL_ROLE_PROP_CC1,
	DUAL_ROLE_PROP_CC2,
	DUAL_ROLE_PROP_PDO1,
	DUAL_ROLE_PROP_PDO2,
	DUAL_ROLE_PROP_PDO3,
	DUAL_ROLE_PROP_PDO4,
	DUAL_ROLE_PROP_PDO5,
	DUAL_ROLE_PROP_PDO6,
	DUAL_ROLE_PROP_PDO7,
	DUAL_ROLE_PROP_RDO,
#endif
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
	DUAL_ROLE_PROP_MOISTURE_EN,
	DUAL_ROLE_PROP_MOISTURE,
#endif
};

static int usbpd_dr_get_property(struct dual_role_phy_instance *dual_role,
		enum dual_role_property prop, unsigned int *val)
{
	struct usbpd *pd = dual_role_get_drvdata(dual_role);

	if (!pd)
		return -ENODEV;

	switch (prop) {
	case DUAL_ROLE_PROP_MODE:
		/* For now associate UFP/DFP with data role only */
#ifdef CONFIG_LGE_USB
		if (pd->current_state == PE_FORCED_PR_SWAP)
			*val = DUAL_ROLE_PROP_MODE_NONE;
		else
#endif
		if (pd->current_dr == DR_UFP)
			*val = DUAL_ROLE_PROP_MODE_UFP;
		else if (pd->current_dr == DR_DFP)
			*val = DUAL_ROLE_PROP_MODE_DFP;
		else
			*val = DUAL_ROLE_PROP_MODE_NONE;
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
		if (pd->sbu_moisture)
			*val = DUAL_ROLE_PROP_MODE_FAULT;
#endif
		break;
	case DUAL_ROLE_PROP_PR:
#ifdef CONFIG_LGE_USB
		if (pd->current_state == PE_FORCED_PR_SWAP)
			*val = DUAL_ROLE_PROP_PR_NONE;
		else
#endif
		if (pd->current_pr == PR_SRC)
			*val = DUAL_ROLE_PROP_PR_SRC;
		else if (pd->current_pr == PR_SINK)
			*val = DUAL_ROLE_PROP_PR_SNK;
		else
			*val = DUAL_ROLE_PROP_PR_NONE;
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
		if (pd->sbu_moisture)
			*val = DUAL_ROLE_PROP_PR_FAULT;
#endif
		break;
	case DUAL_ROLE_PROP_DR:
#ifdef CONFIG_LGE_USB
		if (pd->current_state == PE_FORCED_PR_SWAP)
			*val = DUAL_ROLE_PROP_DR_NONE;
		else
#endif
		if (pd->current_dr == DR_UFP)
			*val = DUAL_ROLE_PROP_DR_DEVICE;
		else if (pd->current_dr == DR_DFP)
			*val = DUAL_ROLE_PROP_DR_HOST;
		else
			*val = DUAL_ROLE_PROP_DR_NONE;
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
		if (pd->sbu_moisture)
			*val = DUAL_ROLE_PROP_DR_FAULT;
#endif
		break;
#ifdef CONFIG_LGE_USB
	case DUAL_ROLE_PROP_VCONN_SUPPLY:
		if (pd->vconn_enabled)
			*val = DUAL_ROLE_PROP_VCONN_SUPPLY_YES;
		else
			*val = DUAL_ROLE_PROP_VCONN_SUPPLY_NO;
		break;
	case DUAL_ROLE_PROP_CC1:
	case DUAL_ROLE_PROP_CC2:
		switch (pd->typec_mode) {
		case POWER_SUPPLY_TYPEC_SINK:
			if ((usbpd_get_plug_orientation(pd) - ORIENTATION_CC1) ==
			    (prop - DUAL_ROLE_PROP_CC1))
				*val = DUAL_ROLE_PROP_CC_RD;
			else
				*val = DUAL_ROLE_PROP_CC_OPEN;
			break;
		case POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE:
			if ((usbpd_get_plug_orientation(pd) - ORIENTATION_CC1) ==
			    (prop - DUAL_ROLE_PROP_CC1))
				*val = DUAL_ROLE_PROP_CC_RD;
			else
				*val = DUAL_ROLE_PROP_CC_RA;
			break;
		case POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY:
			*val = DUAL_ROLE_PROP_CC_RD;
			break;
		case POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER:
			*val = DUAL_ROLE_PROP_CC_RA;
			break;
		case POWER_SUPPLY_TYPEC_POWERED_CABLE_ONLY:
			if ((usbpd_get_plug_orientation(pd) - ORIENTATION_CC1) ==
			    (prop - DUAL_ROLE_PROP_CC1))
				*val = DUAL_ROLE_PROP_CC_RA;
			else
				*val = DUAL_ROLE_PROP_CC_OPEN;
			break;
		case POWER_SUPPLY_TYPEC_SOURCE_DEFAULT:
		case POWER_SUPPLY_TYPEC_SOURCE_MEDIUM:
		case POWER_SUPPLY_TYPEC_SOURCE_HIGH:
			if ((usbpd_get_plug_orientation(pd) - ORIENTATION_CC1) ==
			    (prop - DUAL_ROLE_PROP_CC1))
				*val = DUAL_ROLE_PROP_CC_RP_DEFAULT +
					(pd->typec_mode - POWER_SUPPLY_TYPEC_SOURCE_DEFAULT);
			else
				*val = DUAL_ROLE_PROP_CC_OPEN;

			break;
		case POWER_SUPPLY_TYPEC_NONE:
		case POWER_SUPPLY_TYPEC_NON_COMPLIANT:
		default:
			*val = DUAL_ROLE_PROP_CC_OPEN;
			break;
		}
		break;
	case DUAL_ROLE_PROP_PDO1:
	case DUAL_ROLE_PROP_PDO2:
	case DUAL_ROLE_PROP_PDO3:
	case DUAL_ROLE_PROP_PDO4:
	case DUAL_ROLE_PROP_PDO5:
	case DUAL_ROLE_PROP_PDO6:
	case DUAL_ROLE_PROP_PDO7:
		if (pd->current_state == PE_FORCED_PR_SWAP) {
			*val = 0;
			break;
		}
		switch (pd->current_pr) {
		case PR_SRC:
			if (ARRAY_SIZE(default_src_caps) > (prop - DUAL_ROLE_PROP_PDO1))
				*val = default_src_caps[prop - DUAL_ROLE_PROP_PDO1];
			else
				*val = 0;
			break;
		case PR_SINK:
			*val = pd->received_pdos[prop - DUAL_ROLE_PROP_PDO1];
			break;
		default:
			*val = 0;
			break;
		}
		break;
	case DUAL_ROLE_PROP_RDO:
		if (pd->current_pr == PR_SRC || pd->current_pr == PR_SINK)
			*val = pd->rdo;
		else
			*val = 0;
		break;
#endif
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
	case DUAL_ROLE_PROP_MOISTURE_EN:
		*val = pd->prop_moisture_en;
		break;
	case DUAL_ROLE_PROP_MOISTURE:
		*val = pd->prop_moisture;
		break;
#endif
	default:
		usbpd_warn(&pd->dev, "unsupported property %d\n", prop);
		return -ENODATA;
	}

	return 0;
}

static int usbpd_dr_set_property(struct dual_role_phy_instance *dual_role,
		enum dual_role_property prop, const unsigned int *val)
{
	struct usbpd *pd = dual_role_get_drvdata(dual_role);
	bool do_swap = false;
	int wait_count = 5;

	if (!pd)
		return -ENODEV;

	switch (prop) {
	case DUAL_ROLE_PROP_MODE:
		usbpd_dbg(&pd->dev, "Setting mode to %d\n", *val);

		if (pd->current_state == PE_UNKNOWN) {
			usbpd_warn(&pd->dev, "No active connection. Don't allow MODE change\n");
			return -EAGAIN;
		}

		/*
		 * Forces disconnect on CC and re-establishes connection.
		 * This does not use PD-based PR/DR swap
		 */
		if (*val == DUAL_ROLE_PROP_MODE_UFP)
			pd->forced_pr = POWER_SUPPLY_TYPEC_PR_SINK;
		else if (*val == DUAL_ROLE_PROP_MODE_DFP)
			pd->forced_pr = POWER_SUPPLY_TYPEC_PR_SOURCE;

#ifdef CONFIG_LGE_USB
		usbpd_set_state(pd, PE_FORCED_PR_SWAP);
#else
		/* new mode will be applied in disconnect handler */
		set_power_role(pd, PR_NONE);

		/* wait until it takes effect */
		while (pd->forced_pr != POWER_SUPPLY_TYPEC_PR_NONE &&
							--wait_count)
			msleep(20);
#endif

		if (!wait_count) {
			usbpd_err(&pd->dev, "setting mode timed out\n");
			return -ETIMEDOUT;
		}

		break;

	case DUAL_ROLE_PROP_DR:
		usbpd_dbg(&pd->dev, "Setting data_role to %d\n", *val);

		if (*val == DUAL_ROLE_PROP_DR_HOST) {
			if (pd->current_dr == DR_UFP)
				do_swap = true;
		} else if (*val == DUAL_ROLE_PROP_DR_DEVICE) {
			if (pd->current_dr == DR_DFP)
				do_swap = true;
		} else {
			usbpd_warn(&pd->dev, "setting data_role to 'none' unsupported\n");
			return -ENOTSUPP;
		}

		if (do_swap) {
#ifdef CONFIG_LGE_USB
			if (*val == DUAL_ROLE_PROP_DR_HOST)
				pd->forced_pr = POWER_SUPPLY_TYPEC_PR_SOURCE;
			else if (*val == DUAL_ROLE_PROP_DR_DEVICE)
				pd->forced_pr = POWER_SUPPLY_TYPEC_PR_SINK;

			usbpd_set_state(pd, PE_FORCED_PR_SWAP);
#else
			if (pd->current_state != PE_SRC_READY &&
					pd->current_state != PE_SNK_READY) {
				usbpd_err(&pd->dev, "data_role swap not allowed: PD not in Ready state\n");
				return -EAGAIN;
			}

			if (pd->current_state == PE_SNK_READY &&
					!is_sink_tx_ok(pd)) {
				usbpd_err(&pd->dev, "Rp indicates SinkTxNG\n");
				return -EAGAIN;
			}

			mutex_lock(&pd->swap_lock);
			reinit_completion(&pd->is_ready);
			pd->send_dr_swap = true;
			kick_sm(pd, 0);

			/* wait for operation to complete */
			if (!wait_for_completion_timeout(&pd->is_ready,
					msecs_to_jiffies(100))) {
				usbpd_err(&pd->dev, "data_role swap timed out\n");
				mutex_unlock(&pd->swap_lock);
				return -ETIMEDOUT;
			}

			mutex_unlock(&pd->swap_lock);

			if ((*val == DUAL_ROLE_PROP_DR_HOST &&
					pd->current_dr != DR_DFP) ||
				(*val == DUAL_ROLE_PROP_DR_DEVICE &&
					 pd->current_dr != DR_UFP)) {
				usbpd_err(&pd->dev, "incorrect state (%s) after data_role swap\n",
						pd->current_dr == DR_DFP ?
						"dfp" : "ufp");
				return -EPROTO;
			}
#endif
		}

		break;

	case DUAL_ROLE_PROP_PR:
		usbpd_dbg(&pd->dev, "Setting power_role to %d\n", *val);

		if (*val == DUAL_ROLE_PROP_PR_SRC) {
			if (pd->current_pr == PR_SINK)
				do_swap = true;
		} else if (*val == DUAL_ROLE_PROP_PR_SNK) {
			if (pd->current_pr == PR_SRC)
				do_swap = true;
		} else {
			usbpd_warn(&pd->dev, "setting power_role to 'none' unsupported\n");
			return -ENOTSUPP;
		}

		if (do_swap) {
#ifdef CONFIG_LGE_USB
			if (*val == DUAL_ROLE_PROP_PR_SRC)
				pd->forced_pr = POWER_SUPPLY_TYPEC_PR_SOURCE;
			else if (*val == DUAL_ROLE_PROP_PR_SNK)
				pd->forced_pr = POWER_SUPPLY_TYPEC_PR_SINK;

			usbpd_set_state(pd, PE_FORCED_PR_SWAP);
#else
			if (pd->current_state != PE_SRC_READY &&
					pd->current_state != PE_SNK_READY) {
				usbpd_err(&pd->dev, "power_role swap not allowed: PD not in Ready state\n");
				return -EAGAIN;
			}

			if (pd->current_state == PE_SNK_READY &&
					!is_sink_tx_ok(pd)) {
				usbpd_err(&pd->dev, "Rp indicates SinkTxNG\n");
				return -EAGAIN;
			}

			mutex_lock(&pd->swap_lock);
			reinit_completion(&pd->is_ready);
			pd->send_pr_swap = true;
			kick_sm(pd, 0);

			/* wait for operation to complete */
			if (!wait_for_completion_timeout(&pd->is_ready,
					msecs_to_jiffies(2000))) {
				usbpd_err(&pd->dev, "power_role swap timed out\n");
				mutex_unlock(&pd->swap_lock);
				return -ETIMEDOUT;
			}

			mutex_unlock(&pd->swap_lock);

			if ((*val == DUAL_ROLE_PROP_PR_SRC &&
					pd->current_pr != PR_SRC) ||
				(*val == DUAL_ROLE_PROP_PR_SNK &&
					 pd->current_pr != PR_SINK)) {
				usbpd_err(&pd->dev, "incorrect state (%s) after power_role swap\n",
						pd->current_pr == PR_SRC ?
						"source" : "sink");
				return -EPROTO;
			}
#endif
		}
		break;

#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
	case DUAL_ROLE_PROP_MOISTURE_EN:
		mutex_lock(&pd->moisture_lock);
		if (!pd->adc_initialized) {
			mutex_unlock(&pd->moisture_lock);
			break;
		}
		if (*val == pd->prop_moisture_en) {
			mutex_unlock(&pd->moisture_lock);
			break;
		} else
			pd->prop_moisture_en = *val;

		if (*val == DUAL_ROLE_PROP_MOISTURE_EN_DISABLE) {
			usbpd_info(&pd->dev, "[moisture] %s: disable moisture detection\n", __func__);
			if (pd->edge_sel){
				pd->edge_moisture = 0;
				gpiod_direction_output(pd->sbu_oe, 0); //sbu oe enable
				if (pd->edge_run_work) {
					pd->edge_run_work = false;
					pm_relax(&pd->dev);
				}
				cancel_delayed_work(&pd->edge_adc_work);
				pd->edge_adc_state = ADC_STATE_DRY;
				qpnp_adc_tm_disable_chan_meas(pd->adc_tm_dev, &pd->edge_adc_param);
			}
			if (pd->sbu_sel){
				pd->sbu_moisture = 0;
				if (pd->sbu_run_work) {
					pd->sbu_run_work = false;
					pm_relax(&pd->dev);
				}
				cancel_delayed_work(&pd->sbu_adc_work);
				pd->sbu_adc_state = ADC_STATE_DRY;
				qpnp_adc_tm_disable_chan_meas(pd->adc_tm_dev, &pd->sbu_adc_param);
			}
			pd_set_input_suspend(pd, false);
			pd_set_cc_disable(pd, false);
			pd->prop_moisture = DUAL_ROLE_PROP_MOISTURE_FALSE;
			dual_role_instance_changed(pd->dual_role);
			power_supply_changed(pd->usb_psy);
		} else if (*val == DUAL_ROLE_PROP_MOISTURE_EN_ENABLE){
			usbpd_info(&pd->dev, "[moisture] %s: enable moisture detection\n", __func__);
			if (pd->edge_sel) {
				schedule_delayed_work(&pd->init_edge_adc_work, 0);
			}
			if (pd->sbu_sel) {
				schedule_delayed_work(&pd->init_sbu_adc_work, 0);
			}
		}
		mutex_unlock(&pd->moisture_lock);
		break;
	case DUAL_ROLE_PROP_MOISTURE:
		mutex_lock(&pd->moisture_lock);
		if (!pd->adc_initialized) {
			mutex_unlock(&pd->moisture_lock);
			break;
		}
		if (pd->prop_moisture_en == DUAL_ROLE_PROP_MOISTURE_EN_DISABLE) {
			usbpd_info(&pd->dev, "[moisture] %s: moisture detection is disabled\n",
				__func__);
			mutex_unlock(&pd->moisture_lock);
			break;
		} else if (pd->sbu_moisture) {
			usbpd_info(&pd->dev, "[moisture] %s: skip, wet state\n", __func__);
			mutex_unlock(&pd->moisture_lock);
			break;
		} else if (*val == pd->prop_moisture) {
			mutex_unlock(&pd->moisture_lock);
			break;
		} else
			pd->prop_moisture = *val;

		if (*val == DUAL_ROLE_PROP_MOISTURE_TRUE) {
			usbpd_info(&pd->dev, "[moisture] %s: set moisture true\n", __func__);
			if (pd->sbu_sel) {
				qpnp_adc_tm_disable_chan_meas(pd->adc_tm_dev, &pd->sbu_adc_param);
				if (pd->sbu_run_work) {
					pd->sbu_run_work = false;
					pm_relax(&pd->dev);
				}
				cancel_delayed_work(&pd->sbu_adc_work);
				pd->sbu_lock = false;
				pd->sbu_adc_state = ADC_STATE_WET;
				pd->sbu_tm_state = ADC_TM_LOW_STATE;
			}
			if (pd->edge_sel) {
				qpnp_adc_tm_disable_chan_meas(pd->adc_tm_dev, &pd->edge_adc_param);
				if (pd->edge_run_work) {
					pd->edge_run_work = false;
					pm_relax(&pd->dev);
				}
				cancel_delayed_work(&pd->edge_adc_work);
				pd->edge_lock = false;
				pd->edge_adc_state = ADC_STATE_WET;
				pd->edge_tm_state = ADC_TM_LOW_STATE;
			}
			if (pd->sbu_sel)
				schedule_delayed_work(&pd->sbu_adc_work, msecs_to_jiffies(0));
			if (pd->edge_sel)
				schedule_delayed_work(&pd->edge_adc_work, msecs_to_jiffies(0));

		} else if (*val == DUAL_ROLE_PROP_MOISTURE_FALSE) {
			usbpd_info(&pd->dev, "[moisture] %s: set moisture false\n", __func__);
			/* not used */
		}
		mutex_unlock(&pd->moisture_lock);
		break;
#endif
	default:
		usbpd_warn(&pd->dev, "unsupported property %d\n", prop);
		return -ENOTSUPP;
	}

	return 0;
}

static int usbpd_dr_prop_writeable(struct dual_role_phy_instance *dual_role,
		enum dual_role_property prop)
{
	struct usbpd *pd = dual_role_get_drvdata(dual_role);

	switch (prop) {
	case DUAL_ROLE_PROP_MODE:
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
	case DUAL_ROLE_PROP_MOISTURE_EN:
	case DUAL_ROLE_PROP_MOISTURE:
#endif
		return 1;
	case DUAL_ROLE_PROP_DR:
	case DUAL_ROLE_PROP_PR:
		if (pd)
			return pd->current_state == PE_SNK_READY ||
				pd->current_state == PE_SRC_READY;
		break;
	default:
		break;
	}

	return 0;
}

static int usbpd_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	struct usbpd *pd = dev_get_drvdata(dev);
	int i;

	add_uevent_var(env, "DATA_ROLE=%s", pd->current_dr == DR_DFP ?
			"dfp" : "ufp");

	if (pd->current_pr == PR_SINK) {
		add_uevent_var(env, "POWER_ROLE=sink");
		add_uevent_var(env, "SRC_CAP_ID=%d", pd->src_cap_id);

		for (i = 0; i < ARRAY_SIZE(pd->received_pdos); i++)
			add_uevent_var(env, "PDO%d=%08x", i,
					pd->received_pdos[i]);

		add_uevent_var(env, "REQUESTED_PDO=%d", pd->requested_pdo);
		add_uevent_var(env, "SELECTED_PDO=%d", pd->selected_pdo);
	} else {
		add_uevent_var(env, "POWER_ROLE=source");
		for (i = 0; i < ARRAY_SIZE(default_src_caps); i++)
			add_uevent_var(env, "PDO%d=%08x", i,
					default_src_caps[i]);
	}

	add_uevent_var(env, "RDO=%08x", pd->rdo);
	add_uevent_var(env, "CONTRACT=%s", pd->in_explicit_contract ?
				"explicit" : "implicit");
	add_uevent_var(env, "ALT_MODE=%d", pd->vdm_state == MODE_ENTERED);

	return 0;
}

static ssize_t contract_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct usbpd *pd = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%s\n",
			pd->in_explicit_contract ?  "explicit" : "implicit");
}
static DEVICE_ATTR_RO(contract);

static ssize_t current_pr_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct usbpd *pd = dev_get_drvdata(dev);
	const char *pr = "none";

	if (pd->current_pr == PR_SINK)
		pr = "sink";
	else if (pd->current_pr == PR_SRC)
		pr = "source";

	return snprintf(buf, PAGE_SIZE, "%s\n", pr);
}
static DEVICE_ATTR_RO(current_pr);

static ssize_t initial_pr_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct usbpd *pd = dev_get_drvdata(dev);
	const char *pr = "none";

	if (pd->typec_mode >= POWER_SUPPLY_TYPEC_SOURCE_DEFAULT)
		pr = "sink";
	else if (pd->typec_mode >= POWER_SUPPLY_TYPEC_SINK)
		pr = "source";

	return snprintf(buf, PAGE_SIZE, "%s\n", pr);
}
static DEVICE_ATTR_RO(initial_pr);

static ssize_t current_dr_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct usbpd *pd = dev_get_drvdata(dev);
	const char *dr = "none";

	if (pd->current_dr == DR_UFP)
		dr = "ufp";
	else if (pd->current_dr == DR_DFP)
		dr = "dfp";

	return snprintf(buf, PAGE_SIZE, "%s\n", dr);
}
static DEVICE_ATTR_RO(current_dr);

static ssize_t initial_dr_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct usbpd *pd = dev_get_drvdata(dev);
	const char *dr = "none";

	if (pd->typec_mode >= POWER_SUPPLY_TYPEC_SOURCE_DEFAULT)
		dr = "ufp";
	else if (pd->typec_mode >= POWER_SUPPLY_TYPEC_SINK)
		dr = "dfp";

	return snprintf(buf, PAGE_SIZE, "%s\n", dr);
}
static DEVICE_ATTR_RO(initial_dr);

static ssize_t src_cap_id_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct usbpd *pd = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%d\n", pd->src_cap_id);
}
static DEVICE_ATTR_RO(src_cap_id);

/* Dump received source PDOs in human-readable format */
static ssize_t pdo_h_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct usbpd *pd = dev_get_drvdata(dev);
	int i;
	ssize_t cnt = 0;

	for (i = 0; i < ARRAY_SIZE(pd->received_pdos); i++) {
		u32 pdo = pd->received_pdos[i];

		if (pdo == 0)
			break;

		cnt += scnprintf(&buf[cnt], PAGE_SIZE - cnt, "PDO %d\n", i + 1);

		if (PD_SRC_PDO_TYPE(pdo) == PD_SRC_PDO_TYPE_FIXED) {
			cnt += scnprintf(&buf[cnt], PAGE_SIZE - cnt,
					"\tFixed supply\n"
					"\tDual-Role Power:%d\n"
					"\tUSB Suspend Supported:%d\n"
					"\tExternally Powered:%d\n"
					"\tUSB Communications Capable:%d\n"
					"\tData Role Swap:%d\n"
					"\tPeak Current:%d\n"
					"\tVoltage:%d (mV)\n"
					"\tMax Current:%d (mA)\n",
					PD_SRC_PDO_FIXED_PR_SWAP(pdo),
					PD_SRC_PDO_FIXED_USB_SUSP(pdo),
					PD_SRC_PDO_FIXED_EXT_POWERED(pdo),
					PD_SRC_PDO_FIXED_USB_COMM(pdo),
					PD_SRC_PDO_FIXED_DR_SWAP(pdo),
					PD_SRC_PDO_FIXED_PEAK_CURR(pdo),
					PD_SRC_PDO_FIXED_VOLTAGE(pdo) * 50,
					PD_SRC_PDO_FIXED_MAX_CURR(pdo) * 10);
		} else if (PD_SRC_PDO_TYPE(pdo) == PD_SRC_PDO_TYPE_BATTERY) {
			cnt += scnprintf(&buf[cnt], PAGE_SIZE - cnt,
					"\tBattery supply\n"
					"\tMax Voltage:%d (mV)\n"
					"\tMin Voltage:%d (mV)\n"
					"\tMax Power:%d (mW)\n",
					PD_SRC_PDO_VAR_BATT_MAX_VOLT(pdo) * 50,
					PD_SRC_PDO_VAR_BATT_MIN_VOLT(pdo) * 50,
					PD_SRC_PDO_VAR_BATT_MAX(pdo) * 250);
		} else if (PD_SRC_PDO_TYPE(pdo) == PD_SRC_PDO_TYPE_VARIABLE) {
			cnt += scnprintf(&buf[cnt], PAGE_SIZE - cnt,
					"\tVariable supply\n"
					"\tMax Voltage:%d (mV)\n"
					"\tMin Voltage:%d (mV)\n"
					"\tMax Current:%d (mA)\n",
					PD_SRC_PDO_VAR_BATT_MAX_VOLT(pdo) * 50,
					PD_SRC_PDO_VAR_BATT_MIN_VOLT(pdo) * 50,
					PD_SRC_PDO_VAR_BATT_MAX(pdo) * 10);
		} else if (PD_SRC_PDO_TYPE(pdo) == PD_SRC_PDO_TYPE_AUGMENTED) {
			cnt += scnprintf(&buf[cnt], PAGE_SIZE - cnt,
					"\tProgrammable Power supply\n"
					"\tMax Voltage:%d (mV)\n"
					"\tMin Voltage:%d (mV)\n"
					"\tMax Current:%d (mA)\n",
					PD_APDO_MAX_VOLT(pdo) * 100,
					PD_APDO_MIN_VOLT(pdo) * 100,
					PD_APDO_MAX_CURR(pdo) * 50);
		} else {
			cnt += scnprintf(&buf[cnt], PAGE_SIZE - cnt,
					"Invalid PDO\n");
		}

		buf[cnt++] = '\n';
	}

	return cnt;
}
static DEVICE_ATTR_RO(pdo_h);

static ssize_t pdo_n_show(struct device *dev, struct device_attribute *attr,
		char *buf);

#define PDO_ATTR(n) {					\
	.attr	= { .name = __stringify(pdo##n), .mode = S_IRUGO },	\
	.show	= pdo_n_show,				\
}
static struct device_attribute dev_attr_pdos[] = {
	PDO_ATTR(1),
	PDO_ATTR(2),
	PDO_ATTR(3),
	PDO_ATTR(4),
	PDO_ATTR(5),
	PDO_ATTR(6),
	PDO_ATTR(7),
};

static ssize_t pdo_n_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct usbpd *pd = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < ARRAY_SIZE(dev_attr_pdos); i++)
		if (attr == &dev_attr_pdos[i])
			/* dump the PDO as a hex string */
			return snprintf(buf, PAGE_SIZE, "%08x\n",
					pd->received_pdos[i]);

	usbpd_err(&pd->dev, "Invalid PDO index\n");
	return -EINVAL;
}

static ssize_t select_pdo_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct usbpd *pd = dev_get_drvdata(dev);
	int src_cap_id;
	int pdo, uv = 0, ua = 0;
	int ret;

	mutex_lock(&pd->swap_lock);

	/* Only allowed if we are already in explicit sink contract */
	if (pd->current_state != PE_SNK_READY || !is_sink_tx_ok(pd)) {
		usbpd_err(&pd->dev, "select_pdo: Cannot select new PDO yet\n");
		ret = -EBUSY;
		goto out;
	}

	ret = sscanf(buf, "%d %d %d %d", &src_cap_id, &pdo, &uv, &ua);
	if (ret != 2 && ret != 4) {
		usbpd_err(&pd->dev, "select_pdo: Must specify <src cap id> <PDO> [<uV> <uA>]\n");
		ret = -EINVAL;
		goto out;
	}

	if (src_cap_id != pd->src_cap_id) {
		usbpd_err(&pd->dev, "select_pdo: src_cap_id mismatch.  Requested:%d, current:%d\n",
				src_cap_id, pd->src_cap_id);
		ret = -EINVAL;
		goto out;
	}

	if (pdo < 1 || pdo > 7) {
		usbpd_err(&pd->dev, "select_pdo: invalid PDO:%d\n", pdo);
		ret = -EINVAL;
		goto out;
	}

	ret = pd_select_pdo(pd, pdo, uv, ua);
	if (ret)
		goto out;

	reinit_completion(&pd->is_ready);
	pd->send_request = true;
	kick_sm(pd, 0);

	/* wait for operation to complete */
	if (!wait_for_completion_timeout(&pd->is_ready,
			msecs_to_jiffies(1000))) {
		usbpd_err(&pd->dev, "select_pdo: request timed out\n");
		ret = -ETIMEDOUT;
		goto out;
	}

	/* determine if request was accepted/rejected */
	if (pd->selected_pdo != pd->requested_pdo ||
			pd->current_voltage != pd->requested_voltage) {
		usbpd_err(&pd->dev, "select_pdo: request rejected\n");
		ret = -EINVAL;
	}

out:
	pd->send_request = false;
	mutex_unlock(&pd->swap_lock);
	return ret ? ret : size;
}

static ssize_t select_pdo_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct usbpd *pd = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%d\n", pd->selected_pdo);
}
static DEVICE_ATTR_RW(select_pdo);

static ssize_t rdo_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct usbpd *pd = dev_get_drvdata(dev);

	/* dump the RDO as a hex string */
	return snprintf(buf, PAGE_SIZE, "%08x\n", pd->rdo);
}
static DEVICE_ATTR_RO(rdo);

static ssize_t rdo_h_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct usbpd *pd = dev_get_drvdata(dev);
	int pos = PD_RDO_OBJ_POS(pd->rdo);
	int type = PD_SRC_PDO_TYPE(pd->received_pdos[pos]);
	int len;

	len = scnprintf(buf, PAGE_SIZE, "Request Data Object\n"
			"\tObj Pos:%d\n"
			"\tGiveback:%d\n"
			"\tCapability Mismatch:%d\n"
			"\tUSB Communications Capable:%d\n"
			"\tNo USB Suspend:%d\n",
			PD_RDO_OBJ_POS(pd->rdo),
			PD_RDO_GIVEBACK(pd->rdo),
			PD_RDO_MISMATCH(pd->rdo),
			PD_RDO_USB_COMM(pd->rdo),
			PD_RDO_NO_USB_SUSP(pd->rdo));

	switch (type) {
	case PD_SRC_PDO_TYPE_FIXED:
	case PD_SRC_PDO_TYPE_VARIABLE:
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"(Fixed/Variable)\n"
				"\tOperating Current:%d (mA)\n"
				"\t%s Current:%d (mA)\n",
				PD_RDO_FIXED_CURR(pd->rdo) * 10,
				PD_RDO_GIVEBACK(pd->rdo) ? "Min" : "Max",
				PD_RDO_FIXED_CURR_MINMAX(pd->rdo) * 10);
		break;

	case PD_SRC_PDO_TYPE_BATTERY:
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"(Battery)\n"
				"\tOperating Power:%d (mW)\n"
				"\t%s Power:%d (mW)\n",
				PD_RDO_FIXED_CURR(pd->rdo) * 250,
				PD_RDO_GIVEBACK(pd->rdo) ? "Min" : "Max",
				PD_RDO_FIXED_CURR_MINMAX(pd->rdo) * 250);
		break;

	case PD_SRC_PDO_TYPE_AUGMENTED:
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"(Programmable)\n"
				"\tOutput Voltage:%d (mV)\n"
				"\tOperating Current:%d (mA)\n",
				PD_RDO_PROG_VOLTAGE(pd->rdo) * 20,
				PD_RDO_PROG_CURR(pd->rdo) * 50);
		break;
	}

	return len;
}
static DEVICE_ATTR_RO(rdo_h);

static ssize_t hard_reset_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct usbpd *pd = dev_get_drvdata(dev);
	int val = 0;

	if (sscanf(buf, "%d\n", &val) != 1)
		return -EINVAL;

	if (val)
		usbpd_set_state(pd, pd->current_pr == PR_SRC ?
				PE_SRC_HARD_RESET : PE_SNK_HARD_RESET);

	return size;
}
static DEVICE_ATTR_WO(hard_reset);

static struct attribute *usbpd_attrs[] = {
	&dev_attr_contract.attr,
	&dev_attr_initial_pr.attr,
	&dev_attr_current_pr.attr,
	&dev_attr_initial_dr.attr,
	&dev_attr_current_dr.attr,
	&dev_attr_src_cap_id.attr,
	&dev_attr_pdo_h.attr,
	&dev_attr_pdos[0].attr,
	&dev_attr_pdos[1].attr,
	&dev_attr_pdos[2].attr,
	&dev_attr_pdos[3].attr,
	&dev_attr_pdos[4].attr,
	&dev_attr_pdos[5].attr,
	&dev_attr_pdos[6].attr,
	&dev_attr_select_pdo.attr,
	&dev_attr_rdo.attr,
	&dev_attr_rdo_h.attr,
	&dev_attr_hard_reset.attr,
	NULL,
};
ATTRIBUTE_GROUPS(usbpd);

static struct class usbpd_class = {
	.name = "usbpd",
	.owner = THIS_MODULE,
	.dev_uevent = usbpd_uevent,
	.dev_groups = usbpd_groups,
};

static int match_usbpd_device(struct device *dev, const void *data)
{
	return dev->parent == data;
}

static void devm_usbpd_put(struct device *dev, void *res)
{
	struct usbpd **ppd = res;

	put_device(&(*ppd)->dev);
}

#ifdef CONFIG_LGE_USB_DEBUGGER
static void usb_debugger_work(struct work_struct *w)
{
	struct usbpd *pd = container_of(w, struct usbpd, usb_debugger_work);
#ifdef CONFIG_LGE_USB_FACTORY
	union lge_power_propval lge_val;
	int rc;
#endif

	usbpd_info(&pd->dev,"usb_debugger_work !!! debug_accessory:%d\n",pd->is_debug_accessory);
#ifdef CONFIG_LGE_USB_FACTORY
	if(!pd->lge_power_cd) {
		usbpd_dbg(&pd->dev, "lge_power_cd is NULL\n");
		return;
	}
#endif

	if(pd->is_debug_accessory) {
#ifdef CONFIG_LGE_USB_FACTORY
		rc = pd->lge_power_cd->get_property(pd->lge_power_cd,
				LGE_POWER_PROP_IS_FACTORY_CABLE,
				&lge_val);
		if(rc != 0) {
			usbpd_err(&pd->dev,"usb id only check fail\n");
			return;
		} else if (lge_val.intval == FACTORY_CABLE) {
			usbpd_info(&pd->dev,"factory cable connected\n");
			return;
		}
#endif
		msm_serial_set_uart_console(1);
		gpiod_direction_output(pd->uart_sbu_sel_gpio, 1);
		usbpd_info(&pd->dev,"uart on\n");
	} else {
		gpiod_direction_output(pd->uart_sbu_sel_gpio, 0);
		msm_serial_set_uart_console(0);
		usbpd_info(&pd->dev,"uart off\n");
	}
}
#endif

#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
static int pd_set_input_suspend(struct usbpd *pd, bool enable)
{
	struct power_supply *psy = power_supply_get_by_name("battery");
	union power_supply_propval pval = {0, };

	if (!psy)
		return 0;

	usbpd_info(&pd->dev,"[moisture] %s: set %d\n", __func__, enable);
	pval.intval = enable;

	return power_supply_set_property(psy, POWER_SUPPLY_PROP_MOISTURE_DETECTION, &pval);
}
/*
static int pd_get_input_suspend(void)
{
	struct power_supply *psy = power_supply_get_by_name("battery");
	union power_supply_propval pval = {0, };

	if (!psy)
		return 0;

	power_supply_set_property(psy, POWER_SUPPLY_PROP_INPUT_SUSPEND, &pval);

	return pval.intval;
}
*/

static int pd_set_cc_disable(struct usbpd *pd, bool enable)
{
	union power_supply_propval val = {0, };

	if ((pd->edge_moisture || pd->sbu_moisture)&& !enable)
		return 0;

	pd->cc_disabled = enable;

	if (enable)
		val.intval = POWER_SUPPLY_TYPEC_PR_NONE;
	else
		val.intval = POWER_SUPPLY_TYPEC_PR_DUAL;

	return power_supply_set_property(pd->usb_psy, POWER_SUPPLY_PROP_TYPEC_POWER_ROLE, &val);
}

static int pd_get_is_ocp(struct usbpd *pd)
{
	union power_supply_propval pval = {0, };

	power_supply_get_property(pd->usb_psy, POWER_SUPPLY_PROP_TYPEC_IS_OCP, &pval);

	return pval.intval;
}

#define ADC_POLL_TIMEOUT	(10 * HZ) /* 10 sec */
#define ADC_WFD_TIMEOUT	(1000 * HZ/1000) /* 1000 msec */
#define ADC_CC_CHANGED_TIME	(3000 * HZ/1000) /* 3000 msec */
#define ADC_MAX_DRY_COUNT	5
#define ADC_CHANGE_THR		100000 /* 100mV */

static unsigned int pd_get_check_timeout(struct usbpd *pd, struct timespec mtime)
{
	struct timespec timeout_remain;
	unsigned int timeout_remain_ms;

	timeout_remain = timespec_sub(CURRENT_TIME, mtime);
	timeout_remain_ms = (timeout_remain.tv_sec * 1000) + (timeout_remain.tv_nsec / 1000000);

	if (timeout_remain_ms <= (60 * 1000)) { /* 10s delay for 1M */
		return (10 * HZ);
	}
	/*
	else if (timeout_remain_ms <= (5 * 60 * 1000)) { // 60s delay for 5M
		return (60 * HZ);
	} else if (timeout_remain_ms <= (60 * 60 * 1000)) { // 600s delay for 1H
		return (10 * 60 * HZ);
	}*/

	return (60 * HZ);
}

/*
static unsigned int pd_get_last_mtime_ms(struct usbpd *pd, struct timespec mtime)
{
	struct timespec timeout_remain;
	unsigned int timeout_remain_ms;

	timeout_remain = timespec_sub(CURRENT_TIME, mtime);
	timeout_remain_ms = (timeout_remain.tv_sec * 1000) + (timeout_remain.tv_nsec / 1000000);

	return timeout_remain_ms;
}
*/

static enum hrtimer_restart pd_edge_timeout(struct hrtimer *timer)
{
	struct usbpd *pd = container_of(timer, struct usbpd, edge_timer);

	usbpd_dbg(&pd->dev, "timeout");
	cancel_delayed_work(&pd->edge_adc_work);
	schedule_delayed_work(&pd->edge_adc_work, 0);

	return HRTIMER_NORESTART;
}

static enum hrtimer_restart pd_sbu_timeout(struct hrtimer *timer)
{
	struct usbpd *pd = container_of(timer, struct usbpd, sbu_timer);

	usbpd_dbg(&pd->dev, "timeout");
	cancel_delayed_work(&pd->sbu_adc_work);
	schedule_delayed_work(&pd->sbu_adc_work, 0);

	return HRTIMER_NORESTART;
}

static void pd_edge_adc_work(struct work_struct *w)
{
	struct usbpd *pd = container_of(w, struct usbpd, edge_adc_work.work);
	struct qpnp_vadc_result results;
	static struct qpnp_adc_tm_btm_param prev_adc_param;
	static int wet_adc, dry_count, polling_count;
	int ret, i, work = 0;
	unsigned long delay = 0;

	mutex_lock(&pd->moisture_lock);
	if (!pd->usb_psy) {
		pd->usb_psy = power_supply_get_by_name("usb");
		if (!pd->usb_psy) {
			usbpd_warn(&pd->dev, "[moisture] %s: Could not get usb power_supply\n",
					__func__);
			pd->edge_run_work = false;
			goto out;
		}
	}

	hrtimer_cancel(&pd->edge_timer);
	usbpd_info(&pd->dev, "[moisture] %s: adc state: %d, tm state: %s\n", __func__,
			pd->edge_adc_state, pd->edge_tm_state == ADC_TM_HIGH_STATE ? "high" : "low");

	if (pd->edge_lock) {
		usbpd_info(&pd->dev, "[moisture] %s: cable is connected, skip work\n",
				__func__);
		pd->edge_adc_state = ADC_STATE_DRY;
		pd->edge_run_work = false;
		goto out;
	}

	qpnp_vadc_read(pd->vadc_dev, VADC_AMUX_THM1_PU2, &results);
	usbpd_info(&pd->dev, "[moisture] %s: usb edge adc = %d\n", __func__,
			(int)results.physical);

	if (pd->edge_tm_state == ADC_TM_HIGH_STATE) {
		pd->edge_adc_param.state_request = ADC_TM_LOW_THR_ENABLE;
	} else {
		pd->edge_adc_param.state_request = ADC_TM_HIGH_THR_ENABLE;
	}
	pd->edge_adc_param.low_thr = adc_edge_low_threshold;
	pd->edge_adc_param.high_thr = adc_edge_high_threshold;

	switch (pd->edge_adc_state) {
	case ADC_STATE_DRY:
		if (pd->edge_tm_state == ADC_TM_HIGH_STATE) {
			if (pd->edge_moisture) {
				usbpd_info(&pd->dev, "[moisture] %s: wet state: %s -> %s\n", __func__,
						pd->edge_moisture ? "wet" : "dry", "dry");
				pd->edge_moisture = 0;
				pd_set_cc_disable(pd, false);
				if (lge_get_board_rev_no() >= HW_REV_1_0)
					gpiod_direction_output(pd->edge_sel, 1);
			}
		} else {
			pd->edge_adc_state = ADC_STATE_WDT;
			work = 1;
		}
		break;
	case ADC_STATE_WDT:
		pd_set_cc_disable(pd, true);
		msleep(100);
		qpnp_vadc_read(pd->vadc_dev, VADC_AMUX_THM1_PU2, &results);
		usbpd_info(&pd->dev, "[moisture] %s: usb edge adc1 = %d\n", __func__,
			(int)results.physical);
		pd_set_cc_disable(pd, false);

		 if ((int)results.physical < adc_gnd_low_threshold) {
			 pd->edge_adc_state = ADC_STATE_GND;
			 work = 1;
		 } else if ((int)results.physical < adc_edge_low_threshold) {
			 pd_set_cc_disable(pd, true);
			 for (i = 0; i < 10; ++i) {
				 msleep(20);
				 qpnp_vadc_read(pd->vadc_dev, VADC_AMUX_THM1_PU2, &results);
				 usbpd_info(&pd->dev, "[moisture] %s: usb edge adc(#%d) = %d\n", __func__,
						 i, (int)results.physical);
				 if ((int)results.physical < adc_gnd_low_threshold ||
						 (int)results.physical > adc_edge_high_threshold) {
					 break;
				 }
			 }
			 pd_set_cc_disable(pd, false);
			 if ((int)results.physical < adc_gnd_low_threshold) {
				 pd->edge_adc_state = ADC_STATE_GND;
				 work = 1;
			 } else if ((int)results.physical > adc_edge_high_threshold ||
					 pd->vbus_present) {
				 pd->edge_adc_state = ADC_STATE_DRY;
				 work = 1;
			 } else {
				 wet_adc = (int)results.physical;
				 pd->edge_adc_state = ADC_STATE_WET;
				 delay = 1*HZ;
				 work = 1;
			 }
		} else {
			pd->edge_adc_state = ADC_STATE_DRY;
			work = 1;
		}
		break;
	case ADC_STATE_GND:
		if (pd->edge_tm_state == ADC_TM_HIGH_STATE) {
			pd->edge_adc_state = ADC_STATE_DRY;
			work = 1;
		} else {
			pd->edge_adc_param.high_thr = adc_gnd_high_threshold;
		}
		break;
	case ADC_STATE_WFD:
		if ((int)results.physical > adc_edge_high_threshold) {
			if (dry_count < ADC_MAX_DRY_COUNT) {
				dry_count++;
				delay = ADC_WFD_TIMEOUT;
				work = 1;
			} else {
				pd->edge_adc_state = ADC_STATE_DRY;
				work = 1;
			}
		} else {
			pd->edge_adc_state = ADC_STATE_WET;
			work = 1;
		}
		break;
	case ADC_STATE_WET:
		if (!pd->edge_moisture) {
			usbpd_info(&pd->dev, "[moisture] %s: wet state: %s -> %s\n", __func__,
					pd->edge_moisture ? "wet" : "dry", "wet");
			pd->edge_moisture = 1;
			if (lge_get_board_rev_no() >= HW_REV_1_0)
				gpiod_direction_output(pd->edge_sel, 0);
			polling_count = 0;
			pd->edge_mtime = CURRENT_TIME;
		}

		if (pd->edge_tm_state == ADC_TM_HIGH_STATE) {
			if ((int)results.physical > adc_edge_high_threshold){
				// if state is wet, called when vbus is off
				if (!pd->vbus_present) { // vbus not present
					pd->edge_adc_state = ADC_STATE_WFD;
					dry_count = 0;
					wet_adc = 0;
					work = 1;
					pm_stay_awake(&pd->dev);
					pd->edge_run_work = true;
				} else {
					usbpd_info(&pd->dev, "[moisture] %s: maybe adc is up by cable\n",
							__func__);
				}
			} else if((int)results.physical < adc_gnd_low_threshold) { //for OTG enable
				if (!pd->vbus_present && !pd->sbu_moisture) {
					pd->edge_adc_state = ADC_STATE_DRY;
					work = 1;
				}
			} else {
				if (pd->vbus_present) { // vbus present
					usbpd_info(&pd->dev, "[moisture] %s: vbus is on\n", __func__);
				} else {
					usbpd_info(&pd->dev, "[moisture] %s: vbus is off\n", __func__);
				}
				pd->edge_adc_param.state_request = ADC_TM_HIGH_THR_ENABLE;
			}
		}

		if (pd->edge_adc_state == ADC_STATE_WET) {
			pd->edge_run_work = false;
			pd_set_cc_disable(pd, true);
			pd->edge_tm_state = ADC_TM_HIGH_STATE;
			work = 1;
			polling_count++;

			if ((int)results.physical > adc_edge_low_threshold)
				delay = ADC_POLL_TIMEOUT;
			else
				delay = pd_get_check_timeout(pd, pd->edge_mtime);
			hrtimer_start(&pd->edge_timer, ms_to_ktime(delay/HZ*1000), HRTIMER_MODE_REL);
			usbpd_info(&pd->dev, "[moisture] %s: count: %d delay: %lu(s)\n",
					__func__, polling_count, delay/HZ);
		}
		break;
	default:
		break;
	}

	if (work)
		schedule_delayed_work(&pd->edge_adc_work, delay);
	else {
		pd->edge_run_work = false;
		msleep(50);
		prev_adc_param = pd->edge_adc_param;
		usbpd_info(&pd->dev, "[moisture] %s: ADC PARAM low: %d, high: %d, irq: %d\n",
				__func__, pd->edge_adc_param.low_thr, pd->edge_adc_param.high_thr,
				pd->edge_adc_param.state_request);
		ret = qpnp_adc_tm_channel_measure(pd->adc_tm_dev, &pd->edge_adc_param);
		if (ret) {
			usbpd_err(&pd->dev, "[moisture] %s: request ADC error %d\n", __func__, ret);
			goto out;
		}
	}
out:
	if (!pd->edge_run_work)
		pm_relax(&pd->dev);
	mutex_unlock(&pd->moisture_lock);
}

static void pd_edge_notification(enum qpnp_tm_state state, void *ctx)
{
	struct usbpd *pd = ctx;

	usbpd_info(&pd->dev, "[moisture] %s: state: %s\n", __func__,
			state == ADC_TM_HIGH_STATE ? "high" : "low");
	if (state >= ADC_TM_STATE_NUM) {
		usbpd_err(&pd->dev, "[moisture] %s: invalid notification %d\n",
				__func__, state);
		return;
	}

	pm_stay_awake(&pd->dev);
	pd->edge_run_work = true;

	if (state == ADC_TM_HIGH_STATE) {
		pd->edge_tm_state = ADC_TM_HIGH_STATE;
	} else {
		pd->edge_tm_state = ADC_TM_LOW_STATE;
	}
	schedule_delayed_work(&pd->edge_adc_work, msecs_to_jiffies(1000));
}

static void pd_sbu_ov_adc_work(struct work_struct *w)
{
	struct usbpd *pd = container_of(w, struct usbpd, sbu_ov_adc_work.work);
	struct qpnp_vadc_result results;
	static int count;

	mutex_lock(&pd->moisture_lock);
	if (!pd->sbu_sel || pd->prop_moisture_en == DUAL_ROLE_PROP_MOISTURE_EN_DISABLE) {
		count = 0;
		mutex_unlock(&pd->moisture_lock);
		return;
	}

	if (!pd->vbus_present || pd->current_dr == DR_DFP) {
		count = 0;
		usbpd_info(&pd->dev, "[moisture] %s: vbus off or dfp, stop\n", __func__);
	} else {
		qpnp_vadc_read(pd->vadc_dev, VADC_AMUX_THM2, &results);
		usbpd_info(&pd->dev, "[moisture] %s: usb sbu adc = %d\n", __func__,
					            (int)results.physical);
		if ((int)results.physical > 1875000 && pd_get_is_ocp(pd)) {
			qpnp_adc_tm_disable_chan_meas(pd->adc_tm_dev, &pd->sbu_adc_param);
			if (pd->sbu_run_work) {
				pd->sbu_run_work = false;
				pm_relax(&pd->dev);
			}
			cancel_delayed_work(&pd->sbu_adc_work);
			pd->sbu_lock = false;
			pd->sbu_adc_state = ADC_STATE_WET;
			pd->sbu_tm_state = ADC_TM_LOW_STATE;
			schedule_delayed_work(&pd->sbu_adc_work, msecs_to_jiffies(0));
		} else if (++count < 10) {
			schedule_delayed_work(&pd->sbu_ov_adc_work, msecs_to_jiffies(1000));
		} else {
			count = 0;
			usbpd_info(&pd->dev, "[moisture] %s: exceed count, stop\n", __func__);
		}
	}
	mutex_unlock(&pd->moisture_lock);
}

static void pd_sbu_adc_work(struct work_struct *w)
{
	struct usbpd *pd = container_of(w, struct usbpd, sbu_adc_work.work);
	struct qpnp_vadc_result results;
	static struct qpnp_adc_tm_btm_param prev_adc_param;
	static int prev_adc2, wet_adc, dry_count, gpio_count;
	int prev_adc = 0, wet_count = 0;
	int ret, i, work = 0;
	unsigned long delay = 0;

	mutex_lock(&pd->moisture_lock);
	if (!pd->usb_psy) {
		pd->usb_psy = power_supply_get_by_name("usb");
		if (!pd->usb_psy) {
			usbpd_warn(&pd->dev, "[moisture] %s: Could not get usb power_supply\n",
					__func__);
			pd->sbu_run_work = false;
			goto out;
		}
	}

	hrtimer_cancel(&pd->sbu_timer);
	usbpd_dbg(&pd->dev, "[moisture] %s: adc state: %d, tm state: %s\n", __func__,
			pd->sbu_adc_state, pd->sbu_tm_state == ADC_TM_HIGH_STATE ? "high" : "low");

	if (pd->sbu_lock) {
		usbpd_info(&pd->dev, "[moisture] %s: cable is connected, skip work\n",
				__func__);
		pd->sbu_adc_state = ADC_STATE_DRY;
		pd->sbu_run_work = false;
		goto out;
	}

	qpnp_vadc_read(pd->vadc_dev, VADC_AMUX_THM2, &results);
	if (pd->pullup_volt == HW_PULLUP_1V)
		usbpd_dbg(&pd->dev, "[moisture] %s: usb sbu adc = %d\n", __func__,
				(int)results.physical);
	else
		usbpd_info(&pd->dev, "[moisture] %s: usb sbu adc = %d\n", __func__,
				(int)results.physical);



	if (pd->sbu_tm_state == ADC_TM_HIGH_STATE) {
		pd->sbu_adc_param.state_request = ADC_TM_LOW_THR_ENABLE;
	} else {
		pd->sbu_adc_param.state_request = ADC_TM_HIGH_THR_ENABLE;
	}
	pd->sbu_adc_param.low_thr = adc_low_threshold;
	pd->sbu_adc_param.high_thr = adc_high_threshold;

	switch (pd->sbu_adc_state) {
	case ADC_STATE_DRY:
		usbpd_info(&pd->dev, "[moisture] %s: usb sbu adc = %d\n", __func__,
				(int)results.physical);
		if (pd->sbu_tm_state == ADC_TM_HIGH_STATE) {
			if (pd->sbu_moisture) {
				usbpd_info(&pd->dev, "[moisture] %s: wet state: %s -> %s\n", __func__,
						pd->sbu_moisture ? "wet" : "dry", "dry");
				pd->sbu_moisture = 0;
				pd_set_input_suspend(pd, false);
				pd_set_cc_disable(pd, false);
				gpiod_direction_output(pd->sbu_oe, 0); //sbu oe enable
				gpiod_direction_output(pd->sbu_sel, 1); //sbu sel enable
				dual_role_instance_changed(pd->dual_role);
				power_supply_changed(pd->usb_psy);
			}
		} else {
			pd->sbu_adc_state = ADC_STATE_WDT;
			delay = ADC_CC_CHANGED_TIME;
			work = 1;
		}
		prev_adc = 0;
		prev_adc2 = 0;
		break;
	case ADC_STATE_WDT:
		pd_set_cc_disable(pd, true);
		msleep(100);
		qpnp_vadc_read(pd->vadc_dev, VADC_AMUX_THM2, &results);
		usbpd_info(&pd->dev, "[moisture] %s: usb sbu adc1 = %d\n", __func__,
			(int)results.physical);
		pd_set_cc_disable(pd, false);

		if ((int)results.physical < adc_gnd_low_threshold) {
			pd->sbu_adc_state = ADC_STATE_GND;
			work = 1;
		} else if ((int)results.physical < adc_low_threshold) {
			if(!pd->vbus_present) { //vbus not present
				if (prev_adc2) {
					usbpd_info(&pd->dev, "[moisture] %s: adc changed: %d -> %d", __func__,
							prev_adc2, (int)results.physical);
					wet_adc = (int)results.physical;
					pd->sbu_adc_state = ADC_STATE_WET;
					delay = 0;
					work = 1;
				} else {
					pd_set_cc_disable(pd, true);
					for (i = 0; i < 10; ++i) {
						msleep(20);
						qpnp_vadc_read(pd->vadc_dev, VADC_AMUX_THM2, &results);
						usbpd_info(&pd->dev, "[moisture] %s: sbu adc %d: %d->%d, w:%d\n", __func__, i,
								prev_adc, (int)results.physical, wet_count);
						if (prev_adc && //full
								(prev_adc - (int)results.physical > 100000 ||
								 prev_adc - (int)results.physical < -100000)) {
							wet_count++;
						} else if (prev_adc && (int)results.physical < adc_low_threshold &&
								(prev_adc - (int)results.physical > 500 ||
								 prev_adc - (int)results.physical < -500)) {
							wet_count++;
						}
						prev_adc = (int) results.physical;
					}
					pd_set_cc_disable(pd, false);
					usbpd_info(&pd->dev, "[moisture] %s: wet_count = %d\n", __func__, wet_count);
					if (wet_count >= 0) { //tuning
						wet_adc = (int)results.physical;
						pd->sbu_adc_state = ADC_STATE_WET;
						delay = 0;
						work = 1;
					} else {
						if ((int)results.physical > adc_high_threshold) {
							pd->sbu_adc_state = ADC_STATE_DRY;
							work = 1;
						} else {
							usbpd_info(&pd->dev, "[moisture] %s: detect not wet, wait adc change\n",
									__func__);
							pd->sbu_adc_param.low_thr = (int)results.physical - ADC_CHANGE_THR > 0 ?
								(int)results.physical - ADC_CHANGE_THR : 0;
							pd->sbu_adc_param.high_thr = (int)results.physical + ADC_CHANGE_THR > adc_low_threshold ?
								adc_low_threshold : (int)results.physical + ADC_CHANGE_THR;
							pd->sbu_adc_param.state_request = ADC_TM_HIGH_LOW_THR_ENABLE;
						}
					}
					wet_count = 0;
				}
				prev_adc2 = (int)results.physical;
			} else { //Vbus present
				usbpd_info(&pd->dev, "[moisture] %s: vbus is on, factory cable or usb cable connector is wet",
						__func__);
				prev_adc2 = (int)results.physical;
				pd->sbu_adc_param.low_thr = (int)results.physical - ADC_CHANGE_THR > 0 ?
					(int)results.physical - ADC_CHANGE_THR : 0;
				pd->sbu_adc_param.high_thr = (int)results.physical + ADC_CHANGE_THR > adc_low_threshold ?
					adc_low_threshold : (int)results.physical + ADC_CHANGE_THR;
				pd->sbu_adc_param.state_request = ADC_TM_HIGH_LOW_THR_ENABLE;
			}
		} else {
			pd->sbu_adc_state = ADC_STATE_DRY;
			pd->sbu_tm_state = ADC_TM_HIGH_STATE;
			work = 1;
		}
		break;
	case ADC_STATE_GND:
		if (pd->sbu_tm_state == ADC_TM_HIGH_STATE) {
			pd->sbu_adc_state = ADC_STATE_DRY;
			work = 1;
		} else {
			pd->sbu_adc_param.high_thr = adc_gnd_high_threshold;
		}
		break;
	case ADC_STATE_WFD:
		if ((int)results.physical > adc_high_threshold) {
			if (dry_count < ADC_MAX_DRY_COUNT) {
				dry_count++;
				delay = ADC_WFD_TIMEOUT;
				work = 1;
			} else {
				pd->sbu_adc_state = ADC_STATE_DRY;
				work = 1;
			}
		} else {
			pd->sbu_adc_state = ADC_STATE_WET;
			work = 1;
		}
		break;
	case ADC_STATE_WET:
		if (!pd->sbu_moisture) {
			usbpd_info(&pd->dev, "[moisture] %s: wet state: %s -> %s\n", __func__,
					pd->sbu_moisture ? "wet" : "dry", "wet");
			pd->sbu_moisture = 1;
			pd_set_cc_disable(pd, true);
			pd_set_input_suspend(pd, true);
			dual_role_instance_changed(pd->dual_role);
			power_supply_changed(pd->usb_psy);
			gpio_count = 0;
			pd->sbu_mtime = CURRENT_TIME;
			stop_usb_peripheral(pd);
			stop_usb_host(pd);

			pd->current_pr = PR_NONE;
			kick_sm(pd, 0);
		}

		if (pd->sbu_tm_state == ADC_TM_HIGH_STATE) {
			if ((int)results.physical > adc_high_threshold){
				// if state is wet, called when vbus is off
				if (!pd->vbus_present) { // vbus not present
					pd->sbu_adc_state = ADC_STATE_WFD;
					dry_count = 0;
					wet_adc = 0;
					work = 1;
					pm_stay_awake(&pd->dev);
					pd->sbu_run_work = true;
				} else {
					usbpd_dbg(&pd->dev, "[moisture] %s: maybe adc is up by cable\n",
							__func__);
				}
			} else {
				if (pd->vbus_present) { // vbus present
					usbpd_dbg(&pd->dev, "[moisture] %s: vbus is on\n", __func__);
				} else {
					usbpd_dbg(&pd->dev, "[moisture] %s: vbus is off\n", __func__);
				}
				pd->sbu_adc_param.state_request = ADC_TM_HIGH_THR_ENABLE;
			}
		}

		if (pd->sbu_adc_state == ADC_STATE_WET) {
			pd->sbu_lock = false;
			pd->sbu_run_work = false;
			pd_set_cc_disable(pd, true);
			gpiod_direction_output(pd->sbu_oe, 1); //sbu oe disable
			gpiod_direction_output(pd->sbu_sel, 0); //sbu sel disable
			pd->sbu_tm_state = ADC_TM_HIGH_STATE;
			work = 1;
			gpio_count++;

			if (pd->pullup_volt == HW_PULLUP_1V) {
				delay = (10 * HZ);
			} else if ((int)results.physical > adc_low_threshold) {
				delay = (10 * HZ);
			} else if ((int)results.physical > 1000000) {
				delay = (60 * HZ);
			} else {
				delay = pd_get_check_timeout(pd, pd->sbu_mtime);
			}

			if (pd->pullup_volt == HW_PULLUP_1V) {
				hrtimer_start(&pd->sbu_timer, ms_to_ktime(60 * 1000), HRTIMER_MODE_REL);
				usbpd_dbg(&pd->dev, "[moisture] %s: count: %d delay: %lu(s)\n",
						__func__, gpio_count, delay/HZ);
			} else {
				hrtimer_start(&pd->sbu_timer, ms_to_ktime(delay/HZ*1000), HRTIMER_MODE_REL);
				usbpd_info(&pd->dev, "[moisture] %s: count: %d delay: %lu(s)\n",
						__func__, gpio_count, delay/HZ);
			}

			if (pd->edge_sel && pd->edge_adc_state != ADC_STATE_WET) {
				usbpd_info(&pd->dev, "[moisture] %s: forcely set wet state to edge\n",
						__func__);
				pd->edge_adc_state = ADC_STATE_WET;
				cancel_delayed_work(&pd->edge_adc_work);
				schedule_delayed_work(&pd->edge_adc_work, 0);
			}
		}
		break;
	default:
		break;
	}

	if (work)
		schedule_delayed_work(&pd->sbu_adc_work, delay);
	else {
		pd->sbu_run_work = false;
		msleep(50);
		prev_adc_param = pd->sbu_adc_param;
		usbpd_info(&pd->dev, "[moisture] %s: ADC PARAM low: %d, high: %d, irq: %d\n",
				__func__, pd->sbu_adc_param.low_thr, pd->sbu_adc_param.high_thr,
				pd->sbu_adc_param.state_request);
		ret = qpnp_adc_tm_channel_measure(pd->adc_tm_dev, &pd->sbu_adc_param);
		if (ret) {
			usbpd_err(&pd->dev, "[moisture] %s: request ADC error %d\n", __func__, ret);
			goto out;
		}
	}
out:
	if (!pd->sbu_run_work)
		pm_relax(&pd->dev);
	mutex_unlock(&pd->moisture_lock);
}

static void pd_sbu_notification(enum qpnp_tm_state state, void *ctx)
{
	struct usbpd *pd = ctx;

	usbpd_info(&pd->dev, "[moisture] %s: state: %s\n", __func__,
			state == ADC_TM_HIGH_STATE ? "high" : "low");
	if (state >= ADC_TM_STATE_NUM) {
		usbpd_err(&pd->dev, "[moisture] %s: invalid notification %d\n",
				__func__, state);
		return;
	}

	pm_stay_awake(&pd->dev);
	pd->sbu_run_work = true;

	if (state == ADC_TM_HIGH_STATE) {
		pd->sbu_tm_state = ADC_TM_HIGH_STATE;
	} else {
		pd->sbu_tm_state = ADC_TM_LOW_STATE;
	}
	schedule_delayed_work(&pd->sbu_adc_work, msecs_to_jiffies(1000));
}

static void pd_init_edge_adc_work(struct work_struct *w)
{
	struct usbpd *pd = container_of(w, struct usbpd, init_edge_adc_work.work);
	int ret;
	static int boot_skip;

	if (!pd->edge_sel)
		return;

	mutex_lock(&pd->moisture_lock);
	usbpd_info(&pd->dev, "[moisture] %s\n", __func__);
	if (pd->prop_moisture_en == DUAL_ROLE_PROP_MOISTURE_EN_DISABLE) {
		if (lge_get_board_rev_no() >= HW_REV_1_0)
			gpiod_direction_output(pd->edge_sel, 0);
		goto out;
	}
	if (IS_ERR_OR_NULL(pd->adc_tm_dev)) {
		 pd->adc_tm_dev = qpnp_get_adc_tm(pd->dev.parent, "moisture-detection");
		 if (IS_ERR(pd->adc_tm_dev)) {
			 if (PTR_ERR(pd->adc_tm_dev) == -EPROBE_DEFER) {
				 usbpd_err(&pd->dev, "qpnp vadc not yet "
					"probed.\n");
				 schedule_delayed_work(&pd->init_edge_adc_work,
						 msecs_to_jiffies(200));
				 goto out;
			 }
		 }
	}
	if (IS_ERR_OR_NULL(pd->vadc_dev)) {
		 pd->vadc_dev = qpnp_get_vadc(pd->dev.parent, "moisture-detection");
		 if (IS_ERR(pd->vadc_dev)) {
			 if (PTR_ERR(pd->vadc_dev) == -EPROBE_DEFER) {
				 usbpd_err(&pd->dev, "qpnp vadc not yet "
					"probed.\n");
				 schedule_delayed_work(&pd->init_edge_adc_work,
						 msecs_to_jiffies(200));
				 goto out;
			 }
		 }
	}
	pd->adc_initialized = true;

	if (!boot_skip) {
		boot_skip = 1;
		goto out;
	}

	gpiod_direction_output(pd->edge_sel, 1);
	pd->edge_lock = false;
	pd->edge_adc_state = ADC_STATE_DRY;
	pd->edge_adc_param.low_thr = adc_edge_low_threshold;
	pd->edge_adc_param.high_thr = adc_edge_high_threshold;
	pd->edge_adc_param.timer_interval = adc_meas_interval;
	pd->edge_adc_param.state_request = ADC_TM_HIGH_LOW_THR_ENABLE;
	pd->edge_adc_param.btm_ctx = pd;
	pd->edge_adc_param.threshold_notification = pd_edge_notification;
	pd->edge_adc_param.channel = VADC_AMUX_THM1_PU2; // EDGE
	ret = qpnp_adc_tm_channel_measure(pd->adc_tm_dev, &pd->edge_adc_param);
	if (ret) {
		usbpd_err(&pd->dev, "request ADC error %d\n", ret);
		goto out;
	}
out:
	mutex_unlock(&pd->moisture_lock);
}

static void pd_init_sbu_adc_work(struct work_struct *w)
{
	struct usbpd *pd = container_of(w, struct usbpd, init_sbu_adc_work.work);
	struct qpnp_vadc_result results;
	int ret;
	static int boot_skip;

	if (!pd->sbu_sel)
		return;

	mutex_lock(&pd->moisture_lock);
	usbpd_info(&pd->dev, "[moisture] %s\n", __func__);
	if (pd->prop_moisture_en == DUAL_ROLE_PROP_MOISTURE_EN_DISABLE) {
		gpiod_direction_output(pd->sbu_sel, 0);
		goto out;
	}

	if (IS_ERR_OR_NULL(pd->adc_tm_dev)) {
		 pd->adc_tm_dev = qpnp_get_adc_tm(pd->dev.parent, "moisture-detection");
		 if (IS_ERR(pd->adc_tm_dev)) {
			 if (PTR_ERR(pd->adc_tm_dev) == -EPROBE_DEFER) {
				 usbpd_err(&pd->dev, "qpnp vadc not yet "
					"probed.\n");
				 schedule_delayed_work(&pd->init_sbu_adc_work,
						 msecs_to_jiffies(200));
				 goto out;
			 }
		 }
	}
	if (IS_ERR_OR_NULL(pd->vadc_dev)) {
		 pd->vadc_dev = qpnp_get_vadc(pd->dev.parent, "moisture-detection");
		 if (IS_ERR(pd->vadc_dev)) {
			 if (PTR_ERR(pd->vadc_dev) == -EPROBE_DEFER) {
				 usbpd_err(&pd->dev, "qpnp vadc not yet "
					"probed.\n");
				 schedule_delayed_work(&pd->init_sbu_adc_work,
						 msecs_to_jiffies(200));
				 goto out;
			 }
		 }
	}

	pd->adc_initialized = true;

	if (pd->pullup_volt == HW_PULLUP_NONE) {
		qpnp_vadc_pullup_volt_chk(pd->vadc_dev, VADC_AMUX_THM2, &results);
		usbpd_info(&pd->dev, "[moisture] %s: sbu pullup adc = %d(%s)\n", __func__,
				(int)results.physical,
				(int)results.physical < 1100000 ? "1V" : "1.8V");
		if ((int)results.physical < 1100000) {
			pd->pullup_volt = HW_PULLUP_1V;
			adc_low_threshold = 714000; // 470K ohm
			adc_high_threshold = 846000; // 1100K ohm
			adc_gnd_low_threshold = 20000; //4K ohm
			adc_gnd_high_threshold = 61000; //13K ohm
		} else {
			pd->pullup_volt = HW_PULLUP_1V8;
			adc_low_threshold = 1280000; // 470K ohm
			adc_high_threshold = 1520000; // 1100K ohm
			adc_gnd_low_threshold = 35000; //4K ohm
			adc_gnd_high_threshold = 110000; //13K ohm
		}
	}

	if (!boot_skip) {
		boot_skip = 1;
		goto out;
	}

	usbpd_dbg(&pd->dev, "[moisture] sbu switch on\n");
	gpiod_direction_output(pd->sbu_oe, 0); //sbu oe enable
	gpiod_direction_output(pd->sbu_sel, 1);
	pd->sbu_lock = false;
	pd->sbu_adc_state = ADC_STATE_DRY;
	pd->sbu_adc_param.low_thr = adc_low_threshold;
	pd->sbu_adc_param.high_thr = adc_high_threshold;
	pd->sbu_adc_param.timer_interval = adc_meas_interval;
	pd->sbu_adc_param.state_request = ADC_TM_HIGH_LOW_THR_ENABLE;
	pd->sbu_adc_param.btm_ctx = pd;
	pd->sbu_adc_param.threshold_notification = pd_sbu_notification;
	pd->sbu_adc_param.channel = VADC_AMUX_THM2; // SBU
	ret = qpnp_adc_tm_channel_measure(pd->adc_tm_dev, &pd->sbu_adc_param);
	if (ret) {
		usbpd_err(&pd->dev, "request ADC error %d\n", ret);
		goto out;
	}
out:
	mutex_unlock(&pd->moisture_lock);
}
#endif

struct usbpd *devm_usbpd_get_by_phandle(struct device *dev, const char *phandle)
{
	struct usbpd **ptr, *pd = NULL;
	struct device_node *pd_np;
	struct platform_device *pdev;
	struct device *pd_dev;

	if (!usbpd_class.p) /* usbpd_init() not yet called */
		return ERR_PTR(-EAGAIN);

	if (!dev->of_node)
		return ERR_PTR(-EINVAL);

	pd_np = of_parse_phandle(dev->of_node, phandle, 0);
	if (!pd_np)
		return ERR_PTR(-ENXIO);

	pdev = of_find_device_by_node(pd_np);
	if (!pdev)
		return ERR_PTR(-ENODEV);

	pd_dev = class_find_device(&usbpd_class, NULL, &pdev->dev,
			match_usbpd_device);
	if (!pd_dev) {
		platform_device_put(pdev);
		/* device was found but maybe hadn't probed yet, so defer */
		return ERR_PTR(-EPROBE_DEFER);
	}

	ptr = devres_alloc(devm_usbpd_put, sizeof(*ptr), GFP_KERNEL);
	if (!ptr) {
		put_device(pd_dev);
		platform_device_put(pdev);
		return ERR_PTR(-ENOMEM);
	}

	pd = dev_get_drvdata(pd_dev);
	if (!pd)
		return ERR_PTR(-EPROBE_DEFER);

	*ptr = pd;
	devres_add(dev, ptr);

	return pd;
}
EXPORT_SYMBOL(devm_usbpd_get_by_phandle);

static int num_pd_instances;

/**
 * usbpd_create - Create a new instance of USB PD protocol/policy engine
 * @parent - parent device to associate with
 *
 * This creates a new usbpd class device which manages the state of a
 * USB PD-capable port. The parent device that is passed in should be
 * associated with the physical device port, e.g. a PD PHY.
 *
 * Return: struct usbpd pointer, or an ERR_PTR value
 */
struct usbpd *usbpd_create(struct device *parent)
{
	int ret;
	struct usbpd *pd;
#if defined(CONFIG_LGE_USB_MOISTURE_DETECTION) && defined(CONFIG_LGE_USB_FACTORY)
	union lge_power_propval lge_val;
#endif
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
	struct pd_phy_params phy_params = {
		.shutdown_cb        = phy_shutdown,
	};
#endif

	pd = kzalloc(sizeof(*pd), GFP_KERNEL);
	if (!pd)
		return ERR_PTR(-ENOMEM);

	device_initialize(&pd->dev);
	pd->dev.class = &usbpd_class;
	pd->dev.parent = parent;
	dev_set_drvdata(&pd->dev, pd);

	ret = dev_set_name(&pd->dev, "usbpd%d", num_pd_instances++);
	if (ret)
		goto free_pd;

	ret = device_init_wakeup(&pd->dev, true);
	if (ret)
		goto free_pd;

	ret = device_add(&pd->dev);
	if (ret)
		goto free_pd;

	pd->wq = alloc_ordered_workqueue("usbpd_wq", WQ_FREEZABLE | WQ_HIGHPRI);
	if (!pd->wq) {
		ret = -ENOMEM;
		goto del_pd;
	}
	INIT_WORK(&pd->sm_work, usbpd_sm);
	hrtimer_init(&pd->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	pd->timer.function = pd_timeout;
	mutex_init(&pd->swap_lock);
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
	hrtimer_init(&pd->edge_timer, CLOCK_BOOTTIME, HRTIMER_MODE_REL);
	pd->edge_timer.function = pd_edge_timeout;
	hrtimer_init(&pd->sbu_timer, CLOCK_BOOTTIME, HRTIMER_MODE_REL);
	pd->sbu_timer.function = pd_sbu_timeout;
	mutex_init(&pd->moisture_lock);
#endif

	pd->usb_psy = power_supply_get_by_name("usb");
	if (!pd->usb_psy) {
		usbpd_dbg(&pd->dev, "Could not get USB power_supply, deferring probe\n");
		ret = -EPROBE_DEFER;
		goto destroy_wq;
	}

	/*
	 * associate extcon with the parent dev as it could have a DT
	 * node which will be useful for extcon_get_edev_by_phandle()
	 */
	pd->extcon = devm_extcon_dev_allocate(parent, usbpd_extcon_cable);
	if (IS_ERR(pd->extcon)) {
		usbpd_err(&pd->dev, "failed to allocate extcon device\n");
		ret = PTR_ERR(pd->extcon);
		goto put_psy;
	}

	pd->extcon->mutually_exclusive = usbpd_extcon_exclusive;
	ret = devm_extcon_dev_register(parent, pd->extcon);
	if (ret) {
		usbpd_err(&pd->dev, "failed to register extcon device\n");
		goto put_psy;
	}

	pd->vbus = devm_regulator_get(parent, "vbus");
	if (IS_ERR(pd->vbus)) {
		ret = PTR_ERR(pd->vbus);
		goto put_psy;
	}

	pd->vconn = devm_regulator_get(parent, "vconn");
	if (IS_ERR(pd->vconn)) {
		ret = PTR_ERR(pd->vconn);
		goto put_psy;
	}

	pd->vconn_is_external = device_property_present(parent,
					"qcom,vconn-uses-external-source");

	pd->num_sink_caps = device_property_read_u32_array(parent,
			"qcom,default-sink-caps", NULL, 0);
	if (pd->num_sink_caps > 0) {
		int i;
		u32 sink_caps[14];

		if (pd->num_sink_caps % 2 || pd->num_sink_caps > 14) {
			ret = -EINVAL;
			usbpd_err(&pd->dev, "default-sink-caps must be be specified as voltage/current, max 7 pairs\n");
			goto put_psy;
		}

		ret = device_property_read_u32_array(parent,
				"qcom,default-sink-caps", sink_caps,
				pd->num_sink_caps);
		if (ret) {
			usbpd_err(&pd->dev, "Error reading default-sink-caps\n");
			goto put_psy;
		}

		pd->num_sink_caps /= 2;

		for (i = 0; i < pd->num_sink_caps; i++) {
			int v = sink_caps[i * 2] / 50;
			int c = sink_caps[i * 2 + 1] / 10;

			pd->sink_caps[i] =
				PD_SNK_PDO_FIXED(0, 0, 0, 0, 0, v, c);
		}

		/* First PDO includes additional capabilities */
		pd->sink_caps[0] |= PD_SNK_PDO_FIXED(1, 0, 0, 1, 1, 0, 0);
	} else {
		memcpy(pd->sink_caps, default_snk_caps,
				sizeof(default_snk_caps));
		pd->num_sink_caps = ARRAY_SIZE(default_snk_caps);
	}

	/*
	 * Register the Android dual-role class (/sys/class/dual_role_usb/).
	 * The first instance should be named "otg_default" as that's what
	 * Android expects.
	 * Note this is different than the /sys/class/usbpd/ created above.
	 */
	pd->dr_desc.name = (num_pd_instances == 1) ?
				"otg_default" : dev_name(&pd->dev);
	pd->dr_desc.supported_modes = DUAL_ROLE_SUPPORTED_MODES_DFP_AND_UFP;
	pd->dr_desc.properties = usbpd_dr_properties;
	pd->dr_desc.num_properties = ARRAY_SIZE(usbpd_dr_properties);
	pd->dr_desc.get_property = usbpd_dr_get_property;
	pd->dr_desc.set_property = usbpd_dr_set_property;
	pd->dr_desc.property_is_writeable = usbpd_dr_prop_writeable;

	pd->dual_role = devm_dual_role_instance_register(&pd->dev,
			&pd->dr_desc);
	if (IS_ERR(pd->dual_role)) {
		usbpd_err(&pd->dev, "could not register dual_role instance\n");
		goto put_psy;
	} else {
		pd->dual_role->drv_data = pd;
	}

	/* default support as PD 2.0 source or sink */
	pd->spec_rev = USBPD_REV_20;
	pd->current_pr = PR_NONE;
	pd->current_dr = DR_NONE;
	list_add_tail(&pd->instance, &_usbpd);

	spin_lock_init(&pd->rx_lock);
	INIT_LIST_HEAD(&pd->rx_q);
	INIT_LIST_HEAD(&pd->svid_handlers);
	init_completion(&pd->is_ready);

#ifdef CONFIG_LGE_USB_FACTORY
	pd->lge_power_cd = lge_power_get_by_name("lge_cable_detect");
#endif
#ifdef CONFIG_LGE_USB_DEBUGGER
	INIT_WORK(&pd->usb_debugger_work, usb_debugger_work);
	pd->uart_sbu_sel_gpio = devm_gpiod_get(parent,"lge,uart-sbu-sel",GPIOD_OUT_LOW);
	if (IS_ERR(pd->uart_sbu_sel_gpio)) {
		usbpd_err(&pd->dev, "Unable to uart_sbu gpio\n");
	}
	usbpd_info(&pd->dev,"USB Debugger Initialized\n");
#endif
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
	INIT_DELAYED_WORK(&pd->init_edge_adc_work, pd_init_edge_adc_work);
	INIT_DELAYED_WORK(&pd->init_sbu_adc_work, pd_init_sbu_adc_work);

	INIT_DELAYED_WORK(&pd->sbu_ov_adc_work, pd_sbu_ov_adc_work);
	INIT_DELAYED_WORK(&pd->sbu_adc_work, pd_sbu_adc_work);
	pd->sbu_sel = devm_gpiod_get(parent, "lge,sbu-sel", GPIOD_OUT_LOW);
	if (IS_ERR(pd->sbu_sel)) {
		usbpd_err(&pd->dev, "Unable to sbu gpio\n");
		pd->sbu_sel = NULL;
	}
	pd->sbu_oe = devm_gpiod_get(parent, "lge,sbu-oe", GPIOD_OUT_LOW);
	if (IS_ERR(pd->sbu_oe)) {
		usbpd_err(&pd->dev, "Unable to sbu-oe gpio\n");
		pd->sbu_oe = NULL;
	}

	INIT_DELAYED_WORK(&pd->edge_adc_work, pd_edge_adc_work);
	pd->edge_sel = devm_gpiod_get(parent, "lge,edge-sel", GPIOD_OUT_LOW);
	if (IS_ERR(pd->edge_sel)) {
		usbpd_err(&pd->dev, "Unable to edge gpio\n");
		pd->edge_sel = NULL;
	}
#ifdef CONFIG_MACH_MSM8998_JOAN_VZW
	if (lge_get_board_rev_no() < HW_REV_D) {
		usbpd_err(&pd->dev, "Not support edge detection\n");
		pd->edge_sel = NULL;
	}
#else
	if (lge_get_board_rev_no() < HW_REV_B) {
		usbpd_err(&pd->dev, "Not support edge detection\n");
        pd->edge_sel = NULL;
	}
#endif

	ret = pd_phy_open(&phy_params);
	if (ret) {
		usbpd_err(&pd->dev, "error opening PD PHY %d\n",
				ret);
	} else {
		pd_phy_close();
	}
#ifdef CONFIG_LGE_USB_FACTORY
	if (lge_get_factory_boot()) {
		pd->sbu_sel = NULL;
		pd->edge_sel = NULL;
	} else {
		if(!pd->lge_power_cd){
			usbpd_err(&pd->dev, "lge_power_cd is not registered\n");
		} else {
			ret = pd->lge_power_cd->get_property(pd->lge_power_cd,
					LGE_POWER_PROP_IS_FACTORY_CABLE,
					&lge_val);
		}
		if(ret != 0) {
			usbpd_err(&pd->dev, "usb id only check fail\n");
		} else if (lge_val.intval == FACTORY_CABLE) {
			usbpd_info(&pd->dev, "factory cable connected, disable moisture detection\n");
			pd->sbu_sel = NULL;
			pd->edge_sel = NULL;
		}
	}
#endif
#endif
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
	schedule_delayed_work(&pd->init_edge_adc_work, 0);
	schedule_delayed_work(&pd->init_sbu_adc_work, 0);
	pd->prop_moisture_en = DUAL_ROLE_PROP_MOISTURE_EN_ENABLE;
#ifdef CONFIG_LGE_USB_COMPLIANCE_TEST
	pd->prop_moisture_en = DUAL_ROLE_PROP_MOISTURE_EN_DISABLE;
#endif
#endif

	pd->psy_nb.notifier_call = psy_changed;
	ret = power_supply_reg_notifier(&pd->psy_nb);
	if (ret)
		goto del_inst;

	/* force read initial power_supply values */
	psy_changed(&pd->psy_nb, PSY_EVENT_PROP_CHANGED, pd->usb_psy);

	return pd;

del_inst:
	list_del(&pd->instance);
put_psy:
	power_supply_put(pd->usb_psy);
destroy_wq:
	destroy_workqueue(pd->wq);
del_pd:
	device_del(&pd->dev);
free_pd:
	num_pd_instances--;
	kfree(pd);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL(usbpd_create);

/**
 * usbpd_destroy - Removes and frees a usbpd instance
 * @pd: the instance to destroy
 */
void usbpd_destroy(struct usbpd *pd)
{
	if (!pd)
		return;

	list_del(&pd->instance);
	power_supply_unreg_notifier(&pd->psy_nb);
	power_supply_put(pd->usb_psy);
	destroy_workqueue(pd->wq);
	device_del(&pd->dev);
	kfree(pd);
}
EXPORT_SYMBOL(usbpd_destroy);

static int __init usbpd_init(void)
{
	usbpd_ipc_log = ipc_log_context_create(NUM_LOG_PAGES, "usb_pd", 0);
	return class_register(&usbpd_class);
}
module_init(usbpd_init);

static void __exit usbpd_exit(void)
{
	class_unregister(&usbpd_class);
}
module_exit(usbpd_exit);

MODULE_DESCRIPTION("USB Power Delivery Policy Engine");
MODULE_LICENSE("GPL v2");
