/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2012, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Karl Anderson <karl@2600hz.com>
 * Darren Schreiber <darren@2600hz.com>
 *
 *
 * mod_kazoo.c -- Socket Controlled Event Handler
 *
 */
#include <switch.h>
#include <switch_apr.h>
#include <ei.h>
#include <apr_portable.h>
#include "mod_kazoo.h"

SWITCH_MODULE_LOAD_FUNCTION(mod_kazoo_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_kazoo_shutdown);
SWITCH_MODULE_RUNTIME_FUNCTION(mod_kazoo_runtime);
SWITCH_MODULE_DEFINITION(mod_kazoo, mod_kazoo_load, mod_kazoo_shutdown, mod_kazoo_runtime);

static struct {
	switch_socket_t *sock;
	listener_t *listeners;
	switch_thread_rwlock_t *listeners_lock;
	uint8_t ready;
} acceptor;

SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_pref_ip, prefs.ip);
SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_pref_ei_cookie, prefs.ei_cookie);
SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_pref_ei_nodename, prefs.ei_nodename);

/* Function Definitions */
static void *SWITCH_THREAD_FUNC erl_to_fs_loop(switch_thread_t *thread, void *obj);
static void launch_erl_to_fs_thread(listener_t *listener);
static void stop_listener(listener_t *listener);

static char *API_COMMANDS[] = {
	"listeners",
	"sessions",
	"bindings"
};

typedef enum {
    API_CMD_LISTENERS,
    API_CMD_SESSIONS,
	API_CMD_BINDINGS,
	API_CMD_MAX
} api_commands_t;

static switch_status_t find_api_command(char *arg, int *command) {
	for (int i = 0; i <= API_CMD_MAX; i++) {
		if(!strcmp(arg, API_COMMANDS[i])) {
			*command = i;
			return SWITCH_STATUS_SUCCESS;
		}
	}

	return SWITCH_STATUS_FALSE;
}

static void close_socket(switch_socket_t ** sock) {
	if (*sock) {
		switch_socket_shutdown(*sock, SWITCH_SHUTDOWN_READWRITE);
		switch_socket_close(*sock);
		*sock = NULL;
	}
}

static void close_socketfd(int *sockfd) {
	if (*sockfd) {
		shutdown(*sockfd, SHUT_RDWR);
		close(*sockfd);
	}
}

static void flush_listener(listener_t *listener, switch_bool_t flush_log, switch_bool_t flush_events) {
	void *pop;

	if (listener->event_queue) {
		while (switch_queue_trypop(listener->event_queue, &pop) == SWITCH_STATUS_SUCCESS) {
			switch_event_t *pevent = (switch_event_t *) pop;
			if (!pop)
				continue;
			switch_event_destroy(&pevent);
		}
	}

	if (listener->fetch_queue) {
		while (switch_queue_trypop(listener->fetch_queue, &pop) == SWITCH_STATUS_SUCCESS) {
			char *uuid_str = (char *) pop;
			if (!pop)
				continue;
			switch_safe_free(uuid_str);
		}
	}
}

static void destroy_listener(listener_t *listener) {
	/* ensure nothing else is still using this listener */
	switch_thread_rwlock_wrlock(listener->rwlock);

	/* flush all bindings */
	flush_all_bindings(listener);

	/* Now that we are out of the listener_list we can flush our queues */
	/* since nobody will know about our existence and be unable to add to them */
	flush_listener(listener, SWITCH_TRUE, SWITCH_TRUE);

	/* close the client socket */
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Closing connection to erlang node %s\n", listener->peer_nodename);
	close_socketfd(&listener->clientfd);

	switch_thread_rwlock_unlock(listener->rwlock);

	/* clean up our hashes */
	switch_core_hash_destroy(&listener->event_bindings);
	switch_core_hash_destroy(&listener->session_bindings);
	switch_core_hash_destroy(&listener->fetch_bindings);

	/* clean up our locks */
	switch_mutex_destroy(listener->flag_mutex);

	switch_thread_rwlock_destroy(listener->event_rwlock);
	switch_thread_rwlock_destroy(listener->session_rwlock);
	switch_thread_rwlock_destroy(listener->fetch_rwlock);
	switch_thread_rwlock_destroy(listener->rwlock);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Destroyed listener %p: %s (%s)\n", (void *)listener, listener->peer_nodename, listener->remote_ip);

	/* goodbye and thanks for all the fish! */
	switch_core_destroy_memory_pool(&listener->pool);
}

