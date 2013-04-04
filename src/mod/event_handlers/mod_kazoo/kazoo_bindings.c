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
 *
 *
 *
 */
#include <switch.h>
#include <ei.h>
#include "mod_kazoo.h"

/**
 * =============================================================================
 *
 *                          General Binding Functions
 *
 * =============================================================================
 */
SWITCH_HASH_DELETE_FUNC(bindings_pid_cleanup_callback)
{
    erlang_pid *pid = (erlang_pid *) val;
    
    switch_safe_free(pid);

    return SWITCH_TRUE;
}

switch_status_t add_binding(switch_memory_pool_t *pool, switch_hash_t *bindings, switch_thread_rwlock_t *rwlock, char *key, erlang_pid *from) {
	switch_status_t status = SWITCH_STATUS_FALSE;
    switch_hash_t *hash;
    erlang_pid *pid;
    char sub_key[MAX_PID_CHARS];

    switch_snprintf(sub_key, sizeof(sub_key), "<%d.%d.%d>", from->creation, from->num, from->serial);
    pid = malloc(sizeof(erlang_pid));
    memcpy(pid, from, sizeof(erlang_pid));

	switch_thread_rwlock_wrlock(rwlock);

    if ((hash = switch_core_hash_find(bindings, key))) {
        /* if the bindings hash has a hash as the value already just insert the pid into the sub-hash */
        status = switch_core_hash_insert(hash, sub_key, pid);
    } else {
        /* if the fetch_bindings hash doesnt have a value for this fetch type, create a new hash with the pid then store the new hash in fetch_bindings */
        switch_core_hash_init(&hash, pool);
        switch_core_hash_insert(hash, sub_key, pid);
        status = switch_core_hash_insert(bindings, key, hash);
	}

	switch_thread_rwlock_unlock(rwlock);

	return status;
}

switch_status_t remove_pid_from_binding(switch_hash_t *bindings, switch_thread_rwlock_t *rwlock, char *key, erlang_pid *from) {
    switch_hash_t *hash;

	switch_thread_rwlock_wrlock(rwlock);

    if ((hash = switch_core_hash_find(bindings, key))) {
        erlang_pid *pid;
        char sub_key[MAX_PID_CHARS];

        /* if bindings has an hash value for this section type remove pid from the sub-hash */
        switch_snprintf(sub_key, sizeof(sub_key), "<%d.%d.%d>", from->creation, from->num, from->serial);

        if ((pid = (erlang_pid *)switch_core_hash_find(hash, sub_key))) {
            switch_core_hash_delete(hash, sub_key);
            switch_safe_free(pid);

	        if (!switch_hash_first(NULL, hash)) {
			    switch_core_hash_destroy(&hash);
			    switch_core_hash_delete(bindings, key);
			}
		}
    }

	switch_thread_rwlock_unlock(rwlock);

    return SWITCH_STATUS_SUCCESS;
}

switch_status_t remove_pid_from_bindings(switch_hash_t *bindings, switch_thread_rwlock_t *rwlock, erlang_pid *from) {
    switch_hash_index_t *index;
    switch_hash_t *hash;
	switch_event_t *event = NULL;
    char sub_key[MAX_PID_CHARS];

    switch_snprintf(sub_key, sizeof(sub_key), "<%d.%d.%d>", from->creation, from->num, from->serial);

	switch_thread_rwlock_wrlock(rwlock);

    /* loop over all bindings removing the pid from the hash found as the value */
    for (index = switch_hash_first(NULL, bindings); index; index = switch_hash_next(index)) {
        erlang_pid *pid;
        const void *key;
        void *value;

        switch_hash_this(index, &key, NULL, &value);
        hash = (switch_hash_t*) value;
	
        if ((pid = (erlang_pid *) switch_core_hash_find(hash, sub_key))) {
            switch_core_hash_delete(hash, sub_key);
            switch_safe_free(pid);

			/* if there is nothing left in the sub_hash, destroy it and schedule the key for removal */
	        if (!switch_hash_first(NULL, hash)) {
				if(!event) {
					switch_event_create_subclass(&event, SWITCH_EVENT_CLONE, NULL);
					switch_assert(event);
				}				
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "delete", (const char *) key);
			    switch_core_hash_destroy(&hash);
			}
		}
    }

	if(event) {
		switch_event_header_t *header = NULL;
		for (header = event->headers; header; header = header->next) {
			switch_core_hash_delete(bindings, header->value);
		}
		switch_event_destroy(&event);
	}

	switch_thread_rwlock_unlock(rwlock);

    return SWITCH_STATUS_SUCCESS;
}

