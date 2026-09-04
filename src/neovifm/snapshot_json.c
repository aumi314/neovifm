/* vifm
 * Copyright (C) 2026 NeoVifm contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "snapshot_json.h"

#include <inttypes.h> /* PRId64 PRIu64 */
#include <stddef.h> /* NULL size_t */
#include <stdio.h> /* snprintf() */
#include <string.h> /* strcmp() */

#include "../utils/parson.h"
#include "../compat/neovifm_fs.h"

static nv_protocol_json_result_t serialize_payload(const char type[],
		unsigned int version, unsigned int sequence, JSON_Value *payload_value,
		char **serialized);
static JSON_Value *snapshot_payload(const nv_pane_snapshot_t *snapshot);
static JSON_Value *entry_value(const nv_pane_entry_t *entry);
static char *hello_json(unsigned int version, const char capability[],
             const char secondary_capability[], const char tertiary_capability[],
             const char quaternary_capability[], const char quinary_capability[],
             const char senary_capability[], const char septenary_capability[],
             unsigned int sequence);
static char *error_json(unsigned int version, const nv_snapshot_error_t *error,
		unsigned int sequence);
static int snapshot_model_is_valid(const nv_pane_snapshot_t *snapshot);
static int entry_model_is_valid(const nv_pane_entry_t *entry);
static int string_fits(const char value[], size_t maximum);
static int hex_string_is_valid(const char value[], size_t maximum);
static const char *sort_key_name(nv_pane_sort_key_t key);
static const char *session_resource_name(nv_session_resource_kind_t kind);
static JSON_Value *preview_task_payload(const nv_preview_event_t *event);
static const char *preview_pane_name(nv_preview_pane_t pane);
static const char *preview_kind_name(nv_preview_kind_t kind);
static const char *preview_state_name(nv_preview_task_state_t state);
static const char *resource_task_kind_name(nv_resource_task_kind_t kind);
static const char *resource_task_state_name(nv_resource_task_state_t state);
static JSON_Value *open_argv_value(const nv_open_resolution_t *resolution);

static int
set_i64_string(JSON_Object *object, const char name[], int64_t value)
{
	char buffer[32];
	snprintf(buffer, sizeof(buffer), "%" PRId64, value);
	return json_object_set_string(object, name, buffer);
}

static int
set_u64_string(JSON_Object *object, const char name[], uint64_t value)
{
	char buffer[32];
	snprintf(buffer, sizeof(buffer), "%" PRIu64, value);
	return json_object_set_string(object, name, buffer);
}

static const char *
entry_kind_name(nv_entry_kind_t kind)
{
	switch(kind)
	{
		case NV_ENTRY_DIRECTORY: return "directory";
		case NV_ENTRY_FILE: return "file";
		case NV_ENTRY_SYMLINK: return "symlink";
		case NV_ENTRY_EXECUTABLE: return "executable";
		case NV_ENTRY_FIFO: return "fifo";
		case NV_ENTRY_SOCKET: return "socket";
		case NV_ENTRY_CHAR_DEVICE: return "char-device";
		case NV_ENTRY_BLOCK_DEVICE: return "block-device";
		case NV_ENTRY_UNKNOWN: return "unknown";
	}
	return "unknown";
}

static const char *
entry_resource_kind_name(nv_entry_resource_kind_t kind)
{
	return kind == NV_ENTRY_RESOURCE_ARCHIVE ? "archive" : NULL;
}

static int
string_fits(const char value[], size_t maximum)
{
	if(value == NULL)
	{
		return 0;
	}
	for(size_t i = 0U; i <= maximum; ++i)
	{
		if(value[i] == '\0')
		{
			return 1;
		}
	}
	return 0;
}

static int
hex_string_is_valid(const char value[], size_t maximum)
{
	if(!string_fits(value, maximum))
	{
		return 0;
	}
	const size_t length = strlen(value);
	if(length % 2U != 0U)
	{
		return 0;
	}
	for(size_t i = 0U; i < length; ++i)
	{
		if(!((value[i] >= '0' && value[i] <= '9') ||
				(value[i] >= 'a' && value[i] <= 'f')))
		{
			return 0;
		}
	}
	return 1;
}

static int
entry_model_is_valid(const nv_pane_entry_t *entry)
{
	return entry != NULL &&
		(entry->resource_kind == NV_ENTRY_RESOURCE_NONE ||
		 entry->resource_kind == NV_ENTRY_RESOURCE_ARCHIVE) &&
		string_fits(entry->name_display, NV_PANE_SNAPSHOT_MAX_DISPLAY_BYTES) &&
		hex_string_is_valid(entry->name_bytes_hex, NV_PANE_SNAPSHOT_MAX_HEX_BYTES) &&
		string_fits(entry->path_display, NV_PANE_SNAPSHOT_MAX_DISPLAY_BYTES) &&
		hex_string_is_valid(entry->path_bytes_hex, NV_PANE_SNAPSHOT_MAX_HEX_BYTES) &&
		(entry->owner_display == NULL || string_fits(entry->owner_display,
			NV_PANE_SNAPSHOT_MAX_OWNER_BYTES)) &&
		(entry->group_display == NULL || string_fits(entry->group_display,
			NV_PANE_SNAPSHOT_MAX_OWNER_BYTES));
}

static const char *
sort_key_name(nv_pane_sort_key_t key)
{
	switch(key)
	{
		case NV_SORT_NAME: return "name";
		case NV_SORT_EXTENSION: return "extension";
		case NV_SORT_SIZE: return "size";
		case NV_SORT_CTIME: return "ctime";
		case NV_SORT_MTIME: return "mtime";
		case NV_SORT_MODE: return "mode";
		case NV_SORT_TYPE: return "type";
		case NV_SORT_OTHER: return "other";
	}
	return NULL;
}

static const char *
session_resource_name(nv_session_resource_kind_t kind)
{
	return kind == NV_SESSION_RESOURCE_ARCHIVE ? "archive" :
		kind == NV_SESSION_RESOURCE_SSH ? "ssh" : NULL;
}