static void stop_listener(listener_t *listener) {
	listener_t *l, *last = NULL;

	switch_thread_rwlock_wrlock(acceptor.listeners_lock);
	for (l = acceptor.listeners; l; l = l->next) {
		if (l == listener) {
			if (last) {
				last->next = l->next;
			} else {
				acceptor.listeners = l->next;
			}
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Removed listener %p: %s (%s)\n", (void *)listener, listener->peer_nodename, listener->remote_ip);
		}
		last = l;
	}

	switch_clear_flag(listener, LFLAG_RUNNING);

	switch_thread_rwlock_unlock(acceptor.listeners_lock);
}

static void add_listener(listener_t *listener) {
	if (!switch_test_flag(&globals, LFLAG_RUNNING) || !acceptor.ready) {
		return;
	}

	/* add me to the acceptor so I get events */
	switch_thread_rwlock_wrlock(acceptor.listeners_lock);
	listener->next = acceptor.listeners;
	acceptor.listeners = listener;
	switch_thread_rwlock_unlock(acceptor.listeners_lock);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Added listener %p: %s (%s)\n", (void *)listener, listener->peer_nodename, listener->remote_ip);
}

static void event_handler(switch_event_t *event) {
	char *uuid_str = switch_event_get_header(event, "unique-id");
	listener_t *listener;

	switch_assert(event != NULL);

	if (!switch_test_flag(&globals, LFLAG_RUNNING) || !acceptor.ready) {
		return;
	}

	switch_thread_rwlock_rdlock(acceptor.listeners_lock);
	for (listener = acceptor.listeners; listener; listener = listener->next) {
		switch_event_t *clone = NULL;
		int send = 0;

		/* if this listener has erlang process that are bound to this event type then */
		/* set the flag to duplicate the event into the listener event queue */
		if (!zstr(uuid_str) && has_session_bindings(listener, uuid_str) == SWITCH_STATUS_FOUND) {
			send = 1;
		} else if (has_event_bindings(listener, event) == SWITCH_STATUS_FOUND) {
			send = 1;
		}

		if (send) {
			if (switch_event_dup(&clone, event) == SWITCH_STATUS_SUCCESS) {
				if (switch_queue_trypush(listener->event_queue, clone) != SWITCH_STATUS_SUCCESS) {
					/* if we couldn't place the cloned event into the listeners */
					/* event queue make sure we destroy it, real good like */
					switch_event_destroy(&clone);
				}
			} else {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Memory error: Have a good trip? See you next fall!\n");
			}
		}
	}
	switch_thread_rwlock_unlock(acceptor.listeners_lock);
}

static switch_xml_t fetch_handler(const char *section, const char *tag_name, const char *key_name, const char *key_value, switch_event_t *params, void *user_data) {
	switch_xml_t xml = NULL;
	xml_fetch_msg_t *fetch_msg = NULL;
	switch_uuid_t uuid;
	listener_t *listener = NULL;
	switch_time_t now = 0;

	if (!switch_test_flag(&globals, LFLAG_RUNNING) || !acceptor.ready) {
		return xml;
	}

	now = switch_micro_time_now();

    /* switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Got fetch request: %s / %s / %s = %s\n", section, tag_name, key_name, key_value); */

	switch_thread_rwlock_rdlock(acceptor.listeners_lock);
	for (listener = acceptor.listeners; listener; listener = listener->next) {
		if (has_fetch_bindings(listener, section) == SWITCH_STATUS_FOUND) {
			char *dup_uuid;

			if (!fetch_msg) {
				switch_zmalloc(fetch_msg, sizeof(*fetch_msg));
				switch_mutex_init(&fetch_msg->mutex, SWITCH_MUTEX_DEFAULT, globals.pool);
				switch_thread_cond_create(&fetch_msg->response_available, globals.pool);

				/* Create a unique identifier for this request */
				switch_uuid_get(&uuid);
				switch_uuid_format(fetch_msg->uuid_str, &uuid);

				/* Create a request in our queue for an XML binding. No need to copy memory pointers here, we block until they're used elsewhere and returned */
				fetch_msg->section = section;
				fetch_msg->tag_name = tag_name;
				fetch_msg->key_name = key_name;
				fetch_msg->key_value = key_value;
				fetch_msg->params = params;

				switch_core_hash_insert_wrlock(globals.fetch_resp_hash, fetch_msg->uuid_str, fetch_msg, globals.fetch_resp_lock);

				/* These is an extremely unlikely race condition here.  If the network request and corresponding erlang response arrives */
				/* before we get to switch_thread_cond_timedwait bellow, the response will be thrown away.... but there is no way communication */
				/* between two servers is faster than C code on one.... right?  Ya, I am sure thats right. */
				switch_mutex_lock(fetch_msg->mutex);
			}

			dup_uuid = strdup(fetch_msg->uuid_str);

			if (switch_queue_trypush(listener->fetch_queue, dup_uuid) != SWITCH_STATUS_SUCCESS) {
				switch_safe_free(dup_uuid);				
			}
		}
	}
	switch_thread_rwlock_unlock(acceptor.listeners_lock);

	if (fetch_msg) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Processing fetch request %s: %s / %s / %s = %s\n", fetch_msg->uuid_str, section, tag_name, key_name, key_value);

		switch_thread_cond_timedwait(fetch_msg->response_available, fetch_msg->mutex, 3000000);

		if (fetch_msg->xml) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Fetch request %s reply after %dms\n", fetch_msg->uuid_str, (int) (switch_micro_time_now() - now) / 1000);
			xml = fetch_msg->xml;
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Fetch request %s timeout after %dms\n", fetch_msg->uuid_str, (int) (switch_micro_time_now() - now) / 1000);
		}

		switch_core_hash_delete_wrlock(globals.fetch_resp_hash, fetch_msg->uuid_str, globals.fetch_resp_lock);

		switch_mutex_unlock(fetch_msg->mutex);

		switch_thread_cond_destroy(fetch_msg->response_available);
		switch_mutex_destroy(fetch_msg->mutex);
		switch_safe_free(fetch_msg);
	}

	return xml;
}

