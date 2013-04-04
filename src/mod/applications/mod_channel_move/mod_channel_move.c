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
 * Darren Schreiber <d@d-man.org>
 *
 *
 * mod_channel_move.c -- Utilize eventing and channel recovery routines to allow for channel move abilities
 *
 */
#include <switch.h>
#include "mod_channel_move.h"

/* Prototypes */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_channel_move_shutdown);
SWITCH_MODULE_RUNTIME_FUNCTION(mod_channel_move_runtime);
SWITCH_MODULE_LOAD_FUNCTION(mod_channel_move_load);

/* SWITCH_MODULE_DEFINITION(name, load, shutdown, runtime) 
 */
SWITCH_MODULE_DEFINITION(mod_channel_move, mod_channel_move_load, mod_channel_move_shutdown, NULL);


static int silent_destroy(char *technology, char *channel_id)
{
	switch_core_session_t *session = switch_core_session_locate(channel_id);
	switch_xml_t cdr = NULL;
	char *xml_cdr_text = NULL;
	switch_channel_t *channel = NULL;

	if (!session) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Move Channel: The provided channel_id is invalid.\n");
		return SWITCH_FALSE;
	}

	/* Fire an event with all call XML data, then mark the channel as moving and kill it.
	   NOTE: The responsible endpoint must check for the "moving" flag and kill the channel silently. */
	channel = switch_core_session_get_channel(session);
	if (!channel) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Move Channel: This session has no channel?\n");
		switch_core_session_rwunlock(session);
		return SWITCH_FALSE;
	}

	if (!switch_channel_test_flag(channel, CF_ANSWERED) || switch_channel_get_state(channel) < CS_SOFT_EXECUTE) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Move Channel: This channel is not in an answered state. You can only move answered channels.\n");
		switch_core_session_rwunlock(session);
		return SWITCH_FALSE;
	}

	if (switch_channel_test_flag(channel, CF_RECOVERING) || !switch_channel_test_flag(channel, CF_TRACKABLE)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Move Channel: This channel is not tracked. Please make sure your endpoint has call tracking enabled.\n");
		switch_core_session_rwunlock(session);
		return SWITCH_FALSE;
	}

	switch_channel_set_variable(channel, "channel_is_moving", "true");
	switch_channel_set_variable(channel, "reset_local_network_on_recovery", "true");
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Move Channel: Generating XML metadata...\n");

	if (switch_ivr_generate_xml_cdr(session, &cdr) == SWITCH_STATUS_SUCCESS) {
		xml_cdr_text = switch_xml_toxml_nolock(cdr, SWITCH_FALSE);
		switch_xml_free(cdr);
	}

	if (xml_cdr_text) {
		switch_event_t *event = NULL;

		/* Tell the world about the channel, hoping that someone will pick it up */
		if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, MY_EVENT_MOVE_RELEASED) == SWITCH_STATUS_SUCCESS) {
//				 switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "profile_name", profile->name);
//				 switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "old_node_hostname", mod_sofia_globals.hostname);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "old_node_channel_uuid", switch_core_session_get_uuid(session));
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "metadata", xml_cdr_text);
			switch_event_fire(&event);
		}

		free(xml_cdr_text);
	}

	/* So long and thanks for all the phish */
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Move Channel: Quietly killing channel...\n");
	switch_channel_hangup(channel, SWITCH_CAUSE_REDIRECTION_TO_NEW_DESTINATION);
	switch_core_session_rwunlock(session);

	return SWITCH_TRUE;

}


static char *recover_callback(char *technology, char *xml_str)
{
	switch_xml_t xml;
	switch_endpoint_interface_t *ep;
	switch_core_session_t *session;
	char *r = NULL;

	if (!(xml = switch_xml_parse_str_dynamic(xml_str, SWITCH_TRUE))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "XML ERROR\n");
		return SWITCH_FALSE;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "%s\n", xml_str);

	if (!(ep = switch_loadable_module_get_endpoint_interface(technology))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Move Channel: EP ERROR\n");
		return SWITCH_FALSE;
	}

	if (!(session = switch_core_session_request_xml(ep, NULL, xml))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Move Channel: Invalid XML CDR data, call not recovered\n");
		goto end;
	}

	if (ep->recover_callback) {
		switch_caller_extension_t *extension = NULL;

		if (ep->recover_callback(session) > 0) {
			switch_channel_t *channel = switch_core_session_get_channel(session);

			if (switch_channel_get_partner_uuid(channel)) {
				switch_channel_set_flag(channel, CF_RECOVERING_BRIDGE);
			} else {
				switch_xml_t callflow, param, x_extension;
				if ((extension = switch_caller_extension_new(session, "recovery", "recovery")) == 0) {
					abort();
				}

				if ((callflow = switch_xml_child(xml, "callflow")) && (x_extension = switch_xml_child(callflow, "extension"))) {
					for (param = switch_xml_child(x_extension, "application"); param; param = param->next) {
						const char *var = switch_xml_attr_soft(param, "app_name");
						const char *val = switch_xml_attr_soft(param, "app_data");
						/* skip announcement type apps */
						if (strcasecmp(var, "speak") && strcasecmp(var, "playback") && strcasecmp(var, "gentones") && strcasecmp(var, "say")) {
							switch_caller_extension_add_application(session, extension, var, val);
						}
					}
				}

				switch_channel_set_caller_extension(channel, extension);
			}

			switch_channel_set_state(channel, CS_INIT);
			switch_channel_set_variable(channel, "channel_is_moving", NULL);
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, 
							"Move Channel: Resurrecting fallen channel %s\n", switch_channel_get_name(channel));

			switch_core_session_thread_launch(session);

			r = switch_channel_get_uuid(channel);
		}

	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Move Channel: Endpoint %s has no recovery function\n", technology);
	}


 end:

	UNPROTECT_INTERFACE(ep);

	switch_xml_free(xml);

	return r;
}