static int
snapshot_model_is_valid(const nv_pane_snapshot_t *snapshot)
{
	if(snapshot == NULL ||
			!string_fits(snapshot->cwd_display, NV_PANE_SNAPSHOT_MAX_DISPLAY_BYTES) ||
			!hex_string_is_valid(snapshot->cwd_bytes_hex,
				NV_PANE_SNAPSHOT_MAX_HEX_BYTES) ||
			snapshot->entry_count > NV_PANE_SNAPSHOT_MAX_ENTRIES ||
			snapshot->selection_count > snapshot->entry_count ||
			snapshot->filtered_count > NV_PANE_SNAPSHOT_MAX_ENTRIES ||
			sort_key_name(snapshot->sort_key) == NULL ||
			(snapshot->sort_descending != 0 && snapshot->sort_descending != 1) ||
			(snapshot->filter_active != 0 && snapshot->filter_active != 1) ||
			(snapshot->entry_count == 0U && snapshot->cursor != -1) ||
			(snapshot->entry_count != 0U &&
				(snapshot->cursor < 0 || (size_t)snapshot->cursor >= snapshot->entry_count)) ||
			(snapshot->entry_count != 0U && snapshot->entries == NULL))
	{
		return 0;
	}
	size_t selected = 0U;
	for(size_t i = 0U; i < snapshot->entry_count; ++i)
	{
		if(!entry_model_is_valid(&snapshot->entries[i]))
		{
			return 0;
		}
		selected += snapshot->entries[i].selected != 0;
	}
	return selected == snapshot->selection_count;
}

static nv_protocol_json_result_t
serialize_payload(const char type[], unsigned int version, unsigned int sequence,
		JSON_Value *payload_value, char **serialized)
{
	if(serialized == NULL)
	{
		json_value_free(payload_value);
		return NV_PROTOCOL_JSON_ERROR;
	}
	*serialized = NULL;

	JSON_Value *const root_value = json_value_init_object();
	if(root_value == NULL)
	{
		json_value_free(payload_value);
		return NV_PROTOCOL_JSON_ERROR;
	}

	JSON_Object *const root = json_value_get_object(root_value);
	if(json_object_set_string(root, "protocol", "neovifm-core") != JSONSuccess ||
			json_object_set_number(root, "version", version) != JSONSuccess ||
			json_object_set_string(root, "type", type) != JSONSuccess ||
			json_object_set_number(root, "sequence", sequence) != JSONSuccess ||
			json_object_set_value(root, "payload", payload_value) != JSONSuccess)
	{
		if(json_value_get_parent(payload_value) == NULL)
		{
			json_value_free(payload_value);
		}
		json_value_free(root_value);
		return NV_PROTOCOL_JSON_ERROR;
	}

	const size_t serialized_size = json_serialization_size(root_value);
	if(serialized_size == 0U || serialized_size - 1U >
			NV_PROTOCOL_MAX_RECORD_BYTES)
	{
		json_value_free(root_value);
		return NV_PROTOCOL_JSON_TOO_LARGE;
	}

	char *const result = json_serialize_to_string(root_value);
	json_value_free(root_value);
	if(result == NULL)
	{
		return NV_PROTOCOL_JSON_ERROR;
	}
	*serialized = result;
	return NV_PROTOCOL_JSON_OK;
}

static char *
hello_json(unsigned int version, const char capability[],
		const char secondary_capability[], const char tertiary_capability[],
		const char quaternary_capability[], const char quinary_capability[],
		const char senary_capability[], const char septenary_capability[],
		unsigned int sequence)
{
	JSON_Value *const payload_value = json_value_init_object();
	JSON_Value *const capabilities_value = json_value_init_array();
	if(payload_value == NULL || capabilities_value == NULL)
	{
		json_value_free(payload_value);
		json_value_free(capabilities_value);
		return NULL;
	}

	JSON_Object *const payload = json_value_get_object(payload_value);
	JSON_Array *const capabilities = json_value_get_array(capabilities_value);
	if(json_object_set_string(payload, "implementation",
				"neovifm-core-probe") != JSONSuccess ||
			json_array_append_string(capabilities, capability) != JSONSuccess ||
			(secondary_capability != NULL &&
			 json_array_append_string(capabilities, secondary_capability) !=
			 JSONSuccess) ||
			(tertiary_capability != NULL &&
			 json_array_append_string(capabilities, tertiary_capability) !=
			 JSONSuccess) ||
			(quaternary_capability != NULL &&
			 json_array_append_string(capabilities, quaternary_capability) !=
			 JSONSuccess) ||
			(quinary_capability != NULL &&
			 json_array_append_string(capabilities, quinary_capability) !=
			 JSONSuccess) ||
			(senary_capability != NULL &&
			 json_array_append_string(capabilities, senary_capability) !=
			 JSONSuccess) ||
			(septenary_capability != NULL &&
			 json_array_append_string(capabilities, septenary_capability) !=
			 JSONSuccess) ||
			json_object_set_value(payload, "capabilities",
					capabilities_value) != JSONSuccess)
	{
		if(json_value_get_parent(capabilities_value) == NULL)
		{
			json_value_free(capabilities_value);
		}
		json_value_free(payload_value);
		return NULL;
	}

	char *serialized = NULL;
	return serialize_payload("hello", version, sequence, payload_value, &serialized) ==
			NV_PROTOCOL_JSON_OK
		? serialized
		: NULL;
}

char *
nv_protocol_hello_json(unsigned int sequence)
{
	return hello_json(0U, "snapshot-v0", NULL, NULL, NULL, NULL, NULL, NULL,
			sequence);
}

char *
nv_protocol_workspace_hello_json(unsigned int sequence)
{
	return hello_json(1U, "workspace-v1", NULL, NULL, NULL, NULL, NULL, NULL,
			sequence);
}

char *
nv_protocol_session_hello_json(unsigned int sequence)
{
	return hello_json(2U, "workspace-session-v2", NULL, NULL, NULL, NULL, NULL,
			NULL, sequence);
}

char *
nv_protocol_preview_session_hello_json(unsigned int sequence)
{
	if(nv_fs_actions_supported())
	{
	return hello_json(3U, "preview-session-v3", "workspace-sort-v1",
			"pane-tabs-v1", "file-actions-v1", "open-v1",
			"resource-tasks-v1", "file-rename-v1", sequence);
	}
	return hello_json(3U, "preview-session-v3", "workspace-sort-v1",
			"pane-tabs-v1", NULL, "open-v1", "resource-tasks-v1", NULL, sequence);
}