static void *SWITCH_THREAD_FUNC erl_to_fs_loop(switch_thread_t *thread, void *obj) {
	listener_t *listener = (listener_t *) obj;
	int status = 1;
	void *pop;
	switch_atomic_inc(&prefs.threads);

	switch_assert(listener != NULL);

	/* grab a read lock on the listener so nobody can remove it until we exit... */
	switch_thread_rwlock_rdlock(listener->rwlock);

	add_listener(listener);

	while (switch_test_flag(listener, LFLAG_RUNNING) && switch_test_flag(&globals, LFLAG_RUNNING) && status >= 0) {
		erlang_msg msg;
		ei_x_buff buf;
		ei_x_buff rbuf;

		/* create a new buf for the erlang message and a rbuf for the reply */
		ei_x_new(&buf);
		ei_x_new_with_version(&rbuf);

		/* wait for a erlang message, or timeout after 100ms to check if the module is still running */
		status = ei_xreceive_msg_tmo(listener->clientfd, &msg, &buf, 50);

		switch (status) {
			case ERL_TICK:
				/* erlang nodes send ticks to eachother to validate they are still reachable, we dont have to do anything here */
				break;
			case ERL_MSG:
				switch (msg.msgtype) {
					case ERL_SEND:
						/* we received an erlang message sent to a pid, process it! */
						if (handle_request(listener, &msg, &buf, &rbuf) != SWITCH_STATUS_SUCCESS) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Erlang request resulted in receiver termination\n");
							status = -1;
						}
						break;
					case ERL_REG_SEND:
						/* we received an erlang message sent to a registered process name, process it! */
						if (handle_request(listener, &msg, &buf, &rbuf) != SWITCH_STATUS_SUCCESS) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Erlang request resulted in receiver termination\n");
							status = -1;
						}
						break;
					case ERL_LINK:
						/* we received an erlang link request?  Should we be linking or are they linking to us and this just informs us? */
						break;
					case ERL_UNLINK:
						/* we received an erlang unlink request?  Same question as the ERL_LINK, are we expected to do something? */
						break;
					case ERL_EXIT:
						/* we received a notice that a process we were linked to has exited, clean up any bindings */
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Received erlang exit notice for %s <%d.%d.%d>\n", msg.from.node, msg.from.creation, msg.from.num, msg.from.serial);
						remove_pid_from_all_bindings(listener, &msg.from);
						break;
					default:
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Received unexpected erlang message type %d\n", (int) (msg.msgtype));
						break;
				}
				break;
			case ERL_ERROR:
				switch (erl_errno) {
					case ETIMEDOUT:
						/* if ei_xreceive_msg_tmo just timed out, ignore it and let the while loop check if we are still running */
						status = 1;
						break;
					case EAGAIN:
						/* the erlang lib just wants us to try to receive again, so we will! */
						status = 1;
						break;
					default:
						/* OH NOS! something has gone horribly wrong, shutdown the listener if status set by ei_xreceive_msg_tmo is less than or equal to 0 */
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Erlang communication fault with node %p %s (%s): erl_errno=%d errno=%d\n", (void *)listener, listener->peer_nodename, listener->remote_ip, erl_errno, errno);
						break;
				}
				break;
			default:
				/* HUH? didnt plan for this, whatevs shutdown the listener if status set by ei_xreceive_msg_tmo is less than or equal to 0 */
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unexpected erlang receive status %p %s (%s): %d\n", (void *)listener, listener->peer_nodename, listener->remote_ip, status);
				break;
		}

		if (switch_queue_trypop(listener->fetch_queue, &pop) == SWITCH_STATUS_SUCCESS) {
			char *uuid_str = (char *) pop;
			send_fetch_to_bindings(listener, uuid_str);
			switch_safe_free(uuid_str);
			pop = NULL;
		}

		if (switch_queue_trypop(listener->event_queue, &pop) == SWITCH_STATUS_SUCCESS) {
			switch_event_t *event = (switch_event_t *) pop;
			/* if there was an event waiting in our queue send it to */
			/* any erlang processes bound its type */
			send_event_to_bindings(listener, event);
			switch_event_destroy(&event);
			pop = NULL;
		}

		ei_x_free(&buf);
		ei_x_free(&rbuf);
	}

	/* flag this listener as stopped */
	stop_listener(listener);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Shutting down erlang event receiver %p: %s (%s)\n", (void *)listener, listener->peer_nodename, listener->remote_ip);

	/* remove the read lock that we have been holding on to while running */
	switch_thread_rwlock_unlock(listener->rwlock);

	destroy_listener(listener);

	switch_atomic_dec(&prefs.threads);
	return NULL;
}

