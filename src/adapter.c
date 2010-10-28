/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2006-2010  Nokia Corporation
 *  Copyright (C) 2004-2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/ioctl.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <gdbus.h>

#include "log.h"
#include "textfile.h"

#include "hcid.h"
#include "sdpd.h"
#include "manager.h"
#include "adapter.h"
#include "device.h"
#include "dbus-common.h"
#include "event.h"
#include "error.h"
#include "glib-helper.h"
#include "agent.h"
#include "storage.h"

#define IO_CAPABILITY_DISPLAYONLY	0x00
#define IO_CAPABILITY_DISPLAYYESNO	0x01
#define IO_CAPABILITY_KEYBOARDONLY	0x02
#define IO_CAPABILITY_NOINPUTNOOUTPUT	0x03
#define IO_CAPABILITY_INVALID		0xFF

/* Limited Discoverable bit mask in CoD */
#define LIMITED_BIT			0x002000

#define check_address(address) bachk(address)

static DBusConnection *connection = NULL;
static GSList *adapter_drivers = NULL;

static GSList *ops_candidates = NULL;

const struct btd_adapter_ops *adapter_ops = NULL;

struct session_req {
	struct btd_adapter	*adapter;
	DBusConnection		*conn;		/* Connection reference */
	DBusMessage		*msg;		/* Unreplied message ref */
	char			*owner;		/* Bus name of the owner */
	guint			id;		/* Listener id */
	uint8_t			mode;		/* Requested mode */
	int			refcount;	/* Session refcount */
	gboolean		got_reply;	/* Agent reply received */
};

struct service_auth {
	service_auth_cb cb;
	void *user_data;
	struct btd_device *device;
	struct btd_adapter *adapter;
};

struct btd_adapter {
	uint16_t dev_id;
	int up;
	char *path;			/* adapter object path */
	bdaddr_t bdaddr;		/* adapter Bluetooth Address */
	guint discov_timeout_id;	/* discoverable timeout id */
	guint stop_discov_id;		/* stop inquiry/scanning id */
	uint32_t discov_timeout;	/* discoverable time(sec) */
	guint pairable_timeout_id;	/* pairable timeout id */
	uint32_t pairable_timeout;	/* pairable time(sec) */
	uint8_t scan_mode;		/* scan mode: SCAN_DISABLED, SCAN_PAGE,
					 * SCAN_INQUIRY */
	uint8_t mode;			/* off, connectable, discoverable,
					 * limited */
	uint8_t global_mode;		/* last valid global mode */
	struct session_req *pending_mode;
	int state;			/* standard inq, periodic inq, name
					 * resolving, suspended discovery */
	GSList *found_devices;
	GSList *oor_devices;		/* out of range device list */
	struct agent *agent;		/* For the new API */
	guint auth_idle_id;		/* Ongoing authorization */
	GSList *connections;		/* Connected devices */
	GSList *devices;		/* Devices structure pointers */
	GSList *mode_sessions;		/* Request Mode sessions */
	GSList *disc_sessions;		/* Discovery sessions */
	guint scheduler_id;		/* Scheduler handle */
	sdp_list_t *services;		/* Services associated to adapter */

	struct hci_dev dev;		/* hci info */
	int8_t tx_power;		/* inq response tx power level */
	gboolean pairable;		/* pairable state */

	gboolean initialized;
	gboolean already_up;		/* adapter was already up on init */

	gboolean off_requested;		/* DEVDOWN ioctl was called */

	uint32_t current_cod;		/* Adapter's current class */
	uint32_t pending_cod;
	uint32_t wanted_cod;		/* CoD cache */

	gboolean cache_enable;

	gint ref;

	GSList *powered_callbacks;

	gboolean name_stored;
};

static void adapter_set_pairable_timeout(struct btd_adapter *adapter,
					guint interval);

static inline DBusMessage *invalid_args(DBusMessage *msg)
{
	return g_dbus_create_error(msg, ERROR_INTERFACE ".InvalidArguments",
			"Invalid arguments in method call");
}

static inline DBusMessage *adapter_not_ready(DBusMessage *msg)
{
	return g_dbus_create_error(msg, ERROR_INTERFACE ".NotReady",
			"Adapter is not ready");
}

static inline DBusMessage *failed_strerror(DBusMessage *msg, int err)
{
	return g_dbus_create_error(msg, ERROR_INTERFACE ".Failed",
							"%s", strerror(err));
}

static inline DBusMessage *not_in_progress(DBusMessage *msg, const char *str)
{
	return g_dbus_create_error(msg, ERROR_INTERFACE ".NotInProgress",
								"%s", str);
}

static inline DBusMessage *not_authorized(DBusMessage *msg)
{
	return g_dbus_create_error(msg, ERROR_INTERFACE ".NotAuthorized",
			"Not authorized");
}

static int found_device_cmp(const struct remote_dev_info *d1,
			const struct remote_dev_info *d2)
{
	int ret;

	if (bacmp(&d2->bdaddr, BDADDR_ANY)) {
		ret = bacmp(&d1->bdaddr, &d2->bdaddr);
		if (ret)
			return ret;
	}

	if (d2->name_status != NAME_ANY) {
		ret = (d1->name_status - d2->name_status);
		if (ret)
			return ret;
	}

	return 0;
}

static void dev_info_free(struct remote_dev_info *dev)
{
	g_free(dev->name);
	g_free(dev->alias);
	g_free(dev);
}

/*
 * Device name expansion
 *   %d - device id
 */
static char *expand_name(char *dst, int size, char *str, int dev_id)
{
	register int sp, np, olen;
	char *opt, buf[10];

	if (!str || !dst)
		return NULL;

	sp = np = 0;
	while (np < size - 1 && str[sp]) {
		switch (str[sp]) {
		case '%':
			opt = NULL;

			switch (str[sp+1]) {
			case 'd':
				sprintf(buf, "%d", dev_id);
				opt = buf;
				break;

			case 'h':
				opt = main_opts.host_name;
				break;

			case '%':
				dst[np++] = str[sp++];
				/* fall through */
			default:
				sp++;
				continue;
			}

			if (opt) {
				/* substitute */
				olen = strlen(opt);
				if (np + olen < size - 1)
					memcpy(dst + np, opt, olen);
				np += olen;
			}
			sp += 2;
			continue;

		case '\\':
			sp++;
			/* fall through */
		default:
			dst[np++] = str[sp++];
			break;
		}
	}
	dst[np] = '\0';
	return dst;
}

static void update_ext_inquiry_response(struct btd_adapter *adapter)
{
	uint8_t data[240];
	struct hci_dev *dev = &adapter->dev;
	int ret;

	if (!(dev->features[6] & LMP_EXT_INQ))
		return;

	memset(data, 0, sizeof(data));

	if (dev->ssp_mode > 0)
		create_ext_inquiry_response((char *) dev->name,
						adapter->tx_power,
						adapter->services, data);

	ret = adapter_ops->write_eir_data(adapter->dev_id, data);
	if (ret < 0)
		error("Can't write extended inquiry response: %s (%d)",
						strerror(-ret), -ret);
}

static int adapter_set_service_classes(struct btd_adapter *adapter,
							uint8_t value)
{
	int err;

	/* Update only the service class, keep the limited bit,
	 * major/minor class bits intact */
	adapter->wanted_cod &= 0x00ffff;
	adapter->wanted_cod |= (value << 16);

	/* If the cache is enabled or an existing CoD write is in progress
	 * just bail out */
	if (adapter->cache_enable || adapter->pending_cod)
		return 0;

	/* If we already have the CoD we want, update EIR and return */
	if (adapter->current_cod == adapter->wanted_cod) {
		update_ext_inquiry_response(adapter);
		return 0;
	}

	DBG("Changing service classes to 0x%06x", adapter->wanted_cod);

	err = adapter_ops->set_class(adapter->dev_id, adapter->wanted_cod);
	if (err < 0)
		error("Adapter class update failed: %s(%d)",
						strerror(err), err);
	else
		adapter->pending_cod = adapter->wanted_cod;

	return err;
}

int btd_adapter_set_class(struct btd_adapter *adapter, uint8_t major,
								uint8_t minor)
{
	int err;

	/* Update only the major and minor class bits keeping remaining bits
	 * intact*/
	adapter->wanted_cod &= 0xffe000;
	adapter->wanted_cod |= ((major & 0x1f) << 8) | minor;

	if (adapter->wanted_cod == adapter->current_cod ||
			adapter->cache_enable || adapter->pending_cod)
		return 0;

	DBG("Changing Major/Minor class to 0x%06x", adapter->wanted_cod);

	err = adapter_ops->set_class(adapter->dev_id, adapter->wanted_cod);
	if (err < 0)
		error("Adapter class update failed: %s(%d)",
						strerror(err), err);
	else
		adapter->pending_cod = adapter->wanted_cod;

	return err;
}

static int pending_remote_name_cancel(struct btd_adapter *adapter)
{
	struct remote_dev_info *dev, match;
	int err;

	/* find the pending remote name request */
	memset(&match, 0, sizeof(struct remote_dev_info));
	bacpy(&match.bdaddr, BDADDR_ANY);
	match.name_status = NAME_REQUESTED;

	dev = adapter_search_found_devices(adapter, &match);
	if (!dev) /* no pending request */
		return -ENODATA;

	err = adapter_ops->cancel_resolve_name(adapter->dev_id, &dev->bdaddr);
	if (err < 0)
		error("Remote name cancel failed: %s(%d)",
						strerror(errno), errno);
	return err;
}

int adapter_resolve_names(struct btd_adapter *adapter)
{
	struct remote_dev_info *dev, match;
	int err;

	memset(&match, 0, sizeof(struct remote_dev_info));
	bacpy(&match.bdaddr, BDADDR_ANY);
	match.name_status = NAME_REQUIRED;

	dev = adapter_search_found_devices(adapter, &match);
	if (!dev)
		return -ENODATA;

	/* send at least one request or return failed if the list is empty */
	do {
		/* flag to indicate the current remote name requested */
		dev->name_status = NAME_REQUESTED;

		err = adapter_ops->resolve_name(adapter->dev_id, &dev->bdaddr);

		if (!err)
			break;

		error("Unable to send HCI remote name req: %s (%d)",
						strerror(errno), errno);

		/* if failed, request the next element */
		/* remove the element from the list */
		adapter_remove_found_device(adapter, &dev->bdaddr);

		/* get the next element */
		dev = adapter_search_found_devices(adapter, &match);
	} while (dev);

	return err;
}

static const char *mode2str(uint8_t mode)
{
	switch(mode) {
	case MODE_OFF:
		return "off";
	case MODE_CONNECTABLE:
		return "connectable";
	case MODE_DISCOVERABLE:
	case MODE_LIMITED:
		return "discoverable";
	default:
		return "unknown";
	}
}

static uint8_t get_mode(const bdaddr_t *bdaddr, const char *mode)
{
	if (strcasecmp("off", mode) == 0)
		return MODE_OFF;
	else if (strcasecmp("connectable", mode) == 0)
		return MODE_CONNECTABLE;
	else if (strcasecmp("discoverable", mode) == 0)
		return MODE_DISCOVERABLE;
	else if (strcasecmp("limited", mode) == 0)
		return MODE_LIMITED;
	else if (strcasecmp("on", mode) == 0) {
		char onmode[14], srcaddr[18];

		ba2str(bdaddr, srcaddr);
		if (read_on_mode(srcaddr, onmode, sizeof(onmode)) < 0)
			return MODE_CONNECTABLE;

		return get_mode(bdaddr, onmode);
	} else
		return MODE_UNKNOWN;
}

static void adapter_remove_discov_timeout(struct btd_adapter *adapter)
{
	if (!adapter)
		return;

	if(adapter->discov_timeout_id == 0)
		return;

	g_source_remove(adapter->discov_timeout_id);
	adapter->discov_timeout_id = 0;
}

static gboolean discov_timeout_handler(gpointer user_data)
{
	struct btd_adapter *adapter = user_data;

	adapter->discov_timeout_id = 0;

	adapter_ops->set_connectable(adapter->dev_id);

	return FALSE;
}

static void adapter_set_discov_timeout(struct btd_adapter *adapter,
					guint interval)
{
	if (adapter->discov_timeout_id) {
		g_source_remove(adapter->discov_timeout_id);
		adapter->discov_timeout_id = 0;
	}

	if (interval == 0)
		return;

	adapter->discov_timeout_id = g_timeout_add_seconds(interval,
							discov_timeout_handler,
							adapter);
}