static const char *
preview_pane_name(nv_preview_pane_t pane)
{
	return pane == NV_PREVIEW_PANE_LEFT ? "left" :
		pane == NV_PREVIEW_PANE_RIGHT ? "right" : NULL;
}

static const char *
preview_kind_name(nv_preview_kind_t kind)
{
	return kind == NV_PREVIEW_KIND_TEXT ? "text" :
		kind == NV_PREVIEW_KIND_MARKDOWN ? "markdown" :
		kind == NV_PREVIEW_KIND_PDF ? "pdf" :
		kind == NV_PREVIEW_KIND_DIRECTORY ? "directory" :
		kind == NV_PREVIEW_KIND_ARCHIVE ? "archive" :
		kind == NV_PREVIEW_KIND_BINARY ? "binary" :
		kind == NV_PREVIEW_KIND_IMAGE ? "image" :
		kind == NV_PREVIEW_KIND_AUDIO ? "audio" :
		kind == NV_PREVIEW_KIND_VIDEO ? "video" : NULL;
}

static const char *
preview_state_name(nv_preview_task_state_t state)
{
	switch(state)
	{
		case NV_PREVIEW_TASK_QUEUED: return "queued";
		case NV_PREVIEW_TASK_RUNNING: return "running";
		case NV_PREVIEW_TASK_DONE: return "done";
		case NV_PREVIEW_TASK_FAILED: return "failed";
		case NV_PREVIEW_TASK_CANCELLED: return "cancelled";
	}
	return NULL;
}

static const char *
resource_task_kind_name(nv_resource_task_kind_t kind)
{
	return kind == NV_RESOURCE_TASK_MOUNT_ARCHIVE ? "mount-archive" :
		kind == NV_RESOURCE_TASK_MOUNT_SSH ? "mount-ssh" :
		kind == NV_RESOURCE_TASK_UNMOUNT ? "unmount" : NULL;
}

static const char *
resource_task_state_name(nv_resource_task_state_t state)
{
	switch(state)
	{
		case NV_RESOURCE_TASK_QUEUED: return "queued";
		case NV_RESOURCE_TASK_RUNNING: return "running";
		case NV_RESOURCE_TASK_DONE: return "done";
		case NV_RESOURCE_TASK_FAILED: return "failed";
		case NV_RESOURCE_TASK_CANCELLED: return "cancelled";
	}
	return NULL;
}

static JSON_Value *
preview_task_payload(const nv_preview_event_t *event)
{
	const nv_preview_pane_t target_pane = event != NULL && event->has_target_pane ?
		event->target_pane : event == NULL ? NV_PREVIEW_PANE_LEFT : event->pane;
	if(event == NULL || event->task_id == 0U || event->generation == 0U ||
			preview_pane_name(event->pane) == NULL ||
			preview_pane_name(target_pane) == NULL ||
			preview_kind_name(event->kind) == NULL ||
			preview_state_name(event->state) == NULL ||
			!hex_string_is_valid(event->cwd_bytes_hex, NV_PANE_SNAPSHOT_MAX_HEX_BYTES) ||
			!hex_string_is_valid(event->path_bytes_hex, NV_PANE_SNAPSHOT_MAX_HEX_BYTES) ||
			(event->error_code != NULL &&
				!string_fits(event->error_code, 128U)))
	{
		return NULL;
	}
	JSON_Value *const value = json_value_init_object();
	if(value == NULL) return NULL;
	JSON_Object *const payload = json_value_get_object(value);
	if(set_u64_string(payload, "task_id", event->task_id) != JSONSuccess ||
			set_u64_string(payload, "generation", event->generation) != JSONSuccess ||
			json_object_set_string(payload, "pane", preview_pane_name(event->pane)) != JSONSuccess ||
			json_object_set_string(payload, "target_pane", preview_pane_name(target_pane)) != JSONSuccess ||
			json_object_set_string(payload, "kind", preview_kind_name(event->kind)) != JSONSuccess ||
			json_object_set_string(payload, "state", preview_state_name(event->state)) != JSONSuccess ||
			json_object_set_string(payload, "cwd_bytes_hex", event->cwd_bytes_hex) != JSONSuccess ||
			json_object_set_string(payload, "path_bytes_hex", event->path_bytes_hex) != JSONSuccess ||
			(event->error_code != NULL &&
			 json_object_set_string(payload, "error_code", event->error_code) != JSONSuccess) ||
			(event->os_error != 0 &&
			 json_object_set_number(payload, "os_error", event->os_error) != JSONSuccess))
	{
		json_value_free(value);
		return NULL;
	}
	return value;
}

char *
nv_protocol_preview_task_json(const nv_preview_event_t *event,
		unsigned int output_sequence)
{
	JSON_Value *const payload = preview_task_payload(event);
	char *json = NULL;
	return payload != NULL && serialize_payload("task", 3U, output_sequence,
			payload, &json) == NV_PROTOCOL_JSON_OK ? json : NULL;
}

char *
nv_protocol_preview_json(const nv_preview_event_t *event,
		unsigned int output_sequence)
{
	if(event == NULL || (event->state != NV_PREVIEW_TASK_DONE &&
			event->state != NV_PREVIEW_TASK_FAILED &&
			event->state != NV_PREVIEW_TASK_CANCELLED) ||
			(event->state == NV_PREVIEW_TASK_DONE &&
			 !string_fits(event->content, NV_PREVIEW_MAX_BYTES)))
	{
		return NULL;
	}
	JSON_Value *const payload_value = preview_task_payload(event);
	if(payload_value == NULL) return NULL;
	JSON_Object *const payload = json_value_get_object(payload_value);
	if(json_object_set_boolean(payload, "truncated", event->truncated) != JSONSuccess ||
			(event->state == NV_PREVIEW_TASK_DONE &&
			 json_object_set_string(payload, "content", event->content) != JSONSuccess))
	{
		json_value_free(payload_value);
		return NULL;
	}
	char *json = NULL;
	return serialize_payload("preview", 3U, output_sequence, payload_value,
			&json) == NV_PROTOCOL_JSON_OK ? json : NULL;
}

