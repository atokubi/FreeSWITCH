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
    switch_mutex_t *listener_mutex;
    switch_event_node_t *node;
    int debug;
} globals;

static struct {
    switch_socket_t *sock;
    switch_mutex_t *sock_mutex;
    listener_t *listeners;
    uint8_t ready;
} listen_list;

SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_pref_ip, prefs.ip);
SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_pref_ei_cookie, prefs.ei_cookie);
SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_pref_ei_nodename, prefs.ei_nodename);

/* Function Definitions */
static void *SWITCH_THREAD_FUNC erl_to_fs_loop(switch_thread_t *thread, void *obj);
static void *SWITCH_THREAD_FUNC fs_to_erl_loop(switch_thread_t *thread, void *obj);
static void launch_erl_to_fs_thread(listener_t *listener);
static void launch_fs_to_erl_thread(listener_t *listener);
//static void remove_listener(listener_t *listener);
static void stop_listener(listener_t *listener);
static void stop_all_listeners(void);

static switch_status_t log_handler(const switch_log_node_t *node, switch_log_level_t level) {
    listener_t *l;

    switch_mutex_lock(globals.listener_mutex);
    for (l = listen_list.listeners; l; l = l->next) {
        if (switch_test_flag(l, LFLAG_LOG) && l->level >= node->level) {
            switch_log_node_t *dnode = switch_log_node_dup(node);

            if (switch_queue_trypush(l->log_queue, dnode) == SWITCH_STATUS_SUCCESS) {
                if (l->lost_logs) {
                    int ll = l->lost_logs;
                    switch_event_t *event;
                    l->lost_logs = 0;
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Lost %d log lines!\n", ll);
                    if (switch_event_create(&event, SWITCH_EVENT_TRAP) == SWITCH_STATUS_SUCCESS) {
                        switch_event_add_header(event, SWITCH_STACK_BOTTOM, "info", "lost %d log lines", ll);
                        switch_event_fire(&event);
                    }
                }
            } else {
                switch_log_node_free(&dnode);
                if (++l->lost_logs > MAX_MISSED) {
                    stop_listener(l);
                }
            }
        }
    }
    switch_mutex_unlock(globals.listener_mutex);

    return SWITCH_STATUS_SUCCESS;
}

static void event_handler(switch_event_t *event) {
    listener_t *listener;

    switch_assert(event != NULL);

    if (!listen_list.ready) {
        return;
    }

    switch_mutex_lock(globals.listener_mutex);
    /* nobody else seems to loop over the listeners like this, why is that? */
    for (listener = listen_list.listeners; listener; listener = listener->next) {
        switch_event_t *clone = NULL;
        int send = 0;

        /* if this listener has erlang process that are bound to this event type then */
        /* set the flag to duplicate the event into the listener event queue */
        if (has_event_bindings(listener, event) == SWITCH_STATUS_FOUND) {
            send = 1;
        }
        /* TODO: check if the UUID of the event is in listener->session_bindings */

        if (send) {
            /* TODO: if we tracked the number of listeners we sent this too and */
            /*     added a mutex for decrementing the last listener could destroy */
            /*     it and we wouldn't have to clone (alloc)... just a thought */
            if (switch_event_dup(&clone, event) == SWITCH_STATUS_SUCCESS) {
                if (switch_queue_trypush(listener->event_queue, clone) != SWITCH_STATUS_SUCCESS) {
                    /* if we couldn't place the cloned event into the listeners */
                    /* event queue make sure we destroy it, real good like */
                    switch_event_destroy(&clone);
                }
            } else {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to duplicate event to erlang listener: Memory error!\n");
            }
        }
    }
    switch_mutex_unlock(globals.listener_mutex);
}

/*
static switch_status_t push_into_listener_queue(xml_msg_list_t *xml_msgs, xml_fetch_msg_t *xml_msg) {
    return SWITCH_TRUE;
}
*/