static void adapter_set_limited_discoverable(struct btd_adapter *adapter,
							gboolean limited)
{
	/* Check if limited bit needs to be set/reset */
	if (limited)
		adapter->wanted_cod |= LIMITED_BIT;
	else
		adapter->wanted_cod &= ~LIMITED_BIT;

	/* If we dont need the toggling, save an unnecessary CoD write */
	if (adapter->pending_cod ||
			adapter->wanted_cod == adapter->current_cod)
		return;

	if (adapter_ops->set_limited_discoverable(adapter->dev_id,
					adapter->wanted_cod, limited) == 0)
		adapter->pending_cod = adapter->wanted_cod;
}

static struct session_req *session_ref(struct session_req *req)
{
	req->refcount++;

	DBG("%p: ref=%d", req, req->refcount);

	return req;
}

static struct session_req *create_session(struct btd_adapter *adapter,
					DBusConnection *conn, DBusMessage *msg,
					uint8_t mode, GDBusWatchFunction cb)
{
	const char *sender = dbus_message_get_sender(msg);
	struct session_req *req;

	req = g_new0(struct session_req, 1);
	req->adapter = adapter;
	req->conn = dbus_connection_ref(conn);
	req->msg = dbus_message_ref(msg);
	req->mode = mode;

	if (cb == NULL)
		return session_ref(req);

	req->owner = g_strdup(sender);
	req->id = g_dbus_add_disconnect_watch(conn, sender, cb, req, NULL);

	info("%s session %p with %s activated",
		req->mode ? "Mode" : "Discovery", req, sender);

	return session_ref(req);
}

static int adapter_set_mode(struct btd_adapter *adapter, uint8_t mode)
{
	int err;

	if (mode == MODE_CONNECTABLE)
		err = adapter_ops->set_connectable(adapter->dev_id);
	else
		err = adapter_ops->set_discoverable(adapter->dev_id);

	if (err < 0)
		return err;

	if (mode == MODE_CONNECTABLE)
		return 0;

	adapter_remove_discov_timeout(adapter);

	if (adapter->discov_timeout)
		adapter_set_discov_timeout(adapter, adapter->discov_timeout);

	if (mode != MODE_LIMITED && adapter->mode == MODE_LIMITED)
		adapter_set_limited_discoverable(adapter, FALSE);

	return 0;
}

static int set_mode(struct btd_adapter *adapter, uint8_t new_mode,
			DBusMessage *msg)
{
	int err;
	const char *modestr;

	if (adapter->pending_mode != NULL)
		return -EALREADY;

	if (!adapter->up && new_mode != MODE_OFF) {
		err = adapter_ops->set_powered(adapter->dev_id, TRUE);
		if (err < 0)
			return err;

		goto done;
	}

	if (adapter->up && new_mode == MODE_OFF) {
		err = adapter_ops->set_powered(adapter->dev_id, FALSE);
		if (err < 0)
			return err;

		adapter->off_requested = TRUE;

		goto done;
	}

	if (new_mode == adapter->mode)
		return 0;

	err = adapter_set_mode(adapter, new_mode);

	if (err < 0)
		return err;

done:
	modestr = mode2str(new_mode);
	write_device_mode(&adapter->bdaddr, modestr);

	DBG("%s", modestr);

	if (msg != NULL) {
		/* Limited to Discoverable and vice-versa doesn't cause any
		   change to scan mode */
		if (g_str_equal(modestr, mode2str(adapter->mode)) == TRUE) {
			DBusMessage *reply;

			reply = g_dbus_create_reply(msg, DBUS_TYPE_INVALID);

			g_dbus_send_message(connection, reply);
		} else
			/* Wait for mode change to reply */
			adapter->pending_mode = create_session(adapter,
								connection,
								msg, new_mode,
								NULL);
	} else
		/* Nothing to reply just write the new mode */
		adapter->mode = new_mode;

	return 0;
}

static DBusMessage *set_discoverable(DBusConnection *conn, DBusMessage *msg,
				gboolean discoverable, void *data)
{
	struct btd_adapter *adapter = data;
	uint8_t mode;
	int err;

	mode = discoverable ? MODE_DISCOVERABLE : MODE_CONNECTABLE;

	if (mode == MODE_DISCOVERABLE && adapter->pairable &&
					adapter->discov_timeout > 0 &&
					adapter->discov_timeout <= 60)
		mode = MODE_LIMITED;

	if (mode == adapter->mode)
		return dbus_message_new_method_return(msg);

	err = set_mode(adapter, mode, msg);
	if (err < 0)
		return failed_strerror(msg, -err);

	return NULL;
}

static DBusMessage *set_powered(DBusConnection *conn, DBusMessage *msg,
				gboolean powered, void *data)
{
	struct btd_adapter *adapter = data;
	uint8_t mode;
	int err;

	if (powered) {
		mode = get_mode(&adapter->bdaddr, "on");
		return set_discoverable(conn, msg, mode == MODE_DISCOVERABLE,
									data);
	}

	mode = MODE_OFF;

	if (mode == adapter->mode)
		return dbus_message_new_method_return(msg);

	err = set_mode(adapter, mode, msg);
	if (err < 0)
		return failed_strerror(msg, -err);

	return NULL;
}

static DBusMessage *set_pairable(DBusConnection *conn, DBusMessage *msg,
				gboolean pairable, void *data)
{
	struct btd_adapter *adapter = data;
	uint8_t mode;
	int err;

	if (adapter->scan_mode == SCAN_DISABLED)
		return adapter_not_ready(msg);

	if (pairable == adapter->pairable)
		goto done;

	if (!(adapter->scan_mode & SCAN_INQUIRY))
		goto store;

	mode = (pairable && adapter->discov_timeout > 0 &&
				adapter->discov_timeout <= 60) ?
					MODE_LIMITED : MODE_DISCOVERABLE;

	err = set_mode(adapter, mode, NULL);
	if (err < 0 && msg)
		return failed_strerror(msg, -err);

store:

	adapter->pairable = pairable;

	write_device_pairable(&adapter->bdaddr, pairable);

	emit_property_changed(connection, adapter->path,
				ADAPTER_INTERFACE, "Pairable",
				DBUS_TYPE_BOOLEAN, &pairable);

	if (pairable && adapter->pairable_timeout)
		adapter_set_pairable_timeout(adapter,
						adapter->pairable_timeout);

done:
	return msg ? dbus_message_new_method_return(msg) : NULL;
}

static gboolean pairable_timeout_handler(void *data)
{
	set_pairable(NULL, NULL, FALSE, data);

	return FALSE;
}

static void adapter_set_pairable_timeout(struct btd_adapter *adapter,
					guint interval)
{
	if (adapter->pairable_timeout_id) {
		g_source_remove(adapter->pairable_timeout_id);
		adapter->pairable_timeout_id = 0;
	}

	if (interval == 0)
		return;

	adapter->pairable_timeout_id = g_timeout_add_seconds(interval,
						pairable_timeout_handler,
						adapter);
}

static struct session_req *find_session(GSList *list, const char *sender)
{
	GSList *l;

	for (l = list; l; l = l->next) {
		struct session_req *req = l->data;

		if (g_str_equal(req->owner, sender))
			return req;
	}

	return NULL;
}

static uint8_t get_needed_mode(struct btd_adapter *adapter, uint8_t mode)
{
	GSList *l;

	if (adapter->global_mode > mode)
		mode = adapter->global_mode;

	for (l = adapter->mode_sessions; l; l = l->next) {
		struct session_req *req = l->data;

		if (req->mode > mode)
			mode = req->mode;
	}

	return mode;
}

static void stop_discovery(struct btd_adapter *adapter, gboolean suspend)
{
	pending_remote_name_cancel(adapter);

	if (suspend == FALSE) {
		g_slist_foreach(adapter->found_devices,
				(GFunc) dev_info_free, NULL);
		g_slist_free(adapter->found_devices);
		adapter->found_devices = NULL;
	}

	if (adapter->oor_devices) {
		g_slist_free(adapter->oor_devices);
		adapter->oor_devices = NULL;
	}

	/* Reset if suspended, otherwise remove timer (software scheduler)
	   or request inquiry to stop */
	if (adapter->state & STATE_SUSPENDED) {
		adapter->state &= ~STATE_SUSPENDED;
		return;
	}

	if (adapter->scheduler_id) {
		g_source_remove(adapter->scheduler_id);
		adapter->scheduler_id = 0;
		return;
	}

	if (adapter->state & STATE_LE_SCAN)
		adapter_ops->stop_scanning(adapter->dev_id);
	else
		adapter_ops->stop_inquiry(adapter->dev_id);
}

static void session_remove(struct session_req *req)
{
	struct btd_adapter *adapter = req->adapter;

	/* Ignore set_mode session */
	if (req->owner == NULL)
		return;

	DBG("%s session %p with %s deactivated",
		req->mode ? "Mode" : "Discovery", req, req->owner);

	if (req->mode) {
		uint8_t mode;

		adapter->mode_sessions = g_slist_remove(adapter->mode_sessions,
							req);

		mode = get_needed_mode(adapter, adapter->global_mode);

		if (mode == adapter->mode)
			return;

		DBG("Switching to '%s' mode", mode2str(mode));

		set_mode(adapter, mode, NULL);
	} else {
		adapter->disc_sessions = g_slist_remove(adapter->disc_sessions,
							req);

		if (adapter->disc_sessions)
			return;

		DBG("Stopping discovery");

		stop_discovery(adapter, FALSE);
	}
}

static void session_free(struct session_req *req)
{
	if (req->id)
		g_dbus_remove_watch(req->conn, req->id);

	session_remove(req);

	if (req->msg) {
		dbus_message_unref(req->msg);
		if (!req->got_reply && req->mode && req->adapter->agent)
			agent_cancel(req->adapter->agent);
	}

	if (req->conn)
		dbus_connection_unref(req->conn);
	g_free(req->owner);
	g_free(req);
}

static void session_owner_exit(DBusConnection *conn, void *user_data)
{
	struct session_req *req = user_data;

	req->id = 0;

	session_free(req);
}

static void session_unref(struct session_req *req)
{
	req->refcount--;

	DBG("%p: ref=%d", req, req->refcount);

	if (req->refcount)
		return;

	session_free(req);
}

static void confirm_mode_cb(struct agent *agent, DBusError *derr, void *data)
{
	struct session_req *req = data;
	int err;
	DBusMessage *reply;

	req->got_reply = TRUE;

	if (derr && dbus_error_is_set(derr)) {
		reply = dbus_message_new_error(req->msg, derr->name,
						derr->message);
		g_dbus_send_message(req->conn, reply);
		session_unref(req);
		return;
	}

	err = set_mode(req->adapter, req->mode, NULL);
	if (err < 0)
		reply = failed_strerror(req->msg, -err);
	else
		reply = dbus_message_new_method_return(req->msg);

	g_dbus_send_message(req->conn, reply);

	dbus_message_unref(req->msg);
	req->msg = NULL;

	if (!find_session(req->adapter->mode_sessions, req->owner))
		session_unref(req);
}