/* Create a thread to wait for messages from an erlang node and process them */
static void launch_erl_to_fs_thread(listener_t *listener) {
	switch_thread_t *thread;
	switch_threadattr_t *thd_attr = NULL;

	switch_threadattr_create(&thd_attr, listener->pool);
	switch_threadattr_detach_set(thd_attr, 1);
	switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
	switch_thread_create(&thread, thd_attr, erl_to_fs_loop, listener, listener->pool);
}

static int read_cookie_from_file(char *filename) {
	int fd;
	char cookie[MAXATOMLEN + 1];
	char *end;
	struct stat buf;
	ssize_t res;

	if (!stat(filename, &buf)) {
		if ((buf.st_mode & S_IRWXG) || (buf.st_mode & S_IRWXO)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s must only be accessible by owner only.\n", filename);
			return 2;
		}
		if (buf.st_size > MAXATOMLEN) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s contains a cookie larger than the maximum atom size of %d.\n", filename, MAXATOMLEN);
			return 2;
		}
		fd = open(filename, O_RDONLY);
		if (fd < 1) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to open cookie file %s : %d.\n", filename, errno);
			return 2;
		}

		if ((res = read(fd, cookie, MAXATOMLEN)) < 1) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to read cookie file %s : %d.\n", filename, errno);
		}

		cookie[MAXATOMLEN] = '\0';

		/* replace any end of line characters with a null */
		if ((end = strchr(cookie, '\n'))) {
			*end = '\0';
		}

		if ((end = strchr(cookie, '\r'))) {
			*end = '\0';
		}

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Set cookie from file %s: %s\n", filename, cookie);

		set_pref_ei_cookie(cookie);
		return 0;
	} else {
		/* don't error here, because we might be blindly trying to read $HOME/.erlang.cookie, and that can fail silently */
		return 1;
	}
}

