/* vifm
 * Copyright (C) 2026 NeoVifm contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
# include <unistd.h>
#else
# include <wchar.h>
#endif

#include "snapshot_json.h"
#include "undo_bridge.h"
#include "open_config.h"
#include "session_platform.h"
#include "session_watcher.h"
#include "workspace_session.h"
#include "../compat/neovifm_fs.h"
#include "../utils/parson.h"
#include "../utils/utf8.h"

#define NV_SESSION_MAX_COMMAND_BYTES (16U*1024U)
#define NV_SESSION_MAX_RETRY_HISTORY 64U

typedef struct nv_pending_action_context_t
{
	uint64_t task_id;
	int active;
	nv_session_pane_t source_pane;
	uint64_t source_tab_id;
	int has_destination;
	nv_session_pane_t destination_pane;
	uint64_t destination_tab_id;
	char *undo_path;
	nv_fs_identity_t undo_parent_identity;
	nv_session_prepared_action_t undo_action;
	nv_session_prepared_action_t retry_action;
	int retry_available;
	struct nv_pending_action_context_t *next;
} nv_pending_action_context_t;

typedef struct nv_pending_resource_context_t
{
	uint64_t task_id;
	nv_resource_task_kind_t kind;
	nv_session_pane_t pane;
	uint64_t tab_id;
	char *origin_directory;
	char *origin_cwd_bytes_hex;
	char *origin_entry_path_bytes_hex;
	char *remote;
	struct nv_pending_resource_context_t *next;
} nv_pending_resource_context_t;

static int write_line(const char json[]);
static int write_workspace(const nv_workspace_session_t *session,
		unsigned int output_sequence, unsigned int command_sequence,
		const char trigger[]);
static int write_command_error(const nv_snapshot_error_t *error,
		unsigned int output_sequence, unsigned int command_sequence);
static int parse_command(const char line[], unsigned int previous_sequence,
		unsigned int *sequence, nv_session_command_t *command);
static int parse_open_command(JSON_Object *payload,
		nv_session_command_t *command);
static int open_intent_from_string(const char intent[], nv_open_intent_t *result);
static char *open_hex_decode(const char hex[]);
static int discard_command_tail(void);
static int set_error(nv_snapshot_error_t *error, const char code[],
		const char message[]);
static int process_command_line(nv_workspace_session_t *session, char line[],
		size_t line_capacity, unsigned int *output_sequence,
		unsigned int *command_sequence, int *directory_changed,
		nv_session_watcher_t *watcher,
		nv_preview_queue_t *preview_queue, uint64_t *preview_generation,
		nv_action_queue_t *action_queue,
		nv_pending_action_context_t **pending_actions,
		size_t *retry_history_count,
		nv_resource_task_queue_t *resource_queue,
		nv_pending_resource_context_t **pending_resources);
static int submit_active_preview(const nv_workspace_session_t *session,
		nv_preview_queue_t *queue, uint64_t *generation);
static int submit_preview_request(nv_preview_queue_t *queue, uint64_t *generation,
		nv_session_pane_t source_pane, nv_session_pane_t target_pane,
		const char cwd_bytes_hex[], const char path_bytes_hex[],
		nv_preview_kind_t kind);
static int active_entry_is_archive(const nv_workspace_session_t *session);
static int active_tab_has_resource(const nv_workspace_session_t *session);
static int validate_preview_identity(const nv_workspace_session_t *session,
		const nv_session_command_t *command, nv_entry_kind_t *kind,
		const char **entry_name,
		nv_snapshot_error_t *error);
static int validate_open_identity(const nv_workspace_session_t *session,
		const nv_session_command_t *command, nv_snapshot_error_t *error);
static int submit_requested_preview(const nv_workspace_session_t *session,
		const nv_session_command_t *command, nv_entry_kind_t kind,
		const char entry_name[],
		nv_preview_queue_t *queue, uint64_t *generation);
static nv_preview_kind_t preview_kind_for_entry(nv_entry_kind_t kind,
		const char name[]);
static int drain_preview_events(nv_preview_queue_t *queue,
		unsigned int *output_sequence);
static int drain_action_events(nv_action_queue_t *queue,
		nv_workspace_session_t *session, unsigned int *output_sequence,
		unsigned int command_sequence, nv_preview_queue_t *preview_queue,
		uint64_t *preview_generation,
		nv_pending_action_context_t **pending_actions,
		size_t *retry_history_count, nv_session_watcher_t *watcher);
static int drain_resource_events(nv_resource_task_queue_t *queue,
		nv_workspace_session_t *session, unsigned int *output_sequence,
		nv_preview_queue_t *preview_queue, uint64_t *preview_generation,
		nv_pending_resource_context_t **pending_resources,
		nv_session_watcher_t *watcher);
static int drain_watcher_events(nv_session_watcher_t *watcher,
		nv_workspace_session_t *session, unsigned int *output_sequence,
		unsigned int command_sequence, nv_preview_queue_t *preview_queue,
		uint64_t *preview_generation, nv_action_queue_t *action_queue);
static void sync_watcher_bindings(nv_session_watcher_t *watcher,
		const nv_workspace_session_t *session);
static int submit_archive_mount(const nv_workspace_session_t *session,
		nv_resource_task_queue_t *queue, unsigned int command_sequence,
		nv_pending_resource_context_t **pending_resources);
static int submit_ssh_mount(const nv_workspace_session_t *session,
		const nv_session_command_t *command, nv_resource_task_queue_t *queue,
		unsigned int command_sequence,
		nv_pending_resource_context_t **pending_resources);
static int submit_resource_unmount(const nv_workspace_session_t *session,
		nv_resource_task_queue_t *queue, unsigned int command_sequence,
		nv_pending_resource_context_t **pending_resources);
static int submit_resource_unmount_at(const nv_workspace_session_t *session,
		nv_session_pane_t pane, size_t tab_index,
		nv_resource_task_queue_t *queue, unsigned int command_sequence,
		nv_pending_resource_context_t **pending_resources);
static nv_pending_resource_context_t *find_resource_context(
		nv_pending_resource_context_t *contexts, uint64_t task_id,
		nv_pending_resource_context_t **previous);
static void pending_resource_context_free(nv_pending_resource_context_t *context);
static int command_is_action(nv_session_command_kind_t kind);
static int refresh_action_tab(nv_workspace_session_t *session,
		nv_session_pane_t pane, uint64_t tab_id, nv_snapshot_error_t *error);
static nv_pending_action_context_t *find_retry_context(
		nv_pending_action_context_t *contexts, uint64_t task_id);
static void trim_retry_history(nv_pending_action_context_t **contexts,
		size_t *retry_history_count);
static int clone_prepared_action(const nv_session_prepared_action_t *source,
		nv_session_prepared_action_t *destination);
static int record_action_undo(const nv_pending_action_context_t *context,
		nv_action_task_state_t state);

static int
set_error(nv_snapshot_error_t *error, const char code[], const char message[])
{
	nv_snapshot_error_free(error);
	error->code = strdup(code);
	error->message = strdup(message);
	if(error->code == NULL || error->message == NULL)
	{
		nv_snapshot_error_free(error);
	}
	return -1;
}

static int
write_line(const char json[])
{
	if(json == NULL || fputs(json, stdout) == EOF || fputc('\n', stdout) == EOF ||
			fflush(stdout) == EOF)
	{
		fputs("neovifm-core-session: failed to write protocol output\n", stderr);
		return -1;
	}
	return 0;
}

static int
write_workspace(const nv_workspace_session_t *session, unsigned int output_sequence,
		unsigned int command_sequence, const char trigger[])
{
	char *json = NULL;
	const nv_protocol_json_result_t result =
		nv_protocol_preview_workspace_session_snapshot_json(session, output_sequence,
				command_sequence, trigger, &json);
	if(result != NV_PROTOCOL_JSON_OK)
	{
		fputs("neovifm-core-session: failed to serialize workspace\n", stderr);
		return -1;
	}
	const int write_result = write_line(json);
	nv_protocol_json_free(json);
	return write_result;
}

static int
write_command_error(const nv_snapshot_error_t *error, unsigned int output_sequence,
		unsigned int command_sequence)
{
	char *const json = nv_protocol_preview_session_command_error_json(error,
			output_sequence, command_sequence);
	if(json == NULL)
	{
		fputs("neovifm-core-session: failed to serialize command error\n", stderr);
		return -1;
	}
	const int result = write_line(json);
	nv_protocol_json_free(json);
	return result;
}

static int
pane_from_string(const char pane[], nv_session_pane_t *result)
{
	if(pane == NULL || result == NULL) return -1;
	if(strcmp(pane, "left") == 0) { *result = NV_SESSION_LEFT; return 0; }
	if(strcmp(pane, "right") == 0) { *result = NV_SESSION_RIGHT; return 0; }
	return -1;
}

static int
sort_key_from_string(const char key[], nv_pane_sort_key_t *result)
{
	if(key == NULL || result == NULL) return -1;
	if(strcmp(key, "name") == 0) { *result = NV_SORT_NAME; return 0; }
	if(strcmp(key, "extension") == 0) { *result = NV_SORT_EXTENSION; return 0; }
	if(strcmp(key, "size") == 0) { *result = NV_SORT_SIZE; return 0; }
	if(strcmp(key, "ctime") == 0) { *result = NV_SORT_CTIME; return 0; }
	if(strcmp(key, "mtime") == 0) { *result = NV_SORT_MTIME; return 0; }
	if(strcmp(key, "mode") == 0) { *result = NV_SORT_MODE; return 0; }
	if(strcmp(key, "type") == 0) { *result = NV_SORT_TYPE; return 0; }
	if(strcmp(key, "other") == 0) { *result = NV_SORT_OTHER; return 0; }
	return -1;
}

static int
open_intent_from_string(const char intent[], nv_open_intent_t *result)
{
	if(intent == NULL || result == NULL) return -1;
	if(strcmp(intent, "open") == 0) *result = NV_OPEN_INTENT_OPEN;
	else if(strcmp(intent, "edit") == 0) *result = NV_OPEN_INTENT_EDIT;
	else if(strcmp(intent, "preview") == 0) *result = NV_OPEN_INTENT_PREVIEW;
	else return -1;
	return 0;
}

static int
open_hex_string_valid(const char value[])
{
	if(value == NULL || value[0] == '\0' || strlen(value) >
			NV_PANE_SNAPSHOT_MAX_HEX_BYTES || strlen(value) % 2U != 0U)
		return 0;
	for(const char *character = value; *character != '\0'; ++character)
	{
		if(!((*character >= '0' && *character <= '9') ||
				(*character >= 'a' && *character <= 'f')))
			return 0;
	}
	return 1;
}

static int copy_action_string(JSON_Object *payload, const char field[],
		char **result);
static int copy_search_string(JSON_Object *payload, const char field[],
		char **result);
static int parse_u64_field(JSON_Object *object, const char field[],
		uint64_t *result);

static int
parse_open_command(JSON_Object *payload, nv_session_command_t *command)
{
	const char *const intent = json_object_get_string(payload, "intent");
	const char *const path = json_object_get_string(payload, "path_bytes_hex");
	command->owns_open_fields = 1;
	if(open_intent_from_string(intent, &command->open_intent) != 0 ||
			pane_from_string(json_object_get_string(payload, "pane"),
				&command->open_pane) != 0 ||
			copy_action_string(payload, "cwd_bytes_hex",
				&command->open_cwd_bytes_hex) != 0 ||
			parse_u64_field(payload, "snapshot_revision",
				&command->open_snapshot_revision) != 0 ||
			parse_u64_field(payload, "cwd_device", &command->open_cwd_device) != 0 ||
			parse_u64_field(payload, "cwd_inode", &command->open_cwd_inode) != 0 ||
			parse_u64_field(payload, "cwd_ctime_unix_ns",
				&command->open_cwd_ctime_unix_ns) != 0 ||
			path == NULL || !open_hex_string_valid(path) ||
			copy_action_string(payload, "path_bytes_hex",
				&command->open_path_bytes_hex) != 0 ||
			parse_u64_field(payload, "device", &command->open_entry_device) != 0 ||
			parse_u64_field(payload, "inode", &command->open_entry_inode) != 0 ||
			parse_u64_field(payload, "ctime_unix_ns",
				&command->open_entry_ctime_unix_ns) != 0)
	{
		return -1;
	}
	JSON_Value *const association_value = json_object_get_value(payload,
			"association_argv");
	if(association_value == NULL) return 0;
	if(json_value_get_type(association_value) != JSONArray)
		return -1;
	JSON_Array *const association = json_value_get_array(association_value);
	const size_t count = json_array_get_count(association);
	if(count > NV_OPEN_MAX_ARGS - 1U) return -1;
	if(count == 0U) return 0;
	command->open_association_argv = calloc(count, sizeof(*command->open_association_argv));
	if(command->open_association_argv == NULL) return -1;
	command->open_association_argc = count;
	for(size_t i = 0U; i < count; ++i)
	{
		const char *const argument = json_array_get_string(association, i);
		if(argument == NULL || argument[0] == '\0' ||
				strlen(argument) > NV_OPEN_MAX_ARG_BYTES ||
				(command->open_association_argv[i] = strdup(argument)) == NULL)
			return -1;
	}
	return 0;
}

static int
copy_action_string(JSON_Object *payload, const char field[], char **result)
{
	const char *const value = json_object_get_string(payload, field);
	if(value == NULL || strlen(value) > 32U*1024U) return -1;
	*result = strdup(value);
	return *result == NULL ? -1 : 0;
}

static int
copy_search_string(JSON_Object *payload, const char field[], char **result)
{
	const char *const value = json_object_get_string(payload, field);
	if(value == NULL || value[0] == '\0' ||
			strlen(value) > NV_SESSION_MAX_SEARCH_BYTES)
		return -1;
	*result = strdup(value);
	return *result == NULL ? -1 : 0;
}

static int
parse_u64_field(JSON_Object *object, const char field[], uint64_t *result)
{
	const char *const value = json_object_get_string(object, field);
	if(value == NULL || value[0] == '\0' || strlen(value) > 32U) return -1;
	for(const char *character = value; *character != '\0'; ++character)
	{
		if(*character < '0' || *character > '9') return -1;
	}
	char *end = NULL;
	errno = 0;
	const unsigned long long parsed = strtoull(value, &end, 10);
	if(errno != 0 || end == value || *end != '\0') return -1;
	*result = (uint64_t)parsed;
	return (unsigned long long)*result == parsed ? 0 : -1;
}

static int
entry_kind_from_string(const char kind[], nv_entry_kind_t *result)
{
	if(kind == NULL || result == NULL) return -1;
	static const struct { const char *name; nv_entry_kind_t kind; } kinds[] = {
		{ "directory", NV_ENTRY_DIRECTORY }, { "file", NV_ENTRY_FILE },
		{ "symlink", NV_ENTRY_SYMLINK }, { "executable", NV_ENTRY_EXECUTABLE },
		{ "fifo", NV_ENTRY_FIFO }, { "socket", NV_ENTRY_SOCKET },
		{ "char-device", NV_ENTRY_CHAR_DEVICE },
		{ "block-device", NV_ENTRY_BLOCK_DEVICE }, { "unknown", NV_ENTRY_UNKNOWN },
	};
	for(size_t i = 0U; i < sizeof(kinds)/sizeof(kinds[0]); ++i)
	{
		if(strcmp(kind, kinds[i].name) == 0) { *result = kinds[i].kind; return 0; }
	}
	return -1;
}

static int
parse_action_identity(JSON_Object *payload, nv_session_command_t *command,
		int needs_destination, int needs_paths)
{
	command->owns_action_fields = 1;
	if(pane_from_string(json_object_get_string(payload, "pane"),
			&command->pane) != 0 ||
			copy_action_string(payload, "cwd_bytes_hex",
				&command->action_cwd_bytes_hex) != 0 ||
			parse_u64_field(payload, "snapshot_revision",
				&command->action_snapshot_revision) != 0 ||
			parse_u64_field(payload, "cwd_device",
				&command->action_cwd_device) != 0 ||
			parse_u64_field(payload, "cwd_inode",
				&command->action_cwd_inode) != 0 ||
			parse_u64_field(payload, "cwd_ctime_unix_ns",
				&command->action_cwd_ctime_unix_ns) != 0)
	{
		return -1;
	}
	if(needs_destination && copy_action_string(payload,
			"destination_cwd_bytes_hex",
			&command->action_destination_cwd_bytes_hex) != 0)
	{
		return -1;
	}
	if(needs_destination &&
			(parse_u64_field(payload, "destination_snapshot_revision",
				&command->action_destination_snapshot_revision) != 0 ||
			 parse_u64_field(payload, "destination_cwd_device",
				&command->action_destination_cwd_device) != 0 ||
			 parse_u64_field(payload, "destination_cwd_inode",
				&command->action_destination_cwd_inode) != 0 ||
			 parse_u64_field(payload, "destination_cwd_ctime_unix_ns",
				&command->action_destination_cwd_ctime_unix_ns) != 0)) return -1;
	if(!needs_paths) return 0;

	JSON_Array *const targets = json_object_get_array(payload, "targets");
	const size_t count = targets == NULL ? 0U : json_array_get_count(targets);
	if(count == 0U || count > NV_SESSION_MAX_ACTION_PATHS) return -1;
	command->action_targets = calloc(count, sizeof(*command->action_targets));
	if(command->action_targets == NULL) return -1;
	command->action_target_count = count;
	for(size_t i = 0U; i < count; ++i)
	{
		JSON_Object *const target = json_array_get_object(targets, i);
		const char *const path = target == NULL ? NULL :
			json_object_get_string(target, "path_bytes_hex");
		if(path == NULL || strlen(path) > 32U*1024U ||
				(command->action_targets[i].path_bytes_hex = strdup(path)) == NULL ||
				parse_u64_field(target, "device",
					&command->action_targets[i].device) != 0 ||
				parse_u64_field(target, "inode",
					&command->action_targets[i].inode) != 0 ||
				parse_u64_field(target, "ctime_unix_ns",
					&command->action_targets[i].ctime_unix_ns) != 0 ||
				entry_kind_from_string(json_object_get_string(target, "kind"),
					&command->action_targets[i].kind) != 0)
		{
			return -1;
		}
		for(size_t j = 0U; j < i; ++j)
		{
			if(strcmp(command->action_targets[j].path_bytes_hex, path) == 0)
				return -1;
		}
	}
	return 0;
}

static int
parse_preview_identity(JSON_Object *payload, nv_session_command_t *command)
{
	command->owns_preview_fields = 1;
	if(pane_from_string(json_object_get_string(payload, "pane"),
			&command->pane) != 0 || pane_from_string(json_object_get_string(
			payload, "target_pane"), &command->preview_target_pane) != 0 ||
			copy_action_string(payload, "cwd_bytes_hex",
				&command->preview_cwd_bytes_hex) != 0 ||
			parse_u64_field(payload, "snapshot_revision",
				&command->preview_snapshot_revision) != 0 ||
			parse_u64_field(payload, "cwd_device",
				&command->preview_cwd_device) != 0 ||
			parse_u64_field(payload, "cwd_inode",
				&command->preview_cwd_inode) != 0 ||
			parse_u64_field(payload, "cwd_ctime_unix_ns",
				&command->preview_cwd_ctime_unix_ns) != 0 ||
			copy_action_string(payload, "path_bytes_hex",
				&command->preview_path_bytes_hex) != 0 ||
			parse_u64_field(payload, "device", &command->preview_entry_device) != 0 ||
			parse_u64_field(payload, "inode", &command->preview_entry_inode) != 0 ||
			parse_u64_field(payload, "ctime_unix_ns",
				&command->preview_entry_ctime_unix_ns) != 0)
	{
		return -1;
	}
	return 0;
}

static int
parse_command(const char line[], unsigned int previous_sequence,
		unsigned int *sequence, nv_session_command_t *command)
{
	JSON_Value *const value = json_parse_string(line);
	if(value == NULL || json_value_get_type(value) != JSONObject)
	{
		json_value_free(value);
		return -1;
	}
	JSON_Object *const root = json_value_get_object(value);
	JSON_Object *const payload = json_object_get_object(root, "payload");
	const char *const protocol = json_object_get_string(root, "protocol");
	const char *const type = json_object_get_string(root, "type");
	const double version = json_object_get_number(root, "version");
	const double next_sequence = json_object_get_number(root, "sequence");
	const char *const action = payload == NULL ? NULL : json_object_get_string(payload, "action");
	if(protocol == NULL || strcmp(protocol, "neovifm-core") != 0 || type == NULL ||
			strcmp(type, "command") != 0 || version != 3.0 || next_sequence < 1.0 ||
			next_sequence > 4294967295.0 || next_sequence != (double)(unsigned int)next_sequence ||
			(unsigned int)next_sequence <= previous_sequence || action == NULL)
	{
		json_value_free(value);
		return -1;
	}
	memset(command, 0, sizeof(*command));
	if(strcmp(action, "focus") == 0)
	{
		command->kind = NV_SESSION_FOCUS;
		if(pane_from_string(json_object_get_string(payload, "pane"), &command->pane) != 0)
		{
			json_value_free(value);
			return -1;
		}
	}
	else if(strcmp(action, "preview") == 0)
	{
		command->kind = NV_SESSION_PREVIEW;
		if(parse_preview_identity(payload, command) != 0)
		{
			nv_session_command_free(command);
			json_value_free(value);
			return -1;
		}
	}
	else if(strcmp(action, "open") == 0)
	{
		command->kind = NV_SESSION_OPEN;
		if(parse_open_command(payload, command) != 0)
		{
			nv_session_command_free(command);
			json_value_free(value);
			return -1;
		}
	}
	else if(strcmp(action, "focus-next") == 0) command->kind = NV_SESSION_FOCUS_NEXT;
	else if(strcmp(action, "move") == 0)
	{
		const double delta = json_object_get_number(payload, "delta");
		if(delta != -1.0 && delta != 1.0)
		{
			json_value_free(value);
			return -1;
		}
		command->kind = NV_SESSION_MOVE_CURSOR;
		command->delta = (int)delta;
	}
	else if(strcmp(action, "move-to") == 0)
	{
		const char *const target = json_object_get_string(payload, "target");
		if(target == NULL || (strcmp(target, "first") != 0 && strcmp(target, "last") != 0))
		{
			json_value_free(value);
			return -1;
		}
		command->kind = strcmp(target, "first") == 0 ?
			NV_SESSION_MOVE_FIRST : NV_SESSION_MOVE_LAST;
	}
	else if(strcmp(action, "search") == 0 || strcmp(action, "search-next") == 0)
	{
		const double direction = json_object_get_number(payload, "direction");
		if(direction != -1.0 && direction != 1.0)
		{
			json_value_free(value);
			return -1;
		}
		command->kind = strcmp(action, "search") == 0 ?
			NV_SESSION_SEARCH : NV_SESSION_SEARCH_NEXT;
		command->search_direction = (int)direction;
		if(command->kind == NV_SESSION_SEARCH)
		{
			command->owns_search_fields = 1;
			if(copy_search_string(payload, "query", &command->search_query) != 0)
			{
				nv_session_command_free(command);
				json_value_free(value);
				return -1;
			}
		}
	}
	else if(strcmp(action, "enter") == 0) command->kind = NV_SESSION_ENTER;
	else if(strcmp(action, "mount-ssh") == 0)
	{
		command->kind = NV_SESSION_MOUNT_SSH;
		command->owns_resource_fields = 1;
		if(pane_from_string(json_object_get_string(payload, "pane"),
				&command->pane) != 0 || copy_action_string(payload, "remote",
				&command->resource_remote) != 0)
		{
			nv_session_command_free(command);
			json_value_free(value);
			return -1;
		}
	}
	else if(strcmp(action, "parent") == 0) command->kind = NV_SESSION_PARENT;
	else if(strcmp(action, "toggle-selection") == 0) command->kind = NV_SESSION_TOGGLE_SELECTION;
	else if(strcmp(action, "refresh") == 0) command->kind = NV_SESSION_REFRESH;
	else if(strcmp(action, "undo") == 0) command->kind = NV_SESSION_UNDO;
	else if(strcmp(action, "cancel-action") == 0)
	{
		if(parse_u64_field(payload, "task_id", &command->action_task_id) != 0 ||
				command->action_task_id == 0U)
		{
			json_value_free(value);
			return -1;
		}
		command->kind = NV_SESSION_CANCEL_ACTION;
	}
	else if(strcmp(action, "cancel-resource") == 0)
	{
		if(parse_u64_field(payload, "task_id", &command->resource_task_id) != 0 ||
				command->resource_task_id == 0U)
		{
			json_value_free(value);
			return -1;
		}
		command->kind = NV_SESSION_CANCEL_RESOURCE;
	}
	else if(strcmp(action, "retry-action") == 0)
	{
		if(parse_u64_field(payload, "task_id", &command->action_task_id) != 0 ||
				command->action_task_id == 0U)
		{
			json_value_free(value);
			return -1;
		}
		command->kind = NV_SESSION_RETRY_ACTION;
	}
	else if(strcmp(action, "sort-cycle") == 0)
	{
		const double delta = json_object_get_number(payload, "delta");
		if(delta != -1.0 && delta != 1.0)
		{
			json_value_free(value);
			return -1;
		}
		command->kind = NV_SESSION_SORT_CYCLE;
		command->delta = (int)delta;
		JSON_Value *const pane_value = json_object_get_value(payload, "pane");
		if(pane_value != NULL)
		{
			if(json_value_get_type(pane_value) != JSONString || pane_from_string(
					json_value_get_string(pane_value), &command->pane) != 0)
			{
				json_value_free(value);
				return -1;
			}
			command->has_pane = 1;
		}
	}
	else if(strcmp(action, "sort-by") == 0)
	{
		command->kind = NV_SESSION_SORT_BY;
		if(pane_from_string(json_object_get_string(payload, "pane"),
				&command->pane) != 0 ||
				sort_key_from_string(json_object_get_string(payload, "key"),
					&command->sort_key) != 0)
		{
			json_value_free(value);
			return -1;
		}
	}
	else if(strcmp(action, "select-entry") == 0)
	{
		JSON_Value *const index_value = json_object_get_value(payload, "index");
		const double index = json_value_get_number(index_value);
		JSON_Value *const toggle = json_object_get_value(payload, "toggle");
		if(pane_from_string(json_object_get_string(payload, "pane"),
				&command->pane) != 0 ||
				json_value_get_type(index_value) != JSONNumber || index < 0.0 ||
				index > (double)(NV_PANE_SNAPSHOT_MAX_ENTRIES - 1U) ||
				index != (double)(size_t)index ||
				json_value_get_type(toggle) != JSONBoolean)
		{
			json_value_free(value);
			return -1;
		}
		command->kind = NV_SESSION_SELECT_ENTRY;
		command->entry_index = (size_t)index;
		command->toggle_selection = json_value_get_boolean(toggle);
	}
	else if(strcmp(action, "new-tab") == 0)
	{
		command->kind = NV_SESSION_NEW_TAB;
		if(pane_from_string(json_object_get_string(payload, "pane"),
				&command->pane) != 0)
		{
			json_value_free(value);
			return -1;
		}
	}
	else if(strcmp(action, "activate-tab") == 0 ||
			strcmp(action, "close-tab") == 0)
	{
		command->kind = strcmp(action, "activate-tab") == 0 ?
			NV_SESSION_ACTIVATE_TAB : NV_SESSION_CLOSE_TAB;
		if(pane_from_string(json_object_get_string(payload, "pane"),
				&command->pane) != 0 || parse_u64_field(payload, "tab_id",
					&command->tab_id) != 0 || command->tab_id == 0U)
		{
			json_value_free(value);
			return -1;
		}
	}
	else if(strcmp(action, "tab-cycle") == 0)
	{
		const double delta = json_object_get_number(payload, "delta");
		if(delta == 0.0 || delta < -(double)NV_SESSION_MAX_TAB_CYCLE ||
				delta > (double)NV_SESSION_MAX_TAB_CYCLE ||
				delta != (double)(int)delta)
		{
			json_value_free(value);
			return -1;
		}
		command->kind = NV_SESSION_TAB_CYCLE;
		command->delta = (int)delta;
	}
	else if(strcmp(action, "copy") == 0 || strcmp(action, "move-files") == 0 ||
			strcmp(action, "delete") == 0)
	{
		command->kind = strcmp(action, "copy") == 0 ? NV_SESSION_COPY :
			strcmp(action, "move-files") == 0 ? NV_SESSION_MOVE_FILES :
			NV_SESSION_DELETE;
		if(parse_action_identity(payload, command,
				command->kind != NV_SESSION_DELETE, 1) != 0)
		{
			nv_session_command_free(command);
			json_value_free(value);
			return -1;
		}
	}
	else if(strcmp(action, "mkdir") == 0)
	{
		const char *const name = json_object_get_string(payload, "name");
		command->kind = NV_SESSION_MKDIR;
		if(name == NULL || strlen(name) > NV_SESSION_MAX_NAME_BYTES ||
				parse_action_identity(payload, command, 0, 0) != 0)
		{
			nv_session_command_free(command);
			json_value_free(value);
			return -1;
		}
		strcpy(command->name, name);
	}
	else
	{
		nv_session_command_free(command);
		json_value_free(value);
		return -1;
	}
	*sequence = (unsigned int)next_sequence;
	json_value_free(value);
	return 0;
}

static int
discard_command_tail(void)
{
	int character;
	do { character = fgetc(stdin); } while(character != '\n' && character != EOF);
	return character == EOF ? -1 : 0;
}

static int
command_is_action(nv_session_command_kind_t kind)
{
	return kind == NV_SESSION_COPY || kind == NV_SESSION_MOVE_FILES ||
		kind == NV_SESSION_MKDIR || kind == NV_SESSION_DELETE;
}

static char *
join_action_path(const char directory[], const char name[])
{
	const size_t directory_length = strlen(directory);
	const size_t name_length = strlen(name);
	const int separator = directory_length != 0U &&
		directory[directory_length - 1U] != '/';
	if(directory_length > SIZE_MAX - name_length - (size_t)separator - 1U)
	{
		errno = ENOMEM;
		return NULL;
	}
	char *const path = malloc(directory_length + (size_t)separator +
		name_length + 1U);
	if(path == NULL)
	{
		errno = ENOMEM;
		return NULL;
	}
	memcpy(path, directory, directory_length);
	if(separator) path[directory_length] = '/';
	memcpy(path + directory_length + (size_t)separator, name, name_length + 1U);
	return path;
}

static int
clone_prepared_action(const nv_session_prepared_action_t *source,
		nv_session_prepared_action_t *destination)
{
	if(source == NULL || destination == NULL) return -1;
	*destination = (nv_session_prepared_action_t){
		.kind = source->kind,
		.pane = source->pane,
		.source_directory_identity = source->source_directory_identity,
		.destination_directory_identity = source->destination_directory_identity,
		.target_count = source->target_count,
	};
	destination->source_directory = source->source_directory == NULL ? NULL :
		strdup(source->source_directory);
	destination->destination_directory = source->destination_directory == NULL ? NULL :
		strdup(source->destination_directory);
	destination->name = source->name == NULL ? NULL : strdup(source->name);
	if((source->source_directory != NULL && destination->source_directory == NULL) ||
			(source->destination_directory != NULL && destination->destination_directory == NULL) ||
			(source->name != NULL && destination->name == NULL))
		goto failed;
	if(source->target_count != 0U)
	{
		destination->targets = calloc(source->target_count, sizeof(*destination->targets));
		if(destination->targets == NULL) goto failed;
		for(size_t i = 0U; i < source->target_count; ++i)
		{
			destination->targets[i].identity = source->targets[i].identity;
			destination->targets[i].kind = source->targets[i].kind;
			destination->targets[i].size_bytes = source->targets[i].size_bytes;
			destination->targets[i].size_known = source->targets[i].size_known;
			destination->targets[i].path = source->targets[i].path == NULL ? NULL :
				strdup(source->targets[i].path);
			destination->targets[i].name = source->targets[i].name == NULL ? NULL :
				strdup(source->targets[i].name);
			if((source->targets[i].path != NULL && destination->targets[i].path == NULL) ||
					(source->targets[i].name != NULL && destination->targets[i].name == NULL))
				goto failed;
		}
	}
	return 0;

failed:
	nv_session_prepared_action_free(destination);
	return -1;
}

static int
pending_action_context_prepare(nv_pending_action_context_t *context,
		const nv_workspace_session_t *session,
		const nv_session_command_t *command,
		const nv_session_prepared_action_t *action)
{
	const size_t source_index = nv_workspace_session_active_tab_index(session,
			command->pane);
	*context = (nv_pending_action_context_t){
		.active = 1,
		.source_pane = command->pane,
		.source_tab_id = nv_workspace_session_tab_id(session, command->pane,
				source_index),
	};
	if(command->kind == NV_SESSION_COPY || command->kind == NV_SESSION_MOVE_FILES)
	{
		context->has_destination = 1;
		context->destination_pane = command->pane == NV_SESSION_LEFT ?
			NV_SESSION_RIGHT : NV_SESSION_LEFT;
		const size_t destination_index = nv_workspace_session_active_tab_index(session,
			context->destination_pane);
		context->destination_tab_id = nv_workspace_session_tab_id(session,
				context->destination_pane, destination_index);
	}
	if(action != NULL && action->kind == NV_SESSION_MKDIR)
	{
		context->undo_path = join_action_path(action->source_directory,
				action->name);
		context->undo_parent_identity = action->source_directory_identity;
	}
	if(action != NULL && (action->kind == NV_SESSION_COPY ||
			action->kind == NV_SESSION_MOVE_FILES) &&
			clone_prepared_action(action, &context->undo_action) != 0)
		return -1;
	return 0;
}

static void
pending_action_context_free(nv_pending_action_context_t *context)
{
	if(context == NULL) return;
	free(context->undo_path);
	nv_session_prepared_action_free(&context->undo_action);
	nv_session_prepared_action_free(&context->retry_action);
	free(context);
}

static int
record_action_undo(const nv_pending_action_context_t *context,
		nv_action_task_state_t state)
{
	if(context == NULL)
	{
		errno = EINVAL;
		return -1;
	}
	if(state != NV_ACTION_TASK_DONE) return 0;
	if((context->undo_action.kind != NV_SESSION_COPY &&
			context->undo_action.kind != NV_SESSION_MOVE_FILES) ||
			context->undo_action.target_count == 0U ||
			context->undo_action.destination_directory == NULL)
	{
		errno = EINVAL;
		return -1;
	}
	const size_t count = context->undo_action.target_count;
	nv_undo_bridge_transfer_t *const transfers = calloc(count, sizeof(*transfers));
	char **const destinations = calloc(count, sizeof(*destinations));
	if(transfers == NULL || destinations == NULL)
	{
		free(transfers);
		free(destinations);
		errno = ENOMEM;
		return -1;
	}
	int result = 0;
	for(size_t i = 0U; i < count; ++i)
	{
		destinations[i] = join_action_path(context->undo_action.destination_directory,
				context->undo_action.targets[i].name);
		if(destinations[i] == NULL)
		{
			result = -1;
			break;
		}
		transfers[i] = (nv_undo_bridge_transfer_t){
			.source_path = context->undo_action.targets[i].path,
			.destination_path = destinations[i],
			.source_identity = context->undo_action.targets[i].identity,
		};
	}
	if(result == 0)
	{
		const nv_undo_bridge_location_t location = {
			.pane = (unsigned int)context->source_pane,
			.tab_id = context->source_tab_id,
			.has_destination = context->has_destination,
			.destination_pane = (unsigned int)context->destination_pane,
			.destination_tab_id = context->destination_tab_id,
		};
		result = context->undo_action.kind == NV_SESSION_COPY ?
			nv_undo_bridge_record_copy_group(transfers, count,
				context->undo_action.destination_directory_identity, location) :
			nv_undo_bridge_record_move_group(transfers, count,
				context->undo_action.source_directory_identity,
				context->undo_action.destination_directory_identity, location);
	}
	for(size_t i = 0U; i < count; ++i) free(destinations[i]);
	free(destinations);
	free(transfers);
	return result;
}

static void
pending_resource_context_free(nv_pending_resource_context_t *context)
{
	if(context == NULL) return;
	free(context->origin_directory);
	free(context->origin_cwd_bytes_hex);
	free(context->origin_entry_path_bytes_hex);
	free(context->remote);
	free(context);
}

static nv_pending_resource_context_t *
find_resource_context(nv_pending_resource_context_t *contexts, uint64_t task_id,
		nv_pending_resource_context_t **previous)
{
	if(previous != NULL) *previous = NULL;
	while(contexts != NULL && contexts->task_id != task_id)
	{
		if(previous != NULL) *previous = contexts;
		contexts = contexts->next;
	}
	return contexts;
}

static nv_pending_action_context_t *
find_retry_context(nv_pending_action_context_t *contexts, uint64_t task_id)
{
	while(contexts != NULL)
	{
		if(contexts->task_id == task_id && contexts->retry_available)
			return contexts;
		contexts = contexts->next;
	}
	return NULL;
}

static void
trim_retry_history(nv_pending_action_context_t **contexts,
		size_t *retry_history_count)
{
	if(contexts == NULL || retry_history_count == NULL) return;
	while(*retry_history_count > NV_SESSION_MAX_RETRY_HISTORY)
	{
		nv_pending_action_context_t *oldest = NULL;
		nv_pending_action_context_t *oldest_previous = NULL;
		nv_pending_action_context_t *previous = NULL;
		for(nv_pending_action_context_t *context = *contexts;
				context != NULL; context = context->next)
		{
			if(context->retry_available)
			{
				oldest = context;
				oldest_previous = previous;
			}
			previous = context;
		}
		if(oldest == NULL) break;
		if(oldest_previous == NULL) *contexts = oldest->next;
		else oldest_previous->next = oldest->next;
		--*retry_history_count;
		pending_action_context_free(oldest);
	}
}

static int
apply_undo_command(nv_workspace_session_t *session, nv_action_queue_t *queue,
		nv_snapshot_error_t *error, nv_undo_bridge_location_t *location)
{
	if(queue == NULL)
		return set_error(error, "undo-unavailable",
				"file undo is unavailable on this platform");
	if(nv_action_queue_busy(queue) != 0)
		return set_error(error, "undo-busy",
				"wait for queued file actions to finish");
	switch(nv_undo_bridge_undo(location))
	{
		case NV_UNDO_BRIDGE_SUCCESS:
			break;
		case NV_UNDO_BRIDGE_NONE:
			return set_error(error, "undo-empty",
					"no supported file action can be undone");
		case NV_UNDO_BRIDGE_FAILED:
		default:
			return set_error(error, "undo-failed",
					"the last supported file action could not be undone");
	}
	if(location == NULL || location->tab_id == 0U || location->pane > NV_SESSION_RIGHT)
		return set_error(error, "undo-location-invalid",
				"undo target location is invalid");
	if(refresh_action_tab(session, (nv_session_pane_t)location->pane,
			location->tab_id, error) != 0)
		return -1;
	if(!location->has_destination) return 0;
	if(location->destination_tab_id == 0U ||
			location->destination_pane > NV_SESSION_RIGHT)
		return set_error(error, "undo-location-invalid",
				"undo destination location is invalid");
	return refresh_action_tab(session,
			(nv_session_pane_t)location->destination_pane,
			location->destination_tab_id, error);
}

static int
submit_archive_mount(const nv_workspace_session_t *session,
		nv_resource_task_queue_t *queue, unsigned int command_sequence,
		nv_pending_resource_context_t **pending_resources)
{
	if(session == NULL || queue == NULL || pending_resources == NULL ||
			command_sequence == 0U)
	{
		errno = EINVAL;
		return -1;
	}
	const nv_session_pane_t pane = session->active_pane;
	const nv_pane_snapshot_t *const snapshot = nv_workspace_session_active(session);
	if(snapshot == NULL || snapshot->cursor < 0 ||
			snapshot->entries[snapshot->cursor].resource_kind != NV_ENTRY_RESOURCE_ARCHIVE)
	{
		errno = EINVAL;
		return -1;
	}
	const size_t tab_index = nv_workspace_session_active_tab_index(session, pane);
	const uint64_t tab_id = nv_workspace_session_tab_id(session, pane, tab_index);
	const nv_session_resource_t *const resource =
		nv_workspace_session_tab_resource(session, pane, tab_index);
	if(tab_id == 0U || (resource != NULL && resource->active))
	{
		errno = EBUSY;
		return -1;
	}
	char *const source_path = open_hex_decode(
			snapshot->entries[snapshot->cursor].path_bytes_hex);
	char *const origin_directory = open_hex_decode(snapshot->cwd_bytes_hex);
	nv_pending_resource_context_t *const context = calloc(1U, sizeof(*context));
	if(source_path == NULL || origin_directory == NULL || context == NULL)
	{
		free(source_path);
		free(origin_directory);
		free(context);
		errno = ENOMEM;
		return -1;
	}
	context->kind = NV_RESOURCE_TASK_MOUNT_ARCHIVE;
	context->pane = pane;
	context->tab_id = tab_id;
	context->origin_directory = origin_directory;
	context->origin_cwd_bytes_hex = strdup(snapshot->cwd_bytes_hex);
	context->origin_entry_path_bytes_hex = strdup(
			snapshot->entries[snapshot->cursor].path_bytes_hex);
	if(context->origin_cwd_bytes_hex == NULL ||
			context->origin_entry_path_bytes_hex == NULL)
	{
		free(source_path);
		pending_resource_context_free(context);
		errno = ENOMEM;
		return -1;
	}
	const nv_resource_task_request_t request = {
		.kind = NV_RESOURCE_TASK_MOUNT_ARCHIVE,
		.pane = (unsigned int)pane,
		.tab_id = tab_id,
		.command_sequence = command_sequence,
		.source_path = source_path,
	};
	uint64_t task_id = 0U;
	const int result = nv_resource_task_queue_submit(queue, &request, &task_id);
	free(source_path);
	if(result != 0)
	{
		pending_resource_context_free(context);
		return -1;
	}
	context->task_id = task_id;
	context->next = *pending_resources;
	*pending_resources = context;
	return 0;
}

static int
submit_ssh_mount(const nv_workspace_session_t *session,
		const nv_session_command_t *command, nv_resource_task_queue_t *queue,
		unsigned int command_sequence,
		nv_pending_resource_context_t **pending_resources)
{
	if(session == NULL || command == NULL || queue == NULL ||
			pending_resources == NULL ||
			(command->pane != NV_SESSION_LEFT && command->pane != NV_SESSION_RIGHT) ||
			command->resource_remote == NULL || command_sequence == 0U)
	{
		errno = EINVAL;
		return -1;
	}
	const size_t tab_index = nv_workspace_session_active_tab_index(session,
			command->pane);
	const uint64_t tab_id = nv_workspace_session_tab_id(session, command->pane,
			tab_index);
	const nv_session_resource_t *const resource =
		nv_workspace_session_tab_resource(session, command->pane, tab_index);
	const nv_pane_snapshot_t *const snapshot = command->pane == NV_SESSION_LEFT ?
		&session->left : &session->right;
	if(tab_id == 0U || (resource != NULL && resource->active) ||
			snapshot->cwd_bytes_hex == NULL)
	{
		errno = EBUSY;
		return -1;
	}
	char *const origin_directory = open_hex_decode(snapshot->cwd_bytes_hex);
	nv_pending_resource_context_t *const context = calloc(1U, sizeof(*context));
	if(origin_directory == NULL || context == NULL)
	{
		free(origin_directory);
		free(context);
		errno = ENOMEM;
		return -1;
	}
	context->kind = NV_RESOURCE_TASK_MOUNT_SSH;
	context->pane = command->pane;
	context->tab_id = tab_id;
	context->origin_directory = origin_directory;
	context->origin_cwd_bytes_hex = strdup(snapshot->cwd_bytes_hex);
	context->remote = strdup(command->resource_remote);
	if(context->origin_cwd_bytes_hex == NULL || context->remote == NULL)
	{
		pending_resource_context_free(context);
		errno = ENOMEM;
		return -1;
	}
	const nv_resource_task_request_t request = {
		.kind = NV_RESOURCE_TASK_MOUNT_SSH,
		.pane = (unsigned int)command->pane,
		.tab_id = tab_id,
		.command_sequence = command_sequence,
		.remote = command->resource_remote,
	};
	uint64_t task_id = 0U;
	if(nv_resource_task_queue_submit(queue, &request, &task_id) != 0)
	{
		pending_resource_context_free(context);
		return -1;
	}
	context->task_id = task_id;
	context->next = *pending_resources;
	*pending_resources = context;
	return 0;
}

static int
submit_resource_unmount(const nv_workspace_session_t *session,
		nv_resource_task_queue_t *queue, unsigned int command_sequence,
	nv_pending_resource_context_t **pending_resources)
{
	if(session == NULL) { errno = EINVAL; return -1; }
	const nv_session_pane_t pane = session->active_pane;
	const size_t tab_index = nv_workspace_session_active_tab_index(session, pane);
	return submit_resource_unmount_at(session, pane, tab_index, queue,
			command_sequence, pending_resources);
}

static int
submit_resource_unmount_at(const nv_workspace_session_t *session,
		nv_session_pane_t pane, size_t tab_index,
		nv_resource_task_queue_t *queue, unsigned int command_sequence,
		nv_pending_resource_context_t **pending_resources)
{
	if(session == NULL || queue == NULL || pending_resources == NULL ||
			command_sequence == 0U || pane > NV_SESSION_RIGHT)
	{
		errno = EINVAL;
		return -1;
	}
	const uint64_t tab_id = nv_workspace_session_tab_id(session, pane, tab_index);
	const nv_session_resource_t *const resource =
		nv_workspace_session_tab_resource(session, pane, tab_index);
	if(tab_id == 0U || resource == NULL || !resource->active ||
			resource->mount_point == NULL || resource->unmount_path == NULL)
	{
		errno = ENOENT;
		return -1;
	}
	nv_pending_resource_context_t *const context = calloc(1U, sizeof(*context));
	if(context == NULL)
	{
		errno = ENOMEM;
		return -1;
	}
	context->kind = NV_RESOURCE_TASK_UNMOUNT;
	context->pane = pane;
	context->tab_id = tab_id;
	const nv_resource_task_request_t request = {
		.kind = NV_RESOURCE_TASK_UNMOUNT,
		.pane = (unsigned int)pane,
		.tab_id = tab_id,
		.command_sequence = command_sequence,
		.mount_point = resource->mount_point,
		.unmount_path = resource->unmount_path,
	};
	uint64_t task_id = 0U;
	if(nv_resource_task_queue_submit(queue, &request, &task_id) != 0)
	{
		pending_resource_context_free(context);
		return -1;
	}
	context->task_id = task_id;
	context->next = *pending_resources;
	*pending_resources = context;
	return 0;
}

static int
process_command_line(nv_workspace_session_t *session, char line[],
		size_t line_capacity, unsigned int *output_sequence,
		unsigned int *command_sequence, int *directory_changed,
		nv_session_watcher_t *watcher,
		nv_preview_queue_t *preview_queue, uint64_t *preview_generation,
		nv_action_queue_t *action_queue,
		nv_pending_action_context_t **pending_actions,
		size_t *retry_history_count,
		nv_resource_task_queue_t *resource_queue,
		nv_pending_resource_context_t **pending_resources)
{
	const size_t length = strlen(line);
	nv_session_command_t command = {};
	unsigned int next_sequence = *command_sequence + 1U;
	nv_snapshot_error_t error = {};
	const int had_cwd_stat[] = {
		session->left.has_cwd_stat, session->right.has_cwd_stat,
	};
	const uint64_t cwd_device[] = {
		session->left.cwd_device, session->right.cwd_device,
	};
	const uint64_t cwd_inode[] = {
		session->left.cwd_inode, session->right.cwd_inode,
	};
	if(directory_changed != NULL) *directory_changed = 0;
	if(length == line_capacity - 1U && line[length - 1U] != '\n')
	{
		discard_command_tail();
		set_error(&error, "command-too-large", "command exceeds input limit");
	}
	else if(parse_command(line, *command_sequence, &next_sequence, &command) != 0)
	{
		set_error(&error, "invalid-command", "invalid session command");
	}
	else
	{
		if(command.kind == NV_SESSION_PREVIEW)
		{
			nv_entry_kind_t kind = NV_ENTRY_UNKNOWN;
			const char *entry_name = NULL;
			if(validate_preview_identity(session, &command, &kind, &entry_name,
					&error) == 0)
			{
				*command_sequence = next_sequence;
				const int result = write_workspace(session,
						(*output_sequence)++, *command_sequence, "command");
				if(result == 0 && submit_requested_preview(session, &command,
						kind, entry_name, preview_queue, preview_generation) != 0)
				{
					fputs("neovifm-core-session: failed to queue requested preview\n",
						stderr);
				}
				nv_snapshot_error_free(&error);
				nv_session_command_free(&command);
				return result;
			}
		}
		else if(command.kind == NV_SESSION_OPEN)
		{
			char *const path = open_hex_decode(command.open_path_bytes_hex);
			nv_open_resolution_t resolution = {};
			nv_open_error_t open_error = {};
			const int identity_valid = validate_open_identity(session, &command,
				&error);
			const int resolved = path == NULL || identity_valid != 0 ? -1 : nv_open_resolve(
					command.open_intent, path,
					(const char *const *)command.open_association_argv,
					command.open_association_argc, &resolution, &open_error);
			if(resolved == 0)
			{
				*command_sequence = next_sequence;
				char *const open = nv_protocol_open_json(&resolution,
						command.open_path_bytes_hex, (*output_sequence)++,
						*command_sequence);
				const int result = open == NULL || write_line(open) != 0 ? -1 : 0;
				nv_protocol_json_free(open);
				nv_open_resolution_free(&resolution);
				nv_open_error_free(&open_error);
				free(path);
				nv_session_command_free(&command);
				return result;
			}
			if(identity_valid == 0)
			{
				set_error(&error, open_error.code == NULL ? "open-failed" :
						open_error.code, open_error.message == NULL ?
						"failed to resolve open intent" : open_error.message);
			}
			nv_open_resolution_free(&resolution);
			nv_open_error_free(&open_error);
			free(path);
		}
		else if(command.kind == NV_SESSION_ENTER && active_entry_is_archive(session))
		{
			const nv_pane_snapshot_t *const active =
				nv_workspace_session_active(session);
			const int archive = active != NULL && active->cursor >= 0 &&
				active->entries[active->cursor].resource_kind == NV_ENTRY_RESOURCE_ARCHIVE;
			if(archive)
			{
				if(resource_queue == NULL || pending_resources == NULL)
				{
					set_error(&error, "unsupported-resource",
							"resource mounting is unavailable on this platform");
				}
				else if(submit_archive_mount(session, resource_queue, next_sequence,
						pending_resources) == 0)
				{
					*command_sequence = next_sequence;
					const int result = write_workspace(session, (*output_sequence)++,
							*command_sequence, "command");
					nv_snapshot_error_free(&error);
					nv_session_command_free(&command);
					return result;
				}
				else
				{
					const int submit_error = errno;
					set_error(&error, submit_error == EBUSY ? "resource-queue-full" :
							submit_error == EINVAL ? "invalid-resource" :
							"resource-queue-failed", submit_error == EBUSY ?
							"resource task queue is full" : "failed to queue resource mount");
					error.os_error = submit_error;
					error.retryable = submit_error == EBUSY;
				}
			}
		}
		else if(command.kind == NV_SESSION_MOUNT_SSH)
		{
			if(resource_queue == NULL || pending_resources == NULL)
			{
				set_error(&error, "unsupported-resource",
						"resource mounting is unavailable on this platform");
			}
			else if(submit_ssh_mount(session, &command, resource_queue, next_sequence,
					pending_resources) == 0)
			{
				*command_sequence = next_sequence;
				const int result = write_workspace(session, (*output_sequence)++,
						*command_sequence, "command");
				nv_snapshot_error_free(&error);
				nv_session_command_free(&command);
				return result;
			}
			else
			{
				const int submit_error = errno;
				set_error(&error, submit_error == EBUSY ? "resource-queue-full" :
					submit_error == EINVAL ? "invalid-resource" :
					"resource-queue-failed", submit_error == EBUSY ?
					"resource task queue is full" : "failed to queue ssh mount");
				error.os_error = submit_error;
				error.retryable = submit_error == EBUSY;
			}
		}
		else if(command.kind == NV_SESSION_PARENT && active_tab_has_resource(session))
		{
			const nv_session_pane_t pane = session->active_pane;
			const size_t tab_index = nv_workspace_session_active_tab_index(session, pane);
			const nv_session_resource_t *const resource =
				nv_workspace_session_tab_resource(session, pane, tab_index);
			if(resource != NULL && resource->active)
			{
				if(resource_queue == NULL || pending_resources == NULL)
				{
					set_error(&error, "unsupported-resource",
							"resource unmounting is unavailable on this platform");
				}
				else if(submit_resource_unmount(session, resource_queue, next_sequence,
						pending_resources) == 0)
				{
					*command_sequence = next_sequence;
					const int result = write_workspace(session, (*output_sequence)++,
						*command_sequence, "command");
					nv_snapshot_error_free(&error);
					nv_session_command_free(&command);
					return result;
				}
				else
				{
					const int submit_error = errno;
					set_error(&error, submit_error == EBUSY ? "resource-queue-full" :
						submit_error == ENOENT ? "resource-not-mounted" :
						"resource-queue-failed", submit_error == EBUSY ?
						"resource task queue is full" : "failed to queue resource unmount");
					error.os_error = submit_error;
					error.retryable = submit_error == EBUSY;
				}
			}
		}
		else if(command.kind == NV_SESSION_CANCEL_ACTION)
		{
			if(action_queue == NULL || nv_action_queue_cancel(action_queue,
					command.action_task_id) != 0)
			{
				set_error(&error, action_queue == NULL ? "unsupported-action" :
						errno == ENOENT ? "action-not-found" : "action-cancel-failed",
						action_queue == NULL ? "file actions are unavailable on this platform" :
						errno == ENOENT ? "file action is no longer queued" :
						"failed to cancel file action");
			}
			else
			{
				*command_sequence = next_sequence;
				const int result = write_workspace(session, (*output_sequence)++,
						*command_sequence, "command");
				nv_snapshot_error_free(&error);
				nv_session_command_free(&command);
				return result;
			}
		}
		else if(command.kind == NV_SESSION_CANCEL_RESOURCE)
		{
			if(resource_queue == NULL || nv_resource_task_queue_cancel(resource_queue,
					command.resource_task_id) != 0)
			{
				set_error(&error, resource_queue == NULL ? "unsupported-resource" :
					errno == ENOENT ? "resource-task-not-found" :
					"resource-cancel-failed", resource_queue == NULL ?
					"resource tasks are unavailable on this platform" :
					errno == ENOENT ? "resource task is no longer queued" :
					"failed to cancel resource task");
			}
			else
			{
				*command_sequence = next_sequence;
				const int result = write_workspace(session, (*output_sequence)++,
					*command_sequence, "command");
				nv_snapshot_error_free(&error);
				nv_session_command_free(&command);
				return result;
			}
		}
		else if(command.kind == NV_SESSION_RETRY_ACTION)
		{
			nv_pending_action_context_t *const context =
				find_retry_context(pending_actions == NULL ? NULL : *pending_actions,
					command.action_task_id);
			if(action_queue == NULL)
			{
				set_error(&error, "unsupported-action",
						"file actions are unavailable on this platform");
			}
			else if(context == NULL)
			{
				set_error(&error, "retry-unavailable",
						"the task no longer has a safe retry identity");
			}
			else
			{
				uint64_t task_id = 0U;
				if(nv_action_queue_submit(action_queue, &context->retry_action,
						next_sequence, &task_id) == 0)
				{
					context->task_id = task_id;
					context->active = 1;
					context->retry_available = 0;
					if(retry_history_count != NULL && *retry_history_count != 0U)
						--*retry_history_count;
					*command_sequence = next_sequence;
					const int result = write_workspace(session,
							(*output_sequence)++, *command_sequence, "command");
					nv_snapshot_error_free(&error);
					nv_session_command_free(&command);
					return result;
				}
				const int submit_error = errno;
				set_error(&error, submit_error == EBUSY ? "action-queue-full" :
						"retry-failed", submit_error == EBUSY ?
						"file action queue is full" :
						"failed to queue the retry");
				error.os_error = submit_error;
				error.retryable = submit_error == EBUSY;
			}
		}
		else if(command_is_action(command.kind))
		{
			nv_session_prepared_action_t action = {};
			if(action_queue == NULL)
			{
				set_error(&error, "unsupported-action",
						"file actions are unavailable on this platform");
			}
			else if(nv_workspace_session_prepare_action(session, &command, &action,
					&error) == 0)
			{
				nv_pending_action_context_t *const context =
					calloc(1U, sizeof(*context));
				uint64_t task_id = 0U;
				if(context == NULL)
				{
					set_error(&error, "action-queue-failed",
							"failed to allocate action context");
					error.os_error = ENOMEM;
					error.retryable = 1;
				}
				else if(pending_action_context_prepare(context, session, &command,
					&action) != 0)
				{
					pending_action_context_free(context);
					set_error(&error, "action-queue-failed",
							"failed to prepare action undo context");
					error.os_error = ENOMEM;
					error.retryable = 1;
				}
				else if(nv_action_queue_submit(action_queue, &action, next_sequence,
						&task_id) == 0)
				{
					context->task_id = task_id;
					context->next = *pending_actions;
					*pending_actions = context;
					*command_sequence = next_sequence;
					const int result = write_workspace(session,
							(*output_sequence)++, *command_sequence, "command");
					nv_session_prepared_action_free(&action);
					nv_snapshot_error_free(&error);
					nv_session_command_free(&command);
					return result;
				}
				else
				{
					const int submit_error = errno;
					pending_action_context_free(context);
					set_error(&error, submit_error == EBUSY ? "action-queue-full" :
							"action-queue-failed", submit_error == EBUSY ?
							"file action queue is full" :
							"failed to queue file action");
					error.os_error = submit_error;
					error.retryable = submit_error == EBUSY;
				}
			}
			nv_session_prepared_action_free(&action);
		}
		else if(command.kind == NV_SESSION_UNDO)
		{
			nv_undo_bridge_location_t location = {};
			if(apply_undo_command(session, action_queue, &error, &location) == 0)
			{
				*command_sequence = next_sequence;
				nv_session_watcher_reset(watcher);
				const int result = write_workspace(session, (*output_sequence)++,
						*command_sequence, "command");
				if(result == 0 && submit_active_preview(session, preview_queue,
						preview_generation) != 0)
				{
					fputs("neovifm-core-session: failed to queue undo preview\n", stderr);
				}
				nv_snapshot_error_free(&error);
				nv_session_command_free(&command);
				return result;
			}
			*command_sequence = next_sequence;
		}
		else if(nv_workspace_session_apply(session, &command, &error) == 0)
		{
			*command_sequence = next_sequence;
			sync_watcher_bindings(watcher, session);
			if(directory_changed != NULL)
			{
				for(nv_session_pane_t pane = NV_SESSION_LEFT;
						pane <= NV_SESSION_RIGHT; ++pane)
				{
					const nv_pane_snapshot_t *const snapshot = pane == NV_SESSION_LEFT ?
						&session->left : &session->right;
					if(snapshot->has_cwd_stat != had_cwd_stat[pane] ||
							(snapshot->has_cwd_stat &&
							 (snapshot->cwd_device != cwd_device[pane] ||
							  snapshot->cwd_inode != cwd_inode[pane])))
					{
						*directory_changed |= 1 << pane;
					}
				}
			}
			const int result = write_workspace(session, (*output_sequence)++,
					*command_sequence, "command");
			if(result == 0 && submit_active_preview(session, preview_queue,
					preview_generation) != 0)
			{
				fputs("neovifm-core-session: failed to queue preview\n", stderr);
			}
			nv_snapshot_error_free(&error);
			nv_session_command_free(&command);
			return result;
		}
		/* A recoverable command error is still acknowledged, so subsequent watch
		 * snapshots must use this command sequence instead of becoming stale. */
		*command_sequence = next_sequence;
	}
	/* command-error is an acknowledgement too: keep later watch snapshots at
	 * the record sequence visible to the client, even for malformed input. */
	*command_sequence = next_sequence;
	const int result = write_command_error(&error, (*output_sequence)++,
			next_sequence);
	nv_snapshot_error_free(&error);
	nv_session_command_free(&command);
	return result;
}