static const char *
action_kind_name(nv_session_command_kind_t kind)
{
	return kind == NV_SESSION_COPY ? "copy" :
		kind == NV_SESSION_MOVE_FILES ? "move" :
		kind == NV_SESSION_MKDIR ? "mkdir" :
		kind == NV_SESSION_RENAME ? "rename" :
		kind == NV_SESSION_DELETE ? "delete" : NULL;
}

static const char *
action_state_name(nv_action_task_state_t state)
{
	switch(state)
	{
		case NV_ACTION_TASK_QUEUED: return "queued";
		case NV_ACTION_TASK_RUNNING: return "running";
		case NV_ACTION_TASK_DONE: return "done";
		case NV_ACTION_TASK_FAILED: return "failed";
		case NV_ACTION_TASK_CANCELLED: return "cancelled";
	}
	return NULL;
}

char *
nv_protocol_action_task_json(const nv_action_event_t *event,
		unsigned int output_sequence)
{
	if(event == NULL || event->task_id == 0U || event->command_sequence == 0U ||
			action_kind_name(event->kind) == NULL ||
			action_state_name(event->state) == NULL ||
			event->completed_count > event->total_count ||
			(event->has_failed_index && event->failed_index >= event->total_count) ||
			(event->retryable && (event->state != NV_ACTION_TASK_FAILED &&
				event->state != NV_ACTION_TASK_CANCELLED)) ||
			(event->bytes_known && event->bytes_completed > event->bytes_total) ||
			(event->finished_at_unix_ms != 0U && event->started_at_unix_ms != 0U &&
				event->finished_at_unix_ms < event->started_at_unix_ms) ||
			(event->source_path_bytes_hex != NULL && !hex_string_is_valid(
				event->source_path_bytes_hex, NV_PANE_SNAPSHOT_MAX_HEX_BYTES)) ||
			(event->destination_path_bytes_hex != NULL && !hex_string_is_valid(
				event->destination_path_bytes_hex, NV_PANE_SNAPSHOT_MAX_HEX_BYTES)) ||
			(event->current_path_bytes_hex != NULL && !hex_string_is_valid(
				event->current_path_bytes_hex, NV_PANE_SNAPSHOT_MAX_HEX_BYTES)) ||
			(event->error_code != NULL && !string_fits(event->error_code, 128U)))
	{
		return NULL;
	}
	JSON_Value *const value = json_value_init_object();
	if(value == NULL) return NULL;
	JSON_Object *const payload = json_value_get_object(value);
	if(set_u64_string(payload, "task_id", event->task_id) != JSONSuccess ||
			json_object_set_number(payload, "command_sequence",
				event->command_sequence) != JSONSuccess ||
			json_object_set_string(payload, "pane",
				event->pane == NV_SESSION_LEFT ? "left" : "right") != JSONSuccess ||
			json_object_set_string(payload, "action",
				action_kind_name(event->kind)) != JSONSuccess ||
			json_object_set_string(payload, "state",
				action_state_name(event->state)) != JSONSuccess ||
			json_object_set_number(payload, "completed_count",
				event->completed_count) != JSONSuccess ||
			json_object_set_number(payload, "total_count",
				event->total_count) != JSONSuccess ||
			json_object_set_boolean(payload, "partial", event->partial) != JSONSuccess ||
			json_object_set_boolean(payload, "retryable", event->retryable) != JSONSuccess ||
			json_object_set_boolean(payload, "undo_available", event->undo_available) != JSONSuccess ||
			json_object_set_boolean(payload, "bytes_known", event->bytes_known) != JSONSuccess ||
			(event->started_at_unix_ms != 0U && set_u64_string(payload,
				"started_at_unix_ms", event->started_at_unix_ms) != JSONSuccess) ||
			(event->finished_at_unix_ms != 0U && set_u64_string(payload,
				"finished_at_unix_ms", event->finished_at_unix_ms) != JSONSuccess) ||
			(event->source_path_bytes_hex != NULL && json_object_set_string(payload,
				"source_path_bytes_hex", event->source_path_bytes_hex) != JSONSuccess) ||
			(event->destination_path_bytes_hex != NULL && json_object_set_string(payload,
				"destination_path_bytes_hex", event->destination_path_bytes_hex) != JSONSuccess) ||
			(event->current_path_bytes_hex != NULL && json_object_set_string(payload,
				"current_path_bytes_hex", event->current_path_bytes_hex) != JSONSuccess) ||
			(event->bytes_known && (set_u64_string(payload, "bytes_completed",
					event->bytes_completed) != JSONSuccess || set_u64_string(payload,
					"bytes_total", event->bytes_total) != JSONSuccess)) ||
			(event->has_failed_index && json_object_set_number(payload,
				"failed_index", event->failed_index) != JSONSuccess) ||
			(event->error_code != NULL && json_object_set_string(payload,
				"error_code", event->error_code) != JSONSuccess) ||
			(event->os_error != 0 && json_object_set_number(payload, "os_error",
				event->os_error) != JSONSuccess))
	{
		json_value_free(value);
		return NULL;
	}
	char *json = NULL;
	return serialize_payload("action-task", 3U, output_sequence, value, &json) ==
		NV_PROTOCOL_JSON_OK ? json : NULL;
}