static DBusMessage *set_discoverable_timeout(DBusConnection *conn,
							DBusMessage *msg,
							uint32_t timeout,
							void *data)
{
	struct btd_adapter *adapter = data;
	const char *path;

	if (adapter->discov_timeout == timeout && timeout == 0)
		return dbus_message_new_method_return(msg);

	if (adapter->scan_mode & SCAN_INQUIRY)
		adapter_set_discov_timeout(adapter, timeout);

	adapter->discov_timeout = timeout;

	write_discoverable_timeout(&adapter->bdaddr, timeout);

	path = dbus_message_get_path(msg);

	emit_property_changed(conn, path,
				ADAPTER_INTERFACE, "DiscoverableTimeout",
				DBUS_TYPE_UINT32, &timeout);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *set_pairable_timeout(DBusConnection *conn,
						DBusMessage *msg,
						uint32_t timeout,
						void *data)
{
	struct btd_adapter *adapter = data;
	const char *path;

	if (adapter->pairable_timeout == timeout && timeout == 0)
		return dbus_message_new_method_return(msg);

	if (adapter->pairable)
		adapter_set_pairable_timeout(adapter, timeout);

	adapter->pairable_timeout = timeout;

	write_pairable_timeout(&adapter->bdaddr, timeout);

	path = dbus_message_get_path(msg);

	emit_property_changed(conn, path,
				ADAPTER_INTERFACE, "PairableTimeout",
				DBUS_TYPE_UINT32, &timeout);

	return dbus_message_new_method_return(msg);
}

void adapter_set_class_complete(bdaddr_t *bdaddr, uint8_t status)
{
	uint8_t class[3];
	struct btd_adapter *adapter;
	int err;

	if (status)
		return;

	adapter = manager_find_adapter(bdaddr);
	if (!adapter) {
		error("Unable to find matching adapter");
		return;
	}

	if (adapter->pending_cod == 0)
		return;

	adapter->current_cod = adapter->pending_cod;
	adapter->pending_cod = 0;

	class[2] = (adapter->current_cod >> 16) & 0xff;
	class[1] = (adapter->current_cod >> 8) & 0xff;
	class[0] = adapter->current_cod & 0xff;

	write_local_class(&adapter->bdaddr, class);

	emit_property_changed(connection, adapter->path,
				ADAPTER_INTERFACE, "Class",
				DBUS_TYPE_UINT32, &adapter->current_cod);

	update_ext_inquiry_response(adapter);

	if (adapter->wanted_cod == adapter->current_cod)
		return;

	if (adapter->wanted_cod & LIMITED_BIT &&
			!(adapter->current_cod & LIMITED_BIT))
		err = adapter_ops->set_limited_discoverable(adapter->dev_id,
						adapter->wanted_cod, TRUE);
	else if (!(adapter->wanted_cod & LIMITED_BIT) &&
					adapter->current_cod & LIMITED_BIT)
		err = adapter_ops->set_limited_discoverable(adapter->dev_id,
						adapter->wanted_cod, FALSE);
	else
		err = adapter_ops->set_class(adapter->dev_id,
							adapter->wanted_cod);

	if (err == 0)
		adapter->pending_cod = adapter->wanted_cod;
}

void adapter_update_tx_power(bdaddr_t *bdaddr, uint8_t status, void *ptr)
{
	struct btd_adapter *adapter;

	if (status)
		return;

	adapter = manager_find_adapter(bdaddr);
	if (!adapter) {
		error("Unable to find matching adapter");
		return;
	}

	adapter->tx_power = *((int8_t *) ptr);

	DBG("inquiry respone tx power level is %d", adapter->tx_power);

	update_ext_inquiry_response(adapter);
}

void adapter_update_local_name(bdaddr_t *bdaddr, uint8_t status, void *ptr)
{
	read_local_name_rp rp;
	struct hci_dev *dev;
	struct btd_adapter *adapter;
	gchar *name;

	if (status)
		return;

	adapter = manager_find_adapter(bdaddr);
	if (!adapter) {
		error("Unable to find matching adapter");
		return;
	}

	dev = &adapter->dev;

	memcpy(&rp, ptr, sizeof(rp));
	if (strncmp((char *) rp.name, (char *) dev->name, MAX_NAME_LENGTH) == 0)
		return;

	strncpy((char *) dev->name, (char *) rp.name, MAX_NAME_LENGTH);

	if (!adapter->name_stored) {
		write_local_name(bdaddr, (char *) dev->name);

		name = g_strdup((char *) dev->name);

		if (connection)
			emit_property_changed(connection, adapter->path,
						ADAPTER_INTERFACE, "Name",
						DBUS_TYPE_STRING, &name);
		g_free(name);
	}

	adapter->name_stored = FALSE;

	update_ext_inquiry_response(adapter);
}

void adapter_setname_complete(bdaddr_t *local, uint8_t status)
{
	struct btd_adapter *adapter;
	int err;

	if (status)
		return;

	adapter = manager_find_adapter(local);
	if (!adapter) {
		error("No matching adapter found");
		return;
	}

	err = adapter_ops->read_name(adapter->dev_id);
	if (err < 0)
		error("Sending getting name command failed: %s (%d)",
						strerror(errno), errno);

}

static DBusMessage *set_name(DBusConnection *conn, DBusMessage *msg,
					const char *name, void *data)
{
	struct btd_adapter *adapter = data;
	struct hci_dev *dev = &adapter->dev;

	if (!g_utf8_validate(name, -1, NULL)) {
		error("Name change failed: supplied name isn't valid UTF-8");
		return invalid_args(msg);
	}

	if (strncmp(name, (char *) dev->name, MAX_NAME_LENGTH) == 0)
		goto done;

	strncpy((char *) adapter->dev.name, name, MAX_NAME_LENGTH);
	write_local_name(&adapter->bdaddr, name);
	emit_property_changed(connection, adapter->path,
					ADAPTER_INTERFACE, "Name",
					DBUS_TYPE_STRING, &name);

	if (adapter->up) {
		int err = adapter_ops->set_name(adapter->dev_id, name);
		if (err < 0)
			return failed_strerror(msg, err);

		adapter->name_stored = TRUE;
		update_ext_inquiry_response(adapter);
	}

done:
	return dbus_message_new_method_return(msg);
}

struct btd_device *adapter_find_device(struct btd_adapter *adapter,
							const char *dest)
{
	struct btd_device *device;
	GSList *l;

	if (!adapter)
		return NULL;

	l = g_slist_find_custom(adapter->devices, dest,
					(GCompareFunc) device_address_cmp);
	if (!l)
		return NULL;

	device = l->data;

	return device;
}

struct btd_device *adapter_find_connection(struct btd_adapter *adapter,
						uint16_t handle)
{
	GSList *l;

	for (l = adapter->connections; l; l = l->next) {
		struct btd_device *device = l->data;

		if (device_has_connection(device, handle))
			return device;
	}

	return NULL;
}

static void adapter_update_devices(struct btd_adapter *adapter)
{
	char **devices;
	int i;
	GSList *l;

	/* Devices */
	devices = g_new0(char *, g_slist_length(adapter->devices) + 1);
	for (i = 0, l = adapter->devices; l; l = l->next, i++) {
		struct btd_device *dev = l->data;
		devices[i] = (char *) device_get_path(dev);
	}

	emit_array_property_changed(connection, adapter->path,
					ADAPTER_INTERFACE, "Devices",
					DBUS_TYPE_OBJECT_PATH, &devices, i);
	g_free(devices);
}

static void adapter_emit_uuids_updated(struct btd_adapter *adapter)
{
	char **uuids;
	int i;
	sdp_list_t *list;

	uuids = g_new0(char *, sdp_list_len(adapter->services) + 1);

	for (i = 0, list = adapter->services; list; list = list->next) {
		char *uuid;
		sdp_record_t *rec = list->data;

		uuid = bt_uuid2string(&rec->svclass);
		if (uuid)
			uuids[i++] = uuid;
	}

	emit_array_property_changed(connection, adapter->path,
			ADAPTER_INTERFACE, "UUIDs", DBUS_TYPE_STRING, &uuids, i);

	g_strfreev(uuids);
}

/*
 * adapter_services_inc_rem - Insert or remove UUID from adapter
 */
static void adapter_service_ins_rem(const bdaddr_t *bdaddr, void *rec,
							gboolean insert)
{
	struct btd_adapter *adapter;
	GSList *adapters;

	adapters = NULL;

	if (bacmp(bdaddr, BDADDR_ANY) != 0) {
		/* Only one adapter */
		adapter = manager_find_adapter(bdaddr);
		if (!adapter)
			return;

		adapters = g_slist_append(adapters, adapter);
	} else
		/* Emit D-Bus msg to all adapters */
		adapters = manager_get_adapters();

	for (; adapters; adapters = adapters->next) {
		adapter = adapters->data;

		if (insert == TRUE)
			adapter->services = sdp_list_insert_sorted(
							adapter->services, rec,
							record_sort);
		else
			adapter->services = sdp_list_remove(adapter->services,
									rec);

		adapter_emit_uuids_updated(adapter);
	}
}

void adapter_service_insert(const bdaddr_t *bdaddr, void *rec)
{
	/* TRUE to include service*/
	adapter_service_ins_rem(bdaddr, rec, TRUE);
}

void adapter_service_remove(const bdaddr_t *bdaddr, void *rec)
{
	/* FALSE to remove service*/
	adapter_service_ins_rem(bdaddr, rec, FALSE);
}

sdp_list_t *adapter_get_services(struct btd_adapter *adapter)
{
	return adapter->services;
}

struct btd_device *adapter_create_device(DBusConnection *conn,
						struct btd_adapter *adapter,
						const char *address)
{
	struct btd_device *device;
	const char *path;

	DBG("%s", address);

	device = device_create(conn, adapter, address);
	if (!device)
		return NULL;

	device_set_temporary(device, TRUE);

	adapter->devices = g_slist_append(adapter->devices, device);

	path = device_get_path(device);
	g_dbus_emit_signal(conn, adapter->path,
			ADAPTER_INTERFACE, "DeviceCreated",
			DBUS_TYPE_OBJECT_PATH, &path,
			DBUS_TYPE_INVALID);

	adapter_update_devices(adapter);

	return device;
}

void adapter_remove_device(DBusConnection *conn, struct btd_adapter *adapter,
						struct btd_device *device,
						gboolean remove_storage)
{
	const gchar *dev_path = device_get_path(device);
	struct agent *agent;

	adapter->devices = g_slist_remove(adapter->devices, device);
	adapter->connections = g_slist_remove(adapter->connections, device);

	adapter_update_devices(adapter);

	g_dbus_emit_signal(conn, adapter->path,
			ADAPTER_INTERFACE, "DeviceRemoved",
			DBUS_TYPE_OBJECT_PATH, &dev_path,
			DBUS_TYPE_INVALID);

	agent = device_get_agent(device);

	if (agent && device_is_authorizing(device))
		agent_cancel(agent);

	device_remove(device, remove_storage);
}

struct btd_device *adapter_get_device(DBusConnection *conn,
						struct btd_adapter *adapter,
						const gchar *address)
{
	struct btd_device *device;

	DBG("%s", address);

	if (!adapter)
		return NULL;

	device = adapter_find_device(adapter, address);
	if (device)
		return device;

	return adapter_create_device(conn, adapter, address);
}

static gboolean stop_scanning(gpointer user_data)
{
	struct btd_adapter *adapter = user_data;

	adapter_ops->stop_scanning(adapter->dev_id);

	return FALSE;
}

static int start_discovery(struct btd_adapter *adapter)
{
	int err, type;

	/* Do not start if suspended */
	if (adapter->state & STATE_SUSPENDED)
		return 0;

	/* Postpone discovery if still resolving names */
	if (adapter->state & STATE_RESOLVNAME)
		return 1;

	pending_remote_name_cancel(adapter);

	type = adapter_get_discover_type(adapter) & ~DISC_RESOLVNAME;

	switch (type) {
	case DISC_STDINQ:
	case DISC_INTERLEAVE:
		err = adapter_ops->start_inquiry(adapter->dev_id,
							0x08, FALSE);
		break;
	case DISC_PINQ:
		err = adapter_ops->start_inquiry(adapter->dev_id,
							0x08, TRUE);
		break;
	case DISC_LE:
		err = adapter_ops->start_scanning(adapter->dev_id);
		break;
	default:
		err = -1;
	}

	return err;
}

static DBusMessage *adapter_start_discovery(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct session_req *req;
	struct btd_adapter *adapter = data;
	const char *sender = dbus_message_get_sender(msg);
	int err;

	if (!adapter->up)
		return adapter_not_ready(msg);

	req = find_session(adapter->disc_sessions, sender);
	if (req) {
		session_ref(req);
		return dbus_message_new_method_return(msg);
	}

	if (adapter->disc_sessions)
		goto done;

	err = start_discovery(adapter);
	if (err < 0)
		return failed_strerror(msg, -err);

done:
	req = create_session(adapter, conn, msg, 0,
				session_owner_exit);

	adapter->disc_sessions = g_slist_append(adapter->disc_sessions, req);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *adapter_stop_discovery(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct btd_adapter *adapter = data;
	struct session_req *req;
	const char *sender = dbus_message_get_sender(msg);

	if (!adapter->up)
		return adapter_not_ready(msg);

	req = find_session(adapter->disc_sessions, sender);
	if (!req)
		return g_dbus_create_error(msg, ERROR_INTERFACE ".Failed",
				"Invalid discovery session");

	session_unref(req);
	info("Stopping discovery");
	return dbus_message_new_method_return(msg);
}

struct remote_device_list_t {
	GSList *list;
	time_t time;
};

static DBusMessage *get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct btd_adapter *adapter = data;
	const char *property;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	char str[MAX_NAME_LENGTH + 1], srcaddr[18];
	gboolean value;
	char **devices, **uuids;
	int i;
	GSList *l;
	sdp_list_t *list;

	ba2str(&adapter->bdaddr, srcaddr);

	if (check_address(srcaddr) < 0)
		return adapter_not_ready(msg);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);

	/* Address */
	property = srcaddr;
	dict_append_entry(&dict, "Address", DBUS_TYPE_STRING, &property);

	/* Name */
	memset(str, 0, sizeof(str));
	strncpy(str, (char *) adapter->dev.name, MAX_NAME_LENGTH);
	property = str;

	dict_append_entry(&dict, "Name", DBUS_TYPE_STRING, &property);

	/* Class */
	dict_append_entry(&dict, "Class",
				DBUS_TYPE_UINT32, &adapter->current_cod);

	/* Powered */
	value = (adapter->up && !adapter->off_requested) ? TRUE : FALSE;
	dict_append_entry(&dict, "Powered", DBUS_TYPE_BOOLEAN, &value);

	/* Discoverable */
	value = adapter->scan_mode & SCAN_INQUIRY ? TRUE : FALSE;
	dict_append_entry(&dict, "Discoverable", DBUS_TYPE_BOOLEAN, &value);

	/* Pairable */
	dict_append_entry(&dict, "Pairable", DBUS_TYPE_BOOLEAN,
				&adapter->pairable);

	/* DiscoverableTimeout */
	dict_append_entry(&dict, "DiscoverableTimeout",
				DBUS_TYPE_UINT32, &adapter->discov_timeout);

	/* PairableTimeout */
	dict_append_entry(&dict, "PairableTimeout",
				DBUS_TYPE_UINT32, &adapter->pairable_timeout);


	if (adapter->state & (STATE_PINQ | STATE_STDINQ | STATE_LE_SCAN))
		value = TRUE;
	else
		value = FALSE;

	/* Discovering */
	dict_append_entry(&dict, "Discovering", DBUS_TYPE_BOOLEAN, &value);

	/* Devices */
	devices = g_new0(char *, g_slist_length(adapter->devices) + 1);
	for (i = 0, l = adapter->devices; l; l = l->next, i++) {
		struct btd_device *dev = l->data;
		devices[i] = (char *) device_get_path(dev);
	}
	dict_append_array(&dict, "Devices", DBUS_TYPE_OBJECT_PATH,
								&devices, i);
	g_free(devices);

	/* UUIDs */
	uuids = g_new0(char *, sdp_list_len(adapter->services) + 1);

	for (i = 0, list = adapter->services; list; list = list->next) {
		sdp_record_t *rec = list->data;
		char *uuid;

		uuid = bt_uuid2string(&rec->svclass);
		if (uuid)
			uuids[i++] = uuid;
	}

	dict_append_array(&dict, "UUIDs", DBUS_TYPE_STRING, &uuids, i);

	g_strfreev(uuids);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static DBusMessage *set_property(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct btd_adapter *adapter = data;
	DBusMessageIter iter;
	DBusMessageIter sub;
	const char *property;
	char srcaddr[18];

	ba2str(&adapter->bdaddr, srcaddr);

	if (!dbus_message_iter_init(msg, &iter))
		return invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &property);
	dbus_message_iter_next(&iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return invalid_args(msg);
	dbus_message_iter_recurse(&iter, &sub);

	if (g_str_equal("Name", property)) {
		const char *name;

		if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_STRING)
			return invalid_args(msg);
		dbus_message_iter_get_basic(&sub, &name);

		return set_name(conn, msg, name, data);
	} else if (g_str_equal("Powered", property)) {
		gboolean powered;

		if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_BOOLEAN)
			return invalid_args(msg);

		dbus_message_iter_get_basic(&sub, &powered);

		return set_powered(conn, msg, powered, data);
	} else if (g_str_equal("Discoverable", property)) {
		gboolean discoverable;

		if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_BOOLEAN)
			return invalid_args(msg);

		dbus_message_iter_get_basic(&sub, &discoverable);

		return set_discoverable(conn, msg, discoverable, data);
	} else if (g_str_equal("DiscoverableTimeout", property)) {
		uint32_t timeout;

		if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_UINT32)
			return invalid_args(msg);

		dbus_message_iter_get_basic(&sub, &timeout);

		return set_discoverable_timeout(conn, msg, timeout, data);
	} else if (g_str_equal("Pairable", property)) {
		gboolean pairable;

		if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_BOOLEAN)
			return invalid_args(msg);

		dbus_message_iter_get_basic(&sub, &pairable);

		return set_pairable(conn, msg, pairable, data);
	} else if (g_str_equal("PairableTimeout", property)) {
		uint32_t timeout;

		if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_UINT32)
			return invalid_args(msg);

		dbus_message_iter_get_basic(&sub, &timeout);

		return set_pairable_timeout(conn, msg, timeout, data);
	}

	return invalid_args(msg);
}

