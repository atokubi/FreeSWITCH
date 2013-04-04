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
 * Anthony Minessale II <anthm@freeswitch.org>
 * Andrew Thompson <andrew@hijacked.us>
 * Rob Charlton <rob.charlton@savageminds.com>
 * Darren Schreiber <d@d-man.org>
 * Mike Jerris <mike@jerris.com>
 * Tamas Cseke <tamas.cseke@virtual-call-center.eu>
 *
 *
 * handle_msg.c -- handle messages received from erlang nodes
 *
 */
#include <switch.h>
#include <ei.h>
#include "mod_kazoo.h"

struct api_command_struct_s {
	char *cmd;
	char *arg;
	listener_t *listener;
	char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1];
	erlang_pid pid;
	switch_memory_pool_t *pool;
};
typedef struct api_command_struct_s api_command_struct_t;

static char *TUPLETAG_COMMANDS[] = {
	"bgapi",
	"api",
	"event",
	"nixevent",
	"setevent",
	"sendevent",
	"sendmsg",
	"handlecall",
	"session_event",
	"session_nixevent",
	"session_setevent",
	"bind",
	"fetch_reply",
	"set_log_level",
	"rex"
};

typedef enum {
	TUPLETAG_BGAPI,
	TUPLETAG_API,
	TUPLETAG_EVENT,
	TUPLETAG_NIXEVENT,
	TUPLETAG_SETEVENT,
	TUPLETAG_SENDEVENT,
	TUPLETAG_SENDMSG,
	TUPLETAG_HANDLECALL,
	TUPLETAG_SESSION_EVENT,
	TUPLETAG_SESSION_NIXEVENT,
	TUPLETAG_SESSION_SETEVENT,
	TUPLETAG_BIND,
	TUPLETAG_FETCH_REPLY,
	TUPLETAG_SET_LOG_LEVEL,
	TUPLETAG_REX,
	TUPLETAG_MAX
} tupletag_commands_t;

static char *ATOM_COMMANDS[] = {
	"nolog",
	"register_log_handler",
	"register_event_handler",
	"noevents",
	"session_noevents",
	"exit",
	"getpid",
	"link",
	"version"
};

typedef enum {
	ATOM_NOLOG,
	ATOM_REGISTER_LOG_HANDLER,
	ATOM_REGISTER_EVENT_HANDLER,
	ATOM_NOEVENTS,
	ATOM_SESSION_NOEVENTS,
	ATOM_EXIT,
	ATOM_GETPID,
	ATOM_LINK,
	ATOM_VERSION,
	ATOM_MAX
} atom_commands_t;

static switch_status_t find_tupletag_command(char *tupletag, int *command) {
	for (int i = 0; i <= TUPLETAG_MAX; i++) {
		if(!strncmp(tupletag, TUPLETAG_COMMANDS[i], MAXATOMLEN)) {
			*command = i;
			return SWITCH_STATUS_SUCCESS;
		}
	}

	return SWITCH_STATUS_FALSE;
}

static switch_status_t find_atom_command(char *atom, int *command) {
	for (int i = 0; i <= ATOM_MAX; i++) {
		if(!strncmp(atom, ATOM_COMMANDS[i], MAXATOMLEN)) {
			*command = i;
			return SWITCH_STATUS_SUCCESS;
		}
	}

	return SWITCH_STATUS_FALSE;
}