switch_status_t flush_bindings(switch_hash_t *bindings, switch_thread_rwlock_t *rwlock) {
    switch_hash_index_t *index;
    switch_hash_t *hash;

	switch_thread_rwlock_wrlock(rwlock);

    /* loop over all bindings removing all hashes */
    for (index = switch_hash_first(NULL, bindings); index; index = switch_hash_next(index)) {
        const void *key;
        void *value;

        switch_hash_this(index, &key, NULL, &value);
        hash = (switch_hash_t*) value;

        /* free all the erlang_pids in this hash, then destroy the hash itself */
        switch_core_hash_delete_multi(hash, bindings_pid_cleanup_callback, NULL);    
        switch_core_hash_destroy(&hash);
    }
    
    /* delete all the elements in the bindings hash since they they have been destroyed */
    switch_core_hash_delete_multi(bindings, NULL, NULL);

	switch_thread_rwlock_unlock(rwlock);

    return SWITCH_STATUS_SUCCESS;
}

switch_status_t display_bindings(switch_hash_t *bindings, switch_thread_rwlock_t *rwlock, switch_stream_handle_t *stream) {
    switch_hash_index_t *index;

	switch_thread_rwlock_rdlock(rwlock);
	for (index = switch_hash_first(NULL, bindings); index; index = switch_hash_next(index)) {
		switch_hash_index_t *sub_index;
		switch_hash_t *hash;
		const void *key;
		void *value;

		switch_hash_this(index, &key, NULL, &value);
		hash = (switch_hash_t *) value;

		stream->write_function(stream, "  %s:", (const char *)key);

		for (sub_index = switch_hash_first(NULL, hash); sub_index; sub_index = switch_hash_next(sub_index)) {
			erlang_pid *pid;

			switch_hash_this(sub_index, &key, NULL, &value);
			pid = (erlang_pid*) value;

			stream->write_function(stream, " <%d.%d.%d> ", pid->creation, pid->num, pid->serial);
		}
		stream->write_function(stream, "\n");
	}
	switch_thread_rwlock_unlock(rwlock);

	return SWITCH_STATUS_FOUND;

}

int count_bindings(switch_hash_t *bindings, switch_thread_rwlock_t *rwlock) {
    switch_hash_index_t *index;
	int count = 0;

	switch_thread_rwlock_rdlock(rwlock);

    for (index = switch_hash_first(NULL, bindings); index; index = switch_hash_next(index)) {
		count++;
	}

	switch_thread_rwlock_unlock(rwlock);

	return count;
}

/**
 * =============================================================================
 *
 *                               Fetch Bindings
 *
 * =============================================================================
 */
switch_status_t display_fetch_bindings(listener_t *listener, switch_stream_handle_t *stream) {
	return display_bindings(listener->fetch_bindings, listener->fetch_rwlock, stream);
}

switch_status_t has_fetch_bindings(listener_t *listener, const char *section) {
    /* just check if the hash has an entry for this uuid, dont really care */
    /* what value/pids/ect are there yet */
    if (switch_core_hash_find_rdlock(listener->fetch_bindings, section, listener->fetch_rwlock)) {
        return SWITCH_STATUS_FOUND;
    } else {
        return SWITCH_STATUS_NOTFOUND;
    }
}