char *
nv_protocol_resource_task_json(const nv_resource_task_event_t *event,
		unsigned int output_sequence)
{
	if(event == NULL || event->task_id == 0U || event->command_sequence == 0U ||
		event->pane > NV_SESSION_RIGHT || event->tab_id == 0U ||
		resource_task_kind_name(event->kind) == NULL ||
		resource_task_state_name(event->state) == NULL ||
		(event->source_path != NULL && !string_fits(event->source_path,
			NV_RESOURCE_MAX_PATH_BYTES)) ||
		(event->mount_point != NULL && !string_fits(event->mount_point,
			NV_RESOURCE_MAX_PATH_BYTES)) ||
		(event->unmount_path != NULL && !string_fits(event->unmount_path,
			NV_RESOURCE_MAX_PATH_BYTES)) ||
		(event->error_code != NULL && !string_fits(event->error_code, 128U)))
	{
		return NULL;
	}
	JSON_Value *const value = json_value_init_object();
	if(value == NULL) return NULL;
	JSON_Object *const payload = json_value_get_object(value);
	if(set_u64_string(payload, "task_id", event->task_id) != JSONSuccess ||
			json_object_set_number(payload, "command_sequence",
				event->command_sequence) != JSONSuccess ||
			json_object_set_string(payload, "pane",
				event->pane == NV_SESSION_LEFT ? "left" : "right") != JSONSuccess ||
			set_u64_string(payload, "tab_id", event->tab_id) != JSONSuccess ||
			json_object_set_string(payload, "resource",
				resource_task_kind_name(event->kind)) != JSONSuccess ||
			json_object_set_string(payload, "state",
				resource_task_state_name(event->state)) != JSONSuccess ||
			(event->source_path != NULL && json_object_set_string(payload,
				"source_path", event->source_path) != JSONSuccess) ||
			(event->mount_point != NULL && json_object_set_string(payload,
				"mount_point", event->mount_point) != JSONSuccess) ||
			(event->unmount_path != NULL && json_object_set_string(payload,
				"unmount_path", event->unmount_path) != JSONSuccess) ||
			(event->error_code != NULL && json_object_set_string(payload,
				"error_code", event->error_code) != JSONSuccess) ||
			(event->os_error != 0 && json_object_set_number(payload, "os_error",
				event->os_error) != JSONSuccess))
	{
		json_value_free(value);
		return NULL;
	}
	char *json = NULL;
	return serialize_payload("resource-task", 3U, output_sequence, value,
			&json) == NV_PROTOCOL_JSON_OK ? json : NULL;
}

char *
nv_protocol_open_json(const nv_open_resolution_t *resolution,
		const char path_bytes_hex[], unsigned int output_sequence,
		unsigned int command_sequence)
{
	if(resolution == NULL || command_sequence == 0U ||
			nv_open_intent_name(resolution->intent) == NULL ||
			nv_open_source_name(resolution->source) == NULL ||
			!hex_string_is_valid(path_bytes_hex, NV_PANE_SNAPSHOT_MAX_HEX_BYTES) ||
			path_bytes_hex[0] == '\0' ||
			resolution->argc == 0U || resolution->argc > NV_OPEN_MAX_ARGS ||
			resolution->argv == NULL)
	{
		return NULL;
	}
	JSON_Value *const payload_value = json_value_init_object();
	JSON_Value *const argv_value = open_argv_value(resolution);
	if(payload_value == NULL || argv_value == NULL)
	{
		json_value_free(payload_value);
		json_value_free(argv_value);
		return NULL;
	}
	JSON_Object *const payload = json_value_get_object(payload_value);
	if(json_object_set_number(payload, "command_sequence", command_sequence) != JSONSuccess ||
			json_object_set_string(payload, "intent",
				nv_open_intent_name(resolution->intent)) != JSONSuccess ||
			json_object_set_string(payload, "source",
				nv_open_source_name(resolution->source)) != JSONSuccess ||
			json_object_set_string(payload, "state", "resolved") != JSONSuccess ||
			json_object_set_string(payload, "path_bytes_hex", path_bytes_hex) != JSONSuccess ||
			json_object_set_value(payload, "argv", argv_value) != JSONSuccess)
	{
		if(json_value_get_parent(argv_value) == NULL) json_value_free(argv_value);
		json_value_free(payload_value);
		return NULL;
	}
	char *json = NULL;
	return serialize_payload("open", 3U, output_sequence, payload_value,
			&json) == NV_PROTOCOL_JSON_OK ? json : NULL;
}

static JSON_Value *
open_argv_value(const nv_open_resolution_t *resolution)
{
	JSON_Value *const value = json_value_init_array();
	if(value == NULL) return NULL;
	JSON_Array *const array = json_value_get_array(value);
	for(size_t i = 0U; i < resolution->argc; ++i)
	{
		if(!string_fits(resolution->argv[i], NV_OPEN_MAX_ARG_BYTES) ||
				json_array_append_string(array, resolution->argv[i]) != JSONSuccess)
		{
			json_value_free(value);
			return NULL;
		}
	}
	return value;
}

static int
set_stat_error(JSON_Object *object, int error_number)
{
	JSON_Value *const value = json_value_init_object();
	if(value == NULL)
	{
		return JSONFailure;
	}

	JSON_Object *const stat_error = json_value_get_object(value);
	if(json_object_set_number(stat_error, "code", error_number) != JSONSuccess ||
			json_object_set_string(stat_error, "message",
					"metadata unavailable") != JSONSuccess ||
			json_object_set_value(object, "stat_error", value) != JSONSuccess)
	{
		if(json_value_get_parent(value) == NULL)
		{
			json_value_free(value);
		}
		return JSONFailure;
	}
	return JSONSuccess;
}

static int
set_optional_stat(JSON_Object *object, const nv_pane_entry_t *entry)
{
	if(!entry->has_stat)
	{
		return (entry->stat_error == 0)
			? JSONSuccess
			: set_stat_error(object, entry->stat_error);
	}

	char mode[16];
	snprintf(mode, sizeof(mode), "%o", (unsigned int)entry->mode);
	if(set_u64_string(object, "device", entry->device) != JSONSuccess ||
			set_u64_string(object, "inode", entry->inode) != JSONSuccess ||
			set_u64_string(object, "ctime_unix_ns", entry->ctime_unix_ns) !=
				JSONSuccess ||
			json_object_set_string(object, "mode_octal", mode) != JSONSuccess)
	{
		return JSONFailure;
	}
	if((entry->owner_display != NULL && json_object_set_string(object,
			"owner_display", entry->owner_display) != JSONSuccess) ||
		(entry->group_display != NULL && json_object_set_string(object,
			"group_display", entry->group_display) != JSONSuccess))
	{
		return JSONFailure;
	}
	return JSONSuccess;
}