static switch_status_t erlang_response_badarg(ei_x_buff * rbuf) {
	ei_x_encode_tuple_header(rbuf, 2);
	ei_x_encode_atom(rbuf, "error");
	ei_x_encode_atom(rbuf, "badarg");
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t erlang_response_baduuid(ei_x_buff * rbuf) {
	ei_x_encode_tuple_header(rbuf, 2);
	ei_x_encode_atom(rbuf, "error");
	ei_x_encode_atom(rbuf, "baduuid");
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t erlang_response_ignored(ei_x_buff * rbuf) {
	ei_x_encode_tuple_header(rbuf, 2);
	ei_x_encode_atom(rbuf, "error");
	ei_x_encode_atom(rbuf, "ignored");
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t erlang_response_notimplemented(ei_x_buff * rbuf) {
	ei_x_encode_tuple_header(rbuf, 2);
	ei_x_encode_atom(rbuf, "error");
	ei_x_encode_atom(rbuf, "not_implemented");
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t erlang_response_ok(ei_x_buff * rbuf) {
	ei_x_encode_atom(rbuf, "ok");
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t api_exec(char *cmd, char *arg, char **reply) {
	switch_stream_handle_t stream = { 0 };
	switch_status_t status = SWITCH_STATUS_FALSE;

	SWITCH_STANDARD_STREAM(stream);
		
	if (switch_api_execute(cmd, arg, NULL, &stream) != SWITCH_STATUS_SUCCESS) {
		*reply = switch_mprintf("%s: Command not found", cmd);
		status = SWITCH_STATUS_NOTFOUND;
	} else if (!stream.data || !strlen(stream.data)) {
		*reply = switch_mprintf("%s: Command returned no output", cmd);
		status = SWITCH_STATUS_FALSE;
	} else {
		*reply = strdup(stream.data);
		status = SWITCH_STATUS_SUCCESS;
	}

	/* if the reply starts with the char "-" (the start of -USAGE ...) */
	/* the args were missing or incorrect */
	if (**reply == '-') {
		status = SWITCH_STATUS_FALSE;
	}

	switch_safe_free(stream.data);

	return status;
}

static void *SWITCH_THREAD_FUNC bgapi_exec(switch_thread_t *thread, void *obj) {
	api_command_struct_t *acs = (api_command_struct_t *) obj;
	switch_memory_pool_t *pool = acs->pool;
	ei_x_buff rbuf;
	char *reply = NULL;
	char *cmd = acs->cmd;
	char *arg = acs->arg;
	listener_t *listener = acs->listener;

	if (!listener) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Internal error\n");
		return NULL;
	}

	if(!switch_test_flag(listener, LFLAG_RUNNING) || !switch_test_flag(&globals, LFLAG_RUNNING)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Ignoring command while shuting down\n");
		return NULL;
	}

	if(switch_thread_rwlock_rdlock(listener->rwlock) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to get read lock for listener\n");
		return NULL;
	}

	ei_x_new_with_version(&rbuf);
		
	ei_x_encode_tuple_header(&rbuf, 3);
	
	if (api_exec(cmd, arg, &reply) == SWITCH_STATUS_SUCCESS) {
		ei_x_encode_atom(&rbuf, "bgok");
	} else {
		ei_x_encode_atom(&rbuf, "bgerror");
	}

	_ei_x_encode_string(&rbuf, acs->uuid_str);
	_ei_x_encode_string(&rbuf, reply);

	ei_helper_send(listener, &acs->pid, &rbuf);

	ei_x_free(&rbuf);
	switch_safe_free(reply);
	switch_safe_free(acs->arg);
	switch_core_destroy_memory_pool(&pool);
	switch_thread_rwlock_unlock(listener->rwlock);

	return NULL;
}

static switch_status_t handle_request_api(listener_t *listener, erlang_msg *msg, int arity, ei_x_buff *buf, ei_x_buff *rbuf) {
	char cmd[MAXATOMLEN + 1];
	char *arg, *reply;

	if (arity != 3) {
		return erlang_response_badarg(rbuf);
	}

	if (ei_decode_atom_safe(buf->buff, &buf->index, cmd)) {
		return erlang_response_badarg(rbuf);
	}

	if (ei_decode_string_or_binary(buf->buff, &buf->index, &arg)) {
		return erlang_response_badarg(rbuf);
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "exec: %s(%s)\n", cmd, arg);

	ei_x_encode_tuple_header(rbuf, 2);
	
	if (api_exec(cmd, arg, &reply) == SWITCH_STATUS_SUCCESS) {
		ei_x_encode_atom(rbuf, "ok");
	} else {
		ei_x_encode_atom(rbuf, "error");
	}

	_ei_x_encode_string(rbuf, reply);
		
	switch_safe_free(arg);
	switch_safe_free(reply);

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t handle_request_bgapi(listener_t *listener, erlang_msg * msg, int arity, ei_x_buff * buf, ei_x_buff * rbuf) {
	api_command_struct_t *acs = NULL;
	switch_memory_pool_t *pool;
	switch_thread_t *thread;
	switch_threadattr_t *thd_attr = NULL;
	switch_uuid_t uuid;
	char cmd[MAXATOMLEN + 1];

	if (arity != 3) {
		return erlang_response_badarg(rbuf);
	}

	if (ei_decode_atom_safe(buf->buff, &buf->index, cmd)) {
		return erlang_response_badarg(rbuf);
	}

	switch_core_new_memory_pool(&pool);
	acs = switch_core_alloc(pool, sizeof(*acs));

	if (ei_decode_string_or_binary(buf->buff, &buf->index, &acs->arg)) {
		switch_core_destroy_memory_pool(&pool);
		return erlang_response_badarg(rbuf);
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "bgexec: %s(%s)\n", cmd, acs->arg);
	
	acs->pool = pool;
	acs->listener = listener;
	acs->cmd = switch_core_strdup(pool, cmd);
	acs->pid = msg->from;
	
	switch_threadattr_create(&thd_attr, acs->pool);
	switch_threadattr_detach_set(thd_attr, 1);
	switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
	
	switch_uuid_get(&uuid);
	switch_uuid_format(acs->uuid_str, &uuid);
	switch_thread_create(&thread, thd_attr, bgapi_exec, acs, acs->pool);
	
	ei_x_encode_tuple_header(rbuf, 2);
	ei_x_encode_atom(rbuf, "ok");
	_ei_x_encode_string(rbuf, acs->uuid_str);

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t handle_request_nixevent(listener_t *listener, erlang_msg * msg, int arity, ei_x_buff * buf, ei_x_buff * rbuf) {
    char event_name[MAXATOMLEN + 1];
	switch_event_types_t event_type;
	int custom = 0;

    if (arity == 1) {
		return erlang_response_badarg(rbuf);
    }

	for (int i = 1; i < arity; i++) {
		if (ei_decode_atom_safe(buf->buff, &buf->index, event_name)) {
			return erlang_response_badarg(rbuf);
		}

		if (custom) {
			remove_pid_from_custom_event_binding(listener, event_name, &msg->from);
		} else if (switch_name_event(event_name, &event_type) == SWITCH_STATUS_SUCCESS) {
			switch (event_type) {
			case SWITCH_EVENT_CUSTOM:
				custom++;
				break;
			case SWITCH_EVENT_ALL:
				remove_pid_from_event_bindings(listener, &msg->from);
				break;
			default:
				remove_pid_from_event_binding(listener, event_name, &msg->from);
			}
		} else {
			return erlang_response_badarg(rbuf);
		}
    }

	return erlang_response_ok(rbuf);
}

static switch_status_t handle_request_event(listener_t *listener, erlang_msg * msg, int arity, ei_x_buff * buf, ei_x_buff * rbuf) {
    char event_name[MAXATOMLEN + 1];
	switch_event_types_t event_type;
	int custom = 0;

    if (arity == 1) {
		return erlang_response_badarg(rbuf);
    }

	for (int i = 1; i < arity; i++) {
		if (ei_decode_atom_safe(buf->buff, &buf->index, event_name)) {
			return erlang_response_badarg(rbuf);
		}

		if (custom) {
			add_custom_event_binding(listener, event_name, &msg->from);
		} else if (switch_name_event(event_name, &event_type) == SWITCH_STATUS_SUCCESS) {
			switch (event_type) {
			case SWITCH_EVENT_CUSTOM:
				custom++;
				break;
			case SWITCH_EVENT_ALL:
				for (switch_event_types_t x = 0; x <= SWITCH_EVENT_ALL; x++) {
					switch_snprintf(event_name, sizeof(event_name), "%s", switch_event_name(x));
					add_event_binding(listener, event_name, &msg->from);
				}
				break;
			default:
				add_event_binding(listener, event_name, &msg->from);
			}
		} else {
			return erlang_response_badarg(rbuf);
		}
    }

	return erlang_response_ok(rbuf);
}

static char *expand_vars(char *xml_str) {
	char *var, *val;
	char *rp = xml_str; /* read pointer */
	char *ep, *wp, *buff; /* end pointer, write pointer, write buffer */
	
	if (!(strstr(xml_str, "$${"))) {
		return xml_str;
	}

	switch_zmalloc(buff, strlen(xml_str) * 2);
	wp = buff;
	ep = buff + (strlen(xml_str) * 2) - 1;

	while (*rp && wp < ep) {
		if (*rp == '$' && *(rp + 1) == '$' && *(rp + 2) == '{') {
			char *e = switch_find_end_paren(rp + 2, '{', '}');

			if (e) {
				rp += 3;
				var = rp;
				*e++ = '\0';
				rp = e;

				if ((val = switch_core_get_variable_dup(var))) {
					char *p;
					for (p = val; p && *p && wp <= ep; p++) {
						*wp++ = *p;
					}
					switch_safe_free(val);
				}
				continue;
			}
		}

		*wp++ = *rp++;
	}

	*wp++ = '\0';

	switch_safe_free(xml_str);
	return buff;
}

static switch_status_t handle_request_fetch_reply(listener_t *listener, erlang_msg * msg, int arity, ei_x_buff * buf, ei_x_buff * rbuf) {
	xml_fetch_msg_t *fetch_msg = NULL;
	switch_xml_t xml = NULL;
	char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1];
	char *xml_str;

	/* Try to get the fetch uuid string */
	if (ei_decode_string_or_binary_limited(buf->buff, &buf->index, sizeof(uuid_str), uuid_str)) {
		return erlang_response_badarg(rbuf);
	}

	/* Try to get the XML string */
	if (ei_decode_string_or_binary(buf->buff, &buf->index, &xml_str)) {
		return erlang_response_badarg(rbuf);
	}

	if (zstr(xml_str)) {
	    switch_safe_free(xml_str);
		return erlang_response_ignored(rbuf);
    }

    switch_thread_rwlock_rdlock(globals.fetch_resp_lock);
	if (!(fetch_msg = switch_core_hash_find(globals.fetch_resp_hash, uuid_str))) {
        switch_thread_rwlock_unlock(globals.fetch_resp_lock);
	    return erlang_response_baduuid(rbuf);
	}

	/* If we succeed in parsing the xml_str, it will be free'd by the core */
	xml_str = expand_vars(xml_str);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Parsed XML: %s\n", xml_str);
	if ((xml = switch_xml_parse_str_dynamic(xml_str, SWITCH_FALSE))) {
		fetch_msg->xml = xml;
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error parsing fetch response XML: %s\n", xml_str);
        switch_thread_rwlock_unlock(globals.fetch_resp_lock);
	    switch_safe_free(xml_str);
		return erlang_response_badarg(rbuf);
	}

	if (switch_mutex_trylock(fetch_msg->mutex) != SWITCH_STATUS_SUCCESS) {
        switch_thread_rwlock_unlock(globals.fetch_resp_lock);
		return erlang_response_ignored(rbuf);
	}

	switch_thread_cond_signal(fetch_msg->response_available);
    switch_thread_rwlock_unlock(globals.fetch_resp_lock);
	switch_mutex_unlock(fetch_msg->mutex);

	ei_x_encode_tuple_header(rbuf, 2);
	ei_x_encode_atom(rbuf, "ok");
	_ei_x_encode_string(rbuf, uuid_str);
	return SWITCH_STATUS_SUCCESS;
}

void switch_log_sendmsg(switch_core_session_t *session, switch_event_t *event)
{
	char *cmd = switch_event_get_header(event, "call-command");
	char *uuid = switch_core_session_get_uuid(session);
	unsigned long cmd_hash;
	switch_ssize_t hlen = -1;
	unsigned long CMD_EXECUTE = switch_hashfunc_default("execute", &hlen);
	unsigned long CMD_XFEREXT = switch_hashfunc_default("xferext", &hlen);
//	unsigned long CMD_HANGUP = switch_hashfunc_default("hangup", &hlen);
//	unsigned long CMD_NOMEDIA = switch_hashfunc_default("nomedia", &hlen);
//	unsigned long CMD_UNICAST = switch_hashfunc_default("unicast", &hlen);

	if (zstr(cmd)) {
		return;
	}

	cmd_hash = switch_hashfunc_default(cmd, &hlen);
	
	if (cmd_hash == CMD_EXECUTE) {
		char *app_name = switch_event_get_header(event, "execute-app-name");
		char *app_arg = switch_event_get_header(event, "execute-app-arg");
		
		if(app_name) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "log|%s|executing %s %s \n", uuid, app_name, switch_str_nil(app_arg));
		}
	} else if (cmd_hash == CMD_XFEREXT) {
		switch_event_header_t *hp;
	
		for (hp = event->headers; hp; hp = hp->next) {
			char *app_name;
			char *app_arg;
			
			if (!strcasecmp(hp->name, "application")) {
				app_name = strdup(hp->value);
				app_arg = strchr(app_name, ' ');
			
				if (app_arg) {
					*app_arg++ = '\0';
				}
			
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "log|%s|building xferext extension: %s %s\n", uuid, app_name, app_arg);
			}
		}		
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "log|%s|transfered call to xferext extension\n", uuid);
	}
}

static switch_status_t build_event(switch_event_t *event, ei_x_buff * buf) {
	int propslist_length, arity;

	if(!event) {
		return SWITCH_STATUS_FALSE;
	}

	if (ei_decode_list_header(buf->buff, &buf->index, &propslist_length)) {
		return SWITCH_STATUS_FALSE;
	}

	while (!ei_decode_tuple_header(buf->buff, &buf->index, &arity)) {
		char key[1024];
		char *value;

		if (arity != 2) {
			return SWITCH_STATUS_FALSE;
		}

		if (ei_decode_string_or_binary_limited(buf->buff, &buf->index, sizeof(key), key)) {
			return SWITCH_STATUS_FALSE;
		}

		if (ei_decode_string_or_binary(buf->buff, &buf->index, &value)) {
			return SWITCH_STATUS_FALSE;
		}

		if (!strcmp(key, "body")) {
			switch_safe_free(event->body);
			event->body = value;
		} else  {
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM | SWITCH_STACK_NODUP, key, value);
		}
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t handle_request_sendevent(listener_t *listener, erlang_msg * msg, int arity, ei_x_buff * buf, ei_x_buff * rbuf) {
	char event_name[MAXATOMLEN + 1];
	char subclass_name[MAXATOMLEN + 1];
	switch_event_types_t event_type;
	switch_event_t *event = NULL;

	if (ei_decode_atom_safe(buf->buff, &buf->index, event_name) 
		|| switch_name_event(event_name, &event_type) != SWITCH_STATUS_SUCCESS) 
	{
		return erlang_response_badarg(rbuf);
	}

	if (!strncasecmp(event_name, "CUSTOM", MAXATOMLEN)) {
		if(ei_decode_atom(buf->buff, &buf->index, subclass_name)) {
			return erlang_response_badarg(rbuf);
		}
		switch_event_create_subclass(&event, event_type, subclass_name);
	} else {
		switch_event_create(&event, event_type);
	}

	if (build_event(event, buf) == SWITCH_STATUS_SUCCESS) {
		switch_event_fire(&event);
		return erlang_response_ok(rbuf);
	} 
	
	if(event) {
		switch_event_destroy(&event);
	}

	return erlang_response_badarg(rbuf);
}

static switch_status_t handle_request_sendmsg(listener_t *listener, erlang_msg * msg, int arity, ei_x_buff * buf, ei_x_buff * rbuf) {
	switch_core_session_t *session;
	switch_event_t *event = NULL;
	char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1];

	if (ei_decode_string_or_binary_limited(buf->buff, &buf->index, sizeof(uuid_str), uuid_str)) {
		return erlang_response_badarg(rbuf);
	}

	if (zstr_buf(uuid_str) || !(session = switch_core_session_locate(uuid_str))) {
		return erlang_response_baduuid(rbuf);
	}

	switch_event_create(&event, SWITCH_EVENT_SEND_MESSAGE);

	if (build_event(event, buf) == SWITCH_STATUS_SUCCESS) {
		switch_log_sendmsg(session, event);
		switch_core_session_queue_private_event(session, &event, SWITCH_FALSE);
		switch_core_session_rwunlock(session);
		return erlang_response_ok(rbuf);
	} else {
		switch_core_session_rwunlock(session);
		return erlang_response_badarg(rbuf);
	}
}

static switch_status_t handle_request_bind(listener_t *listener, erlang_msg * msg, ei_x_buff * buf, ei_x_buff * rbuf) {
	char section_str[MAXATOMLEN + 1];
	switch_xml_section_t section;

	if (ei_decode_atom_safe(buf->buff, &buf->index, section_str) || !(section = switch_xml_parse_section_string(section_str))) {
		return erlang_response_badarg(rbuf);
	}

	add_fetch_binding(listener, section_str, &msg->from);

	return erlang_response_ok(rbuf);
}

/* NOTE: this does NOT support mod_erlang_event handlecall/3, pid only */
static switch_status_t handle_request_handlecall(listener_t *listener, erlang_msg * msg, int arity, ei_x_buff * buf, ei_x_buff * rbuf) {
    char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1];
	switch_core_session_t *session;

    if (arity != 2) {
		return erlang_response_badarg(rbuf);
	}

	if (ei_decode_string_or_binary_limited(buf->buff, &buf->index, sizeof(uuid_str), uuid_str)) {
		return erlang_response_badarg(rbuf);
	}

	if (!zstr_buf(uuid_str) 
		&& (session = switch_core_session_locate(uuid_str))
		&& (add_session_binding(listener, uuid_str, &msg->from) == SWITCH_STATUS_SUCCESS)) 
	{
		/* release the lock returned by session locate */
		switch_core_session_rwunlock(session);

		return erlang_response_ok(rbuf);
	}

	return erlang_response_baduuid(rbuf);
}