/* TODO: this needs to look into listener.fetch_bindings inorder to determine if it can/needs to make the request to that listener 
static switch_xml_t xml_erlang_fetch(const char *section, const char *tag_name, const char *key_name, const char *key_value, switch_event_t *params,
        void *user_data) {
    switch_xml_t xml = NULL;
    xml_fetch_msg_t *xml_msg;
    switch_uuid_t uuid;
    listener_t *l = NULL;

    l = listen_list.listeners;
    if (!l) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Received a request for XML, ignoring since there are no erlang nodes connected.\n");
        return xml;
    }

    // Try each erlang listener
    while (l) {
        xml_msg = switch_core_alloc(l->pool, sizeof (xml_msg));
        memset(&xml_msg, 0, sizeof (xml_msg));

        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Received a request for XML - %s / %s / %s\n", section, tag_name, key_name);

        // Create a unique identifier for this request
        switch_uuid_get(&uuid);
        switch_uuid_format(xml_msg->uuid_str, &uuid);

        // Create a request in our queue for an XML binding. No need to copy memory pointers here, we block until they're used elsewhere and returned
        xml_msg->responded = SWITCH_FALSE;
        xml_msg->section = section;
        xml_msg->tag_name = tag_name;
        xml_msg->key_name = key_name;
        xml_msg->key_value = key_value;

        push_into_listener_queue(l->xml_msgs, xml_msg);

        // Wait for a response or a timeout
        while (xml_msg->responded != SWITCH_TRUE) {
            // FIXME: need to actually go get the xml here hehe
            xml_msg->responded = SWITCH_TRUE;
        }

        l = l->next;
    }

    switch_safe_free(xml_msg);

    return xml;
}
*/

static void close_socket(switch_socket_t ** sock) {
    switch_mutex_lock(listen_list.sock_mutex);
    if (*sock) {
        switch_socket_shutdown(*sock, SWITCH_SHUTDOWN_READWRITE);
        switch_socket_close(*sock);
        *sock = NULL;
    }
    switch_mutex_unlock(listen_list.sock_mutex);
}

static void close_socketfd(int *sockfd) {
    if (*sockfd) {
        shutdown(*sockfd, SHUT_RDWR);
        close(*sockfd);
    }
}

static void add_listener(listener_t *listener) {
    /* add me to the listeners so I get events */
    switch_mutex_lock(globals.listener_mutex);
    listener->next = listen_list.listeners;
    listen_list.listeners = listener;
    switch_mutex_unlock(globals.listener_mutex);
}

static void flush_listener(listener_t *listener, switch_bool_t flush_log, switch_bool_t flush_events) {
    void *pop;

    if (listener->log_queue) {
        while (switch_queue_trypop(listener->log_queue, &pop) == SWITCH_STATUS_SUCCESS) {
            switch_log_node_t *dnode = (switch_log_node_t *) pop;
            if (dnode) {
                switch_log_node_free(&dnode);
            }
        }
    }

    if (listener->event_queue) {
        while (switch_queue_trypop(listener->event_queue, &pop) == SWITCH_STATUS_SUCCESS) {
            switch_event_t *pevent = (switch_event_t *) pop;
            if (!pop)
                continue;
            switch_event_destroy(&pevent);
        }
    }
}

static void destroy_listener(listener_t *listener) {
    listener_t *l, *last = NULL;

    switch_mutex_lock(globals.listener_mutex);
    for (l = listen_list.listeners; l; l = l->next) {
        if (l == listener) {
            if (last) {
                last->next = l->next;
            } else {
                listen_list.listeners = l->next;
            }
        }
        last = l;
    }
    switch_mutex_unlock(globals.listener_mutex);
    
    /* ensure nothing else is still using this listener */
    switch_thread_rwlock_wrlock(listener->rwlock);
    switch_thread_rwlock_unlock(listener->rwlock);

    /* Now that we are out of the listener_list we can flush our queues */
    /* since nobody will know about our existence and be unable to add to them */
    flush_listener(listener, SWITCH_TRUE, SWITCH_TRUE);

    /* flush all bindings */
    flush_all_bindings(listener);
    
    /* close the client socket */
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Closing connection to erlang node %s\n", listener->peer_nodename);
    close_socketfd(&listener->clientfd);
        
    /* TODO: These are in the listener pool, do we need to destroy them? */
    switch_core_hash_destroy(&listener->event_hash);
    switch_core_hash_destroy(&listener->event_bindings);
    switch_core_hash_destroy(&listener->session_bindings);
    switch_core_hash_destroy(&listener->log_bindings);
    switch_core_hash_destroy(&listener->fetch_bindings);
    
    /* TODO: what about the locks? */

    /* goodbye and thanks for all the fish! */
    switch_core_destroy_memory_pool(&listener->pool);    
}

