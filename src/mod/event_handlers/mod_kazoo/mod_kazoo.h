#define MAX_ACL 100
#define CMD_BUFLEN 1024 * 1000
#define MAX_QUEUE_LEN 25000
#define MAX_MISSED 500
#define MAX_PID_CHARS 255

typedef enum {
	LFLAG_READY = (1 << 0),
	LFLAG_RUNNING = (1 << 1),
	LFLAG_EVENTS = (1 << 2),
	LFLAG_LOG = (1 << 3),
	LFLAG_FULL = (1 << 4),
	LFLAG_MYEVENTS = (1 << 5),
	LFLAG_SESSION = (1 << 6),
	LFLAG_ASYNC = (1 << 7),
	LFLAG_STATEFUL = (1 << 8),
	LFLAG_OUTBOUND = (1 << 9),
	LFLAG_LINGER = (1 << 10),
	LFLAG_HANDLE_DISCO = (1 << 11),
	LFLAG_CONNECTED = (1 << 12),
	LFLAG_RESUME = (1 << 13),
	LFLAG_AUTH_EVENTS = (1 << 14),
	LFLAG_ALL_EVENTS_AUTHED = (1 << 15),
	LFLAG_ALLOW_LOG = (1 << 16)
} event_flag_t;

typedef enum {
	ERLANG_STRING = 0,
	ERLANG_BINARY
} erlang_encoding_t;

struct listener_s {
	int clientfd;
	switch_memory_pool_t *pool;

	uint32_t flags;
	switch_mutex_t *flag_mutex;

	switch_queue_t *fetch_queue;
	switch_queue_t *event_queue;

	switch_hash_t *event_bindings;
	switch_hash_t *session_bindings;
	switch_hash_t *fetch_bindings;

	switch_thread_rwlock_t *event_rwlock;
	switch_thread_rwlock_t *session_rwlock;
	switch_thread_rwlock_t *fetch_rwlock;
	switch_thread_rwlock_t *rwlock;

	char remote_ip[50];
	char *peer_nodename;
	struct ei_cnode_s *ec;
	struct listener_s *next;
};
typedef struct listener_s listener_t;

struct xml_fetch_msg_s {
	switch_xml_t xml;
	switch_mutex_t *mutex;
    switch_thread_cond_t *response_available;
	char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1];
	const char *section;
	const char *tag_name;
	const char *key_name;
	const char *key_value;
	switch_event_t *params;	
};
typedef struct xml_fetch_msg_s xml_fetch_msg_t;

struct prefs_s {
	switch_mutex_t *mutex;
	char *ip;
	uint16_t port;
	char *acl[MAX_ACL];
	uint32_t acl_count;
	uint32_t id;
	int nat_map;
	char *ei_cookie;
	char *ei_nodename;
	switch_bool_t ei_shortname;
	int ei_compat_rel;
	erlang_encoding_t encoding;

	switch_atomic_t threads;
} prefs;
typedef struct prefs_s prefs_t;

struct globals_s {
	switch_hash_t *fetch_resp_hash;
	switch_thread_rwlock_t *fetch_resp_lock;
	switch_memory_pool_t *pool;
	switch_event_node_t *event_binding;
	switch_xml_binding_t *config_fetch_binding;
	switch_xml_binding_t *directory_fetch_binding;
	switch_xml_binding_t *dialplan_fetch_binding;
	switch_xml_binding_t *chatplan_fetch_binding;
	uint32_t flags;
	uint32_t debug;
} globals;
typedef struct globals_s globals_t;

/* kazoo_binding.c */
switch_status_t flush_all_bindings(listener_t *listener);
switch_status_t remove_pid_from_all_bindings(listener_t *listener, erlang_pid *from);

switch_status_t display_fetch_bindings(listener_t *listener, switch_stream_handle_t *stream);
switch_status_t has_fetch_bindings(listener_t *listener, const char *section);
switch_status_t send_fetch_to_bindings(listener_t *listener, char *uuid_str);
switch_status_t add_fetch_binding(listener_t *listener, char *section, erlang_pid *from);
switch_status_t remove_pid_from_fetch_binding(listener_t *listener, char *section, erlang_pid *from);
switch_status_t remove_pid_from_fetch_bindings(listener_t *listener, erlang_pid *from);
switch_status_t flush_fetch_bindings(listener_t *listener);

int count_session_bindings(listener_t *listener);
switch_status_t display_session_bindings(listener_t *listener, switch_stream_handle_t *stream);
switch_status_t has_session_bindings(listener_t *listener, char *uuid_str);
switch_status_t send_session_to_bindings(listener_t *listener, char *uuid_str);
switch_status_t add_session_binding(listener_t *listener, char *uuid_str, erlang_pid *from);
switch_status_t remove_pid_from_session_binding(listener_t *listener, char *uuid_str, erlang_pid *from);
switch_status_t remove_pid_from_session_bindings(listener_t *listener, erlang_pid *from);
switch_status_t flush_session_bindings(listener_t *listener);

switch_status_t display_event_bindings(listener_t *listener, switch_stream_handle_t *stream);
switch_status_t has_event_bindings(listener_t *listener, switch_event_t *event);
switch_status_t send_event_to_bindings(listener_t *listener, switch_event_t *event);
switch_status_t add_event_binding(listener_t *listener, char *event_name, erlang_pid *from);
switch_status_t add_custom_event_binding(listener_t *listener, char *subclass_name, erlang_pid *from);
switch_status_t remove_pid_from_event_binding(listener_t *listener, char *event_name, erlang_pid *from);
switch_status_t remove_pid_from_custom_event_binding(listener_t *listener, char *subclass_name, erlang_pid *from);
switch_status_t remove_pid_from_event_bindings(listener_t *listener, erlang_pid *from);
switch_status_t list_event_bindings(listener_t *listener);

/* kazoo_request.c */
switch_status_t handle_request(listener_t *listener, erlang_msg * msg, ei_x_buff * buf, ei_x_buff * rbuf);

/* ei_helpers.c */
void ei_link(listener_t *listener, erlang_pid * from, erlang_pid * to);
void ei_encode_switch_event_headers(ei_x_buff * ebuf, switch_event_t *event);
void ei_encode_switch_event_tag(ei_x_buff * ebuf, switch_event_t *event, char *tag);
int ei_pid_from_rpc(struct ei_cnode_s *ec, int sockfd, erlang_ref * ref, char *module, char *function);
void ei_x_print_reg_msg(ei_x_buff * buf, char *dest, int send);
void ei_x_print_msg(ei_x_buff * buf, erlang_pid * pid, int send);
int ei_helper_send(listener_t *listener, erlang_pid* to, ei_x_buff *buf);
void ei_hash_ref(erlang_ref * ref, char *output);
int ei_compare_pids(erlang_pid * pid1, erlang_pid * pid2);
int ei_decode_atom_safe(char *buf, int *index, char *dst);
int ei_decode_string_or_binary_limited(char *buf, int *index, int maxsize, char *dst);
int ei_decode_string_or_binary(char *buf, int *index, char **dst);
switch_status_t initialize_ei(struct ei_cnode_s *ec, switch_sockaddr_t *sa, prefs_t *prefs);

#define ei_encode_switch_event(_b, _e) ei_encode_switch_event_tag(_b, _e, "event");
#define _ei_x_encode_string(buf, string) { ei_x_encode_binary(buf, string, strlen(string)); }

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