/* fake enough of the net_kernel module to be able to respond to net_adm:ping */
static switch_status_t handle_net_kernel_request(listener_t *listener, erlang_msg * msg, ei_x_buff * buf, ei_x_buff * rbuf) {
    int size, type, arity;
    char atom[MAXATOMLEN + 1];
    erlang_ref ref;
    erlang_pid pid;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Received net_kernel message, attempting to reply\n");

    ei_get_type(buf->buff, &buf->index, &type, &size);

    /* is_tuple(Buff) */
    if (type != ERL_SMALL_TUPLE_EXT) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Received net_kernel message of an unexpected type\n");
        return SWITCH_STATUS_FALSE;
    }

    ei_decode_tuple_header(buf->buff, &buf->index, &arity);

    /* {_, _, _} = Buf */
    if (arity != 3) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Received net_kernel tuple has an unexpected arity\n");
        return SWITCH_STATUS_FALSE;
    }

    /* {'$gen_call', _, _} = Buf */
    if (ei_decode_atom_safe(buf->buff, &buf->index, atom) || strncmp(atom, "$gen_call", MAXATOMLEN)) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Received net_kernel message tuple does not begin with the atom '$gen_call'\n");
        return SWITCH_STATUS_FALSE;
    }

    ei_get_type(buf->buff, &buf->index, &type, &size);

    /* {_, Sender, _}=Buff, is_tuple(Sender) */
    if (type != ERL_SMALL_TUPLE_EXT) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Second element of the net_kernel tuple is an unexpected type\n");
        return SWITCH_STATUS_FALSE;
    }

    ei_decode_tuple_header(buf->buff, &buf->index, &arity);

    /* {_, _}=Sender */
    if (arity != 2) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Second element of the net_kernel message has an unexpected arity\n");
        return SWITCH_STATUS_FALSE;
    }

    /* {Pid, Ref}=Sender */
    if (ei_decode_pid(buf->buff, &buf->index, &pid) || ei_decode_ref(buf->buff, &buf->index, &ref)) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Unable to decode erlang pid or ref of the net_kernel tuple second element\n");
        return SWITCH_STATUS_FALSE;
    }

    ei_get_type(buf->buff, &buf->index, &type, &size);

    /* {_, _, Request}=Buff, is_tuple(Request) */
    if (type != ERL_SMALL_TUPLE_EXT) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Third element of the net_kernel message is an unexpected type\n");
        return SWITCH_STATUS_FALSE;
    }

    ei_decode_tuple_header(buf->buff, &buf->index, &arity);

    /* {_, _}=Request */
    if (arity != 2) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Third element of the net_kernel message has an unexpected arity\n");
        return SWITCH_STATUS_FALSE;
    }

    /* {is_auth, _}=Request */
    if (ei_decode_atom_safe(buf->buff, &buf->index, atom) || strncmp(atom, "is_auth", MAXATOMLEN)) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "The net_kernel message third element does not begin with the atom 'is_auth'\n");
        return SWITCH_STATUS_FALSE;
    }

    /* To ! {Tag, Reply} */
    ei_x_encode_tuple_header(rbuf, 2);
    ei_x_encode_ref(rbuf, &ref);
    ei_x_encode_atom(rbuf, "yes");

    ei_helper_send(listener, &pid, rbuf);

    return SWITCH_STATUS_SUCCESS;
}