static int
active_entry_is_archive(const nv_workspace_session_t *session)
{
	if(session == NULL) return 0;
	const nv_pane_snapshot_t *const active = nv_workspace_session_active(session);
	return active != NULL && active->cursor >= 0 &&
		active->entries[active->cursor].resource_kind == NV_ENTRY_RESOURCE_ARCHIVE;
}

static int
active_tab_has_resource(const nv_workspace_session_t *session)
{
	if(session == NULL) return 0;
	const nv_session_pane_t pane = session->active_pane;
	const size_t tab_index = nv_workspace_session_active_tab_index(session, pane);
	const nv_session_resource_t *const resource =
		nv_workspace_session_tab_resource(session, pane, tab_index);
	return resource != NULL && resource->active;
}

static void
resolve_preview_association(const char path[], nv_open_resolution_t *resolution)
{
	if(resolution == NULL) return;
	*resolution = (nv_open_resolution_t){};
	const char *const config_path = getenv("MYVIFMRC");
	if(config_path == NULL || config_path[0] == '\0') return;
	nv_open_config_t config = {};
	nv_open_error_t error = {};
	if(nv_open_config_load_env(&config, &error) != 0)
	{
		fprintf(stderr, "neovifm-core-session: preview config ignored: %s\n",
			error.message == NULL ? "failed to load MYVIFMRC" : error.message);
		nv_open_error_free(&error);
		return;
	}
	if(config.previewprg != NULL)
	{
		const nv_open_association_rule_t preview_rule = {
			.kind = NV_OPEN_ASSOC_FILEVIEWER,
			.pattern = "*",
			.command = config.previewprg,
		};
		if(nv_open_resolve_rules(NV_OPEN_INTENT_PREVIEW, path,
				&preview_rule, 1U, resolution, &error) == 0)
		{
			nv_open_error_free(&error);
			nv_open_config_free(&config);
			return;
		}
		/* An invalid previewprg must not hide a usable fileviewer rule. */
		nv_open_resolution_free(resolution);
		nv_open_error_free(&error);
	}
	if(nv_open_resolve_rules(NV_OPEN_INTENT_PREVIEW, path, config.rules,
			config.rule_count, resolution, &error) != 0 &&
			(error.code == NULL || strcmp(error.code, "no-association") != 0))
	{
		fprintf(stderr, "neovifm-core-session: preview association ignored: %s\n",
			error.message == NULL ? "failed to resolve fileviewer" : error.message);
		nv_open_resolution_free(resolution);
	}
	nv_open_error_free(&error);
	nv_open_config_free(&config);
}