static JSON_Value *
entry_value(const nv_pane_entry_t *entry)
{
	JSON_Value *const value = json_value_init_object();
	if(value == NULL)
	{
		return NULL;
	}

	JSON_Object *const object = json_value_get_object(value);
	if(json_object_set_string(object, "name_display", entry->name_display) !=
			JSONSuccess ||
			json_object_set_string(object, "name_bytes_hex",
					entry->name_bytes_hex) != JSONSuccess ||
			json_object_set_string(object, "path_display", entry->path_display) !=
			JSONSuccess ||
			json_object_set_string(object, "path_bytes_hex",
					entry->path_bytes_hex) != JSONSuccess ||
			json_object_set_string(object, "kind", entry_kind_name(entry->kind)) !=
				JSONSuccess ||
			set_u64_string(object, "size_bytes", entry->size_bytes) != JSONSuccess ||
			set_i64_string(object, "mtime_unix_ms", entry->mtime_unix_ms) !=
			JSONSuccess ||
			json_object_set_boolean(object, "selected", entry->selected) !=
			JSONSuccess ||
			json_object_set_boolean(object, "hidden", entry->hidden) != JSONSuccess ||
			(entry->resource_kind != NV_ENTRY_RESOURCE_NONE &&
			 json_object_set_string(object, "resource_kind",
					entry_resource_kind_name(entry->resource_kind)) != JSONSuccess) ||
			set_optional_stat(object, entry) != JSONSuccess)
	{
		json_value_free(value);
		return NULL;
	}
	return value;
}

static JSON_Value *
snapshot_payload(const nv_pane_snapshot_t *snapshot)
{
	JSON_Value *const payload_value = json_value_init_object();
	JSON_Value *const entries_value = json_value_init_array();
	if(payload_value == NULL || entries_value == NULL)
	{
		json_value_free(payload_value);
		json_value_free(entries_value);
		return NULL;
	}

	JSON_Object *const payload = json_value_get_object(payload_value);
	JSON_Array *const entries = json_value_get_array(entries_value);
	for(size_t i = 0U; i < snapshot->entry_count; ++i)
	{
		JSON_Value *const value = entry_value(&snapshot->entries[i]);
		if(value == NULL || json_array_append_value(entries, value) != JSONSuccess)
		{
			json_value_free(value);
			json_value_free(entries_value);
			json_value_free(payload_value);
			return NULL;
		}
	}

	if(json_object_set_string(payload, "cwd_display", snapshot->cwd_display) !=
			JSONSuccess ||
			json_object_set_string(payload, "cwd_bytes_hex",
					snapshot->cwd_bytes_hex) != JSONSuccess ||
			set_i64_string(payload, "generated_at_unix_ms",
					snapshot->generated_at_unix_ms) != JSONSuccess ||
			set_u64_string(payload, "snapshot_revision",
					snapshot->snapshot_revision) != JSONSuccess ||
			(snapshot->has_cwd_stat &&
			 (set_u64_string(payload, "cwd_device", snapshot->cwd_device) !=
			  JSONSuccess ||
			  set_u64_string(payload, "cwd_inode", snapshot->cwd_inode) !=
			  JSONSuccess ||
			  set_u64_string(payload, "cwd_ctime_unix_ns",
					snapshot->cwd_ctime_unix_ns) !=
			  JSONSuccess)) ||
			json_object_set_number(payload, "cursor", snapshot->cursor) != JSONSuccess ||
			json_object_set_number(payload, "entry_count", snapshot->entry_count) !=
			JSONSuccess ||
			json_object_set_number(payload, "selection_count",
				snapshot->selection_count) != JSONSuccess ||
			json_object_set_number(payload, "filtered_count",
				snapshot->filtered_count) != JSONSuccess ||
			json_object_set_string(payload, "sort_key",
				sort_key_name(snapshot->sort_key)) != JSONSuccess ||
			json_object_set_boolean(payload, "sort_descending",
				snapshot->sort_descending) != JSONSuccess ||
			json_object_set_boolean(payload, "filter_active",
				snapshot->filter_active) != JSONSuccess ||
			json_object_set_value(payload, "entries", entries_value) != JSONSuccess)
	{
		if(json_value_get_parent(entries_value) == NULL)
		{
			json_value_free(entries_value);
		}
		json_value_free(payload_value);
		return NULL;
	}
	return payload_value;
}

nv_protocol_json_result_t
nv_protocol_snapshot_json(const nv_pane_snapshot_t *snapshot,
		unsigned int sequence, char **json)
{
	if(json != NULL)
	{
		*json = NULL;
	}
	if(!snapshot_model_is_valid(snapshot) || json == NULL)
	{
		return NV_PROTOCOL_JSON_ERROR;
	}

	JSON_Value *const payload = snapshot_payload(snapshot);
	return (payload == NULL) ? NV_PROTOCOL_JSON_ERROR :
		serialize_payload("snapshot", 0U, sequence, payload, json);
}

nv_protocol_json_result_t
nv_protocol_workspace_snapshot_json(const nv_pane_snapshot_t *left,
		const nv_pane_snapshot_t *right, const char active_pane[],
		unsigned int sequence, char **json)
{
	if(json != NULL)
	{
		*json = NULL;
	}
	if(left != NULL && right != NULL &&
			left->entry_count > NV_PANE_SNAPSHOT_MAX_ENTRIES - right->entry_count)
	{
		return NV_PROTOCOL_JSON_TOO_LARGE;
	}
	if(!snapshot_model_is_valid(left) || !snapshot_model_is_valid(right) ||
			active_pane == NULL ||
			(strcmp(active_pane, "left") != 0 && strcmp(active_pane, "right") != 0) ||
			json == NULL)
	{
		return NV_PROTOCOL_JSON_ERROR;
	}

	JSON_Value *const payload_value = json_value_init_object();
	JSON_Value *const left_value = snapshot_payload(left);
	JSON_Value *const right_value = snapshot_payload(right);
	if(payload_value == NULL || left_value == NULL || right_value == NULL)
	{
		json_value_free(payload_value);
		json_value_free(left_value);
		json_value_free(right_value);
		return NV_PROTOCOL_JSON_ERROR;
	}
	JSON_Object *const payload = json_value_get_object(payload_value);
	if(json_object_set_string(payload, "active_pane", active_pane) != JSONSuccess ||
			json_object_set_value(payload, "left", left_value) != JSONSuccess ||
			json_object_set_value(payload, "right", right_value) != JSONSuccess)
	{
		if(json_value_get_parent(left_value) == NULL)
		{
			json_value_free(left_value);
		}
		if(json_value_get_parent(right_value) == NULL)
		{
			json_value_free(right_value);
		}
		json_value_free(payload_value);
		return NV_PROTOCOL_JSON_ERROR;
	}
	return serialize_payload("workspace-snapshot", 1U, sequence, payload_value,
			json);
}

