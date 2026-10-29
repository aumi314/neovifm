/* vifm
 * Copyright (C) 2026 NeoVifm contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef VIFM__NEOVIFM__WORKSPACE_SESSION_H__
#define VIFM__NEOVIFM__WORKSPACE_SESSION_H__

#include "pane_snapshot.h"
#include "open_resolver.h"
#include "../compat/neovifm_fs.h"

#define NV_SESSION_MAX_NAME_BYTES 255U
#define NV_SESSION_MAX_SEARCH_BYTES 255U
#define NV_SESSION_MAX_ACTION_PATHS 64U
#define NV_SESSION_MAX_TABS 8U
#define NV_SESSION_MAX_TAB_CYCLE 999
#define NV_SESSION_MAX_CURSOR_HISTORY 128U

typedef enum
{
	NV_SESSION_LEFT,
	NV_SESSION_RIGHT,
} nv_session_pane_t;

typedef enum
{
	NV_SESSION_RESOURCE_NONE,
	NV_SESSION_RESOURCE_ARCHIVE,
	NV_SESSION_RESOURCE_SSH,
} nv_session_resource_kind_t;

typedef enum
{
	NV_SESSION_FOCUS,
	NV_SESSION_FOCUS_NEXT,
	NV_SESSION_MOVE_CURSOR,
	NV_SESSION_MOVE_FIRST,
	NV_SESSION_MOVE_LAST,
	NV_SESSION_SEARCH,
	NV_SESSION_SEARCH_NEXT,
	NV_SESSION_ENTER,
	NV_SESSION_MOUNT_SSH,
	NV_SESSION_PARENT,
	NV_SESSION_TOGGLE_SELECTION,
	NV_SESSION_SELECT_ALL,
	NV_SESSION_CLEAR_SELECTION,
	NV_SESSION_REFRESH,
	NV_SESSION_SORT_CYCLE,
	NV_SESSION_SORT_BY,
	NV_SESSION_COPY,
	NV_SESSION_MOVE_FILES,
	NV_SESSION_MKDIR,
	NV_SESSION_DELETE,
	NV_SESSION_RENAME,
	NV_SESSION_UNDO,
	NV_SESSION_SELECT_ENTRY,
	NV_SESSION_NEW_TAB,
	NV_SESSION_ACTIVATE_TAB,
	NV_SESSION_CLOSE_TAB,
	NV_SESSION_TAB_CYCLE,
	NV_SESSION_PREVIEW,
	NV_SESSION_OPEN,
	NV_SESSION_CANCEL_ACTION,
	NV_SESSION_CANCEL_RESOURCE,
	NV_SESSION_RETRY_ACTION,
} nv_session_command_kind_t;

typedef struct
{
	char *path_bytes_hex;
	uint64_t device;
	uint64_t inode;
	uint64_t ctime_unix_ns;
	nv_entry_kind_t kind;
} nv_session_action_target_t;

typedef struct
{
	nv_session_command_kind_t kind;
	nv_session_pane_t pane;
	int delta;
	nv_pane_sort_key_t sort_key;
	char name[NV_SESSION_MAX_NAME_BYTES + 1U];
	char *search_query;
	int search_direction;
	int owns_search_fields;
	char *action_cwd_bytes_hex;
	uint64_t action_snapshot_revision;
	uint64_t action_cwd_device;
	uint64_t action_cwd_inode;
	uint64_t action_cwd_ctime_unix_ns;
	char *action_destination_cwd_bytes_hex;
	uint64_t action_destination_snapshot_revision;
	uint64_t action_destination_cwd_device;
	uint64_t action_destination_cwd_inode;
	uint64_t action_destination_cwd_ctime_unix_ns;
	nv_session_action_target_t *action_targets;
	size_t action_target_count;
	int owns_action_fields;
	int has_pane;
	size_t entry_index;
	int toggle_selection;
	uint64_t tab_id;
	uint64_t action_task_id;
	uint64_t resource_task_id;
	char *resource_remote;
	int owns_resource_fields;
	nv_session_pane_t preview_target_pane;
	char *preview_cwd_bytes_hex;
	uint64_t preview_snapshot_revision;
	uint64_t preview_cwd_device;
	uint64_t preview_cwd_inode;
	uint64_t preview_cwd_ctime_unix_ns;
	char *preview_path_bytes_hex;
	uint64_t preview_entry_device;
	uint64_t preview_entry_inode;
	uint64_t preview_entry_ctime_unix_ns;
	int owns_preview_fields;
	nv_open_intent_t open_intent;
	nv_session_pane_t open_pane;
	char *open_cwd_bytes_hex;
	uint64_t open_snapshot_revision;
	uint64_t open_cwd_device;
	uint64_t open_cwd_inode;
	uint64_t open_cwd_ctime_unix_ns;
	char *open_path_bytes_hex;
	uint64_t open_entry_device;
	uint64_t open_entry_inode;
	uint64_t open_entry_ctime_unix_ns;
	char **open_association_argv;
	size_t open_association_argc;
	int owns_open_fields;
} nv_session_command_t;

typedef struct
{
	char *path;
	char *name;
	nv_fs_identity_t identity;
	nv_entry_kind_t kind;
	uint64_t size_bytes;
	int size_known;
} nv_session_prepared_target_t;

typedef struct
{
	nv_session_command_kind_t kind;
	nv_session_pane_t pane;
	char *source_directory;
	char *destination_directory;
	nv_fs_identity_t source_directory_identity;
	nv_fs_identity_t destination_directory_identity;
	char *name;
	nv_session_prepared_target_t *targets;
	size_t target_count;
} nv_session_prepared_action_t;

typedef struct
{
	int active;
	nv_session_resource_kind_t kind;
	char *origin_directory;
	char *origin_cwd_bytes_hex;
	char *origin_entry_path_bytes_hex;
	char *remote;
	char *mount_point;
	char *unmount_path;
} nv_session_resource_t;

typedef struct
{
	uint64_t id;
	nv_pane_snapshot_t snapshot;
	nv_session_resource_t resource;
} nv_session_tab_t;

typedef struct
{
	nv_session_tab_t items[NV_SESSION_MAX_TABS];
	size_t count;
	size_t active;
} nv_session_tabs_t;

typedef struct
{
	uint64_t tab_id;
	char *directory_bytes_hex;
	char *entry_path_bytes_hex;
} nv_session_cursor_history_entry_t;

typedef struct
{
	nv_session_cursor_history_entry_t entries[NV_SESSION_MAX_CURSOR_HISTORY];
	size_t count;
} nv_session_cursor_history_t;

typedef struct
{
	nv_pane_snapshot_t left;
	nv_pane_snapshot_t right;
	nv_session_pane_t active_pane;
	uint64_t next_snapshot_revision;
	nv_session_tabs_t left_tabs;
	nv_session_tabs_t right_tabs;
	nv_session_cursor_history_t left_cursor_history;
	nv_session_cursor_history_t right_cursor_history;
	char *left_search_query;
	char *right_search_query;
	int left_search_direction;
	int right_search_direction;
	uint64_t next_tab_id;
} nv_workspace_session_t;

int nv_workspace_session_init(const char left_path[], const char right_path[],
		nv_workspace_session_t *session, nv_snapshot_error_t *error);
int nv_workspace_session_apply(nv_workspace_session_t *session,
		const nv_session_command_t *command, nv_snapshot_error_t *error);
int nv_workspace_session_prepare_action(const nv_workspace_session_t *session,
		const nv_session_command_t *command,
		nv_session_prepared_action_t *action, nv_snapshot_error_t *error);
void nv_session_prepared_action_free(nv_session_prepared_action_t *action);
int nv_workspace_session_refresh_pane(nv_workspace_session_t *session,
		nv_session_pane_t pane, nv_snapshot_error_t *error);
int nv_workspace_session_refresh_tab(nv_workspace_session_t *session,
		nv_session_pane_t pane, uint64_t tab_id, nv_snapshot_error_t *error);
const nv_pane_snapshot_t *nv_workspace_session_active(
		const nv_workspace_session_t *session);
const char *nv_workspace_session_active_name(const nv_workspace_session_t *session);
size_t nv_workspace_session_tab_count(const nv_workspace_session_t *session,
		nv_session_pane_t pane);
size_t nv_workspace_session_active_tab_index(
		const nv_workspace_session_t *session, nv_session_pane_t pane);
uint64_t nv_workspace_session_tab_id(const nv_workspace_session_t *session,
		nv_session_pane_t pane, size_t index);
const nv_pane_snapshot_t *nv_workspace_session_tab_snapshot(
		const nv_workspace_session_t *session, nv_session_pane_t pane,
		size_t index);
const nv_session_resource_t *nv_workspace_session_tab_resource(
		const nv_workspace_session_t *session, nv_session_pane_t pane,
		size_t index);
int nv_workspace_session_attach_resource(nv_workspace_session_t *session,
		nv_session_pane_t pane, uint64_t tab_id, nv_session_resource_kind_t kind,
		const char origin_directory[], const char origin_cwd_bytes_hex[],
		const char origin_entry_path_bytes_hex[], const char remote[],
		const char mount_point[], const char unmount_path[],
		nv_snapshot_error_t *error);
int nv_workspace_session_detach_resource(nv_workspace_session_t *session,
		nv_session_pane_t pane, uint64_t tab_id, nv_snapshot_error_t *error);
/* Persist and restore the local workspace layout using a bounded JSON file.
 * Missing or invalid state is reported as 1 and is safe to ignore at startup;
 * successful restore returns 0 and allocation/I/O failures return -1. */
int nv_workspace_session_save_state(const nv_workspace_session_t *session,
		const char path[], nv_snapshot_error_t *error);
int nv_workspace_session_load_state(nv_workspace_session_t *session,
		const char path[], nv_snapshot_error_t *error);
void nv_session_command_free(nv_session_command_t *command);
void nv_workspace_session_free(nv_workspace_session_t *session);

#endif /* VIFM__NEOVIFM__WORKSPACE_SESSION_H__ */