static int
submit_preview_request(nv_preview_queue_t *queue, uint64_t *generation,
		nv_session_pane_t source_pane, nv_session_pane_t target_pane,
		const char cwd_bytes_hex[], const char path_bytes_hex[],
		nv_preview_kind_t kind)
{
	if(queue == NULL || generation == NULL || cwd_bytes_hex == NULL ||
			path_bytes_hex == NULL) return -1;
	char *const path = open_hex_decode(path_bytes_hex);
	nv_open_resolution_t resolution = {};
	if(path != NULL) resolve_preview_association(path, &resolution);
	const nv_preview_request_t request = {
		.pane = source_pane == NV_SESSION_LEFT ? NV_PREVIEW_PANE_LEFT :
			NV_PREVIEW_PANE_RIGHT,
		.target_pane = target_pane == NV_SESSION_LEFT ? NV_PREVIEW_PANE_LEFT :
			NV_PREVIEW_PANE_RIGHT,
		.has_target_pane = 1,
		.generation = ++*generation,
		.cwd_bytes_hex = cwd_bytes_hex,
		.path_bytes_hex = path_bytes_hex,
		.kind = kind,
		.max_bytes = NV_PREVIEW_MAX_BYTES,
		.timeout_ms = kind == NV_PREVIEW_KIND_PDF ? NV_PREVIEW_PDF_TIMEOUT_MS :
			(kind == NV_PREVIEW_KIND_IMAGE || kind == NV_PREVIEW_KIND_AUDIO || kind == NV_PREVIEW_KIND_VIDEO) ?
			NV_PREVIEW_MEDIA_TIMEOUT_MS : NV_PREVIEW_DEFAULT_TIMEOUT_MS,
		.viewer_argv = (const char *const *)resolution.argv,
		.viewer_argc = resolution.argc,
	};
	const int result = nv_preview_queue_submit(queue, &request, NULL);
	nv_open_resolution_free(&resolution);
	free(path);
	return result;
}

