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
 * Karl Anderson <karl@2600hz.com>
 *
 * Original from mod_erlang_event.
 * ei_helpers.c -- helper functions for ei
 *
 */
#include <switch.h>
#include <ei.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/nameser.h>
#include <resolv.h>
#include "mod_kazoo.h"

/* Stolen from code added to ei in R12B-5.
 * Since not everyone has this version yet;
 * provide our own version.
 * */

#define put8(s,n) do { \
	(s)[0] = (char)((n) & 0xff); \
	(s) += 1; \
} while (0)

#define put32be(s,n) do {  \
	(s)[0] = ((n) >>  24) & 0xff; \
	(s)[1] = ((n) >>  16) & 0xff; \
	(s)[2] = ((n) >>  8) & 0xff;  \
	(s)[3] = (n) & 0xff; \
	(s) += 4; \
} while (0)

void ei_link(listener_t *listener, erlang_pid * from, erlang_pid * to) {
    char msgbuf[2048];
    char *s;
    int index = 0;

    index = 5; /* max sizes: */
    ei_encode_version(msgbuf, &index); /*   1 */
    ei_encode_tuple_header(msgbuf, &index, 3);
    ei_encode_long(msgbuf, &index, ERL_LINK);
    ei_encode_pid(msgbuf, &index, from); /* 268 */
    ei_encode_pid(msgbuf, &index, to); /* 268 */

    /* 5 byte header missing */
    s = msgbuf;
    put32be(s, index - 4); /*   4 */
    put8(s, ERL_PASS_THROUGH); /*   1 */
    /* sum:  542 */

    switch_thread_rwlock_rdlock(listener->rwlock);
    if (write(listener->clientfd, msgbuf, index) == -1) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Failed to link to process on %s\n", listener->peer_nodename);
    }
    switch_thread_rwlock_unlock(listener->rwlock);
}

void ei_encode_switch_event_headers(ei_x_buff * ebuf, switch_event_t *event) {
    int i;
    char *uuid = switch_event_get_header(event, "unique-id");

    switch_event_header_t *hp;

    for (i = 0, hp = event->headers; hp; hp = hp->next, i++);

    if (event->body)
        i++;

    ei_x_encode_list_header(ebuf, i + 1);

    if (uuid) {
        _ei_x_encode_string(ebuf, switch_event_get_header(event, "unique-id"));
    } else {
        ei_x_encode_atom(ebuf, "undefined");
    }

    for (hp = event->headers; hp; hp = hp->next) {
        ei_x_encode_tuple_header(ebuf, 2);
        _ei_x_encode_string(ebuf, hp->name);
        switch_url_decode(hp->value);
        _ei_x_encode_string(ebuf, hp->value);
    }

    if (event->body) {
        ei_x_encode_tuple_header(ebuf, 2);
        _ei_x_encode_string(ebuf, "body");
        _ei_x_encode_string(ebuf, event->body);
    }

    ei_x_encode_empty_list(ebuf);
}

void ei_encode_switch_event_tag(ei_x_buff * ebuf, switch_event_t *event, char *tag) {
    ei_x_encode_tuple_header(ebuf, 2);
    ei_x_encode_atom(ebuf, tag);
    ei_encode_switch_event_headers(ebuf, event);
}

/* function to make rpc call to remote node to retrieve a pid -
   calls module:function(Ref). The response comes back as
   {rex, {Ref, Pid}}
 */
int ei_pid_from_rpc(struct ei_cnode_s *ec, int sockfd, erlang_ref * ref, char *module, char *function) {
    ei_x_buff buf;
    ei_x_new(&buf);
    ei_x_encode_list_header(&buf, 1);
    ei_x_encode_ref(&buf, ref);
    ei_x_encode_empty_list(&buf);

    ei_rpc_to(ec, sockfd, module, function, buf.buff, buf.index);
    ei_x_free(&buf);

    return 0;
}

void ei_x_print_reg_msg(ei_x_buff * buf, char *dest, int send) {
    char *mbuf = NULL;
    int i = 1;

    ei_s_print_term(&mbuf, buf->buff, &i);

    if (send) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Encoded term %s to '%s'\n", mbuf, dest);
    } else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Decoded term %s for '%s'\n", mbuf, dest);
    }

    free(mbuf);
}

void ei_x_print_msg(ei_x_buff * buf, erlang_pid * pid, int send) {
    char *pbuf = NULL;
    int i = 0;
    ei_x_buff pidbuf;

    ei_x_new(&pidbuf);
    ei_x_encode_pid(&pidbuf, pid);

    ei_s_print_term(&pbuf, pidbuf.buff, &i);

    ei_x_print_reg_msg(buf, pbuf, send);
    free(pbuf);
}

int ei_helper_send(listener_t *listener, erlang_pid *to, ei_x_buff *buf) {
    int ret = 0;

    /* TODO: this isn't good enough, the fs_to_erl_loop might also try to write to the socket */
    /*		because this can be called to send a response via erl_to_fs_loop handle_msg */
    /*		We either need to lock or put the request into a fs_to_erl_loop queue */
    switch_thread_rwlock_rdlock(listener->rwlock);
    if (listener->clientfd) {
#ifdef EI_DEBUG
		ei_x_print_msg(buf, to, 1);
#endif
        ret = ei_send(listener->clientfd, to, buf->buff, buf->index);
    }
    switch_thread_rwlock_unlock(listener->rwlock);

    return ret;
}