static void stop_listener(listener_t *listener) {
    /* at the moment, clear the running flag and hope for the best */
    switch_clear_flag(listener, LFLAG_RUNNING);
}

static void stop_all_listeners(void) {
    listener_t *listener;

    switch_mutex_lock(globals.listener_mutex);
    for (listener = listen_list.listeners; listener; listener = listener->next) {
        stop_listener(listener);
    }
    switch_mutex_unlock(globals.listener_mutex);
}

/*
   static void *SWITCH_THREAD_FUNC api_exec(switch_thread_t *thread, void *obj)
   {
   }
 */

static void *SWITCH_THREAD_FUNC fs_to_erl_loop(switch_thread_t *thread, void *obj) {
    listener_t *listener = (listener_t *) obj;

    switch_assert(listener != NULL);

    /* add ourselfs to the modules thread count */
    switch_mutex_lock(globals.listener_mutex);
    prefs.threads++;
    switch_mutex_unlock(globals.listener_mutex);

    /* This thread is responsible for adding/removing from the listener_list */
    /* as erl_to_fs_loop does not need to be in the list (since its used to */
    /* duplicate events destined for erlang) */
    add_listener(listener);

    /* grab a read lock on the listener so nobody can remove it until we exit... */
    switch_thread_rwlock_rdlock(listener->rwlock);
    while (switch_test_flag(listener, LFLAG_RUNNING)) {
        void *pop;

        if (switch_queue_trypop(listener->event_queue, &pop) == SWITCH_STATUS_SUCCESS) {
            switch_event_t *event = (switch_event_t *) pop;
            /* if there was an event waiting in our queue send it to */
            /* any erlang processes bound its type */
            send_event_to_bindings(listener, event);
            switch_event_destroy(&event);
        }
        
        /* TODO: try to pop the logs queue and send to erlang */
        /* TODO: try to pop the fetch queue and send to erlang */
        /*      Before sending a fetch request, store the UUID */
        /*      and pointer somewhere so we can map the reply */
        switch_yield(1000);
    }

    /* flag this listener as stopped */
    stop_listener(listener);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Shutting down freeswitch event sender for erlang node %s (%s)\n", listener->peer_nodename, listener->remote_ip);

    /* remove the read lock that we have been holding on to while running */
    switch_thread_rwlock_unlock(listener->rwlock);

    /* This thread is responsible for cleaning up the listener */
    destroy_listener(listener);

    /* remove ourself from this modules thread count */
    switch_mutex_lock(globals.listener_mutex);
    prefs.threads--;
    switch_mutex_unlock(globals.listener_mutex);

    return NULL;
}