static int
submit_active_preview(const nv_workspace_session_t *session,
		nv_preview_queue_t *queue, uint64_t *generation)
{
	if(session == NULL || queue == NULL || generation == NULL) return -1;
	const nv_pane_snapshot_t *const snapshot = nv_workspace_session_active(session);
	if(snapshot == NULL || snapshot->cursor < 0) return 0;
	const nv_pane_entry_t *const entry = &snapshot->entries[snapshot->cursor];
	return submit_preview_request(queue, generation, session->active_pane,
		session->active_pane, snapshot->cwd_bytes_hex, entry->path_bytes_hex,
		preview_kind_for_entry(entry->kind, entry->name_display));
}

static int
validate_preview_identity(const nv_workspace_session_t *session,
		const nv_session_command_t *command, nv_entry_kind_t *kind,
		const char **entry_name,
		nv_snapshot_error_t *error)
{
	if(session == NULL || command == NULL || kind == NULL || entry_name == NULL ||
			error == NULL ||
			(command->pane != NV_SESSION_LEFT && command->pane != NV_SESSION_RIGHT) ||
			(command->preview_target_pane != NV_SESSION_LEFT &&
			 command->preview_target_pane != NV_SESSION_RIGHT))
	{
		return set_error(error, "invalid-command", "invalid preview pane");
	}
	const nv_pane_snapshot_t *const snapshot = command->pane == NV_SESSION_LEFT ?
		&session->left : &session->right;
	if(snapshot->snapshot_revision != command->preview_snapshot_revision ||
			strcmp(snapshot->cwd_bytes_hex, command->preview_cwd_bytes_hex) != 0 ||
			!snapshot->has_cwd_stat || snapshot->cwd_device != command->preview_cwd_device ||
			snapshot->cwd_inode != command->preview_cwd_inode ||
			snapshot->cwd_ctime_unix_ns != command->preview_cwd_ctime_unix_ns)
	{
		return set_error(error, "stale-preview", "source pane changed before preview");
	}
	for(size_t i = 0U; i < snapshot->entry_count; ++i)
	{
		const nv_pane_entry_t *const entry = &snapshot->entries[i];
		if(strcmp(entry->path_bytes_hex, command->preview_path_bytes_hex) != 0)
			continue;
		if(!entry->has_stat || entry->device != command->preview_entry_device ||
				entry->inode != command->preview_entry_inode ||
				entry->ctime_unix_ns != command->preview_entry_ctime_unix_ns)
		{
			return set_error(error, "stale-preview", "preview entry changed");
		}
		*kind = entry->kind;
		*entry_name = entry->name_display;
		return 0;
	}
	return set_error(error, "stale-preview", "preview entry no longer exists");
}