static DBusMessage *request_session(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct btd_adapter *adapter = data;
	struct session_req *req;
	const char *sender = dbus_message_get_sender(msg);
	uint8_t new_mode;
	int err;

	if (!adapter->agent)
		return g_dbus_create_error(msg, ERROR_INTERFACE ".Failed",
						"No agent registered");

	if (!adapter->mode_sessions)
		adapter->global_mode = adapter->mode;

	new_mode = get_mode(&adapter->bdaddr, "on");

	req = find_session(adapter->mode_sessions, sender);
	if (req) {
		session_ref(req);
		return dbus_message_new_method_return(msg);
	} else {
		req = create_session(adapter, conn, msg, new_mode,
					session_owner_exit);
		adapter->mode_sessions = g_slist_append(adapter->mode_sessions,
							req);
	}

	/* No need to change mode */
	if (adapter->mode >= new_mode)
		return dbus_message_new_method_return(msg);

	err = agent_confirm_mode_change(adapter->agent, mode2str(new_mode),
					confirm_mode_cb, req, NULL);
	if (err < 0) {
		session_unref(req);
		return failed_strerror(msg, -err);
	}

	return NULL;
}

static DBusMessage *release_session(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct btd_adapter *adapter = data;
	struct session_req *req;
	const char *sender = dbus_message_get_sender(msg);

	req = find_session(adapter->mode_sessions, sender);
	if (!req)
		return g_dbus_create_error(msg, ERROR_INTERFACE ".Failed",
				"No Mode to release");

	session_unref(req);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *list_devices(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct btd_adapter *adapter = data;
	DBusMessage *reply;
	GSList *l;
	DBusMessageIter iter;
	DBusMessageIter array_iter;
	const gchar *dev_path;

	if (!dbus_message_has_signature(msg, DBUS_TYPE_INVALID_AS_STRING))
		return invalid_args(msg);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
				DBUS_TYPE_OBJECT_PATH_AS_STRING, &array_iter);

	for (l = adapter->devices; l; l = l->next) {
		struct btd_device *device = l->data;

		dev_path = device_get_path(device);

		dbus_message_iter_append_basic(&array_iter,
				DBUS_TYPE_OBJECT_PATH, &dev_path);
	}

	dbus_message_iter_close_container(&iter, &array_iter);

	return reply;
}

static DBusMessage *cancel_device_creation(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct btd_adapter *adapter = data;
	const gchar *address, *sender = dbus_message_get_sender(msg);
	struct btd_device *device;

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &address,
						DBUS_TYPE_INVALID) == FALSE)
		return invalid_args(msg);

	if (check_address(address) < 0)
		return invalid_args(msg);

	device = adapter_find_device(adapter, address);
	if (!device || !device_is_creating(device, NULL))
		return not_in_progress(msg, "Device creation not in progress");

	if (!device_is_creating(device, sender))
		return not_authorized(msg);

	device_set_temporary(device, TRUE);

	if (device_is_connected(device)) {
		device_request_disconnect(device, msg);
		return NULL;
	}

	adapter_remove_device(conn, adapter, device, TRUE);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *create_device(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct btd_adapter *adapter = data;
	struct btd_device *device;
	const gchar *address;

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &address,
						DBUS_TYPE_INVALID) == FALSE)
		return invalid_args(msg);

	if (check_address(address) < 0)
		return invalid_args(msg);

	if (adapter_find_device(adapter, address))
		return g_dbus_create_error(msg,
				ERROR_INTERFACE ".AlreadyExists",
				"Device already exists");

	DBG("%s", address);

	device = adapter_create_device(conn, adapter, address);
	if (!device)
		return NULL;

	device_browse(device, conn, msg, NULL, FALSE);

	return NULL;
}

static uint8_t parse_io_capability(const char *capability)
{
	if (g_str_equal(capability, ""))
		return IO_CAPABILITY_DISPLAYYESNO;
	if (g_str_equal(capability, "DisplayOnly"))
		return IO_CAPABILITY_DISPLAYONLY;
	if (g_str_equal(capability, "DisplayYesNo"))
		return IO_CAPABILITY_DISPLAYYESNO;
	if (g_str_equal(capability, "KeyboardOnly"))
		return IO_CAPABILITY_KEYBOARDONLY;
	if (g_str_equal(capability, "NoInputNoOutput"))
		return IO_CAPABILITY_NOINPUTNOOUTPUT;
	return IO_CAPABILITY_INVALID;
}

static DBusMessage *create_paired_device(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct btd_adapter *adapter = data;
	struct btd_device *device;
	const gchar *address, *agent_path, *capability, *sender;
	uint8_t cap;

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &address,
					DBUS_TYPE_OBJECT_PATH, &agent_path,
					DBUS_TYPE_STRING, &capability,
					DBUS_TYPE_INVALID) == FALSE)
		return invalid_args(msg);

	if (check_address(address) < 0)
		return invalid_args(msg);

	sender = dbus_message_get_sender(msg);
	if (adapter->agent &&
			agent_matches(adapter->agent, sender, agent_path)) {
		error("Refusing adapter agent usage as device specific one");
		return invalid_args(msg);
	}

	cap = parse_io_capability(capability);
	if (cap == IO_CAPABILITY_INVALID)
		return invalid_args(msg);

	device = adapter_get_device(conn, adapter, address);
	if (!device)
		return g_dbus_create_error(msg,
				ERROR_INTERFACE ".Failed",
				"Unable to create a new device object");

	return device_create_bonding(device, conn, msg, agent_path, cap);
}

static gint device_path_cmp(struct btd_device *device, const gchar *path)
{
	const gchar *dev_path = device_get_path(device);

	return strcasecmp(dev_path, path);
}

static DBusMessage *remove_device(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	struct btd_adapter *adapter = data;
	struct btd_device *device;
	const char *path;
	GSList *l;

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
						DBUS_TYPE_INVALID) == FALSE)
		return invalid_args(msg);

	l = g_slist_find_custom(adapter->devices,
			path, (GCompareFunc) device_path_cmp);
	if (!l)
		return g_dbus_create_error(msg,
				ERROR_INTERFACE ".DoesNotExist",
				"Device does not exist");
	device = l->data;

	if (device_is_temporary(device) || device_is_busy(device))
		return g_dbus_create_error(msg,
				ERROR_INTERFACE ".DoesNotExist",
				"Device creation in progress");

	device_set_temporary(device, TRUE);

	if (!device_is_connected(device)) {
		adapter_remove_device(conn, adapter, device, TRUE);
		return dbus_message_new_method_return(msg);
	}

	device_request_disconnect(device, msg);
	return NULL;
}

static DBusMessage *find_device(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	struct btd_adapter *adapter = data;
	struct btd_device *device;
	DBusMessage *reply;
	const gchar *address;
	GSList *l;
	const gchar *dev_path;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &address,
						DBUS_TYPE_INVALID))
		return invalid_args(msg);

	l = g_slist_find_custom(adapter->devices,
			address, (GCompareFunc) device_address_cmp);
	if (!l)
		return g_dbus_create_error(msg,
				ERROR_INTERFACE ".DoesNotExist",
				"Device does not exist");

	device = l->data;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dev_path = device_get_path(device);

	dbus_message_append_args(reply,
				DBUS_TYPE_OBJECT_PATH, &dev_path,
				DBUS_TYPE_INVALID);

	return reply;
}

static void agent_removed(struct agent *agent, struct btd_adapter *adapter)
{
	adapter->agent = NULL;
}

static DBusMessage *register_agent(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	const char *path, *name, *capability;
	struct agent *agent;
	struct btd_adapter *adapter = data;
	uint8_t cap;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
			DBUS_TYPE_STRING, &capability, DBUS_TYPE_INVALID))
		return NULL;

	if (adapter->agent)
		return g_dbus_create_error(msg,
				ERROR_INTERFACE ".AlreadyExists",
				"Agent already exists");

	cap = parse_io_capability(capability);
	if (cap == IO_CAPABILITY_INVALID)
		return invalid_args(msg);

	name = dbus_message_get_sender(msg);

	agent = agent_create(adapter, name, path, cap,
				(agent_remove_cb) agent_removed, adapter);
	if (!agent)
		return g_dbus_create_error(msg,
				ERROR_INTERFACE ".Failed",
				"Failed to create a new agent");

	adapter->agent = agent;

	DBG("Agent registered for hci%d at %s:%s", adapter->dev_id, name,
			path);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *unregister_agent(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	const char *path, *name;
	struct btd_adapter *adapter = data;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
						DBUS_TYPE_INVALID))
		return NULL;

	name = dbus_message_get_sender(msg);

	if (!adapter->agent || !agent_matches(adapter->agent, name, path))
		return g_dbus_create_error(msg,
					ERROR_INTERFACE ".DoesNotExist",
					"No such agent");

	agent_free(adapter->agent);
	adapter->agent = NULL;

	return dbus_message_new_method_return(msg);
}