switch_status_t send_fetch_to_bindings(listener_t *listener, char *uuid_str) {
	xml_fetch_msg_t *fetch_msg = NULL;
	const char *section;
	const char *tag_name;
	const char *key_name;
	const char *key_value;
    switch_hash_t *bindings;
    switch_hash_index_t *binding;

	switch_thread_rwlock_rdlock(globals.fetch_resp_lock);
	if (!(fetch_msg = switch_core_hash_find(globals.fetch_resp_hash, uuid_str))) {
		switch_thread_rwlock_unlock(globals.fetch_resp_lock);
		return SWITCH_STATUS_SUCCESS;
	}

	section = fetch_msg->section;
	tag_name = fetch_msg->tag_name;
	key_name = fetch_msg->key_name;
	key_value = fetch_msg->key_value;

	switch_thread_rwlock_rdlock(listener->fetch_rwlock);
    if ((bindings = switch_core_hash_find(listener->fetch_bindings, section))) {
		ei_x_buff ebuf;
		int start_index;

		ei_x_new_with_version(&ebuf);
		
		ei_x_encode_tuple_header(&ebuf, 7);
		ei_x_encode_atom(&ebuf, "fetch");
		ei_x_encode_atom(&ebuf, section);
		_ei_x_encode_string(&ebuf, tag_name ? tag_name : "undefined");
		_ei_x_encode_string(&ebuf, key_name ? key_name : "undefined");
		_ei_x_encode_string(&ebuf, key_value ? key_value : "undefined");
		_ei_x_encode_string(&ebuf, uuid_str);
		
		if (fetch_msg->params) {
			ei_encode_switch_event_headers(&ebuf, fetch_msg->params);
		} else {
			ei_x_encode_empty_list(&ebuf);
		}

		switch_thread_rwlock_unlock(globals.fetch_resp_lock);

        /* loop over all the entries of this sub-hash and send the fetch request to each */
		start_index = ebuf.index;
        for (binding = switch_hash_first(NULL, bindings); binding; binding = switch_hash_next(binding)) {
            erlang_pid *pid;
            const void *key;
            void *value;
			
            switch_hash_this(binding, &key, NULL, &value);
            pid = (erlang_pid*) value;

            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Sending erlang (%s) <%d.%d.%d> fetch request %s: %s / %s / %s = %s\n"
							  ,listener->peer_nodename, pid->creation, pid->num, pid->serial, uuid_str, section, tag_name, key_name, key_value);

			ebuf.index = start_index;
            ei_helper_send(listener, pid, &ebuf);
		}

		ei_x_free(&ebuf);
	} else {
		switch_thread_rwlock_unlock(globals.fetch_resp_lock);
	}

	switch_thread_rwlock_unlock(listener->fetch_rwlock);

    return SWITCH_STATUS_SUCCESS;
}

switch_status_t add_fetch_binding(listener_t *listener, char *section, erlang_pid *from) {
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Creating erlang (%s) <%d.%d.%d> fetch binding for %s\n"
					  ,listener->peer_nodename, from->creation, from->num, from->serial, section);	
	
	if (add_binding(listener->pool, listener->fetch_bindings, listener->fetch_rwlock, section, from) == SWITCH_STATUS_SUCCESS) {
		ei_link(listener, ei_self(listener->ec), from);
		return SWITCH_STATUS_SUCCESS;
	} 

	return SWITCH_STATUS_FALSE;
}

switch_status_t remove_pid_from_fetch_binding(listener_t *listener, char *section, erlang_pid *from) {
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Removed erlang (%s) <%d.%d.%d> fetch binding for %s\n"
					  ,listener->peer_nodename, from->creation, from->num, from->serial, section);

	return remove_pid_from_binding(listener->fetch_bindings, listener->fetch_rwlock, section, from);
}

switch_status_t remove_pid_from_fetch_bindings(listener_t *listener, erlang_pid *from) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Removed erlang (%s) <%d.%d.%d> fetch bindings\n"
					  ,listener->peer_nodename, from->creation, from->num, from->serial);

	return remove_pid_from_bindings(listener->fetch_bindings, listener->fetch_rwlock, from);
}

switch_status_t flush_fetch_bindings(listener_t *listener) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Flushed all erlang fetch bindings for node %s\n", listener->peer_nodename);

	return flush_bindings(listener->fetch_bindings, listener->fetch_rwlock);
}


/**
 * =============================================================================
 *
 *                               Session Bindings
 *
 * =============================================================================
 */
switch_status_t display_session_bindings(listener_t *listener, switch_stream_handle_t *stream) {
	return display_bindings(listener->session_bindings, listener->session_rwlock, stream);
}

int count_session_bindings(listener_t *listener) {
	return count_bindings(listener->session_bindings, listener->session_rwlock);
}