/* channel_move_event_handler  handles events requesting channels be moved (destroy and recover)
 *    technology - We always need the technology. This is the endpoint technology that is being recovered (like mod_sofia)
 *    channel_id - Required if we are destroying (starting a move) a channel in preparation for recreation
 *    metadata   - Required if we are restoring (completing) a channel move
 *
 *  As a rule, if channel_id is present we are starting a move. If metadata is present, we are finishing a move.
 */
SWITCH_DECLARE(void) channel_move_event_handler(switch_event_t *event)
{
	char *channel_id = switch_event_get_header(event, "channel_id");
	char *metadata = switch_event_get_header(event, "metadata");
	char *technology = switch_event_get_header(event, "technology");
	switch_event_t *completion_event = NULL;

	if (!zstr(technology) && !zstr(metadata)) {
		 switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Move Channel: Attempting to recreate previously destroyed channel\n");
		 if ((channel_id = recover_callback(technology, metadata))) {

			/* Tell the world that the channel has moved! */
			if (switch_event_create_subclass(&completion_event, SWITCH_EVENT_CUSTOM, MY_EVENT_MOVE_COMPLETE) == SWITCH_STATUS_SUCCESS) {
//				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "profile_name", h->profile->name);
//				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "old_node_hostname", mod_sofia_globals.hostname);
				switch_event_add_header_string(completion_event, SWITCH_STACK_BOTTOM, "old_node_channel_uuid", channel_id);
				switch_event_fire(&completion_event);
			}

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Move Channel: Channel is restored on new server as %s!\n", channel_id);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Move Channel: Channel restore failed...\n");
		}

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Move Channel: Done restoring channel.\n");

	} else if (!zstr(technology) && !zstr(channel_id)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Move Channel: Attempting silent destruction of channel\n");

		if (silent_destroy(technology, channel_id) == SWITCH_TRUE) {
			/* Tell the world about the channel, hoping that the call shall resume */
			if (switch_event_create_subclass(&completion_event, SWITCH_EVENT_CUSTOM, MY_EVENT_MOVE_COMPLETE) == SWITCH_STATUS_SUCCESS) {
				switch_event_add_header_string(completion_event, SWITCH_STACK_BOTTOM, "old_node_channel_uuid", channel_id);
				switch_event_fire(&completion_event);
			}

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Move Channel: Channel is released! Complete the move...\n");
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Move Channel: Channel release failed...\n");
		}

	} else {
		 switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Move Channel: Move requested but technology and either metadata or channel_id is required\n");
	}
}






/*
	switch_channel_set_variable(channel, "channel_is_moving", "false");


/ *      switch_event_t *event = NULL;

	// Tell the world about the channel, hoping that someone will pick it up
	if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, MY_EVENT_RECOVERY_SEND) == SWITCH_STATUS_SUCCESS) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "profile_name", profile->name);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "new_node_runtime_uuid", switch_core_get_uuid());
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "new_node_hostname", mod_sofia_globals.hostname);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "new_node_channel_uuid", switch_core_session_get_uuid(session));
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "metadata", xml_cdr_text);
		switch_event_fire(&event);
	}* /

*/



/*
/ * Received a request to move a channel (either to or from)
   If to this box, will require metadata and will send an event on successful restoration
   If from this box, will require a channel ID and will send an event containing metadata on successful destroy * /
void sofia_glue_move_request_event_handler(switch_event_t *event)
{
	char *buf = NULL;
	char *profile_name = NULL;

	switch_assert(event);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Request to move a channel, deciding if this is new node or old node...\n");
	if ((buf = switch_event_get_header(event, "metadata")) && (profile_name = switch_event_get_header(event, "profile_name"))) {
		sofia_profile_t *profile;

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Got profile name %s and metadata:\n%s\n", profile_name, buf);

		if ((profile = sofia_glue_find_profile(profile_name))) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Found profile, attempting recovery of channel\n");
			//sofia_glue_move_restore_channel(profile, buf);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Channel restored!\n");
			sofia_glue_release_profile(profile);
		}
	} else if ((buf = switch_event_get_header(event, "channel_id")) && (profile_name = switch_event_get_header(event, "profile_name"))) {
		sofia_profile_t *profile;
		switch_core_session_t *session;

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Got request to move/kill profile name %s and channel %s\n", profile_name, buf);

		if ((profile = sofia_glue_find_profile(profile_name))) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Found profile, attempting to move channel\n");
			if ((session = switch_core_session_locate(buf))) {
				sofia_glue_move_release_channel(profile, session);
				switch_core_session_rwunlock(session);
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Done moving.\n");
			} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Couldn't find the requested channel.\n");
		}
			sofia_glue_release_profile(profile);
		}
	}

	return;
}
*/






SWITCH_MODULE_LOAD_FUNCTION(mod_channel_move_load)
{
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

        if (switch_event_bind(modname, SWITCH_EVENT_CUSTOM, MY_EVENT_MOVE_REQUEST, channel_move_event_handler, NULL) != SWITCH_STATUS_SUCCESS) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't bind!\n");
                return SWITCH_STATUS_TERM;
        }

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Ready to move channels around the globe!\n");

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_channel_move_shutdown)
{
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
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4
 */