static GDBusMethodTable adapter_methods[] = {
	{ "GetProperties",	"",	"a{sv}",get_properties		},
	{ "SetProperty",	"sv",	"",	set_property,
						G_DBUS_METHOD_FLAG_ASYNC},
	{ "RequestSession",	"",	"",	request_session,
						G_DBUS_METHOD_FLAG_ASYNC},
	{ "ReleaseSession",	"",	"",	release_session		},
	{ "StartDiscovery",	"",	"",	adapter_start_discovery },
	{ "StopDiscovery",	"",	"",	adapter_stop_discovery,
						G_DBUS_METHOD_FLAG_ASYNC},
	{ "ListDevices",	"",	"ao",	list_devices,
						G_DBUS_METHOD_FLAG_DEPRECATED},
	{ "CreateDevice",	"s",	"o",	create_device,
						G_DBUS_METHOD_FLAG_ASYNC},
	{ "CreatePairedDevice",	"sos",	"o",	create_paired_device,
						G_DBUS_METHOD_FLAG_ASYNC},
	{ "CancelDeviceCreation","s",	"",	cancel_device_creation,
						G_DBUS_METHOD_FLAG_ASYNC},
	{ "RemoveDevice",	"o",	"",	remove_device,
						G_DBUS_METHOD_FLAG_ASYNC},
	{ "FindDevice",		"s",	"o",	find_device		},
	{ "RegisterAgent",	"os",	"",	register_agent		},
	{ "UnregisterAgent",	"o",	"",	unregister_agent	},
	{ }
};

static GDBusSignalTable adapter_signals[] = {
	{ "PropertyChanged",		"sv"		},
	{ "DeviceCreated",		"o"		},
	{ "DeviceRemoved",		"o"		},
	{ "DeviceFound",		"sa{sv}"	},
	{ "DeviceDisappeared",		"s"		},
	{ }
};

static inline uint8_t get_inquiry_mode(struct hci_dev *dev)
{
	if (dev->features[6] & LMP_EXT_INQ)
		return 2;

	if (dev->features[3] & LMP_RSSI_INQ)
		return 1;

	if (dev->manufacturer == 11 &&
			dev->hci_rev == 0x00 && dev->lmp_subver == 0x0757)
		return 1;

	if (dev->manufacturer == 15) {
		if (dev->hci_rev == 0x03 && dev->lmp_subver == 0x6963)
			return 1;
		if (dev->hci_rev == 0x09 && dev->lmp_subver == 0x6963)
			return 1;
		if (dev->hci_rev == 0x00 && dev->lmp_subver == 0x6965)
			return 1;
	}

	if (dev->manufacturer == 31 &&
			dev->hci_rev == 0x2005 && dev->lmp_subver == 0x1805)
		return 1;

	return 0;
}

static int adapter_setup(struct btd_adapter *adapter, const char *mode)
{
	struct hci_dev *dev = &adapter->dev;
	uint8_t events[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0x1f, 0x00, 0x00 };
	uint8_t inqmode;
	int err;
	char name[MAX_NAME_LENGTH + 1];
	uint8_t cls[3];

	if (dev->lmp_ver > 1) {
		if (dev->features[5] & LMP_SNIFF_SUBR)
			events[5] |= 0x20;

		if (dev->features[5] & LMP_PAUSE_ENC)
			events[5] |= 0x80;

		if (dev->features[6] & LMP_EXT_INQ)
			events[5] |= 0x40;

		if (dev->features[6] & LMP_NFLUSH_PKTS)
			events[7] |= 0x01;

		if (dev->features[7] & LMP_LSTO)
			events[6] |= 0x80;

		if (dev->features[6] & LMP_SIMPLE_PAIR) {
			events[6] |= 0x01;	/* IO Capability Request */
			events[6] |= 0x02;	/* IO Capability Response */
			events[6] |= 0x04;	/* User Confirmation Request */
			events[6] |= 0x08;	/* User Passkey Request */
			events[6] |= 0x10;	/* Remote OOB Data Request */
			events[6] |= 0x20;	/* Simple Pairing Complete */
			events[7] |= 0x04;	/* User Passkey Notification */
			events[7] |= 0x08;	/* Keypress Notification */
			events[7] |= 0x10;	/* Remote Host Supported
						 * Features Notification */
		}

		if (dev->features[4] & LMP_LE)
			events[7] |= 0x20;	/* LE Meta-Event */

		adapter_ops->set_event_mask(adapter->dev_id, events,
							sizeof(events));
	}

	inqmode = get_inquiry_mode(dev);
	if (inqmode < 1)
		return 0;

	err = adapter_ops->write_inq_mode(adapter->dev_id, inqmode);
	if (err < 0) {
		error("Can't write inquiry mode for %s: %s (%d)",
					adapter->path, strerror(-err), -err);
		return err;
	}

	if (dev->features[7] & LMP_INQ_TX_PWR)
		adapter_ops->read_inq_tx_pwr(adapter->dev_id);

	if (dev->features[4] & LMP_LE) {
		uint8_t simul = (dev->features[6] & LMP_LE_BREDR) ? 0x01 : 0x00;
		err = adapter_ops->write_le_host(adapter->dev_id, 0x01, simul);
		if (err < 0) {
			error("Can't write LE host supported for %s: %s(%d)",
					adapter->path, strerror(-err), -err);
			return err;
		}
	}

	if (read_local_name(&adapter->bdaddr, name) < 0)
		expand_name(name, MAX_NAME_LENGTH, main_opts.name,
							adapter->dev_id);

	adapter_ops->set_name(adapter->dev_id, name);
	if (g_str_equal(mode, "off"))
		strncpy((char *) adapter->dev.name, name, MAX_NAME_LENGTH);

	/* Set device class */
	if (adapter->initialized && adapter->wanted_cod) {
		cls[1] = (adapter->wanted_cod >> 8) & 0xff;
		cls[0] = adapter->wanted_cod & 0xff;
	} else if (read_local_class(&adapter->bdaddr, cls) < 0) {
		uint32_t class = htobl(main_opts.class);
		if (class)
			memcpy(cls, &class, 3);
		else
			return 0;
	}

	btd_adapter_set_class(adapter, cls[1], cls[0]);

	return 0;
}

static void create_stored_device_from_profiles(char *key, char *value,
						void *user_data)
{
	struct btd_adapter *adapter = user_data;
	GSList *uuids = bt_string2list(value);
	struct btd_device *device;

	if (g_slist_find_custom(adapter->devices,
				key, (GCompareFunc) device_address_cmp))
		return;

	device = device_create(connection, adapter, key);
	if (!device)
		return;

	device_set_temporary(device, FALSE);
	adapter->devices = g_slist_append(adapter->devices, device);

	device_probe_drivers(device, uuids);

	g_slist_foreach(uuids, (GFunc) g_free, NULL);
	g_slist_free(uuids);
}

static void create_stored_device_from_linkkeys(char *key, char *value,
							void *user_data)
{
	struct btd_adapter *adapter = user_data;
	struct btd_device *device;

	if (g_slist_find_custom(adapter->devices, key,
					(GCompareFunc) device_address_cmp))
		return;

	device = device_create(connection, adapter, key);
	if (device) {
		device_set_temporary(device, FALSE);
		adapter->devices = g_slist_append(adapter->devices, device);
	}
}

static void create_stored_device_from_blocked(char *key, char *value,
							void *user_data)
{
	struct btd_adapter *adapter = user_data;
	struct btd_device *device;

	if (g_slist_find_custom(adapter->devices,
				key, (GCompareFunc) device_address_cmp))
		return;

	device = device_create(connection, adapter, key);
	if (device) {
		device_set_temporary(device, FALSE);
		adapter->devices = g_slist_append(adapter->devices, device);
	}
}

static void load_devices(struct btd_adapter *adapter)
{
	char filename[PATH_MAX + 1];
	char srcaddr[18];

	ba2str(&adapter->bdaddr, srcaddr);

	create_name(filename, PATH_MAX, STORAGEDIR, srcaddr, "profiles");
	textfile_foreach(filename, create_stored_device_from_profiles,
								adapter);

	create_name(filename, PATH_MAX, STORAGEDIR, srcaddr, "linkkeys");
	textfile_foreach(filename, create_stored_device_from_linkkeys,
								adapter);

	create_name(filename, PATH_MAX, STORAGEDIR, srcaddr, "blocked");
	textfile_foreach(filename, create_stored_device_from_blocked, adapter);
}

int btd_adapter_block_address(struct btd_adapter *adapter, bdaddr_t *bdaddr)
{
	return adapter_ops->block_device(adapter->dev_id, bdaddr);
}

int btd_adapter_unblock_address(struct btd_adapter *adapter, bdaddr_t *bdaddr)
{
	return adapter_ops->unblock_device(adapter->dev_id, bdaddr);
}

static void clear_blocked(struct btd_adapter *adapter)
{
	int err;

	err = adapter_ops->unblock_device(adapter->dev_id, BDADDR_ANY);
	if (err < 0)
		error("Clearing blocked list failed: %s (%d)",
						strerror(-err), -err);
}

static void probe_driver(gpointer data, gpointer user_data)
{
	struct btd_adapter *adapter = data;
	struct btd_adapter_driver *driver = user_data;
	int err;

	if (!adapter->up)
		return;

	err = driver->probe(adapter);
	if (err < 0)
		error("%s: %s (%d)", driver->name, strerror(-err), -err);
}

static void load_drivers(struct btd_adapter *adapter)
{
	GSList *l;

	for (l = adapter_drivers; l; l = l->next) {
		struct btd_adapter_driver *driver = l->data;

		if (driver->probe == NULL)
			continue;

		probe_driver(adapter, driver);
	}
}

static void load_connections(struct btd_adapter *adapter)
{
	GSList *l, *conns;
	int err;

	err = adapter_ops->get_conn_list(adapter->dev_id, &conns);
	if (err < 0) {
		error("Unable to fetch existing connections: %s (%d)",
							strerror(-err), -err);
		return;
	}

	for (l = conns; l != NULL; l = g_slist_next(l)) {
		struct hci_conn_info *ci = l->data;
		struct btd_device *device;
		char address[18];

		ba2str(&ci->bdaddr, address);
		device = adapter_get_device(connection, adapter, address);
		if (device)
			adapter_add_connection(adapter, device, ci->handle);
	}

	g_slist_foreach(conns, (GFunc) g_free, NULL);
	g_slist_free(conns);
}

static int get_discoverable_timeout(const char *src)
{
	int timeout;

	if (read_discoverable_timeout(src, &timeout) == 0)
		return timeout;

	return main_opts.discovto;
}

static int get_pairable_timeout(const char *src)
{
	int timeout;

	if (read_pairable_timeout(src, &timeout) == 0)
		return timeout;

	return main_opts.pairto;
}

static void adapter_disable_cod_cache(struct btd_adapter *adapter)
{
	int err;

	if (!adapter)
		return;

	if (!adapter->cache_enable)
		return;

	/* Disable and flush svc cache. All successive service class updates
	   will be written to the device */
	adapter->cache_enable = FALSE;

	if (adapter->current_cod == adapter->wanted_cod)
		return;

	err = adapter_ops->set_class(adapter->dev_id, adapter->wanted_cod);
	if (err < 0)
		error("Adapter class update failed: %s(%d)",
						strerror(err), err);
	else
		adapter->pending_cod = adapter->wanted_cod;
}

static void call_adapter_powered_callbacks(struct btd_adapter *adapter,
						gboolean powered)
{
	GSList *l;

	for (l = adapter->powered_callbacks; l; l = l->next) {
		btd_adapter_powered_cb cb = l->data;

		cb(adapter, powered);
       }
}

static void emit_device_disappeared(gpointer data, gpointer user_data)
{
	struct remote_dev_info *dev = data;
	struct btd_adapter *adapter = user_data;
	char address[18];
	const char *paddr = address;

	ba2str(&dev->bdaddr, address);

	g_dbus_emit_signal(connection, adapter->path,
			ADAPTER_INTERFACE, "DeviceDisappeared",
			DBUS_TYPE_STRING, &paddr,
			DBUS_TYPE_INVALID);

	adapter->found_devices = g_slist_remove(adapter->found_devices, dev);
}

static void update_oor_devices(struct btd_adapter *adapter)
{
	g_slist_foreach(adapter->oor_devices, emit_device_disappeared, adapter);
	g_slist_foreach(adapter->oor_devices, (GFunc) dev_info_free, NULL);
	g_slist_free(adapter->oor_devices);
	adapter->oor_devices =  g_slist_copy(adapter->found_devices);
}