switch_status_t has_session_bindings(listener_t *listener, char *uuid_str) {
    /* just check if the hash has an entry for this uuid, dont really care */
    /* what value/pids/ect are there yet */
    if (switch_core_hash_find_rdlock(listener->session_bindings, uuid_str, listener->session_rwlock)) {
        return SWITCH_STATUS_FOUND;
    } else {
        return SWITCH_STATUS_NOTFOUND;
    }
}

switch_status_t add_session_binding(listener_t *listener, char *uuid_str, erlang_pid *from) {
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Creating erlang (%s) <%d.%d.%d> session binding for %s\n"
					  ,listener->peer_nodename, from->creation, from->num, from->serial, uuid_str);

	if (add_binding(listener->pool, listener->session_bindings, listener->session_rwlock, uuid_str, from) == SWITCH_STATUS_SUCCESS) {
		ei_link(listener, ei_self(listener->ec), from);
		return SWITCH_STATUS_SUCCESS;
	} 

	return SWITCH_STATUS_FALSE;
}

switch_status_t remove_pid_from_session_binding(listener_t *listener, char *uuid_str, erlang_pid *from) {
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Removed erlang (%s) <%d.%d.%d> session binding for %s\n"
					  ,listener->peer_nodename, from->creation, from->num, from->serial, uuid_str);

	return remove_pid_from_binding(listener->session_bindings, listener->session_rwlock, uuid_str, from);
}

switch_status_t remove_pid_from_session_bindings(listener_t *listener, erlang_pid *from) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Removed erlang (%s) <%d.%d.%d> session bindings\n"
					  ,listener->peer_nodename, from->creation, from->num, from->serial);

	return remove_pid_from_bindings(listener->session_bindings, listener->session_rwlock, from);
}

switch_status_t flush_session_bindings(listener_t *listener) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Flushed all erlang session bindings for node %s\n", listener->peer_nodename);

	return flush_bindings(listener->session_bindings, listener->session_rwlock);
}


/**
 * =============================================================================
 *
 *                               Event Bindings
 *
 * =============================================================================
 */
switch_status_t display_event_bindings(listener_t *listener, switch_stream_handle_t *stream) {
	return display_bindings(listener->event_bindings, listener->event_rwlock, stream);
}

char * get_event_hash_key(switch_event_t *event) {
	/* TODO: this should be something more like "custom {subclass_name}" */
	/* but I dont want to malloc the memory, and what are the chances the */
	/* subclass name has a collision with an actual event name... */
	switch(event->event_id) {
	case SWITCH_EVENT_CUSTOM:
		return event->subclass_name;
		break;
	default:
		return (char *)switch_event_name(event->event_id);
	}
}

switch_status_t has_event_bindings(listener_t *listener, switch_event_t *event) {
    char *event_name = get_event_hash_key(event);

    /* just check if the hash has an entry for this event type, dont really care */
    /* what value/pids/ect are there yet */
    if (switch_core_hash_find_rdlock(listener->event_bindings, event_name, listener->event_rwlock)) {
        return SWITCH_STATUS_FOUND;
    }

	return SWITCH_STATUS_NOTFOUND;
}

switch_status_t add_event_binding(listener_t *listener, char *event_name, erlang_pid *from) {
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Creating erlang (%s) <%d.%d.%d> event binding for %s\n"
					  ,listener->peer_nodename, from->creation, from->num, from->serial, event_name);

	if (add_binding(listener->pool, listener->event_bindings, listener->event_rwlock, event_name, from) == SWITCH_STATUS_SUCCESS) {
		ei_link(listener, ei_self(listener->ec), from);
		return SWITCH_STATUS_SUCCESS;
	} 

	return SWITCH_STATUS_FALSE;
}

switch_status_t add_custom_event_binding(listener_t *listener, char *subclass_name, erlang_pid *from) {
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Creating erlang (%s) <%d.%d.%d> event binding for %s\n"
					  ,listener->peer_nodename, from->creation, from->num, from->serial, subclass_name);

	if (add_binding(listener->pool, listener->event_bindings, listener->event_rwlock, subclass_name, from) == SWITCH_STATUS_SUCCESS) {
		ei_link(listener, ei_self(listener->ec), from);
		return SWITCH_STATUS_SUCCESS;
	} 

	return SWITCH_STATUS_FALSE;
}