static int
validate_open_identity(const nv_workspace_session_t *session,
		const nv_session_command_t *command, nv_snapshot_error_t *error)
{
	if(session == NULL || command == NULL || error == NULL ||
			(command->open_pane != NV_SESSION_LEFT &&
				command->open_pane != NV_SESSION_RIGHT))
	{
		return set_error(error, "invalid-command", "invalid open pane");
	}
	const nv_pane_snapshot_t *const snapshot = command->open_pane == NV_SESSION_LEFT ?
		&session->left : &session->right;
	if(snapshot->snapshot_revision != command->open_snapshot_revision ||
			strcmp(snapshot->cwd_bytes_hex, command->open_cwd_bytes_hex) != 0 ||
			!snapshot->has_cwd_stat || snapshot->cwd_device != command->open_cwd_device ||
			snapshot->cwd_inode != command->open_cwd_inode ||
			snapshot->cwd_ctime_unix_ns != command->open_cwd_ctime_unix_ns)
	{
		return set_error(error, "stale-open", "source pane changed before open");
	}
	for(size_t i = 0U; i < snapshot->entry_count; ++i)
	{
		const nv_pane_entry_t *const entry = &snapshot->entries[i];
		if(strcmp(entry->path_bytes_hex, command->open_path_bytes_hex) != 0)
			continue;
		if(!entry->has_stat || entry->device != command->open_entry_device ||
				entry->inode != command->open_entry_inode ||
				entry->ctime_unix_ns != command->open_entry_ctime_unix_ns)
		{
			return set_error(error, "stale-open", "open entry changed");
		}
		if(entry->resource_kind == NV_ENTRY_RESOURCE_ARCHIVE)
		{
			return set_error(error, "enter-required", "archive resources must be entered");
		}
		if(entry->kind == NV_ENTRY_DIRECTORY)
		{
			return set_error(error, "enter-required", "directories must be entered");
		}
		return 0;
	}
	return set_error(error, "stale-open", "open entry no longer exists");
}