static gboolean bredr_capable(struct btd_adapter *adapter)
{
	struct hci_dev *dev = &adapter->dev;

	return (dev->features[4] & LMP_NO_BREDR) == 0 ? TRUE : FALSE;
}

static gboolean le_capable(struct btd_adapter *adapter)
{
	struct hci_dev *dev = &adapter->dev;

	return (dev->features[4] & LMP_LE &&
			dev->extfeatures[0] & LMP_HOST_LE) ? TRUE : FALSE;
}

int adapter_get_discover_type(struct btd_adapter *adapter)
{
	gboolean le, bredr;
	int type;

	le = le_capable(adapter);
	bredr = bredr_capable(adapter);

	if (le)
		type = bredr ? DISC_INTERLEAVE : DISC_LE;
	else
		type = main_opts.discov_interval ? DISC_STDINQ :
							DISC_PINQ;

	if (main_opts.name_resolv)
		type |= DISC_RESOLVNAME;

	return type;
}

static int adapter_up(struct btd_adapter *adapter, const char *mode)
{
	char srcaddr[18];
	uint8_t scan_mode;
	gboolean powered, dev_down = FALSE;
	int err;

	ba2str(&adapter->bdaddr, srcaddr);

	adapter->off_requested = FALSE;
	adapter->up = 1;
	adapter->discov_timeout = get_discoverable_timeout(srcaddr);
	adapter->pairable_timeout = get_pairable_timeout(srcaddr);
	adapter->state = STATE_IDLE;
	adapter->mode = MODE_CONNECTABLE;
	adapter->cache_enable = TRUE;
	scan_mode = SCAN_PAGE;
	powered = TRUE;

	/* Set pairable mode */
	if (read_device_pairable(&adapter->bdaddr, &adapter->pairable) < 0)
		adapter->pairable = TRUE;

	if (g_str_equal(mode, "off")) {
		char onmode[14];

		powered = FALSE;

		if (!adapter->initialized) {
			dev_down = TRUE;
			goto proceed;
		}

		if (read_on_mode(srcaddr, onmode, sizeof(onmode)) < 0 ||
						g_str_equal(onmode, "off"))
			strcpy(onmode, "connectable");

		write_device_mode(&adapter->bdaddr, onmode);

		return adapter_up(adapter, onmode);
	} else if (!g_str_equal(mode, "connectable")) {
		adapter->mode = MODE_DISCOVERABLE;
		scan_mode = SCAN_PAGE | SCAN_INQUIRY;
	}

proceed:
	err = adapter_set_mode(adapter, adapter->mode);

	if (err < 0)
		return err;

	if (adapter->initialized == FALSE) {
		load_drivers(adapter);
		clear_blocked(adapter);
		load_devices(adapter);

		/* retrieve the active connections: address the scenario where
		 * the are active connections before the daemon've started */
		load_connections(adapter);

		adapter->initialized = TRUE;

		manager_add_adapter(adapter->path);

	}

	if (dev_down) {
		adapter_ops->stop(adapter->dev_id);
		adapter->off_requested = TRUE;
		return 1;
	} else
		emit_property_changed(connection, adapter->path,
					ADAPTER_INTERFACE, "Powered",
					DBUS_TYPE_BOOLEAN, &powered);

	call_adapter_powered_callbacks(adapter, TRUE);

	adapter_disable_cod_cache(adapter);

	return 0;
}

int adapter_start(struct btd_adapter *adapter)
{
	struct hci_dev *dev = &adapter->dev;
	struct hci_dev_info di;
	struct hci_version ver;
	uint8_t features[8];
	int err;
	char mode[14], address[18];

	if (hci_devinfo(adapter->dev_id, &di) < 0)
		return -errno;

	if (ignore_device(&di)) {
		dev->ignore = 1;
		return -1;
	}

	if (!bacmp(&di.bdaddr, BDADDR_ANY)) {
		int err;

		DBG("Adapter %s without an address", adapter->path);

		err = adapter_ops->read_bdaddr(adapter->dev_id, &di.bdaddr);
		if (err < 0)
			return err;
	}

	bacpy(&adapter->bdaddr, &di.bdaddr);
	memcpy(dev->features, di.features, 8);
	ba2str(&adapter->bdaddr, address);

	err = read_device_mode(address, mode, sizeof(mode));

	if ((!adapter->initialized && !main_opts.remember_powered) || err < 0) {
		if (!adapter->initialized && main_opts.mode == MODE_OFF)
			strcpy(mode, "off");
		else
			strcpy(mode, "connectable");
	}

	err = adapter_ops->read_local_version(adapter->dev_id, &ver);
	if (err < 0) {
		error("Can't read version info for %s: %s (%d)",
					adapter->path, strerror(-err), -err);
		return err;
	}

	dev->hci_rev = ver.hci_rev;
	dev->lmp_ver = ver.lmp_ver;
	dev->lmp_subver = ver.lmp_subver;
	dev->manufacturer = ver.manufacturer;

	err = adapter_ops->read_local_features(adapter->dev_id, features);
	if (err < 0) {
		error("Can't read features for %s: %s (%d)",
					adapter->path, strerror(-err), -err);
		return err;
	}

	memcpy(dev->features, features, 8);

	if (!(features[6] & LMP_SIMPLE_PAIR))
		goto setup;

	adapter_ops->init_ssp_mode(adapter->dev_id, &dev->ssp_mode);

setup:
	adapter_ops->read_link_policy(adapter->dev_id);

	adapter->current_cod = 0;

	adapter_setup(adapter, mode);

	if (!adapter->initialized && adapter->already_up) {
		DBG("Stopping Inquiry at adapter startup");
		adapter_ops->stop_inquiry(adapter->dev_id);
	}

	err = adapter_up(adapter, mode);

	info("Adapter %s has been enabled", adapter->path);

	return err;
}

static void reply_pending_requests(struct btd_adapter *adapter)
{
	GSList *l;

	if (!adapter)
		return;

	/* pending bonding */
	for (l = adapter->devices; l; l = l->next) {
		struct btd_device *device = l->data;

		if (device_is_bonding(device, NULL))
			device_cancel_bonding(device,
						HCI_OE_USER_ENDED_CONNECTION);
	}
}

static void unload_drivers(struct btd_adapter *adapter)
{
	GSList *l;

	for (l = adapter_drivers; l; l = l->next) {
		struct btd_adapter_driver *driver = l->data;

		if (driver->remove)
			driver->remove(adapter);
	}
}

static void set_mode_complete(struct btd_adapter *adapter)
{
	struct session_req *pending;
	const char *modestr;
	int err;

	if (adapter->pending_mode == NULL)
		return;

	pending = adapter->pending_mode;
	adapter->pending_mode = NULL;

	err = (pending->mode != adapter->mode) ? -EINVAL : 0;

	if (pending->msg != NULL) {
		DBusMessage *msg = pending->msg;
		DBusMessage *reply;

		if (err < 0)
			reply = failed_strerror(msg, -err);
		else
			reply = g_dbus_create_reply(msg, DBUS_TYPE_INVALID);

		g_dbus_send_message(connection, reply);
	}

	modestr = mode2str(adapter->mode);

	DBG("%s", modestr);

	/* restore if the mode doesn't matches the pending */
	if (err != 0) {
		write_device_mode(&adapter->bdaddr, modestr);
		error("unable to set mode: %s", mode2str(pending->mode));
	}

	session_unref(pending);
}

int adapter_stop(struct btd_adapter *adapter)
{
	gboolean powered, discoverable, pairable;

	/* cancel pending timeout */
	if (adapter->discov_timeout_id) {
		g_source_remove(adapter->discov_timeout_id);
		adapter->discov_timeout_id = 0;
	}

	/* check pending requests */
	reply_pending_requests(adapter);

	stop_discovery(adapter, FALSE);

	if (adapter->disc_sessions) {
		g_slist_foreach(adapter->disc_sessions, (GFunc) session_free,
				NULL);
		g_slist_free(adapter->disc_sessions);
		adapter->disc_sessions = NULL;
	}

	while (adapter->connections) {
		struct btd_device *device = adapter->connections->data;
		adapter_remove_connection(adapter, device, 0);
	}

	if (adapter->scan_mode == (SCAN_PAGE | SCAN_INQUIRY)) {
		discoverable = FALSE;
		emit_property_changed(connection, adapter->path,
					ADAPTER_INTERFACE, "Discoverable",
					DBUS_TYPE_BOOLEAN, &discoverable);
	}

	if ((adapter->scan_mode & SCAN_PAGE) && adapter->pairable == TRUE) {
		pairable = FALSE;
		emit_property_changed(connection, adapter->path,
					ADAPTER_INTERFACE, "Pairable",
					DBUS_TYPE_BOOLEAN, &pairable);
	}

	powered = FALSE;
	emit_property_changed(connection, adapter->path, ADAPTER_INTERFACE,
				"Powered", DBUS_TYPE_BOOLEAN, &powered);

	adapter->up = 0;
	adapter->scan_mode = SCAN_DISABLED;
	adapter->mode = MODE_OFF;
	adapter->state = STATE_IDLE;
	adapter->cache_enable = TRUE;
	adapter->pending_cod = 0;
	adapter->off_requested = FALSE;
	adapter->name_stored = FALSE;

	call_adapter_powered_callbacks(adapter, FALSE);

	info("Adapter %s has been disabled", adapter->path);

	set_mode_complete(adapter);

	return 0;
}

int adapter_update(struct btd_adapter *adapter, uint8_t new_svc)
{
	struct hci_dev *dev = &adapter->dev;

	if (dev->ignore)
		return 0;

	adapter_set_service_classes(adapter, new_svc);

	return 0;
}

int adapter_update_ssp_mode(struct btd_adapter *adapter, uint8_t mode)
{
	struct hci_dev *dev = &adapter->dev;

	dev->ssp_mode = mode;

	update_ext_inquiry_response(adapter);

	return 0;
}

static void adapter_free(gpointer user_data)
{
	struct btd_adapter *adapter = user_data;

	agent_free(adapter->agent);
	adapter->agent = NULL;

	DBG("%p", adapter);

	if (adapter->auth_idle_id)
		g_source_remove(adapter->auth_idle_id);

	g_free(adapter->path);
	g_free(adapter);
}

struct btd_adapter *btd_adapter_ref(struct btd_adapter *adapter)
{
	adapter->ref++;

	DBG("%p: ref=%d", adapter, adapter->ref);

	return adapter;
}

void btd_adapter_unref(struct btd_adapter *adapter)
{
	gchar *path;

	adapter->ref--;

	DBG("%p: ref=%d", adapter, adapter->ref);

	if (adapter->ref > 0)
		return;

	path = g_strdup(adapter->path);

	g_dbus_unregister_interface(connection, path, ADAPTER_INTERFACE);

	g_free(path);
}

struct btd_adapter *adapter_create(DBusConnection *conn, int id,
				gboolean devup)
{
	char path[MAX_PATH_LENGTH];
	struct btd_adapter *adapter;
	const char *base_path = manager_get_base_path();

	if (!connection)
		connection = conn;

	snprintf(path, sizeof(path), "%s/hci%d", base_path, id);

	adapter = g_try_new0(struct btd_adapter, 1);
	if (!adapter) {
		error("adapter_create: failed to alloc memory for %s", path);
		return NULL;
	}

	adapter->dev_id = id;
	adapter->path = g_strdup(path);
	adapter->already_up = devup;

	adapter->tx_power = 0;

	if (!g_dbus_register_interface(conn, path, ADAPTER_INTERFACE,
			adapter_methods, adapter_signals, NULL,
			adapter, adapter_free)) {
		error("Adapter interface init failed on path %s", path);
		adapter_free(adapter);
		return NULL;
	}

	return btd_adapter_ref(adapter);
}

void adapter_remove(struct btd_adapter *adapter)
{
	GSList *l;

	DBG("Removing adapter %s", adapter->path);

	for (l = adapter->devices; l; l = l->next)
		device_remove(l->data, FALSE);
	g_slist_free(adapter->devices);

	if (adapter->initialized)
		unload_drivers(adapter);

	/* Return adapter to down state if it was not up on init */
	if (adapter->up && !adapter->already_up)
		adapter_ops->stop(adapter->dev_id);

	btd_adapter_unref(adapter);
}

uint16_t adapter_get_dev_id(struct btd_adapter *adapter)
{
	return adapter->dev_id;
}

const gchar *adapter_get_path(struct btd_adapter *adapter)
{
	if (!adapter)
		return NULL;

	return adapter->path;
}

void adapter_get_address(struct btd_adapter *adapter, bdaddr_t *bdaddr)
{
	bacpy(bdaddr, &adapter->bdaddr);
}