/* convert an erlang reference to some kind of hashed string so we can store it as a hash key */
void ei_hash_ref(erlang_ref * ref, char *output) {
    /* very lazy */
    sprintf(output, "%d.%d.%d@%s", ref->n[0], ref->n[1], ref->n[2], ref->node);
}

int ei_compare_pids(erlang_pid * pid1, erlang_pid * pid2) {
    if ((!strcmp(pid1->node, pid2->node)) && pid1->creation == pid2->creation && pid1->num == pid2->num && pid1->serial == pid2->serial) {
        return 0;
    } else {
        return 1;
    }
}

int ei_decode_atom_safe(char *buf, int *index, char *dst) {
    int type, size;

    ei_get_type(buf, index, &type, &size);
	
	if (type != ERL_ATOM_EXT) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unexpected erlang term type %d (size %d), needed atom\n", type, size);
        return -1;
	} else if (size > MAXATOMLEN) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Requested decoding of atom with size %d into a buffer of size %d\n", size, MAXATOMLEN);
        return -1;
	} else {
		return ei_decode_atom(buf, index, dst);
	}
}

int ei_decode_string_or_binary(char *buf, int *index, char **dst) {
    int type, size, res;
    long len;

    ei_get_type(buf, index, &type, &size);

    if (type != ERL_STRING_EXT && type != ERL_BINARY_EXT && type != ERL_NIL_EXT) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unexpected erlang term type %d (size %d), needed binary or string\n", type, size);
        return -1;
    }

	*dst = malloc(size + 1);

	if (type == ERL_NIL_EXT) {
		res = 0;
		**dst = '\0';
	} else if (type == ERL_BINARY_EXT) {
        res = ei_decode_binary(buf, index, *dst, &len);
        (*dst)[len] = '\0'; /* binaries aren't null terminated */
    } else {
        res = ei_decode_string(buf, index, *dst);
    }

    return res;
}

int ei_decode_string_or_binary_limited(char *buf, int *index, int maxsize, char *dst) {
    int type, size, res;
    long len;

    ei_get_type(buf, index, &type, &size);

    if (type != ERL_STRING_EXT && type != ERL_BINARY_EXT && type != ERL_NIL_EXT) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unexpected erlang term type %d (size %d), needed binary or string\n", type, size);
        return -1;
    }

	if (size > maxsize) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Requested decoding of %s with size %d into a buffer of size %d\n",
						  type == ERL_BINARY_EXT ? "binary" : "string", size, maxsize);
		return -1;
	}

	if (type == ERL_NIL_EXT) {
		res = 0;
		dst = '\0';
	} else if (type == ERL_BINARY_EXT) {
        res = ei_decode_binary(buf, index, dst, &len);
        dst[len] = '\0'; /* binaries aren't null terminated */
    } else {
        res = ei_decode_string(buf, index, dst);
    }

    return res;
}

switch_status_t initialize_ei(struct ei_cnode_s *ec, switch_sockaddr_t *sa, prefs_t *prefs) {
    struct hostent *nodehost;
    char thishostname[EI_MAXHOSTNAMELEN + 1] = "";
    char thisnodename[MAXNODELEN + 1];
    char thisalivename[EI_MAXALIVELEN + 1];
    //EI_MAX_COOKIE_SIZE+1
    char ipbuf[25];
    const char *ip_addr;
    char *atsign;
		
    /* copy the erlang interface nodename into something we can modify */
    strncpy(thisalivename, prefs->ei_nodename, EI_MAXALIVELEN);

    ip_addr = switch_get_addr(ipbuf, sizeof (ipbuf), sa);

    if ((atsign = strchr(thisalivename, '@'))) {
        /* we got a qualified node name, don't guess the host/domain */
        snprintf(thisnodename, MAXNODELEN + 1, "%s", prefs->ei_nodename);
        /* truncate the alivename at the @ */
        *atsign = '\0';
    } else {
        if ((nodehost = gethostbyaddr(ip_addr, sizeof (ip_addr), AF_INET))) {
            memcpy(thishostname, nodehost->h_name, EI_MAXHOSTNAMELEN);
        }

        if (zstr_buf(thishostname) || !strncasecmp(prefs->ip, "0.0.0.0", 7)) {
            gethostname(thishostname, EI_MAXHOSTNAMELEN);
        }

        if (prefs->ei_shortname) {
            char *off;
            if ((off = strchr(thishostname, '.'))) {
                *off = '\0';
            }
        } else {
            if (!(_res.options & RES_INIT)) {
                // init the resolver
                res_init();
            }
            if (_res.dnsrch[0] && !zstr_buf(_res.dnsrch[0])) {
                strncat(thishostname, ".", 1);
                strncat(thishostname, _res.dnsrch[0], EI_MAXHOSTNAMELEN - strlen(thishostname));
            }

        }
        snprintf(thisnodename, MAXNODELEN + 1, "%s@%s", prefs->ei_nodename, thishostname);
    }

    /* init the ec stuff */
    if (ei_connect_xinit(ec, thishostname, thisalivename, thisnodename, (Erl_IpAddr) ip_addr, prefs->ei_cookie, 0) < 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to initialize the erlang interface connection structure\n");
        return SWITCH_STATUS_FALSE;
    }

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