static int
submit_requested_preview(const nv_workspace_session_t *session,
		const nv_session_command_t *command, nv_entry_kind_t kind,
		const char entry_name[],
		nv_preview_queue_t *queue, uint64_t *generation)
{
	if(session == NULL || command == NULL || queue == NULL || generation == NULL)
		return -1;
	return submit_preview_request(queue, generation, command->pane,
		command->preview_target_pane, command->preview_cwd_bytes_hex,
		command->preview_path_bytes_hex, preview_kind_for_entry(kind, entry_name));
}

static int
has_suffix_ci(const char name[], const char suffix[])
{
	if(name == NULL || suffix == NULL) return 0;
	const size_t name_length = strlen(name);
	const size_t suffix_length = strlen(suffix);
	if(name_length < suffix_length) return 0;
	for(size_t i = 0U; i < suffix_length; ++i)
	{
		const unsigned char left = (unsigned char)name[name_length - suffix_length + i];
		const unsigned char right = (unsigned char)suffix[i];
		if(tolower(left) != tolower(right)) return 0;
	}
	return 1;
}

static nv_preview_kind_t
preview_kind_for_entry(nv_entry_kind_t kind, const char name[])
{
	if(kind == NV_ENTRY_DIRECTORY) return NV_PREVIEW_KIND_DIRECTORY;
	if(kind == NV_ENTRY_FILE || kind == NV_ENTRY_EXECUTABLE)
	{
		static const char *const archive_suffixes[] = {
			".zip", ".tar", ".tgz", ".tar.gz", ".tbz", ".tbz2",
			".tar.bz2", ".txz", ".tar.xz", ".7z", ".rar", ".jar",
		};
		for(size_t i = 0U; i < sizeof(archive_suffixes)/sizeof(archive_suffixes[0]); ++i)
		{
			if(has_suffix_ci(name, archive_suffixes[i])) return NV_PREVIEW_KIND_ARCHIVE;
		}
		static const char *const binary_suffixes[] = {
			".bin", ".dat", ".o", ".a", ".so", ".dylib", ".dll", ".exe",
			".class", ".wasm", ".pyc", ".pyo",
		};
		for(size_t i = 0U; i < sizeof(binary_suffixes)/sizeof(binary_suffixes[0]); ++i)
		{
			if(has_suffix_ci(name, binary_suffixes[i])) return NV_PREVIEW_KIND_BINARY;
		}
		static const char *const image_suffixes[] = {
			".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".tif", ".tiff", ".svg",
		};
		for(size_t i = 0U; i < sizeof(image_suffixes)/sizeof(image_suffixes[0]); ++i)
		{
			if(has_suffix_ci(name, image_suffixes[i])) return NV_PREVIEW_KIND_IMAGE;
		}
		static const char *const audio_suffixes[] = {
			".mp3", ".m4a", ".aac", ".flac", ".wav", ".ogg", ".opus", ".aiff", ".ape",
		};
		for(size_t i = 0U; i < sizeof(audio_suffixes)/sizeof(audio_suffixes[0]); ++i)
		{
			if(has_suffix_ci(name, audio_suffixes[i])) return NV_PREVIEW_KIND_AUDIO;
		}
		static const char *const video_suffixes[] = {
			".mp4", ".m4v", ".mkv", ".mov", ".webm", ".avi", ".ts", ".mpeg", ".mpg",
		};
		for(size_t i = 0U; i < sizeof(video_suffixes)/sizeof(video_suffixes[0]); ++i)
		{
			if(has_suffix_ci(name, video_suffixes[i])) return NV_PREVIEW_KIND_VIDEO;
		}
		static const char *const markdown_suffixes[] = {
			".md", ".markdown", ".mdown", ".mkdn", ".mdwn",
		};
		for(size_t i = 0U; i < sizeof(markdown_suffixes)/sizeof(markdown_suffixes[0]); ++i)
		{
			if(has_suffix_ci(name, markdown_suffixes[i])) return NV_PREVIEW_KIND_MARKDOWN;
		}
		if(has_suffix_ci(name, ".pdf")) return NV_PREVIEW_KIND_PDF;
	}
	return NV_PREVIEW_KIND_TEXT;
}