void adapter_set_state(struct btd_adapter *adapter, int state)
{
	const char *path = adapter->path;
	gboolean discov_active;
	int previous, type;

	if (adapter->state == state)
		return;

	previous = adapter->state;
	adapter->state = state;

	type = adapter_get_discover_type(adapter);

	switch (state) {
	case STATE_STDINQ:
	case STATE_PINQ:
		discov_active = TRUE;

		/* Started a new session while resolving names ? */
		if (previous & STATE_RESOLVNAME)
			return;
		break;
	case STATE_LE_SCAN:
		/* Scanning enabled */
		adapter->stop_discov_id = g_timeout_add(5120,
						stop_scanning, adapter);

		/* For dual mode: don't send "Discovering = TRUE"  */
		if (bredr_capable(adapter) == TRUE)
			return;

		/* LE only */
		discov_active = TRUE;

		break;
	case STATE_IDLE:
		/*
		 * Interleave: from inquiry to scanning. Interleave is not
		 * applicable to requests triggered by external applications.
		 */
		if (adapter->disc_sessions && (type & DISC_INTERLEAVE) &&
						(previous & STATE_STDINQ)) {
			adapter_ops->start_scanning(adapter->dev_id);
			return;
		}
		/* BR/EDR only: inquiry finished */
		discov_active = FALSE;
		break;
	default:
		discov_active = FALSE;
		break;
	}

	if (discov_active == FALSE) {
		update_oor_devices(adapter);
		if (type & DISC_RESOLVNAME) {
			if (adapter_resolve_names(adapter) == 0) {
				adapter->state |= STATE_RESOLVNAME;
				return;
			}
		}
	} else if (adapter->disc_sessions && main_opts.discov_interval)
			adapter->scheduler_id = g_timeout_add_seconds(
						main_opts.discov_interval,
						(GSourceFunc) start_discovery,
						adapter);

	emit_property_changed(connection, path,
				ADAPTER_INTERFACE, "Discovering",
				DBUS_TYPE_BOOLEAN, &discov_active);
}

int adapter_get_state(struct btd_adapter *adapter)
{
	return adapter->state;
}

gboolean adapter_is_ready(struct btd_adapter *adapter)
{
	return adapter->initialized;
}

struct remote_dev_info *adapter_search_found_devices(struct btd_adapter *adapter,
						struct remote_dev_info *match)
{
	GSList *l;

	l = g_slist_find_custom(adapter->found_devices, match,
					(GCompareFunc) found_device_cmp);
	if (l)
		return l->data;

	return NULL;
}

static int dev_rssi_cmp(struct remote_dev_info *d1, struct remote_dev_info *d2)
{
	int rssi1, rssi2;

	rssi1 = d1->rssi < 0 ? -d1->rssi : d1->rssi;
	rssi2 = d2->rssi < 0 ? -d2->rssi : d2->rssi;

	return rssi1 - rssi2;
}

static void append_dict_valist(DBusMessageIter *iter,
					const char *first_key,
					va_list var_args)
{
	DBusMessageIter dict;
	const char *key;
	int type;
	int n_elements;
	void *val;

	dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);

	key = first_key;
	while (key) {
		type = va_arg(var_args, int);
		val = va_arg(var_args, void *);
		if (type == DBUS_TYPE_ARRAY) {
			n_elements = va_arg(var_args, int);
			if (n_elements > 0)
				dict_append_array(&dict, key, DBUS_TYPE_STRING,
						val, n_elements);
		} else
			dict_append_entry(&dict, key, type, val);
		key = va_arg(var_args, char *);
	}

	dbus_message_iter_close_container(iter, &dict);
}

static void emit_device_found(const char *path, const char *address,
				const char *first_key, ...)
{
	DBusMessage *signal;
	DBusMessageIter iter;
	va_list var_args;

	signal = dbus_message_new_signal(path, ADAPTER_INTERFACE,
					"DeviceFound");
	if (!signal) {
		error("Unable to allocate new %s.DeviceFound signal",
				ADAPTER_INTERFACE);
		return;
	}
	dbus_message_iter_init_append(signal, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &address);

	va_start(var_args, first_key);
	append_dict_valist(&iter, first_key, var_args);
	va_end(var_args);

	g_dbus_send_message(connection, signal);
}

static char **get_eir_uuids(uint8_t *eir_data, size_t *uuid_count)
{
	uint16_t len = 0;
	char **uuids;
	size_t total;
	size_t uuid16_count = 0;
	size_t uuid32_count = 0;
	size_t uuid128_count = 0;
	uint8_t *uuid16;
	uint8_t *uuid32;
	uint8_t *uuid128;
	uuid_t service;
	unsigned int i;

	while (len < EIR_DATA_LENGTH - 1) {
		uint8_t type = eir_data[1];
		uint8_t field_len = eir_data[0];

		/* Check for the end of EIR */
		if (field_len == 0)
			break;

		switch (type) {
		case EIR_UUID16_SOME:
		case EIR_UUID16_ALL:
			uuid16_count = field_len / 2;
			uuid16 = &eir_data[2];
			break;
		case EIR_UUID32_SOME:
		case EIR_UUID32_ALL:
			uuid32_count = field_len / 4;
			uuid32 = &eir_data[2];
			break;
		case EIR_UUID128_SOME:
		case EIR_UUID128_ALL:
			uuid128_count = field_len / 16;
			uuid128 = &eir_data[2];
			break;
		}

		len += field_len + 1;
		eir_data += field_len + 1;
	}

	/* Bail out if got incorrect length */
	if (len > EIR_DATA_LENGTH)
		return NULL;

	total = uuid16_count + uuid32_count + uuid128_count;
	*uuid_count = total;

	if (!total)
		return NULL;

	uuids = g_new0(char *, total + 1);

	/* Generate uuids in SDP format (EIR data is Little Endian) */
	service.type = SDP_UUID16;
	for (i = 0; i < uuid16_count; i++) {
		uint16_t val16 = uuid16[1];

		val16 = (val16 << 8) + uuid16[0];
		service.value.uuid16 = val16;
		uuids[i] = bt_uuid2string(&service);
		uuid16 += 2;
	}

	service.type = SDP_UUID32;
	for (i = uuid16_count; i < uuid32_count + uuid16_count; i++) {
		uint32_t val32 = uuid32[3];
		int k;

		for (k = 2; k >= 0; k--)
			val32 = (val32 << 8) + uuid32[k];

		service.value.uuid32 = val32;
		uuids[i] = bt_uuid2string(&service);
		uuid32 += 4;
	}

	service.type = SDP_UUID128;
	for (i = uuid32_count + uuid16_count; i < total; i++) {
		int k;

		for (k = 0; k < 16; k++)
			service.value.uuid128.data[k] = uuid128[16 - k - 1];

		uuids[i] = bt_uuid2string(&service);
		uuid128 += 16;
	}

	return uuids;
}

void adapter_emit_device_found(struct btd_adapter *adapter,
				struct remote_dev_info *dev, uint8_t *eir_data)
{
	struct btd_device *device;
	char peer_addr[18], local_addr[18];
	const char *icon, *paddr = peer_addr;
	dbus_bool_t paired = FALSE;
	dbus_int16_t rssi = dev->rssi;
	char *alias;
	char **uuids = NULL;
	size_t uuid_count = 0;

	ba2str(&dev->bdaddr, peer_addr);
	ba2str(&adapter->bdaddr, local_addr);

	device = adapter_find_device(adapter, paddr);
	if (device)
		paired = device_is_paired(device);

	icon = class_to_icon(dev->class);

	if (!dev->alias) {
		if (!dev->name) {
			alias = g_strdup(peer_addr);
			g_strdelimit(alias, ":", '-');
		} else
			alias = g_strdup(dev->name);
	} else
		alias = g_strdup(dev->alias);

	/* Extract UUIDs from extended inquiry response if any*/
	if (eir_data != NULL)
		uuids = get_eir_uuids(eir_data, &uuid_count);

	emit_device_found(adapter->path, paddr,
			"Address", DBUS_TYPE_STRING, &paddr,
			"Class", DBUS_TYPE_UINT32, &dev->class,
			"Icon", DBUS_TYPE_STRING, &icon,
			"RSSI", DBUS_TYPE_INT16, &rssi,
			"Name", DBUS_TYPE_STRING, &dev->name,
			"Alias", DBUS_TYPE_STRING, &alias,
			"LegacyPairing", DBUS_TYPE_BOOLEAN, &dev->legacy,
			"Paired", DBUS_TYPE_BOOLEAN, &paired,
			"UUIDs", DBUS_TYPE_ARRAY, &uuids, uuid_count,
			NULL);

	g_free(alias);
	g_strfreev(uuids);
}

void adapter_update_found_devices(struct btd_adapter *adapter, bdaddr_t *bdaddr,
				int8_t rssi, uint32_t class, const char *name,
				const char *alias, gboolean legacy,
				name_status_t name_status, uint8_t *eir_data)
{
	struct remote_dev_info *dev, match;

	memset(&match, 0, sizeof(struct remote_dev_info));
	bacpy(&match.bdaddr, bdaddr);
	match.name_status = NAME_ANY;

	dev = adapter_search_found_devices(adapter, &match);
	if (dev) {
		/* Out of range list update */
		adapter->oor_devices = g_slist_remove(adapter->oor_devices,
							dev);

		if (rssi == dev->rssi)
			return;

		goto done;
	}

	dev = g_new0(struct remote_dev_info, 1);

	bacpy(&dev->bdaddr, bdaddr);
	dev->class = class;
	if (name)
		dev->name = g_strdup(name);
	if (alias)
		dev->alias = g_strdup(alias);
	dev->legacy = legacy;
	dev->name_status = name_status;

	adapter->found_devices = g_slist_prepend(adapter->found_devices, dev);

done:
	dev->rssi = rssi;

	adapter->found_devices = g_slist_sort(adapter->found_devices,
						(GCompareFunc) dev_rssi_cmp);

	adapter_emit_device_found(adapter, dev, eir_data);
}

int adapter_remove_found_device(struct btd_adapter *adapter, bdaddr_t *bdaddr)
{
	struct remote_dev_info *dev, match;

	memset(&match, 0, sizeof(struct remote_dev_info));
	bacpy(&match.bdaddr, bdaddr);

	dev = adapter_search_found_devices(adapter, &match);
	if (!dev)
		return -1;

	dev->name_status = NAME_NOT_REQUIRED;

	return 0;
}

void adapter_mode_changed(struct btd_adapter *adapter, uint8_t scan_mode)
{
	const gchar *path = adapter_get_path(adapter);
	gboolean discoverable, pairable;

	if (adapter->scan_mode == scan_mode)
		return;

	adapter_remove_discov_timeout(adapter);

	switch (scan_mode) {
	case SCAN_DISABLED:
		adapter->mode = MODE_OFF;
		discoverable = FALSE;
		pairable = FALSE;
		break;
	case SCAN_PAGE:
		adapter->mode = MODE_CONNECTABLE;
		discoverable = FALSE;
		pairable = adapter->pairable;
		break;
	case (SCAN_PAGE | SCAN_INQUIRY):
		adapter->mode = MODE_DISCOVERABLE;
		discoverable = TRUE;
		pairable = adapter->pairable;
		if (adapter->discov_timeout != 0)
			adapter_set_discov_timeout(adapter,
						adapter->discov_timeout);
		break;
	case SCAN_INQUIRY:
		/* Address the scenario where a low-level application like
		 * hciconfig changed the scan mode */
		if (adapter->discov_timeout != 0)
			adapter_set_discov_timeout(adapter,
						adapter->discov_timeout);

		/* ignore, this event should not be sent */
	default:
		/* ignore, reserved */
		return;
	}

	/* If page scanning gets toggled emit the Pairable property */
	if ((adapter->scan_mode & SCAN_PAGE) != (scan_mode & SCAN_PAGE))
		emit_property_changed(connection, adapter->path,
					ADAPTER_INTERFACE, "Pairable",
					DBUS_TYPE_BOOLEAN, &pairable);

	if (discoverable && adapter->pairable && adapter->discov_timeout > 0 &&
						adapter->discov_timeout <= 60)
		adapter_set_limited_discoverable(adapter, TRUE);
	else if (!discoverable)
		adapter_set_limited_discoverable(adapter, FALSE);

	emit_property_changed(connection, path,
				ADAPTER_INTERFACE, "Discoverable",
				DBUS_TYPE_BOOLEAN, &discoverable);

	adapter->scan_mode = scan_mode;

	set_mode_complete(adapter);
}

struct agent *adapter_get_agent(struct btd_adapter *adapter)
{
	if (!adapter || !adapter->agent)
		return NULL;

	return adapter->agent;
}

void adapter_add_connection(struct btd_adapter *adapter,
				struct btd_device *device, uint16_t handle)
{
	if (g_slist_find(adapter->connections, device)) {
		error("Unable to add connection %d", handle);
		return;
	}