static switch_status_t handle_tuple_request(listener_t *listener, erlang_msg * msg, ei_x_buff * buf, ei_x_buff * rbuf) {
    char tupletag[MAXATOMLEN + 1];
    int command, arity;

    ei_decode_tuple_header(buf->buff, &buf->index, &arity);
	
	if (ei_decode_atom_safe(buf->buff, &buf->index, tupletag)) {
		return erlang_response_badarg(rbuf);
    }

	if (find_tupletag_command(tupletag, &command) != SWITCH_STATUS_SUCCESS) {
		return erlang_response_badarg(rbuf);
	}

	switch(command) {
	case TUPLETAG_BGAPI:
		return handle_request_bgapi(listener, msg, arity, buf, rbuf);
	case TUPLETAG_API:
		return  handle_request_api(listener, msg, arity, buf, rbuf);
	case TUPLETAG_EVENT:
		return  handle_request_event(listener, msg, arity, buf, rbuf);
	case TUPLETAG_NIXEVENT:
		return handle_request_nixevent(listener, msg, arity, buf, rbuf);
	case TUPLETAG_SENDEVENT:
		return handle_request_sendevent(listener, msg, arity, buf, rbuf);
	case TUPLETAG_SENDMSG:
		return handle_request_sendmsg(listener, msg, arity, buf, rbuf);
	case TUPLETAG_HANDLECALL:
		return handle_request_handlecall(listener, msg, arity, buf, rbuf);
	case TUPLETAG_BIND:
		return handle_request_bind(listener, msg, buf, rbuf);
	case TUPLETAG_FETCH_REPLY:
		return handle_request_fetch_reply(listener, msg, arity, buf, rbuf);
	default:
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Recieved erlang tuple with unimplemented tupletag: %s\n", tupletag);
		return erlang_response_notimplemented(rbuf);		
	}
}