static int
drain_preview_events(nv_preview_queue_t *queue, unsigned int *output_sequence)
{
	for(;;)
	{
		nv_preview_event_t event = {};
		const int popped = nv_preview_queue_pop(queue, &event);
		if(popped < 0) return -1;
		if(popped == 0) return 0;
		char *const task = nv_protocol_preview_task_json(&event,
				(*output_sequence)++);
		char *const preview = (event.state == NV_PREVIEW_TASK_DONE ||
			event.state == NV_PREVIEW_TASK_FAILED ||
			event.state == NV_PREVIEW_TASK_CANCELLED) ?
			nv_protocol_preview_json(&event, (*output_sequence)++) : NULL;
		const int result = task == NULL || write_line(task) != 0 ||
			(preview != NULL && write_line(preview) != 0) ? -1 : 0;
		nv_protocol_json_free(task);
		nv_protocol_json_free(preview);
		nv_preview_event_free(&event);
		if(result != 0) return result;
	}
}

static int
action_terminal(nv_action_task_state_t state)
{
	return state == NV_ACTION_TASK_DONE || state == NV_ACTION_TASK_FAILED ||
		state == NV_ACTION_TASK_CANCELLED;
}

static int
refresh_action_tab(nv_workspace_session_t *session, nv_session_pane_t pane,
		uint64_t tab_id, nv_snapshot_error_t *error)
{
	if(nv_workspace_session_refresh_tab(session, pane, tab_id, error) == 0)
		return 0;
	if(error->code != NULL && strcmp(error->code, "invalid-tab") == 0)
	{
		nv_snapshot_error_free(error);
		return 0;
	}
	return -1;
}

static int
drain_action_events(nv_action_queue_t *queue,
		nv_workspace_session_t *session, unsigned int *output_sequence,
		unsigned int command_sequence, nv_preview_queue_t *preview_queue,
		uint64_t *preview_generation,
		nv_pending_action_context_t **pending_actions,
		size_t *retry_history_count, nv_session_watcher_t *watcher)
{
	if(queue == NULL) return 0;
	if(nv_action_queue_failed(queue))
	{
		fputs("neovifm-core-session: failed to publish action terminal event\n",
				stderr);
		return -1;
	}
	for(;;)
	{
		nv_action_event_t event = {};
		const int popped = nv_action_queue_pop(queue, &event);
		if(popped < 0) return -1;
		if(popped == 0) return 0;
		int refresh_failed = 0;
		if(action_terminal(event.state))
		{
			nv_snapshot_error_t error = {};
			int undo_recorded = 0;
			nv_session_prepared_action_t retry_action = {};
			nv_pending_action_context_t *previous = NULL;
			nv_pending_action_context_t *context = pending_actions == NULL ? NULL :
				*pending_actions;
			while(context != NULL && context->task_id != event.task_id)
			{
				previous = context;
				context = context->next;
			}
			if(context != NULL && context->active)
			{
				const int retry_candidate = event.retryable &&
					(event.state == NV_ACTION_TASK_FAILED ||
					 event.state == NV_ACTION_TASK_CANCELLED);
				const int retain_retry = retry_candidate &&
					nv_action_queue_take_terminal_action(queue, event.task_id,
						&retry_action) == 0;
				if(retain_retry)
				{
					context->retry_action = retry_action;
					context->retry_available = 1;
					context->active = 0;
					if(retry_history_count != NULL) ++*retry_history_count;
				}
				else
				{
					event.retryable = 0;
				}
				if(event.state == NV_ACTION_TASK_DONE && event.kind == NV_SESSION_MKDIR)
				{
					if(context->undo_path != NULL && nv_undo_bridge_record_mkdir(
							context->undo_path, context->undo_parent_identity,
							(nv_undo_bridge_location_t){
								.pane = (unsigned int)context->source_pane,
								.tab_id = context->source_tab_id,
							}) == 0)
						undo_recorded = 1;
					else
						fprintf(stderr,
							"neovifm-core-session: mkdir undo record failed: %s\n",
							strerror(errno == 0 ? ENOMEM : errno));
				}
				if(event.state == NV_ACTION_TASK_DONE &&
						(event.kind == NV_SESSION_COPY ||
						 event.kind == NV_SESSION_MOVE_FILES))
				{
					if(record_action_undo(context, event.state) == 0)
						undo_recorded = 1;
					else
						fprintf(stderr,
							"neovifm-core-session: transfer undo record failed: %s\n",
							strerror(errno == 0 ? EINVAL : errno));
				}
				event.undo_available = undo_recorded;
				refresh_failed = refresh_action_tab(session,
						context->source_pane, context->source_tab_id,
						&error) != 0;
				if(!refresh_failed && context->has_destination)
				{
					refresh_failed = refresh_action_tab(session,
							context->destination_pane, context->destination_tab_id,
						&error) != 0;
				}
				if(retain_retry)
					trim_retry_history(pending_actions, retry_history_count);
				if(!retain_retry)
				{
					if(previous == NULL) *pending_actions = context->next;
					else previous->next = context->next;
					pending_action_context_free(context);
				}
			}
			else
			{
				event.retryable = 0;
				refresh_failed = nv_workspace_session_refresh_pane(session,
						NV_SESSION_LEFT, &error) != 0 ||
					nv_workspace_session_refresh_pane(session, NV_SESSION_RIGHT,
							&error) != 0;
			}
			/* The action terminal snapshot already includes these filesystem
			 * changes.  Reopen watchers so queued native notifications cannot
			 * immediately invalidate the snapshot revision as a duplicate watch. */
			if(!refresh_failed) nv_session_watcher_reset(watcher);
			if(!refresh_failed && write_workspace(session, (*output_sequence)++,
					command_sequence, "action") != 0) refresh_failed = 1;
			if(refresh_failed)
			{
				fprintf(stderr, "neovifm-core-session: action refresh failed: %s\n",
						error.message == NULL ? "unknown error" : error.message);
			}
			nv_snapshot_error_free(&error);
			if(!refresh_failed && submit_active_preview(session, preview_queue,
					preview_generation) != 0)
			{
				fputs("neovifm-core-session: failed to queue action preview\n",
						stderr);
			}
		}
		char *const json = nv_protocol_action_task_json(&event,
				(*output_sequence)++);
		const int result = json == NULL || write_line(json) != 0 ? -1 : 0;
		nv_protocol_json_free(json);
		if(action_terminal(event.state))
			nv_action_queue_ack_terminal(queue, event.task_id);
		nv_action_event_free(&event);
		if(result != 0 || refresh_failed) return -1;
	}
}

static int
drain_resource_events(nv_resource_task_queue_t *queue,
		nv_workspace_session_t *session, unsigned int *output_sequence,
		nv_preview_queue_t *preview_queue, uint64_t *preview_generation,
		nv_pending_resource_context_t **pending_resources,
		nv_session_watcher_t *watcher)
{
	if(queue == NULL) return 0;
	if(nv_resource_task_queue_failed(queue))
	{
		fputs("neovifm-core-session: failed to publish resource terminal event\n",
				stderr);
		return -1;
	}
	for(;;)
	{
		nv_resource_task_event_t event = {};
		const int popped = nv_resource_task_queue_pop(queue, &event);
		if(popped < 0) return -1;
		if(popped == 0) return 0;
		int state_changed = 0;
		int refresh_failed = 0;
		if(event.state == NV_RESOURCE_TASK_DONE && pending_resources != NULL &&
				(event.kind == NV_RESOURCE_TASK_MOUNT_ARCHIVE ||
				 event.kind == NV_RESOURCE_TASK_MOUNT_SSH ||
				 event.kind == NV_RESOURCE_TASK_UNMOUNT))
		{
			nv_pending_resource_context_t *previous = NULL;
			nv_pending_resource_context_t *const context = find_resource_context(
					*pending_resources, event.task_id, &previous);
			if(context != NULL)
			{
				nv_snapshot_error_t error = {};
				if(context->kind == NV_RESOURCE_TASK_UNMOUNT)
				{
					if(nv_workspace_session_detach_resource(session, context->pane,
							context->tab_id, &error) == 0) state_changed = 1;
				}
				else
				{
					const nv_session_resource_kind_t kind = context->kind ==
						NV_RESOURCE_TASK_MOUNT_ARCHIVE ? NV_SESSION_RESOURCE_ARCHIVE :
						NV_SESSION_RESOURCE_SSH;
					if(event.mount_point != NULL && event.unmount_path != NULL &&
							nv_workspace_session_attach_resource(session, context->pane,
								context->tab_id, kind, context->origin_directory,
								context->origin_cwd_bytes_hex,
								context->origin_entry_path_bytes_hex, context->remote,
								event.mount_point, event.unmount_path, &error) == 0)
						state_changed = 1;
					else if(error.code == NULL)
						set_error(&error, "resource-mount-result-invalid",
							"resource task completed without mount ownership");
				}
				if(!state_changed)
				{
					fprintf(stderr, "neovifm-core-session: resource state update failed: %s\n",
							error.message == NULL ? "unknown error" : error.message);
					refresh_failed = 1;
				}
				nv_snapshot_error_free(&error);
				if(previous == NULL) *pending_resources = context->next;
				else previous->next = context->next;
				pending_resource_context_free(context);
			}
		}
		else if((event.state == NV_RESOURCE_TASK_FAILED ||
				event.state == NV_RESOURCE_TASK_CANCELLED) && pending_resources != NULL)
		{
			nv_pending_resource_context_t *previous = NULL;
			nv_pending_resource_context_t *const context = find_resource_context(
					*pending_resources, event.task_id, &previous);
			if(context != NULL)
			{
				if(previous == NULL) *pending_resources = context->next;
				else previous->next = context->next;
				pending_resource_context_free(context);
			}
		}
		if(state_changed) sync_watcher_bindings(watcher, session);
		if(state_changed && write_workspace(session, (*output_sequence)++,
				event.command_sequence, "resource") != 0)
			refresh_failed = 1;
		char *const json = nv_protocol_resource_task_json(&event,
				(*output_sequence)++);
		const int result = json == NULL || write_line(json) != 0 ? -1 : 0;
		nv_protocol_json_free(json);
		nv_resource_task_event_free(&event);
		if(!refresh_failed && state_changed && preview_queue != NULL &&
				preview_generation != NULL && submit_active_preview(session,
					preview_queue, preview_generation) != 0)
		{
			fputs("neovifm-core-session: failed to queue resource preview\n", stderr);
		}
		if(result != 0 || refresh_failed) return -1;
	}
}

static int
drain_resource_events_until_idle(nv_resource_task_queue_t *queue,
		nv_workspace_session_t *session, unsigned int *output_sequence,
		nv_preview_queue_t *preview_queue, uint64_t *preview_generation,
		nv_pending_resource_context_t **pending_resources)
{
	if(queue == NULL || pending_resources == NULL) return 0;
	for(;;)
	{
		if(drain_resource_events(queue, session, output_sequence, preview_queue,
				preview_generation, pending_resources, NULL) != 0)
			return -1;
		if(!nv_resource_task_queue_busy(queue) && *pending_resources == NULL)
			return 0;
	}
}

static int
submit_resource_cleanup(const nv_workspace_session_t *session,
		nv_resource_task_queue_t *queue, unsigned int command_sequence,
		nv_pending_resource_context_t **pending_resources)
{
	if(session == NULL || queue == NULL || pending_resources == NULL ||
			command_sequence == 0U)
		return -1;
	int result = 0;
	for(nv_session_pane_t pane = NV_SESSION_LEFT; pane <= NV_SESSION_RIGHT;
			++pane)
	{
		const size_t count = nv_workspace_session_tab_count(session, pane);
		for(size_t index = 0U; index < count; ++index)
		{
			const nv_session_resource_t *const resource =
				nv_workspace_session_tab_resource(session, pane, index);
			if(resource == NULL || !resource->active) continue;
			if(submit_resource_unmount_at(session, pane, index, queue,
					command_sequence, pending_resources) != 0)
			{
				fprintf(stderr,
					"neovifm-core-session: failed to queue resource cleanup for %s tab %zu\n",
					pane == NV_SESSION_LEFT ? "left" : "right", index);
				result = -1;
			}
		}
	}
	return result;
}