	device_add_connection(device, connection, handle);

	adapter->connections = g_slist_append(adapter->connections, device);
}

void adapter_remove_connection(struct btd_adapter *adapter,
				struct btd_device *device, uint16_t handle)
{
	bdaddr_t bdaddr;

	if (!g_slist_find(adapter->connections, device)) {
		error("No matching connection for handle %u", handle);
		return;
	}

	device_remove_connection(device, connection, handle);

	adapter->connections = g_slist_remove(adapter->connections, device);

	/* clean pending HCI cmds */
	device_get_address(device, &bdaddr);

	if (device_is_authenticating(device))
		device_cancel_authentication(device, TRUE);

	if (device_is_temporary(device)) {
		const char *path = device_get_path(device);

		DBG("Removing temporary device %s", path);
		adapter_remove_device(connection, adapter, device, TRUE);
	}
}

gboolean adapter_has_discov_sessions(struct btd_adapter *adapter)
{
	if (!adapter || !adapter->disc_sessions)
		return FALSE;

	return TRUE;
}

void adapter_suspend_discovery(struct btd_adapter *adapter)
{
	if (adapter->disc_sessions == NULL ||
			adapter->state & STATE_SUSPENDED)
		return;

	DBG("Suspending discovery");

	stop_discovery(adapter, TRUE);
	adapter->state |= STATE_SUSPENDED;
}

void adapter_resume_discovery(struct btd_adapter *adapter)
{
	if (adapter->disc_sessions == NULL)
		return;

	DBG("Resuming discovery");

	adapter->state &= ~STATE_SUSPENDED;
	start_discovery(adapter);
}

int btd_register_adapter_driver(struct btd_adapter_driver *driver)
{
	GSList *adapters;

	adapter_drivers = g_slist_append(adapter_drivers, driver);

	if (driver->probe == NULL)
		return 0;

	adapters = manager_get_adapters();
	g_slist_foreach(adapters, probe_driver, driver);

	return 0;
}

void btd_unregister_adapter_driver(struct btd_adapter_driver *driver)
{
	adapter_drivers = g_slist_remove(adapter_drivers, driver);
}

static void agent_auth_cb(struct agent *agent, DBusError *derr,
							void *user_data)
{
	struct service_auth *auth = user_data;

	device_set_authorizing(auth->device, FALSE);

	auth->cb(derr, auth->user_data);
}

static gboolean auth_idle_cb(gpointer user_data)
{
	struct service_auth *auth = user_data;
	struct btd_adapter *adapter = auth->adapter;

	adapter->auth_idle_id = 0;

	auth->cb(NULL, auth->user_data);

	return FALSE;
}

static int btd_adapter_authorize(struct btd_adapter *adapter,
					const bdaddr_t *dst,
					const char *uuid,
					service_auth_cb cb, void *user_data)
{
	struct service_auth *auth;
	struct btd_device *device;
	struct agent *agent;
	char address[18];
	const gchar *dev_path;
	int err;

	ba2str(dst, address);
	device = adapter_find_device(adapter, address);
	if (!device)
		return -EPERM;

	/* Device connected? */
	if (!g_slist_find(adapter->connections, device))
		return -ENOTCONN;

	if (adapter->auth_idle_id)
		return -EBUSY;

	auth = g_try_new0(struct service_auth, 1);
	if (!auth)
		return -ENOMEM;

	auth->cb = cb;
	auth->user_data = user_data;
	auth->device = device;
	auth->adapter = adapter;

	if (device_is_trusted(device) == TRUE) {
		adapter->auth_idle_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
							auth_idle_cb, auth,
							g_free);
		return 0;
	}

	agent = device_get_agent(device);
	if (!agent) {
		g_free(auth);
		return -EPERM;
	}

	dev_path = device_get_path(device);

	err = agent_authorize(agent, dev_path, uuid, agent_auth_cb, auth, g_free);
	if (err < 0)
		g_free(auth);
	else
		device_set_authorizing(device, TRUE);

	return err;
}

int btd_request_authorization(const bdaddr_t *src, const bdaddr_t *dst,
		const char *uuid, service_auth_cb cb, void *user_data)
{
	struct btd_adapter *adapter;
	GSList *adapters;

	if (src == NULL || dst == NULL)
		return -EINVAL;

	if (bacmp(src, BDADDR_ANY) != 0)
		goto proceed;

	/* Handle request authorization for ANY adapter */
	adapters = manager_get_adapters();

	for (; adapters; adapters = adapters->next) {
		int err;
		adapter = adapters->data;

		err = btd_adapter_authorize(adapter, dst, uuid, cb, user_data);
		if (err == 0)
			return 0;
	}

	return -EPERM;

proceed:
	adapter = manager_find_adapter(src);
	if (!adapter)
		return -EPERM;

	return btd_adapter_authorize(adapter, dst, uuid, cb, user_data);
}

int btd_cancel_authorization(const bdaddr_t *src, const bdaddr_t *dst)
{
	struct btd_adapter *adapter = manager_find_adapter(src);
	struct btd_device *device;
	struct agent *agent;
	char address[18];
	int err;

	if (!adapter)
		return -EPERM;

	ba2str(dst, address);
	device = adapter_find_device(adapter, address);
	if (!device)
		return -EPERM;

	if (adapter->auth_idle_id) {
		g_source_remove(adapter->auth_idle_id);
		adapter->auth_idle_id = 0;
		return 0;
	}

	/*
	 * FIXME: Cancel fails if authorization is requested to adapter's
	 * agent and in the meanwhile CreatePairedDevice is called.
	 */

	agent = device_get_agent(device);
	if (!agent)
		return -EPERM;

	err = agent_cancel(agent);

	if (err == 0)
		device_set_authorizing(device, FALSE);

	return err;
}

static gchar *adapter_any_path = NULL;
static int adapter_any_refcount = 0;

const char *adapter_any_get_path(void)
{
	return adapter_any_path;
}

const char *btd_adapter_any_request_path(void)
{
	if (adapter_any_refcount > 0)
		return adapter_any_path;

	adapter_any_path = g_strdup_printf("%s/any", manager_get_base_path());
	adapter_any_refcount++;

	return adapter_any_path;
}

void btd_adapter_any_release_path(void)
{
	adapter_any_refcount--;

	if (adapter_any_refcount > 0)
		return;

	g_free(adapter_any_path);
	adapter_any_path = NULL;
}

gboolean adapter_is_pairable(struct btd_adapter *adapter)
{
	return adapter->pairable;
}

gboolean adapter_powering_down(struct btd_adapter *adapter)
{
	return adapter->off_requested;
}

int btd_adapter_restore_powered(struct btd_adapter *adapter)
{
	char mode[14], address[18];

	if (!adapter_ops)
		return -EINVAL;

	if (!main_opts.remember_powered)
		return -EINVAL;

	if (adapter->up)
		return 0;

	ba2str(&adapter->bdaddr, address);
	if (read_device_mode(address, mode, sizeof(mode)) == 0 &&
						g_str_equal(mode, "off"))
		return 0;

	return adapter_ops->set_powered(adapter->dev_id, TRUE);
}

int btd_adapter_switch_online(struct btd_adapter *adapter)
{
	if (!adapter_ops)
		return -EINVAL;

	if (adapter->up)
		return 0;

	return adapter_ops->set_powered(adapter->dev_id, TRUE);
}

int btd_adapter_switch_offline(struct btd_adapter *adapter)
{
	if (!adapter_ops)
		return -EINVAL;

	if (!adapter->up)
		return 0;

	return adapter_ops->set_powered(adapter->dev_id, FALSE);
}

int btd_register_adapter_ops(struct btd_adapter_ops *ops, gboolean priority)
{
	if (ops->setup == NULL)
		return -EINVAL;

	if (priority)
		ops_candidates = g_slist_prepend(ops_candidates, ops);
	else
		ops_candidates = g_slist_append(ops_candidates, ops);

	return 0;
}

void btd_adapter_cleanup_ops(struct btd_adapter_ops *ops)
{
	ops_candidates = g_slist_remove(ops_candidates, ops);
	ops->cleanup();

	if (adapter_ops == ops)
		adapter_ops = NULL;
}

int adapter_ops_setup(void)
{
	GSList *l;
	int ret;

	if (!ops_candidates)
		return -EINVAL;

	for (l = ops_candidates; l != NULL; l = g_slist_next(l)) {
		struct btd_adapter_ops *ops = l->data;

		ret = ops->setup();
		if (ret < 0)
			continue;

		adapter_ops = ops;
		break;
	}

	return ret;
}

void btd_adapter_register_powered_callback(struct btd_adapter *adapter,
						btd_adapter_powered_cb cb)
{
	adapter->powered_callbacks =
			g_slist_append(adapter->powered_callbacks, cb);
}

void btd_adapter_unregister_powered_callback(struct btd_adapter *adapter,
						btd_adapter_powered_cb cb)
{
	adapter->powered_callbacks =
			g_slist_remove(adapter->powered_callbacks, cb);
}

int btd_adapter_set_fast_connectable(struct btd_adapter *adapter,
							gboolean enable)
{
	if (!adapter_ops)
		return -EINVAL;

	if (!adapter->up)
		return -EINVAL;

	return adapter_ops->set_fast_connectable(adapter->dev_id, enable);
}

int btd_adapter_read_clock(struct btd_adapter *adapter, int handle, int which,
			int timeout, uint32_t *clock, uint16_t *accuracy)
{
	if (!adapter_ops)
		return -EINVAL;

	if (!adapter->up)
		return -EINVAL;

	return adapter_ops->read_clock(adapter->dev_id, handle, which,
						timeout, clock, accuracy);
}

int btd_adapter_get_conn_handle(struct btd_adapter *adapter,
				const bdaddr_t *bdaddr, int *handle)
{
	if (!adapter_ops)
		return -EINVAL;

	if (!adapter->up)
		return -EINVAL;

	return adapter_ops->get_conn_handle(adapter->dev_id, bdaddr, handle);
}

int btd_adapter_disconnect_device(struct btd_adapter *adapter, uint16_t handle)
{
	return adapter_ops->disconnect(adapter->dev_id, handle);
}

int btd_adapter_remove_bonding(struct btd_adapter *adapter, bdaddr_t *bdaddr)
{
	return adapter_ops->remove_bonding(adapter->dev_id, bdaddr);
}

int btd_adapter_request_authentication(struct btd_adapter *adapter,
					uint16_t handle, uint8_t *status)
{
	return adapter_ops->request_authentication(adapter->dev_id,
							handle, status);
}

int btd_adapter_pincode_reply(struct btd_adapter *adapter, bdaddr_t *bdaddr,
							const char *pin)
{
	return adapter_ops->pincode_reply(adapter->dev_id, bdaddr, pin);
}

int btd_adapter_confirm_reply(struct btd_adapter *adapter, bdaddr_t *bdaddr,
							gboolean success)
{
	return adapter_ops->confirm_reply(adapter->dev_id, bdaddr, success);
}

int btd_adapter_passkey_reply(struct btd_adapter *adapter, bdaddr_t *bdaddr,
							uint32_t passkey)
{
	return adapter_ops->passkey_reply(adapter->dev_id, bdaddr, passkey);
}

int btd_adapter_get_auth_info(struct btd_adapter *adapter, bdaddr_t *bdaddr,
								uint8_t *auth)
{
	return adapter_ops->get_auth_info(adapter->dev_id, bdaddr, auth);
}

int btd_adapter_read_scan_enable(struct btd_adapter *adapter)
{
	return adapter_ops->read_scan_enable(adapter->dev_id);
}

int btd_adapter_read_ssp_mode(struct btd_adapter *adapter)
{
	return adapter_ops->read_ssp_mode(adapter->dev_id);
}

int btd_adapter_read_local_ext_features(struct btd_adapter *adapter)
{
	return adapter_ops->read_local_ext_features(adapter->dev_id);
}

void btd_adapter_update_local_ext_features(struct btd_adapter *adapter,
						const uint8_t *features)
{
	struct hci_dev *dev = &adapter->dev;

	memcpy(dev->extfeatures, features, 8);
}

int btd_adapter_get_remote_name(struct btd_adapter *adapter, bdaddr_t *bdaddr)
{
	return adapter_ops->resolve_name(adapter->dev_id, bdaddr);
}

int btd_adapter_get_remote_version(struct btd_adapter *adapter,
					uint16_t handle, gboolean delayed)
{
	return adapter_ops->get_remote_version(adapter->dev_id, handle,
								delayed);
}

int btd_adapter_encrypt_link(struct btd_adapter *adapter, bdaddr_t *bdaddr,
					bt_hci_result_t cb, gpointer user_data)
{
	return adapter_ops->encrypt_link(adapter->dev_id, bdaddr, cb, user_data);
}