nv_protocol_json_result_t
nv_protocol_session_snapshot_json(const nv_pane_snapshot_t *left,
		const nv_pane_snapshot_t *right, const char active_pane[],
		unsigned int output_sequence, unsigned int request_sequence,
		const char trigger[], char **json)
{
	if(json != NULL)
	{
		*json = NULL;
	}
	if(left != NULL && right != NULL &&
			left->entry_count > NV_PANE_SNAPSHOT_MAX_ENTRIES - right->entry_count)
	{
		return NV_PROTOCOL_JSON_TOO_LARGE;
	}
	if(!snapshot_model_is_valid(left) || !snapshot_model_is_valid(right) ||
			active_pane == NULL ||
			(strcmp(active_pane, "left") != 0 && strcmp(active_pane, "right") != 0) ||
			trigger == NULL || (strcmp(trigger, "initial") != 0 &&
					strcmp(trigger, "command") != 0 && strcmp(trigger, "watch") != 0 &&
					strcmp(trigger, "action") != 0 && strcmp(trigger, "resource") != 0) ||
			json == NULL)
	{
		return NV_PROTOCOL_JSON_ERROR;
	}
	JSON_Value *const payload_value = json_value_init_object();
	JSON_Value *const left_value = snapshot_payload(left);
	JSON_Value *const right_value = snapshot_payload(right);
	if(payload_value == NULL || left_value == NULL || right_value == NULL)
	{
		json_value_free(payload_value);
		json_value_free(left_value);
		json_value_free(right_value);
		return NV_PROTOCOL_JSON_ERROR;
	}
	JSON_Object *const payload = json_value_get_object(payload_value);
	if(json_object_set_string(payload, "active_pane", active_pane) != JSONSuccess ||
			json_object_set_number(payload, "command_sequence", request_sequence) != JSONSuccess ||
			json_object_set_string(payload, "trigger", trigger) != JSONSuccess ||
			json_object_set_value(payload, "left", left_value) != JSONSuccess ||
			json_object_set_value(payload, "right", right_value) != JSONSuccess)
	{
		if(json_value_get_parent(left_value) == NULL) json_value_free(left_value);
		if(json_value_get_parent(right_value) == NULL) json_value_free(right_value);
		json_value_free(payload_value);
		return NV_PROTOCOL_JSON_ERROR;
	}
	return serialize_payload("workspace-snapshot", 2U, output_sequence,
			payload_value, json);
}

nv_protocol_json_result_t
nv_protocol_preview_session_snapshot_json(const nv_pane_snapshot_t *left,
		const nv_pane_snapshot_t *right, const char active_pane[],
		unsigned int output_sequence, unsigned int request_sequence,
		const char trigger[], char **json)
{
	char *v2 = NULL;
	const nv_protocol_json_result_t result = nv_protocol_session_snapshot_json(left,
		right, active_pane, output_sequence, request_sequence, trigger, &v2);
	if(result != NV_PROTOCOL_JSON_OK) return result;
	JSON_Value *const value = json_parse_string(v2);
	nv_protocol_json_free(v2);
	if(value == NULL || json_object_set_number(json_value_get_object(value),
			"version", 3U) != JSONSuccess)
	{
		json_value_free(value);
		return NV_PROTOCOL_JSON_ERROR;
	}
	const size_t size = json_serialization_size(value);
	if(size == 0U || size - 1U > NV_PROTOCOL_MAX_RECORD_BYTES)
	{
		json_value_free(value);
		return NV_PROTOCOL_JSON_TOO_LARGE;
	}
	*json = json_serialize_to_string(value);
	json_value_free(value);
	return *json == NULL ? NV_PROTOCOL_JSON_ERROR : NV_PROTOCOL_JSON_OK;
}

static int
session_tabs_are_valid(const nv_workspace_session_t *session)
{
	for(nv_session_pane_t pane = NV_SESSION_LEFT; pane <= NV_SESSION_RIGHT; ++pane)
	{
		const size_t count = nv_workspace_session_tab_count(session, pane);
		if(count == 0U || count > NV_SESSION_MAX_TABS ||
				nv_workspace_session_active_tab_index(session, pane) >= count)
		{
			return 0;
		}
		for(size_t i = 0U; i < count; ++i)
		{
			const uint64_t id = nv_workspace_session_tab_id(session, pane, i);
			const nv_pane_snapshot_t *const snapshot =
				nv_workspace_session_tab_snapshot(session, pane, i);
			if(id == 0U || snapshot == NULL || !string_fits(snapshot->cwd_display,
					NV_PANE_SNAPSHOT_MAX_DISPLAY_BYTES)) return 0;
			for(nv_session_pane_t other_pane = NV_SESSION_LEFT;
					other_pane <= NV_SESSION_RIGHT; ++other_pane)
			{
				const size_t other_count = nv_workspace_session_tab_count(session,
						other_pane);
				for(size_t j = 0U; j < other_count; ++j)
				{
					if(other_pane == pane && j == i) continue;
					if(nv_workspace_session_tab_id(session, other_pane, j) == id)
						return 0;
				}
			}
		}
	}
	return 1;
}