static void *SWITCH_THREAD_FUNC erl_to_fs_loop(switch_thread_t *thread, void *obj) {
    listener_t *listener = (listener_t *) obj;
    int status = 1;

    switch_assert(listener != NULL);

    /* add ourselfs to the modules thread count */
    switch_mutex_lock(globals.listener_mutex);
    prefs.threads++;
    switch_mutex_unlock(globals.listener_mutex);

    /* grab a read lock on the listener so nobody can remove it until we exit... */
    switch_thread_rwlock_rdlock(listener->rwlock);
    while (switch_test_flag(listener, LFLAG_RUNNING) && status >= 0) {
        erlang_msg msg;
        ei_x_buff buf;
        ei_x_buff rbuf;

        /* create a new buf for the erlang message and a rbuf for the reply */
        ei_x_new(&buf);
        ei_x_new_with_version(&rbuf);

        /* wait for a erlang message, or timeout after 100ms to check if the module is still running */
        status = ei_xreceive_msg_tmo(listener->clientfd, &msg, &buf, 100);

        switch (status) {
            case ERL_TICK:
                /* erlang nodes send ticks to eachother to validate they are still reachable, we dont have to do anything here */
                break;
            case ERL_MSG:
                switch (msg.msgtype) {
                    case ERL_SEND:
                        /* we received an erlang message sent to a pid, process it! */
                        if (1) { //globals.debug
                            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Received erlang send from %s <%d.%d.%d>\n", msg.from.node, msg.from.creation, msg.from.num, msg.from.serial);
                            ei_x_print_msg(&buf, &msg.from, 0);
                        }

                        if (handle_msg(listener, &msg, &buf, &rbuf) != SWITCH_STATUS_SUCCESS) {
                            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Erlang handle_msg requested event receiver shutdown\n");
                            status = -1;
                        }
                        break;
                    case ERL_REG_SEND:
                        /* we received an erlang message sent to a registered process name, process it! */
                        if (1) { //globals.debug
                            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Received erlang message to registered process '%s' from %s <%d.%d.%d>\n", msg.toname, msg.from.node, msg.from.creation, msg.from.num, msg.from.serial);
                            ei_x_print_reg_msg(&buf, msg.toname, 0);
                        }

                        if (handle_msg(listener, &msg, &buf, &rbuf) != SWITCH_STATUS_SUCCESS) {
                            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Erlang handle_msg requested event receiver shutdown\n");
                            status = -1;
                        }
                        break;
                    case ERL_LINK:
                        /* we received an erlang link request?  Should we be linking or are they linking to us and this just informs us? */
                        if (1) { //globals.debug
                            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Received erlang link request from %s <%d.%d.%d>\n", msg.from.node, msg.from.creation, msg.from.num, msg.from.serial);
                        }
                        break;
                    case ERL_UNLINK:
                        /* we received an erlang unlink request?  Same question as the ERL_LINK, are we expected to do something? */
                        if (1) { //globals.debug
                            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Received erlang unlink request from %s <%d.%d.%d>\n", msg.from.node, msg.from.creation, msg.from.num, msg.from.serial);
                        }
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
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Erlang communication fault with node %s (%s): erl_errno=%d errno=%d\n", listener->peer_nodename, listener->remote_ip, erl_errno, errno);
                        break;
                }
                break;
            default:
                /* HUH? didnt plan for this, whatevs shutdown the listener if status set by ei_xreceive_msg_tmo is less than or equal to 0 */
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unexpected erlang receive status for node %s (%s): %d\n", listener->peer_nodename, listener->remote_ip, status);
                break;
        }

        /* TODO: Shouldnt we be free'n msg? */
        ei_x_free(&buf);
        ei_x_free(&rbuf);
    }

    /* flag this listener as stopped */
    stop_listener(listener);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Shutting down erlang event receiver for node %s (%s)\n", listener->peer_nodename, listener->remote_ip);

    /* remove the read lock that we have been holding on to while running */
    switch_thread_rwlock_unlock(listener->rwlock);

    /* remove ourself from this modules thread count */
    switch_mutex_lock(globals.listener_mutex);
    prefs.threads--;
    switch_mutex_unlock(globals.listener_mutex);

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

/* Create a thread to send freeswitch events, logs, and fetch requests to an erlang node */
static void launch_fs_to_erl_thread(listener_t *listener) {
    switch_thread_t *thread;
    switch_threadattr_t *thd_attr = NULL;

    switch_threadattr_create(&thd_attr, listener->pool);
    switch_threadattr_detach_set(thd_attr, 1);
    switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
    switch_thread_create(&thread, thd_attr, fs_to_erl_loop, listener, listener->pool);
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

        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Read %d bytes from cookie file %s.\n", (int) res, filename);

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
                    set_pref_ip(val);
                } else if (!strcmp(var, "listen-port")) {
                    prefs.port = (uint16_t) atoi(val);
                } else if (!strcmp(var, "cookie")) {
                    set_pref_ei_cookie(val);
                } else if (!strcmp(var, "cookie-file")) {
                    if (read_cookie_from_file(val) == 1) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to read cookie from %s\n", val);
                    }
                } else if (!strcmp(var, "nodename")) {
                    set_pref_ei_nodename(val);
                } else if (!strcmp(var, "shortname")) {
                    prefs.ei_shortname = switch_true(val);
                } else if (!strcmp(var, "bind-to-logger")) {
                    prefs.bind_to_logger = switch_true(val);
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

SWITCH_MODULE_LOAD_FUNCTION(mod_kazoo_load) {
    // FIX ME:        switch_api_interface_t *api_interface;

    memset(&globals, 0, sizeof(globals));

    /* initialize the listener mutex */
    switch_mutex_init(&globals.listener_mutex, SWITCH_MUTEX_NESTED, pool);

    /* initialize listen_list */
    memset(&listen_list, 0, sizeof(listen_list));
    switch_mutex_init(&listen_list.sock_mutex, SWITCH_MUTEX_NESTED, pool);

    /* bind to all switch events */
    if (switch_event_bind_removable(modname, SWITCH_EVENT_ALL, SWITCH_EVENT_SUBCLASS_ANY, event_handler, NULL, &globals.node) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't bind!\n");
        return SWITCH_STATUS_GENERR;
    }

    /* bind to all logs */
    if (prefs.bind_to_logger) {
        switch_log_bind_logger(log_handler, SWITCH_LOG_DEBUG, SWITCH_FALSE);
    }

    /* bind to all XML requests */
    //        switch_xml_bind_search_function(xml_erlang_fetch, switch_xml_parse_section_string("directory"), NULL);
    //        switch_xml_bind_search_function(xml_erlang_fetch, switch_xml_parse_section_string("dialplan"), NULL);

    /* connect my internal structure to the blank pointer passed to me */
    *module_interface = switch_loadable_module_create_module_interface(pool, modname);

    /* create an api for cli debug commands */
    // FIX ME:        SWITCH_ADD_API(api_interface, "kazoo", "kazoo information", api_exec, "<command> [<args>]");
    // FIX ME:        SWITCH_ADD_API(api_interface, "kazoo_bind_logs", "kazoo information", api_exec, "<command> [<args>]");
    // FIX ME:        switch_console_set_complete("add kazoo listeners");

    /* indicate that the module should continue to be loaded */
    return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_kazoo_shutdown) {
    int sanity = 0;

    /* TODO: this is stock and likely erroneous/dangerous.... */

    prefs.done = 1;

    switch_log_unbind_logger(log_handler);
    switch_event_unbind(&globals.node);
    //switch_xml_unbind_search_function_ptr(xml_erlang_fetch);
    
    stop_all_listeners();
    
    while (prefs.threads) {
        switch_yield(100000);
        stop_all_listeners();
        if (++sanity >= 200) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to kill erlang listeners, continuing. Good luck!\n");
            break;
        }
    }

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

    if (switch_core_new_memory_pool(&pool) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Out Of Memory: Oh My God! They killed Kenny! YOU BASTARDS!\n");
        return SWITCH_STATUS_TERM;
    }

    config();

    /* while the module is still running repeatedly try to open and listen to the provided ip:port until successful */
    while (!prefs.done) {
        status = switch_sockaddr_info_get(&sa, prefs.ip, SWITCH_UNSPEC, prefs.port, 0, pool);

        if (!status) {
            status = switch_socket_create(&listen_list.sock, switch_sockaddr_get_family(sa), SOCK_STREAM, SWITCH_PROTO_TCP, pool);
        }

        if (!status && listen_list.sock) {
            status = switch_socket_opt_set(listen_list.sock, SWITCH_SO_REUSEADDR, 1);
        }

        if (!status && listen_list.sock) {
            status = switch_socket_bind(listen_list.sock, sa);
        }

        if (!status && listen_list.sock) {
            status = switch_socket_listen(listen_list.sock, 5);
        }

        if (!status && listen_list.sock) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Erlang connection acceptor listening on %s:%u\n", prefs.ip, prefs.port);

            if (prefs.nat_map) {
                switch_nat_add_mapping(prefs.port, SWITCH_NAT_TCP, NULL, SWITCH_FALSE);
            }

            break;
        } else {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Erlang connection acceptor socket error, could not listen on %s:%u\n", prefs.ip, prefs.port);
            switch_yield(500000);
        }
    }

    /* if the config has specified an erlang release compatability then pass that along to the erlang interface */
    if (!prefs.done && prefs.ei_compat_rel) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Compatability with OTP R%d requested\n", prefs.ei_compat_rel);
        ei_set_compat_rel(prefs.ei_compat_rel);
    }

    /* try to initialize the erlang interface */
    if (!prefs.done && SWITCH_STATUS_SUCCESS != initialize_ei(&ec, sa, &prefs)) {
        prefs.done = 1;
    }

    /* tell the erlang port manager where we can be reached.  this returns a file descriptor pointing to epmd or -1 */
    if (!prefs.done && (epmdfd = ei_publish(&ec, prefs.port)) == -1) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                "Failed to start epmd, is it in the freeswith user $PATH? Try starting it yourself or run an erl shell with the -sname or -name option.  Shutting down.\n");
        prefs.done = 1;
    } else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Connected to epmd and published erlang cnode name %s at port %d\n", ec.thisnodename, prefs.port);
        
        /* we are listening on a socket, configured the erlang interface, and published our node name to ip:port mapping... we are ready! */
        listen_list.ready = 1;        
    }
    
    /* accept connections, negotiate cookies with the connecting node, then spawn two new threads for the node (one to send message to it and one to receive) */
    while (!prefs.done) {
        /* zero out errno because ei_accept doesn't differentiate between a */
        /* failed authentication or a socket failure, or a client version */
        /* mismatch or a godzilla attack (and a godzilla attack is highly likely) */
        errno = 0;

        /* TODO: move this into switch_apr as something like switch_os_sock_get(switch_os_sock_t *sockfd, switch_socket_t *sock); */
        apr_os_sock_get((apr_os_sock_t *) & sockfd, (apr_socket_t *) listen_list.sock);

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

        if (prefs.done) {
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

        /* create a mutex and some queues for the work we will be doing to process events */
        switch_thread_rwlock_create(&listener->rwlock, listener_pool);
        switch_queue_create(&listener->event_queue, MAX_QUEUE_LEN, listener_pool);
        switch_queue_create(&listener->log_queue, MAX_QUEUE_LEN, listener_pool);

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
        switch_mutex_init(&listener->flag_mutex, SWITCH_MUTEX_NESTED, listener->pool);

        /* create a bunch of hashes for tracking things like bindings and such */
        switch_core_hash_init(&listener->event_hash, listener->pool);
        switch_core_hash_init(&listener->event_bindings, listener->pool);
        switch_core_hash_init(&listener->session_bindings, listener->pool);
        switch_core_hash_init(&listener->log_bindings, listener->pool);
        switch_core_hash_init(&listener->fetch_bindings, listener->pool);

        /* store the IP and node name we are talking with */
        switch_inet_ntop(AF_INET, conn.ipadr, listener->remote_ip, sizeof (listener->remote_ip));
        listener->peer_nodename = switch_core_strdup(listener->pool, conn.nodename);

        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "New erlang connection from node %s (%s)\n", listener->peer_nodename, listener->remote_ip);

        /* Go do some real work - start the threads for this erlang node! */
        launch_erl_to_fs_thread(listener);
        launch_fs_to_erl_thread(listener);
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Erlang connection acceptor shutting down\n");
    
    /* TODO: stop listeners?  seems to make sense to do so... */
    
    if (epmdfd) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Closing epmd socket\n");
        close_socketfd(&epmdfd);
    }
    
    if (listen_list.sock) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Closing listening socket %s:%u\n", prefs.ip, prefs.port);
        close_socket(&listen_list.sock);
    }
    
    /* Close the port we reserved for uPnP/Switch behind firewall, if necessary */
    if (prefs.nat_map && switch_nat_get_type()) {
        switch_nat_del_mapping(prefs.port, SWITCH_NAT_TCP);
    }

    switch_safe_free(prefs.ip);
    switch_safe_free(prefs.ei_cookie);
    switch_safe_free(prefs.ei_nodename);
    
    /* Free our memory pool for handling sockets */
    if (pool) {
        switch_core_destroy_memory_pool(&pool);
    }

    for (uint32_t x = 0; x < prefs.acl_count; x++) {
        switch_safe_free(prefs.acl[x]);
    }

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