static int config(void) {
	char *cf = "kazoo.conf";
	switch_xml_t cfg, xml, settings, param;

	memset(&prefs, 0, sizeof(prefs));

	if (!(xml = switch_xml_open_cfg(cf, &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Open of %s failed\n", cf);
	} else {
		if ((settings = switch_xml_child(cfg, "settings"))) {
			for (param = switch_xml_child(settings, "param"); param; param = param->next) {
				char *var = (char *) switch_xml_attr_soft(param, "name");
				char *val = (char *) switch_xml_attr_soft(param, "value");

				if (!strcmp(var, "listen-ip")) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Set bind ip address: %s\n", val);
					set_pref_ip(val);
				} else if (!strcmp(var, "listen-port")) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Set port: %d\n", atoi(val));
					prefs.port = (uint16_t) atoi(val);
				} else if (!strcmp(var, "cookie")) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Set cookie: %s\n", val);
					set_pref_ei_cookie(val);
				} else if (!strcmp(var, "cookie-file")) {
					if (read_cookie_from_file(val) == 1) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to read cookie from %s\n", val);
					}
				} else if (!strcmp(var, "nodename")) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Set node name: %s\n", val);
					set_pref_ei_nodename(val);
				} else if (!strcmp(var, "shortname")) {
					prefs.ei_shortname = switch_true(val);
				} else if (!strcmp(var, "compat-rel")) {
					if (atoi(val) >= 7)
						prefs.ei_compat_rel = atoi(val);
					else
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Invalid compatability release '%s' specified\n", val);
				} else if (!strcmp(var, "debug")) {
					globals.debug = atoi(val);
				} else if (!strcmp(var, "encoding")) {
					if (!strcasecmp(val, "string")) {
						prefs.encoding = ERLANG_STRING;
					} else if (!strcasecmp(val, "binary")) {
						prefs.encoding = ERLANG_BINARY;
					} else {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Invalid encoding strategy '%s' specified\n", val);
					}
				} else if (!strcmp(var, "nat-map")) {
					if (switch_true(val) && switch_nat_get_type()) {
						prefs.nat_map = 1;
					}
				} else if (!strcasecmp(var, "apply-inbound-acl") && !zstr(val)) {
					if (prefs.acl_count < MAX_ACL) {
						prefs.acl[prefs.acl_count++] = strdup(val);
					} else {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Max acl records of %d reached\n", MAX_ACL);
					}
				}
			}
		}
		switch_xml_free(xml);
	}

	if (zstr(prefs.ip)) {
		set_pref_ip("0.0.0.0");
	}

	if (!prefs.port) {
		prefs.port = 8031;
	}

	if (zstr(prefs.ei_cookie)) {
		int res;
		char *home_dir = getenv("HOME");
		char path_buf[1024];

		if (!zstr(home_dir)) {
			/* $HOME/.erlang.cookie */
			switch_snprintf(path_buf, sizeof (path_buf), "%s%s%s", home_dir, SWITCH_PATH_SEPARATOR, ".erlang.cookie");
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Checking for cookie at path: %s\n", path_buf);

			res = read_cookie_from_file(path_buf);
			if (res) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No cookie or valid cookie file specified, using default cookie\n");
				set_pref_ei_cookie("ClueCon");
			}
		}
	}

	if (!prefs.ei_nodename) {
		set_pref_ei_nodename("freeswitch");
	}

	if (!prefs.nat_map) {
		prefs.nat_map = 0;
	}

	if (!prefs.port) {
		prefs.port = 8021;
	}

	return 0;
}

switch_status_t api_erlang_listeners(switch_stream_handle_t *stream) {
	listener_t *listener;

	switch_thread_rwlock_rdlock(acceptor.listeners_lock);
	for (listener = acceptor.listeners; listener; listener = listener->next) {
		stream->write_function(stream, "Listener to %s with %d outbound sessions\n"
							   ,listener->peer_nodename, count_session_bindings(listener));
	}
	switch_thread_rwlock_unlock(acceptor.listeners_lock);

	return SWITCH_STATUS_SUCCESS;
}

switch_status_t api_erlang_sessions(switch_stream_handle_t *stream) {
	listener_t *listener;

	switch_thread_rwlock_rdlock(acceptor.listeners_lock);
	for (listener = acceptor.listeners; listener; listener = listener->next) {
		stream->write_function(stream, "Listener to %s sessions\n", listener->peer_nodename);
		stream->write_function(stream, "-----------------------------------------------------------\n");
		display_session_bindings(listener, stream);
	}
	switch_thread_rwlock_unlock(acceptor.listeners_lock);

	return SWITCH_STATUS_SUCCESS;
}