static char *
open_hex_decode(const char hex[])
{
	if(!open_hex_string_valid(hex)) return NULL;
	const size_t length = strlen(hex);
	char *const decoded = malloc(length/2U + 1U);
	if(decoded == NULL) return NULL;
	for(size_t i = 0U; i < length; i += 2U)
	{
		const char high_char = hex[i], low_char = hex[i + 1U];
		const int high = high_char <= '9' ? high_char - '0' : high_char - 'a' + 10;
		const int low = low_char <= '9' ? low_char - '0' : low_char - 'a' + 10;
		decoded[i/2U] = (char)((high << 4U) | low);
	}
	decoded[length/2U] = '\0';
	return decoded;
}

static void
sync_watcher_bindings(nv_session_watcher_t *watcher,
		const nv_workspace_session_t *session)
{
	unsigned int failed_panes = 0U;
	nv_session_watcher_sync(watcher, session, &failed_panes);
	for(nv_session_pane_t pane = NV_SESSION_LEFT; pane <= NV_SESSION_RIGHT; ++pane)
	{
		if((failed_panes & (1U << (unsigned int)pane)) == 0U) continue;
		fprintf(stderr, "neovifm-core-session: %s pane watcher disabled: %s\n",
			pane == NV_SESSION_LEFT ? "left" : "right", strerror(errno));
	}
}

static int
drain_watcher_events(nv_session_watcher_t *watcher,
		nv_workspace_session_t *session, unsigned int *output_sequence,
		unsigned int command_sequence, nv_preview_queue_t *preview_queue,
		uint64_t *preview_generation, nv_action_queue_t *action_queue)
{
	sync_watcher_bindings(watcher, session);
	if(action_queue != NULL && nv_action_queue_busy(action_queue)) return 0;
	unsigned int changed_panes = 0U;
	unsigned int failed_panes = 0U;
	nv_session_watcher_poll(watcher, &changed_panes, &failed_panes);
	for(nv_session_pane_t pane = NV_SESSION_LEFT; pane <= NV_SESSION_RIGHT; ++pane)
	{
		const unsigned int mask = 1U << (unsigned int)pane;
		if((failed_panes & mask) != 0U)
		{
			fprintf(stderr, "neovifm-core-session: %s pane watcher stopped\n",
				pane == NV_SESSION_LEFT ? "left" : "right");
		}
		if((changed_panes & mask) == 0U) continue;
		nv_snapshot_error_t error = {};
		if(nv_workspace_session_refresh_pane(session, pane, &error) != 0)
		{
			fprintf(stderr, "neovifm-core-session: %s pane watch refresh stopped: %s\n",
					pane == NV_SESSION_LEFT ? "left" : "right",
					error.message == NULL ? "unknown error" : error.message);
			nv_snapshot_error_free(&error);
			nv_session_watcher_disable(watcher, pane);
			changed_panes &= ~mask;
			continue;
		}
		nv_snapshot_error_free(&error);
	}
	if(changed_panes == 0U) return 0;
	sync_watcher_bindings(watcher, session);
	if(write_workspace(session, (*output_sequence)++, command_sequence, "watch") != 0)
		return -1;
	if(submit_active_preview(session, preview_queue, preview_generation) != 0)
		fputs("neovifm-core-session: failed to queue watch preview\n", stderr);
	return 0;
}

static int
core_main(int argc, char *argv[])
{
	if(argc != 3)
	{
		fputs("neovifm-core-session: expected left and right directory arguments\n", stderr);
		return 2;
	}
	/* Readiness checks happen before fgets().  Keep stdin unbuffered so a second
	 * command already present in the pipe is not hidden inside stdio after the
	 * descriptor is reported as drained. */
	(void)setvbuf(stdin, NULL, _IONBF, 0);
	nv_workspace_session_t session = {};
	nv_snapshot_error_t error = {};
	if(nv_workspace_session_init(argv[1], argv[2], &session, &error) != 0)
	{
		fputs(error.message == NULL ? "neovifm-core-session: failed to initialize\n" : error.message, stderr);
		fputc('\n', stderr);
		nv_snapshot_error_free(&error);
		return 1;
	}
	const int actions_supported = nv_fs_actions_supported();
	if(actions_supported && nv_undo_bridge_init() != 0)
	{
		fputs("neovifm-core-session: failed to initialize undo bridge\n", stderr);
		nv_workspace_session_free(&session);
		return 1;
	}
	nv_preview_queue_t *const preview_queue = nv_preview_queue_alloc();
	if(preview_queue == NULL)
	{
		fputs("neovifm-core-session: failed to initialize preview queue\n", stderr);
		nv_undo_bridge_reset();
		nv_workspace_session_free(&session);
		return 1;
	}
	nv_action_queue_t *action_queue = NULL;
	if(actions_supported) action_queue = nv_action_queue_alloc();
	if(actions_supported && action_queue == NULL)
	if(action_queue == NULL)
	{
		fputs("neovifm-core-session: failed to initialize action queue\n", stderr);
		nv_preview_queue_free(preview_queue);
		nv_undo_bridge_reset();
		nv_workspace_session_free(&session);
		return 1;
	}
	nv_resource_task_queue_t *const resource_queue = nv_resource_task_queue_alloc();
	if(resource_queue == NULL)
	{
		fputs("neovifm-core-session: failed to initialize resource task queue\n", stderr);
		nv_action_queue_free(action_queue);
		nv_preview_queue_free(preview_queue);
		nv_undo_bridge_reset();
		nv_workspace_session_free(&session);
		return 1;
	}
	const int persistence_enabled = getenv("NEOVIFM_SESSION_PERSIST") != NULL &&
		strcmp(getenv("NEOVIFM_SESSION_PERSIST"), "1") == 0;
	const int resume_requested = getenv("NEOVIFM_SESSION_RESUME") != NULL &&
		strcmp(getenv("NEOVIFM_SESSION_RESUME"), "1") == 0;
	char *const state_path = persistence_enabled ? nv_session_state_path() : NULL;
	if(resume_requested && state_path != NULL)
	{
		nv_snapshot_error_t restore_error = {};
		const int restored = nv_workspace_session_load_state(&session, state_path,
				&restore_error);
		if(restored < 0)
		{
			fprintf(stderr, "neovifm-core-session: session restore failed: %s\n",
				restore_error.message == NULL ? "unknown error" : restore_error.message);
		}
		nv_snapshot_error_free(&restore_error);
	}
	uint64_t preview_generation = 0U;
	char *const hello = nv_protocol_preview_session_hello_json(0U);
	if(write_line(hello) != 0 || write_workspace(&session, 1U, 0U, "initial") != 0)
	{
		nv_protocol_json_free(hello);
		free(state_path);
		nv_action_queue_free(action_queue);
		nv_resource_task_queue_free(resource_queue);
		nv_preview_queue_free(preview_queue);
		nv_undo_bridge_reset();
		nv_workspace_session_free(&session);
		return 1;
	}
	nv_protocol_json_free(hello);
	if(submit_active_preview(&session, preview_queue, &preview_generation) != 0)
	{
		fputs("neovifm-core-session: failed to queue initial preview\n", stderr);
	}

	char line[NV_SESSION_MAX_COMMAND_BYTES + 2U];
	unsigned int output_sequence = 2U;
	unsigned int command_sequence = 0U;
	nv_pending_action_context_t *pending_actions = NULL;
	nv_pending_resource_context_t *pending_resources = NULL;
	size_t retry_history_count = 0U;
	int result = 0;
	nv_session_watcher_t *const watcher = nv_session_watcher_alloc();
	if(watcher == NULL)
		fputs("neovifm-core-session: watcher unavailable; automatic refresh disabled\n",
			stderr);
	for(;;)
	{
		if(drain_preview_events(preview_queue, &output_sequence) != 0 ||
				drain_action_events(action_queue, &session, &output_sequence,
					command_sequence, preview_queue, &preview_generation,
					&pending_actions, &retry_history_count, watcher) != 0 ||
				drain_resource_events(resource_queue, &session, &output_sequence,
					preview_queue, &preview_generation, &pending_resources,
					watcher) != 0 ||
				drain_watcher_events(watcher, &session, &output_sequence,
					command_sequence, preview_queue, &preview_generation,
					action_queue) != 0)
		{
			result = 1;
			break;
		}
		int stdin_ready = 0;
		if(nv_session_poll_stdin(&stdin_ready) != 0)
		{
			result = 1;
			break;
		}
		if(!stdin_ready) continue;
		if(fgets(line, sizeof(line), stdin) == NULL) break;
		if(process_command_line(&session, line, sizeof(line), &output_sequence,
				&command_sequence, NULL, watcher, preview_queue, &preview_generation,
				action_queue, &pending_actions, &retry_history_count,
				resource_queue, &pending_resources) != 0)
		{
			result = 1;
			break;
		}
	}
	nv_session_watcher_free(watcher);
	nv_action_queue_cancel_all(action_queue);
	nv_resource_task_queue_cancel_all(resource_queue);
	if(drain_action_events(action_queue, &session, &output_sequence,
				command_sequence, preview_queue, &preview_generation,
				&pending_actions, &retry_history_count, NULL) != 0) result = 1;
	if(drain_preview_events(preview_queue, &output_sequence) != 0) result = 1;
	if(drain_resource_events_until_idle(resource_queue, &session,
			&output_sequence, preview_queue, &preview_generation,
			&pending_resources) != 0) result = 1;
	const unsigned int cleanup_sequence = command_sequence == UINT_MAX ?
		command_sequence : command_sequence + 1U;
	if(submit_resource_cleanup(&session, resource_queue, cleanup_sequence,
			&pending_resources) != 0) result = 1;
	if(drain_resource_events_until_idle(resource_queue, &session,
			&output_sequence, preview_queue, &preview_generation,
			&pending_resources) != 0) result = 1;
	if(persistence_enabled && state_path != NULL)
	{
		if(nv_session_ensure_parent_directory(state_path) != 0)
		{
			fputs("neovifm-core-session: unable to create session state directory\n",
				stderr);
		}
		else
		{
			nv_snapshot_error_t state_error = {};
			if(nv_workspace_session_save_state(&session, state_path, &state_error) != 0)
			{
				fprintf(stderr, "neovifm-core-session: session state save failed: %s\n",
					state_error.message == NULL ? "unknown error" : state_error.message);
			}
			nv_snapshot_error_free(&state_error);
		}
	}
	free(state_path);
	nv_snapshot_error_free(&error);
	while(pending_actions != NULL)
	{
		nv_pending_action_context_t *const next = pending_actions->next;
		pending_action_context_free(pending_actions);
		pending_actions = next;
	}
	while(pending_resources != NULL)
	{
		nv_pending_resource_context_t *const next = pending_resources->next;
		pending_resource_context_free(pending_resources);
		pending_resources = next;
	}
	nv_action_queue_free(action_queue);
	nv_resource_task_queue_free(resource_queue);
	nv_preview_queue_free(preview_queue);
	nv_undo_bridge_reset();
	nv_workspace_session_free(&session);
	return result != 0 || ferror(stdin) ? 1 : 0;
}

#ifdef _WIN32
int
wmain(int argc, wchar_t *wide_argv[])
{
	char **const argv = calloc((size_t)argc + 1U, sizeof(*argv));
	if(argv == NULL) return 1;
	for(int i = 0; i < argc; ++i)
	{
		argv[i] = utf8_from_utf16(wide_argv[i]);
		if(argv[i] == NULL)
		{
			while(i-- > 0) free(argv[i]);
			free(argv);
			return 1;
		}
	}
	const int result = core_main(argc, argv);
	for(int i = 0; i < argc; ++i) free(argv[i]);
	free(argv);
	return result;
}
#else
int
main(int argc, char *argv[])
{
	return core_main(argc, argv);
}
#endif