switch_status_t remove_pid_from_event_binding(listener_t *listener, char *event_name, erlang_pid *from) {
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Removed erlang (%s) <%d.%d.%d> event binding for %s\n"
					  ,listener->peer_nodename, from->creation, from->num, from->serial, event_name);

	return remove_pid_from_binding(listener->event_bindings, listener->event_rwlock, event_name, from);
}

switch_status_t remove_pid_from_custom_event_binding(listener_t *listener, char *subclass_name, erlang_pid *from) {
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Removed erlang (%s) <%d.%d.%d> event binding for %s\n"
					  ,listener->peer_nodename, from->creation, from->num, from->serial, subclass_name);

	return remove_pid_from_binding(listener->event_bindings, listener->event_rwlock, subclass_name, from);
}

switch_status_t remove_pid_from_event_bindings(listener_t *listener, erlang_pid *from) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Removed erlang (%s) <%d.%d.%d> event bindings\n"
					  ,listener->peer_nodename, from->creation, from->num, from->serial);

	return remove_pid_from_bindings(listener->event_bindings, listener->event_rwlock, from);
}

switch_status_t flush_event_bindings(listener_t *listener) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Flushed all erlang event bindings for node %s\n", listener->peer_nodename);

	return flush_bindings(listener->event_bindings, listener->event_rwlock);
}


/**
 * =============================================================================
 *
 *                             Collective Functions
 *
 * =============================================================================
 */
switch_status_t send_event_to_bindings(listener_t *listener, switch_event_t *event) {
    switch_hash_t *bindings;
    switch_hash_index_t *binding;
	char *uuid_str = switch_event_get_header(event, "unique-id");
	char *event_name = get_event_hash_key(event);

	if (!zstr(uuid_str)) {
		switch_thread_rwlock_rdlock(listener->session_rwlock);
		if ((bindings = switch_core_hash_find(listener->session_bindings, uuid_str))) {
			ei_x_buff ebuf;
			int start_index;

			ei_x_new_with_version(&ebuf);

			ei_x_encode_tuple_header(&ebuf, 2);
			ei_x_encode_atom(&ebuf, "call_event");
			ei_encode_switch_event(&ebuf, event);

			/* loop over all the entries of this sub-hash and send the event to each */
			start_index = ebuf.index;
			for (binding = switch_hash_first(NULL, bindings); binding; binding = switch_hash_next(binding)) {
				erlang_pid *pid;
				const void *key;
				void *value;
				
				switch_hash_this(binding, &key, NULL, &value);
				pid = (erlang_pid*) value;

				ebuf.index = start_index;
				ei_helper_send(listener, pid, &ebuf);
			}
			ei_x_free(&ebuf);
		}
		switch_thread_rwlock_unlock(listener->session_rwlock);
	}
	
    switch_thread_rwlock_rdlock(listener->event_rwlock);
	if ((bindings = switch_core_hash_find(listener->event_bindings, event_name))) {
		ei_x_buff ebuf;
		int start_index;

		ei_x_new_with_version(&ebuf);

		ei_encode_switch_event(&ebuf, event);

		/* loop over all the entries of this sub-hash and send the event to each */
		start_index = ebuf.index;
		for (binding = switch_hash_first(NULL, bindings); binding; binding = switch_hash_next(binding)) {
			erlang_pid *pid;
			const void *key;
			void *value;
			
			switch_hash_this(binding, &key, NULL, &value);
			pid = (erlang_pid*) value;
			
			ebuf.index = start_index;
			ei_helper_send(listener, pid, &ebuf);
		}
		ei_x_free(&ebuf);
	}
    switch_thread_rwlock_unlock(listener->event_rwlock);

    return SWITCH_STATUS_SUCCESS;
}

switch_status_t flush_all_bindings(listener_t *listener) {
    flush_session_bindings(listener);
    flush_event_bindings(listener);
    flush_fetch_bindings(listener);
    
    return SWITCH_STATUS_SUCCESS;
}

switch_status_t remove_pid_from_all_bindings(listener_t *listener, erlang_pid *from) {
    remove_pid_from_session_bindings(listener, from);
    remove_pid_from_event_bindings(listener, from);
    remove_pid_from_fetch_bindings(listener, from);

    return SWITCH_STATUS_SUCCESS;
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