switch_status_t api_erlang_event(switch_stream_handle_t *stream) {
	listener_t *listener;

	switch_thread_rwlock_rdlock(acceptor.listeners_lock);
	for (listener = acceptor.listeners; listener; listener = listener->next) {
		stream->write_function(stream, "Listener to %s bindings\n", listener->peer_nodename);
		stream->write_function(stream, "-----------------------------------------------------------\n");
		display_fetch_bindings(listener, stream);
		display_event_bindings(listener, stream);
	}
	switch_thread_rwlock_unlock(acceptor.listeners_lock);

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_API(exec_api_cmd)
{
	char *argv[1024] = { 0 };
	int command, argc = 0;
	char *mycmd = NULL;

	const char *usage_string = "USAGE:\n"
		"--------------------------------------------------------------------------------\n"
		"erlang listeners\n"
		"erlang sessions\n"
		"erlang bindings\n"
		"--------------------------------------------------------------------------------\n";

	if (zstr(cmd)) {
		stream->write_function(stream, "%s", usage_string);
		return SWITCH_STATUS_SUCCESS;
	}

	if (!(mycmd = strdup(cmd))) {
		switch_safe_free(mycmd);
		return SWITCH_STATUS_MEMERR;
	}

	if (!(argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0]))))) {
		stream->write_function(stream, "%s", usage_string);
		switch_safe_free(mycmd);
		return SWITCH_STATUS_SUCCESS;
	}

	if (find_api_command(argv[0], &command) != SWITCH_STATUS_SUCCESS) {
		stream->write_function(stream, "%s", usage_string);
		switch_safe_free(mycmd);
		return SWITCH_STATUS_SUCCESS;
	}

	switch(command) {
	case API_CMD_LISTENERS:
		api_erlang_listeners(stream);
		break;
	case API_CMD_SESSIONS:
		api_erlang_sessions(stream);
		break;
	case API_CMD_BINDINGS:
		api_erlang_event(stream);
		break;
	default:
		stream->write_function(stream, "%s", usage_string);
	}

	switch_safe_free(mycmd);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_kazoo_load) {
	switch_api_interface_t *api_interface;

	memset(&globals, 0, sizeof(globals));
	memset(&acceptor, 0, sizeof(acceptor));

	globals.pool = pool;

	/* initialize the listener mutex */
	switch_thread_rwlock_create(&acceptor.listeners_lock, pool);

	/* initialize the xml fetch response lock */
	switch_thread_rwlock_create(&globals.fetch_resp_lock, pool);

	/* initialize the xml fetch response hash */
	switch_core_hash_init(&globals.fetch_resp_hash, pool);

	/* bind to all switch events */
	if (switch_event_bind_removable(modname, SWITCH_EVENT_ALL, SWITCH_EVENT_SUBCLASS_ANY, event_handler, NULL, &globals.event_binding) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not bind to FreeSWITCH events\n");
		return SWITCH_STATUS_GENERR;
	}

	/* bind to all XML requests */
	if (switch_xml_bind_search_function_ret(fetch_handler, SWITCH_XML_SECTION_CONFIG, NULL, &globals.config_fetch_binding) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not bind to FreeSWITCH configuration XML\n");
		return SWITCH_STATUS_GENERR;
	}

	if (switch_xml_bind_search_function_ret(fetch_handler, SWITCH_XML_SECTION_DIRECTORY, NULL, &globals.directory_fetch_binding) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not bind to FreeSWITCH directory XML\n");
		return SWITCH_STATUS_GENERR;
	}

	if (switch_xml_bind_search_function_ret(fetch_handler, SWITCH_XML_SECTION_DIALPLAN, NULL, &globals.dialplan_fetch_binding) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not bind to FreeSWITCH dialplan XML\n");
		return SWITCH_STATUS_GENERR;
	}

	if (switch_xml_bind_search_function_ret(fetch_handler, SWITCH_XML_SECTION_CHATPLAN, NULL, &globals.chatplan_fetch_binding) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not bind to FreeSWITCH chatplan XML\n");
		return SWITCH_STATUS_GENERR;
	}

	/* connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	/* create an api for cli debug commands */
	SWITCH_ADD_API(api_interface, "erlang", "kazoo information", exec_api_cmd, "<command> [<args>]");
	switch_console_set_complete("add erlang listeners");
	switch_console_set_complete("add erlang sessions");
	switch_console_set_complete("add erlang bindings");

	config();

	switch_set_flag(&globals, LFLAG_RUNNING);

	switch_set_flag(&globals, LFLAG_READY);

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_kazoo_shutdown) {
	int sanity = 0;

	/* stop taking new requests and start shuting down the threads */
	switch_clear_flag(&globals, LFLAG_RUNNING);

	switch_event_unbind(&globals.event_binding);
	switch_xml_unbind_search_function(&globals.config_fetch_binding);
	switch_xml_unbind_search_function(&globals.directory_fetch_binding);
	switch_xml_unbind_search_function(&globals.dialplan_fetch_binding);
	switch_xml_unbind_search_function(&globals.chatplan_fetch_binding);

	/* give everyone time to cleanly shutdown */
	while (switch_atomic_read(&prefs.threads)) {
		switch_yield(100000);
		if (++sanity >= 200) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to kill all threads, continuing. Good luck!\n");
			break;
		}
	}

	/* Close the port we reserved for uPnP/Switch behind firewall, if necessary */
	if (prefs.nat_map && switch_nat_get_type()) {
		switch_nat_del_mapping(prefs.port, SWITCH_NAT_TCP);
	}

	for (uint32_t x = 0; x < prefs.acl_count; x++) {
		switch_safe_free(prefs.acl[x]);
	}

	/* clean up our allocated preferences */
	switch_safe_free(prefs.ip);
	switch_safe_free(prefs.ei_cookie);
	switch_safe_free(prefs.ei_nodename);

	/* clean up the fetch response hash */
	switch_core_hash_destroy(&globals.fetch_resp_hash);

	/* clean up the fetch response lock */
	switch_thread_rwlock_destroy(globals.fetch_resp_lock);

	/* clean up the listeners mutex */
	switch_thread_rwlock_destroy(acceptor.listeners_lock);

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_RUNTIME_FUNCTION(mod_kazoo_runtime) {
	switch_memory_pool_t *pool = NULL, *listener_pool = NULL;
	switch_status_t status;
	switch_sockaddr_t *sa;
	listener_t *listener;
	struct ei_cnode_s ec; /* erlang c node interface connection */
	ErlConnect conn;
	apr_os_sock_t sockfd;
	int clientfd, epmdfd = 0;
	switch_atomic_inc(&prefs.threads);

	while(!switch_test_flag(&globals, LFLAG_READY)) {
		switch_yield(100000);
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Erlang connection acceptor starting...\n");

	if (switch_core_new_memory_pool(&pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Out Of Memory: Oh My God! They killed Kenny! YOU BASTARDS!\n");
		return SWITCH_STATUS_TERM;
	}

	/* while the module is still running repeatedly try to open and listen to the provided ip:port until successful */
	while (switch_test_flag(&globals, LFLAG_RUNNING)) {
		status = switch_sockaddr_info_get(&sa, prefs.ip, SWITCH_UNSPEC, prefs.port, 0, pool);

		if (!status) {
			status = switch_socket_create(&acceptor.sock, switch_sockaddr_get_family(sa), SOCK_STREAM, SWITCH_PROTO_TCP, pool);
		}

		if (!status && acceptor.sock) {
			status = switch_socket_opt_set(acceptor.sock, SWITCH_SO_REUSEADDR, 1);
		}

		if (!status && acceptor.sock) {
			status = switch_socket_bind(acceptor.sock, sa);
		}

		if (!status && acceptor.sock) {
			status = switch_socket_listen(acceptor.sock, 5);
		}

		if (!status && acceptor.sock) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Erlang connection acceptor listening on %s:%u\n", prefs.ip, prefs.port);

			if (prefs.nat_map) {
				switch_nat_add_mapping(prefs.port, SWITCH_NAT_TCP, NULL, SWITCH_FALSE);
			}

			/* if the config has specified an erlang release compatability then pass that along to the erlang interface */
			if (prefs.ei_compat_rel) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Compatability with OTP R%d requested\n", prefs.ei_compat_rel);
				ei_set_compat_rel(prefs.ei_compat_rel);
			}

			/* try to initialize the erlang interface */
			if (SWITCH_STATUS_SUCCESS != initialize_ei(&ec, sa, &prefs)) {
				continue;
			}

			/* tell the erlang port manager where we can be reached.  this returns a file descriptor pointing to epmd or -1 */
			if ((epmdfd = ei_publish(&ec, prefs.port)) == -1) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
								  "Failed to start epmd, is it in the freeswith user $PATH? Try starting it yourself or run an erl shell with the -sname or -name option.  Shutting down.\n");
				continue;
			} else {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Connected to epmd and published erlang cnode name %s at port %d\n", ec.thisnodename, prefs.port);

				/* we are listening on a socket, configured the erlang interface, and published our node name to ip:port mapping... we are ready! */
				acceptor.ready = 1;

				break;
			}
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Erlang connection acceptor socket error, could not listen on %s:%u\n", prefs.ip, prefs.port);
			switch_yield(500000);
		}
	}

	/* TODO: move this into switch_apr as something like switch_os_sock_get(switch_os_sock_t *sockfd, switch_socket_t *sock); */
	apr_os_sock_get((apr_os_sock_t *) & sockfd, (apr_socket_t *) acceptor.sock);

	/* accept connections, negotiate cookies with the connecting node, then spawn two new threads for the node (one to send message to it and one to receive) */
	while (switch_test_flag(&globals, LFLAG_RUNNING)) {
		/* zero out errno because ei_accept doesn't differentiate between a */
		/* failed authentication or a socket failure, or a client version */
		/* mismatch or a godzilla attack (and a godzilla attack is highly likely) */
		errno = 0;

		/* wait here for an erlang node to connect, timming out to check if our module is still running every now-and-again */
		if ((clientfd = ei_accept_tmo(&ec, (int) sockfd, &conn, 498)) == ERL_ERROR) {
			if (erl_errno == ETIMEDOUT) {
				continue;
			} else if (errno) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Erlang connection acceptor socket error %d %d\n", erl_errno, errno);
			} else {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						"Erlang node connection failed - ensure your cookie matches '%s' and you are using a good nodename\n", prefs.ei_cookie);
			}
			continue;
		}

		if (!switch_test_flag(&globals, LFLAG_RUNNING)) {
			break;
		}

		/* NEW ERLANG NODE CONNECTION! Hello friend! */

		/* create memory pool for this listener */
		if (switch_core_new_memory_pool(&listener_pool) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Out of memory: Too bad drinking scotch isn't a paying job or Kenny's dad would be a millionare!\n");
			break;
		}

		/* from the listener's memory pool, allocate some memory for the listener's own structure */
		if (!(listener = switch_core_alloc(listener_pool, sizeof (*listener)))) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Out of memory: Stan, don't you know the first law of physics? Anything that's fun costs at least eight dollars.\n");
			break;
		}

		/* save the file descriptor that the erlang interface lib uses to communicate with the new node */
		listener->clientfd = clientfd;

		/* store the location of our pool in the listener and reset the var we used to temporarily store that */
		listener->pool = listener_pool;
		listener_pool = NULL;

		/* copy in the connection info, for use with the erlang interface lib later */
		listener->ec = switch_core_alloc(listener->pool, sizeof (ei_cnode));
		memcpy(listener->ec, &ec, sizeof (ei_cnode));

		/* when we start we are running */
		switch_set_flag(listener, LFLAG_RUNNING);

		/* create a mutex to controll access to the flags */
		switch_mutex_init(&listener->flag_mutex, SWITCH_MUTEX_DEFAULT, listener->pool);

		/* create queues for fetch requests and events to send to erlang */
		switch_queue_create(&listener->fetch_queue, MAX_QUEUE_LEN, listener->pool);
		switch_queue_create(&listener->event_queue, MAX_QUEUE_LEN, listener->pool);

		/* create a bunch of hashes for tracking things like bindings and such */
		switch_core_hash_init(&listener->event_bindings, listener->pool);
		switch_core_hash_init(&listener->session_bindings, listener->pool);
		switch_core_hash_init(&listener->fetch_bindings, listener->pool);

		/* create a mutex and some queues for the work we will be doing to process events */
		switch_thread_rwlock_create(&listener->event_rwlock, listener->pool);
		switch_thread_rwlock_create(&listener->session_rwlock, listener->pool);
		switch_thread_rwlock_create(&listener->fetch_rwlock, listener->pool);
		switch_thread_rwlock_create(&listener->rwlock, listener->pool);

		/* store the IP and node name we are talking with */
		switch_inet_ntop(AF_INET, conn.ipadr, listener->remote_ip, sizeof (listener->remote_ip));
		listener->peer_nodename = switch_core_strdup(listener->pool, conn.nodename);

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "New erlang connection from node %s (%s)\n", listener->peer_nodename, listener->remote_ip);

		/* Go do some real work - start the thread for this erlang node! */
		launch_erl_to_fs_thread(listener);

        listener = NULL;
	}

	if (epmdfd) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Closing epmd socket\n");
		close_socketfd(&epmdfd);
	}

	if (acceptor.sock) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Closing listening socket %s:%u\n", prefs.ip, prefs.port);
		close_socket(&acceptor.sock);
	}

	/* Free our memory pool for handling sockets */
	if (pool) {
		switch_core_destroy_memory_pool(&pool);
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Erlang connection acceptor shut down\n");

	switch_atomic_dec(&prefs.threads);
	return SWITCH_STATUS_TERM;
}


/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4:
 */