static int
set_session_tabs(JSON_Object *payload, const char field[],
		const nv_workspace_session_t *session, nv_session_pane_t pane)
{
	JSON_Value *const array_value = json_value_init_array();
	if(array_value == NULL) return JSONFailure;
	JSON_Array *const array = json_value_get_array(array_value);
	const size_t count = nv_workspace_session_tab_count(session, pane);
	const size_t active = nv_workspace_session_active_tab_index(session, pane);
	for(size_t i = 0U; i < count; ++i)
	{
		JSON_Value *const tab_value = json_value_init_object();
		if(tab_value == NULL)
		{
			json_value_free(array_value);
			return JSONFailure;
		}
		JSON_Object *const tab = json_value_get_object(tab_value);
		const nv_pane_snapshot_t *const snapshot =
			nv_workspace_session_tab_snapshot(session, pane, i);
		const nv_session_resource_t *const resource =
			nv_workspace_session_tab_resource(session, pane, i);
		if(set_u64_string(tab, "id", nv_workspace_session_tab_id(session, pane,
				i)) != JSONSuccess || json_object_set_string(tab, "cwd_display",
					snapshot->cwd_display) != JSONSuccess ||
				json_object_set_boolean(tab, "active", i == active) != JSONSuccess ||
				(resource != NULL && resource->active && json_object_set_string(tab,
					"resource_kind", session_resource_name(resource->kind)) != JSONSuccess) ||
				json_array_append_value(array, tab_value) != JSONSuccess)
		{
			if(json_value_get_parent(tab_value) == NULL) json_value_free(tab_value);
			json_value_free(array_value);
			return JSONFailure;
		}
	}
	if(json_object_set_value(payload, field, array_value) != JSONSuccess)
	{
		if(json_value_get_parent(array_value) == NULL) json_value_free(array_value);
		return JSONFailure;
	}
	return JSONSuccess;
}

nv_protocol_json_result_t
nv_protocol_preview_workspace_session_snapshot_json(
		const nv_workspace_session_t *session, unsigned int output_sequence,
		unsigned int request_sequence, const char trigger[], char **json)
{
	if(json != NULL) *json = NULL;
	if(session == NULL || json == NULL || !session_tabs_are_valid(session))
		return NV_PROTOCOL_JSON_ERROR;
	char *base = NULL;
	const nv_protocol_json_result_t result =
		nv_protocol_preview_session_snapshot_json(&session->left, &session->right,
				nv_workspace_session_active_name(session), output_sequence,
				request_sequence, trigger, &base);
	if(result != NV_PROTOCOL_JSON_OK) return result;
	JSON_Value *const value = json_parse_string(base);
	nv_protocol_json_free(base);
	JSON_Object *const root = value == NULL ? NULL : json_value_get_object(value);
	JSON_Object *const payload = root == NULL ? NULL :
		json_object_get_object(root, "payload");
	if(payload == NULL || set_session_tabs(payload, "left_tabs", session,
			NV_SESSION_LEFT) != JSONSuccess || set_session_tabs(payload, "right_tabs",
			session, NV_SESSION_RIGHT) != JSONSuccess)
	{
		json_value_free(value);
		return NV_PROTOCOL_JSON_ERROR;
	}
	const size_t size = json_serialization_size(value);
	if(size == 0U || size - 1U > NV_PROTOCOL_MAX_RECORD_BYTES)
	{
		json_value_free(value);
		return NV_PROTOCOL_JSON_TOO_LARGE;
	}
	*json = json_serialize_to_string(value);
	json_value_free(value);
	return *json == NULL ? NV_PROTOCOL_JSON_ERROR : NV_PROTOCOL_JSON_OK;
}

static char *
error_json(unsigned int version, const nv_snapshot_error_t *error,
		unsigned int sequence)
{
	if(error == NULL || error->code == NULL || error->message == NULL)
	{
		return NULL;
	}

	JSON_Value *const payload_value = json_value_init_object();
	if(payload_value == NULL)
	{
		return NULL;
	}
	JSON_Object *const payload = json_value_get_object(payload_value);
	if(json_object_set_string(payload, "code", error->code) != JSONSuccess ||
			json_object_set_string(payload, "message", error->message) != JSONSuccess ||
			json_object_set_boolean(payload, "retryable", error->retryable) !=
			JSONSuccess ||
			(error->os_error != 0 &&
			 json_object_set_number(payload, "os_error", error->os_error) !=
			 JSONSuccess) ||
			(error->path_display != NULL &&
			 json_object_set_string(payload, "path_display", error->path_display) !=
			 JSONSuccess) ||
			(error->path_bytes_hex != NULL &&
			 json_object_set_string(payload, "path_bytes_hex",
					 error->path_bytes_hex) != JSONSuccess))
	{
		json_value_free(payload_value);
		return NULL;
	}
	char *serialized = NULL;
	return serialize_payload("error", version, sequence, payload_value, &serialized) ==
			NV_PROTOCOL_JSON_OK
		? serialized
		: NULL;
}

char *
nv_protocol_error_json(const nv_snapshot_error_t *error, unsigned int sequence)
{
	return error_json(0U, error, sequence);
}

char *
nv_protocol_workspace_error_json(const nv_snapshot_error_t *error,
		unsigned int sequence)
{
	return error_json(1U, error, sequence);
}

char *
nv_protocol_session_command_error_json(const nv_snapshot_error_t *error,
		unsigned int output_sequence, unsigned int request_sequence)
{
	if(error == NULL || error->code == NULL || error->message == NULL)
	{
		return NULL;
	}
	JSON_Value *const payload_value = json_value_init_object();
	if(payload_value == NULL)
	{
		return NULL;
	}
	JSON_Object *const payload = json_value_get_object(payload_value);
	if(json_object_set_number(payload, "command_sequence", request_sequence) != JSONSuccess ||
			json_object_set_string(payload, "code", error->code) != JSONSuccess ||
			json_object_set_string(payload, "message", error->message) != JSONSuccess ||
			json_object_set_boolean(payload, "retryable", error->retryable) != JSONSuccess)
	{
		json_value_free(payload_value);
		return NULL;
	}
	char *serialized = NULL;
	return serialize_payload("command-error", 2U, output_sequence,
			payload_value, &serialized) == NV_PROTOCOL_JSON_OK ? serialized : NULL;
}

char *
nv_protocol_preview_session_command_error_json(const nv_snapshot_error_t *error,
		unsigned int output_sequence, unsigned int request_sequence)
{
	char *v2 = nv_protocol_session_command_error_json(error, output_sequence,
		request_sequence);
	if(v2 == NULL) return NULL;
	JSON_Value *const value = json_parse_string(v2);
	nv_protocol_json_free(v2);
	if(value == NULL || json_object_set_number(json_value_get_object(value),
			"version", 3U) != JSONSuccess)
	{
		json_value_free(value);
		return NULL;
	}
	char *const json = json_serialize_to_string(value);
	json_value_free(value);
	return json;
}

void
nv_protocol_json_free(char *json)
{
	json_free_serialized_string(json);
}

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 filetype=c : */