static switch_status_t handle_atom_request(listener_t *listener, erlang_msg * msg, ei_x_buff * buf, ei_x_buff * rbuf) {
    char atom[MAXATOMLEN + 1];
	int command;

    if (ei_decode_atom_safe(buf->buff, &buf->index, atom)) {
		return erlang_response_badarg(rbuf);
    }

	if (find_atom_command(atom, &command) != SWITCH_STATUS_SUCCESS) {
		return erlang_response_badarg(rbuf);
	}

	switch(command) {
	case ATOM_REGISTER_EVENT_HANDLER:
		return erlang_response_ok(rbuf);
	case ATOM_NOEVENTS:
        remove_pid_from_event_bindings(listener, &msg->from);
		return erlang_response_ok(rbuf);
	case ATOM_EXIT:
		erlang_response_ok(rbuf);
        return SWITCH_STATUS_TERM;
	case ATOM_GETPID:
        ei_x_encode_tuple_header(rbuf, 2);
        ei_x_encode_atom(rbuf, "ok");
        ei_x_encode_pid(rbuf, ei_self(listener->ec));
		return SWITCH_STATUS_SUCCESS;
	case ATOM_LINK:		
        ei_link(listener, ei_self(listener->ec), &msg->from);
        return SWITCH_STATUS_NOOP;
	case ATOM_VERSION:
        ei_x_encode_tuple_header(rbuf, 2);
        ei_x_encode_atom(rbuf, "ok");
		_ei_x_encode_string(rbuf, "mod_kazoo v1.0.0");
		return SWITCH_STATUS_SUCCESS;
	default:
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Recieved erlang atom for unimplemented feature: %s\n", atom);
		return erlang_response_notimplemented(rbuf);
	}
}

switch_status_t handle_request(listener_t *listener, erlang_msg * msg, ei_x_buff * buf, ei_x_buff * rbuf) {
    int type, size, version;
    switch_status_t ret = SWITCH_STATUS_SUCCESS;

	buf->index = 0;
	ei_decode_version(buf->buff, &buf->index, &version);

	if (msg->msgtype == ERL_REG_SEND && !strncmp(msg->toname, "net_kernel", MAXATOMLEN)) {
		return handle_net_kernel_request(listener, msg, buf, rbuf);
	}

	ei_get_type(buf->buff, &buf->index, &type, &size);
	
	switch (type) {
	case ERL_SMALL_TUPLE_EXT:
	case ERL_LARGE_TUPLE_EXT:
		ret = handle_tuple_request(listener, msg, buf, rbuf);
		break;
	case ERL_ATOM_EXT:
		ret = handle_atom_request(listener, msg, buf, rbuf);
		break;
	default:
		/* some other kind of erlang term */
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Received unexpected erlang term, started with type %d\n", type);
		ret = erlang_response_badarg(rbuf);
		break;
	}

    if (ret == SWITCH_STATUS_NOOP) {
        ret = SWITCH_STATUS_SUCCESS;
    } else if (rbuf->index > 1) {
        ei_helper_send(listener, &msg->from, rbuf);
    }

    return ret;
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
