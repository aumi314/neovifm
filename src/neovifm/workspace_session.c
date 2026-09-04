/* vifm
 * Copyright (C) 2026 NeoVifm contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "workspace_session.h"

#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef _WIN32
# include <unistd.h>
#else
# include <io.h>
#endif

#include "../compat/neovifm_fs.h"
#include "../utils/parson.h"
#include "session_platform.h"

#define NV_SESSION_STATE_VERSION 1U
#define NV_SESSION_STATE_MAX_BYTES (256U*1024U)

typedef struct
{
	char *cwd_bytes_hex;
	char *cursor_path_bytes_hex;
	nv_pane_sort_key_t sort_key;
	int sort_descending;
	int resource;
} nv_persisted_tab_t;

typedef struct
{
	nv_persisted_tab_t items[NV_SESSION_MAX_TABS];
	size_t count;
	size_t active;
} nv_persisted_tabs_t;

typedef struct
{
	nv_persisted_tabs_t panes[2];
	nv_session_pane_t active_pane;
} nv_persisted_state_t;

static int set_error(nv_snapshot_error_t *error, const char code[],
		const char message[]);
static nv_pane_snapshot_t *active_snapshot(nv_workspace_session_t *session);
static nv_pane_snapshot_t *pane_snapshot(nv_workspace_session_t *session,
		nv_session_pane_t pane);
static const nv_pane_snapshot_t *const_pane_snapshot(
		const nv_workspace_session_t *session, nv_session_pane_t pane);
static nv_session_tabs_t *pane_tabs(nv_workspace_session_t *session,
		nv_session_pane_t pane);
static const nv_session_tabs_t *const_pane_tabs(
		const nv_workspace_session_t *session, nv_session_pane_t pane);
static int ensure_tabs(nv_workspace_session_t *session,
		nv_snapshot_error_t *error);
static int clone_snapshot(const nv_pane_snapshot_t *source,
		nv_pane_snapshot_t *clone, nv_snapshot_error_t *error);
static void resource_free(nv_session_resource_t *resource);
static nv_pane_snapshot_t *tab_snapshot(nv_workspace_session_t *session,
		nv_session_pane_t pane, size_t index);
static nv_session_resource_t *tab_resource(nv_workspace_session_t *session,
		nv_session_pane_t pane, size_t index);
static nv_session_cursor_history_t *pane_cursor_history(
		nv_workspace_session_t *session, nv_session_pane_t pane);
static void cursor_history_free(nv_session_cursor_history_t *history);
static int cursor_history_record(nv_session_cursor_history_t *history,
		uint64_t tab_id, const char directory_bytes_hex[],
		const char entry_path_bytes_hex[], nv_snapshot_error_t *error);
static const char *cursor_history_lookup(
		const nv_session_cursor_history_t *history, uint64_t tab_id,
		const char directory_bytes_hex[]);
static char **pane_search_query(nv_workspace_session_t *session,
		nv_session_pane_t pane);
static int *pane_search_direction(nv_workspace_session_t *session,
		nv_session_pane_t pane);
static int search_snapshot(nv_pane_snapshot_t *snapshot, const char query[],
		int direction, nv_snapshot_error_t *error);
static int find_tab_index(const nv_session_tabs_t *tabs, uint64_t id,
		size_t *index);
static int replace_snapshot(nv_workspace_session_t *session,
		nv_pane_snapshot_t *current, nv_session_pane_t pane, uint64_t tab_id,
		const char path[],
		nv_snapshot_error_t *error);
static char *hex_decode(const char hex[]);
static char *parent_path(const char path[]);
static int refresh_snapshot(nv_workspace_session_t *session,
		nv_pane_snapshot_t *snapshot, nv_session_pane_t pane, uint64_t tab_id,
		nv_snapshot_error_t *error);
static int hex_digit(char character);
static void persisted_state_free(nv_persisted_state_t *state);
static int write_state_json_atomic(const JSON_Value *value, const char path[]);
static int new_tab(nv_workspace_session_t *session, nv_session_pane_t pane,
		nv_snapshot_error_t *error);
static int activate_tab_at(nv_workspace_session_t *session,
		nv_session_pane_t pane, size_t index, int focus_pane,
		nv_snapshot_error_t *error);
static int restore_persisted_pane(nv_workspace_session_t *session,
		nv_session_pane_t pane, const nv_persisted_tabs_t *stored,
		nv_snapshot_error_t *error);

static void
resource_free(nv_session_resource_t *resource)
{
	if(resource == NULL) return;
	free(resource->origin_directory);
	free(resource->origin_cwd_bytes_hex);
	free(resource->origin_entry_path_bytes_hex);
	free(resource->remote);
	free(resource->mount_point);
	free(resource->unmount_path);
	*resource = (nv_session_resource_t){};
}

static int
set_error(nv_snapshot_error_t *error, const char code[], const char message[])
{
	if(error == NULL)
	{
		return -1;
	}
	nv_snapshot_error_free(error);
	error->code = strdup(code);
	error->message = strdup(message);
	if(error->code == NULL || error->message == NULL)
	{
		nv_snapshot_error_free(error);
		return -1;
	}
	return -1;
}

static nv_pane_snapshot_t *
active_snapshot(nv_workspace_session_t *session)
{
	return session->active_pane == NV_SESSION_LEFT ? &session->left : &session->right;
}

static int
valid_pane(nv_session_pane_t pane)
{
	return pane == NV_SESSION_LEFT || pane == NV_SESSION_RIGHT;
}

static nv_pane_snapshot_t *
pane_snapshot(nv_workspace_session_t *session, nv_session_pane_t pane)
{
	return pane == NV_SESSION_LEFT ? &session->left : &session->right;
}

static const nv_pane_snapshot_t *
const_pane_snapshot(const nv_workspace_session_t *session,
		nv_session_pane_t pane)
{
	return pane == NV_SESSION_LEFT ? &session->left : &session->right;
}

static nv_session_tabs_t *
pane_tabs(nv_workspace_session_t *session, nv_session_pane_t pane)
{
	return pane == NV_SESSION_LEFT ? &session->left_tabs : &session->right_tabs;
}

static const nv_session_tabs_t *
const_pane_tabs(const nv_workspace_session_t *session, nv_session_pane_t pane)
{
	return pane == NV_SESSION_LEFT ? &session->left_tabs : &session->right_tabs;
}

static nv_pane_snapshot_t *
tab_snapshot(nv_workspace_session_t *session, nv_session_pane_t pane,
		size_t index)
{
	nv_session_tabs_t *const tabs = pane_tabs(session, pane);
	if(tabs->count == 0U || index >= tabs->count) return NULL;
	return index == tabs->active ? pane_snapshot(session, pane) :
		&tabs->items[index].snapshot;
}

static nv_session_resource_t *
tab_resource(nv_workspace_session_t *session, nv_session_pane_t pane,
		size_t index)
{
	nv_session_tabs_t *const tabs = pane_tabs(session, pane);
	if(tabs->count == 0U || index >= tabs->count) return NULL;
	return &tabs->items[index].resource;
}

static nv_session_cursor_history_t *
pane_cursor_history(nv_workspace_session_t *session, nv_session_pane_t pane)
{
	return pane == NV_SESSION_LEFT ? &session->left_cursor_history :
		&session->right_cursor_history;
}

static void
cursor_history_free(nv_session_cursor_history_t *history)
{
	if(history == NULL) return;
	for(size_t i = 0U; i < history->count; ++i)
	{
		free(history->entries[i].directory_bytes_hex);
		free(history->entries[i].entry_path_bytes_hex);
	}
	*history = (nv_session_cursor_history_t){};
}

static int
cursor_history_record(nv_session_cursor_history_t *history, uint64_t tab_id,
		const char directory_bytes_hex[], const char entry_path_bytes_hex[],
		nv_snapshot_error_t *error)
{
	if(history == NULL || tab_id == 0U || directory_bytes_hex == NULL ||
			entry_path_bytes_hex == NULL)
		return set_error(error, "invalid-cursor-history",
				"cursor history entry is invalid");
	for(size_t i = 0U; i < history->count; ++i)
	{
		if(history->entries[i].tab_id == tab_id &&
				strcmp(history->entries[i].directory_bytes_hex,
					directory_bytes_hex) == 0)
		{
			char *const replacement = strdup(entry_path_bytes_hex);
			if(replacement == NULL)
				return set_error(error, "out-of-memory",
						"failed to update cursor history");
			free(history->entries[i].entry_path_bytes_hex);
			history->entries[i].entry_path_bytes_hex = replacement;
			return 0;
		}
	}
	char *const directory = strdup(directory_bytes_hex);
	char *const entry = strdup(entry_path_bytes_hex);
	if(directory == NULL || entry == NULL)
	{
		free(directory);
		free(entry);
		return set_error(error, "out-of-memory",
				"failed to allocate cursor history");
	}
	if(history->count == NV_SESSION_MAX_CURSOR_HISTORY)
	{
		free(history->entries[history->count - 1U].directory_bytes_hex);
		free(history->entries[history->count - 1U].entry_path_bytes_hex);
		--history->count;
	}
	for(size_t i = history->count; i > 0U; --i)
		history->entries[i] = history->entries[i - 1U];
	history->entries[0] = (nv_session_cursor_history_entry_t){
		.tab_id = tab_id,
		.directory_bytes_hex = directory,
		.entry_path_bytes_hex = entry,
	};
	++history->count;
	return 0;
}

static const char *
cursor_history_lookup(const nv_session_cursor_history_t *history,
		uint64_t tab_id, const char directory_bytes_hex[])
{
	if(history == NULL || tab_id == 0U || directory_bytes_hex == NULL) return NULL;
	for(size_t i = 0U; i < history->count; ++i)
	{
		if(history->entries[i].tab_id == tab_id &&
				strcmp(history->entries[i].directory_bytes_hex,
					directory_bytes_hex) == 0)
			return history->entries[i].entry_path_bytes_hex;
	}
	return NULL;
}

static char **
pane_search_query(nv_workspace_session_t *session, nv_session_pane_t pane)
{
	return pane == NV_SESSION_LEFT ? &session->left_search_query :
		&session->right_search_query;
}

static int *
pane_search_direction(nv_workspace_session_t *session, nv_session_pane_t pane)
{
	return pane == NV_SESSION_LEFT ? &session->left_search_direction :
		&session->right_search_direction;
}

static int
name_contains_case_insensitive(const char name[], const char query[])
{
	if(name == NULL || query == NULL || query[0] == '\0') return 0;
	for(size_t start = 0U; name[start] != '\0'; ++start)
	{
		size_t offset = 0U;
		while(query[offset] != '\0' && name[start + offset] != '\0' &&
				tolower((unsigned char)name[start + offset]) ==
					tolower((unsigned char)query[offset])) ++offset;
		if(query[offset] == '\0') return 1;
	}
	return 0;
}

static int
search_snapshot(nv_pane_snapshot_t *snapshot, const char query[], int direction,
		nv_snapshot_error_t *error)
{
	if(snapshot == NULL || query == NULL || query[0] == '\0' ||
			(direction != -1 && direction != 1))
		return set_error(error, "invalid-search", "search query is invalid");
	if(snapshot->entry_count == 0U)
		return set_error(error, "search-not-found", "no search match");
	const size_t current = snapshot->cursor < 0 ?
		(direction > 0 ? snapshot->entry_count - 1U : 0U) :
		(size_t)snapshot->cursor;
	for(size_t step = 1U; step <= snapshot->entry_count; ++step)
	{
		const size_t index = direction > 0 ?
			(current + step) % snapshot->entry_count :
			(current + snapshot->entry_count - step % snapshot->entry_count) %
				snapshot->entry_count;
		if(name_contains_case_insensitive(snapshot->entries[index].name_display,
				query))
		{
			snapshot->cursor = (int)index;
			return 0;
		}
	}
	return set_error(error, "search-not-found", "no search match");
}

static uint64_t
next_tab_id(nv_workspace_session_t *session)
{
	if(session->next_tab_id == 0U) return 0U;
	return session->next_tab_id++;
}

static int
ensure_tabs(nv_workspace_session_t *session, nv_snapshot_error_t *error)
{
	if(session->left_tabs.count > NV_SESSION_MAX_TABS ||
			session->right_tabs.count > NV_SESSION_MAX_TABS)
	{
		return set_error(error, "invalid-tab", "session tab state is invalid");
	}
	if(session->left_tabs.count != 0U &&
			session->left_tabs.active >= session->left_tabs.count)
	{
		return set_error(error, "invalid-tab", "left tab state is invalid");
	}
	if(session->right_tabs.count != 0U &&
			session->right_tabs.active >= session->right_tabs.count)
	{
		return set_error(error, "invalid-tab", "right tab state is invalid");
	}
	if(session->next_tab_id == 0U && session->left_tabs.count == 0U &&
			session->right_tabs.count == 0U)
	{
		session->next_tab_id = 1U;
	}
	if(session->next_snapshot_revision == 0U)
	{
		const uint64_t latest = session->left.snapshot_revision >
				session->right.snapshot_revision ? session->left.snapshot_revision :
				session->right.snapshot_revision;
		session->next_snapshot_revision = latest == UINT64_MAX ? 1U : latest + 1U;
	}
	for(nv_session_pane_t pane = NV_SESSION_LEFT; pane <= NV_SESSION_RIGHT; ++pane)
	{
		nv_session_tabs_t *const tabs = pane_tabs(session, pane);
		if(tabs->count != 0U) continue;
		const uint64_t id = next_tab_id(session);
		if(id == 0U)
		{
			return set_error(error, "tab-id-exhausted",
					"no more tab identifiers are available");
		}
		tabs->items[0].id = id;
		tabs->count = 1U;
		tabs->active = 0U;
	}
	return 0;
}

const nv_pane_snapshot_t *
nv_workspace_session_active(const nv_workspace_session_t *session)
{
	if(session == NULL)
	{
		return NULL;
	}
	return session->active_pane == NV_SESSION_LEFT ? &session->left : &session->right;
}

const char *
nv_workspace_session_active_name(const nv_workspace_session_t *session)
{
	return session != NULL && session->active_pane == NV_SESSION_RIGHT ? "right" : "left";
}

size_t
nv_workspace_session_tab_count(const nv_workspace_session_t *session,
		nv_session_pane_t pane)
{
	if(session == NULL || !valid_pane(pane)) return 0U;
	const size_t count = const_pane_tabs(session, pane)->count;
	return count == 0U ? 1U : count;
}

size_t
nv_workspace_session_active_tab_index(const nv_workspace_session_t *session,
		nv_session_pane_t pane)
{
	if(session == NULL || !valid_pane(pane)) return 0U;
	const nv_session_tabs_t *const tabs = const_pane_tabs(session, pane);
	return tabs->count == 0U ? 0U : tabs->active;
}

uint64_t
nv_workspace_session_tab_id(const nv_workspace_session_t *session,
		nv_session_pane_t pane, size_t index)
{
	if(session == NULL || !valid_pane(pane) ||
			index >= nv_workspace_session_tab_count(session, pane)) return 0U;
	const nv_session_tabs_t *const tabs = const_pane_tabs(session, pane);
	if(tabs->count == 0U) return pane == NV_SESSION_LEFT ? 1U : 2U;
	return tabs->items[index].id;
}

const nv_pane_snapshot_t *
nv_workspace_session_tab_snapshot(const nv_workspace_session_t *session,
		nv_session_pane_t pane, size_t index)
{
	if(session == NULL || !valid_pane(pane) ||
			index >= nv_workspace_session_tab_count(session, pane)) return NULL;
	const nv_session_tabs_t *const tabs = const_pane_tabs(session, pane);
	if(tabs->count == 0U || index == tabs->active)
	{
		return const_pane_snapshot(session, pane);
	}
	return &tabs->items[index].snapshot;
}

const nv_session_resource_t *
nv_workspace_session_tab_resource(const nv_workspace_session_t *session,
		nv_session_pane_t pane, size_t index)
{
	if(session == NULL || !valid_pane(pane) ||
			index >= nv_workspace_session_tab_count(session, pane)) return NULL;
	const nv_session_tabs_t *const tabs = const_pane_tabs(session, pane);
	if(tabs->count == 0U) return NULL;
	return &tabs->items[index].resource;
}

int
nv_workspace_session_init(const char left_path[], const char right_path[],
		nv_workspace_session_t *session, nv_snapshot_error_t *error)
{
	if(session == NULL || error == NULL)
	{
		return -1;
	}
	nv_workspace_session_t next = {};
	nv_snapshot_error_t next_error = {};
	if(nv_pane_snapshot_build(left_path, &next.left, &next_error) != 0 ||
			nv_pane_snapshot_build(right_path, &next.right, &next_error) != 0)
	{
		nv_pane_snapshot_free(&next.left);
		nv_pane_snapshot_free(&next.right);
		nv_snapshot_error_free(error);
		*error = next_error;
		return -1;
	}
	next.next_snapshot_revision = 1U;
	next.left.snapshot_revision = next.next_snapshot_revision++;
	next.right.snapshot_revision = next.next_snapshot_revision++;
	next.left_tabs.items[0].id = 1U;
	next.left_tabs.count = 1U;
	next.right_tabs.items[0].id = 2U;
	next.right_tabs.count = 1U;
	next.next_tab_id = 3U;
	nv_workspace_session_free(session);
	nv_snapshot_error_free(error);
	*session = next;
	return 0;
}

static int
clone_string(char **destination, const char source[])
{
	*destination = source == NULL ? NULL : strdup(source);
	return source == NULL || *destination != NULL ? 0 : -1;
}

static int
clone_snapshot(const nv_pane_snapshot_t *source, nv_pane_snapshot_t *clone,
		nv_snapshot_error_t *error)
{
	nv_pane_snapshot_t next = *source;
	next.cwd_display = NULL;
	next.cwd_bytes_hex = NULL;
	next.entries = NULL;
	next.entry_count = 0U;
	if(clone_string(&next.cwd_display, source->cwd_display) != 0 ||
			clone_string(&next.cwd_bytes_hex, source->cwd_bytes_hex) != 0)
	{
		goto failed;
	}
	if(source->entry_count != 0U)
	{
		next.entries = calloc(source->entry_count, sizeof(*next.entries));
		if(next.entries == NULL) goto failed;
	}
	for(size_t i = 0U; i < source->entry_count; ++i)
	{
		next.entries[i] = source->entries[i];
		next.entries[i].name_display = NULL;
		next.entries[i].name_bytes_hex = NULL;
		next.entries[i].path_display = NULL;
		next.entries[i].path_bytes_hex = NULL;
		next.entries[i].owner_display = NULL;
		next.entries[i].group_display = NULL;
		if(clone_string(&next.entries[i].name_display,
				source->entries[i].name_display) != 0 ||
				clone_string(&next.entries[i].name_bytes_hex,
					source->entries[i].name_bytes_hex) != 0 ||
				clone_string(&next.entries[i].path_display,
					source->entries[i].path_display) != 0 ||
				clone_string(&next.entries[i].path_bytes_hex,
					source->entries[i].path_bytes_hex) != 0 ||
				clone_string(&next.entries[i].owner_display,
					source->entries[i].owner_display) != 0 ||
				clone_string(&next.entries[i].group_display,
					source->entries[i].group_display) != 0)
		{
			next.entry_count = i + 1U;
			goto failed;
		}
		next.entry_count = i + 1U;
	}
	nv_pane_snapshot_free(clone);
	*clone = next;
	return 0;

failed:
	nv_pane_snapshot_free(&next);
	return set_error(error, "out-of-memory", "failed to clone pane snapshot");
}

static int
replace_snapshot(nv_workspace_session_t *session, nv_pane_snapshot_t *current,
		nv_session_pane_t pane, uint64_t tab_id, const char path[],
		nv_snapshot_error_t *error)
{
	const nv_pane_sort_key_t sort_key = current->sort_key;
	const int sort_descending = current->sort_descending;
	char *const cursor_name = current->cursor < 0 ||
			(size_t)current->cursor >= current->entry_count ? NULL :
			strdup(current->entries[current->cursor].name_bytes_hex);
	char *const cursor_path = current->cursor < 0 ||
			(size_t)current->cursor >= current->entry_count ? NULL :
			strdup(current->entries[current->cursor].path_bytes_hex);
	nv_pane_snapshot_t next = {};
	if(nv_pane_snapshot_build(path, &next, error) != 0)
	{
		free(cursor_name);
		free(cursor_path);
		return -1;
	}
	if(cursor_path != NULL && cursor_history_record(
			pane_cursor_history(session, pane), tab_id,
				current->cwd_bytes_hex, cursor_path, error) != 0)
	{
		free(cursor_name);
		free(cursor_path);
		nv_pane_snapshot_free(&next);
		return -1;
	}
	const char *const remembered_path = cursor_history_lookup(
			pane_cursor_history(session, pane), tab_id, next.cwd_bytes_hex);
	for(size_t i = 0U; i < next.entry_count; ++i)
	{
		for(size_t j = 0U; j < current->entry_count; ++j)
		{
			if(strcmp(next.entries[i].name_bytes_hex,
					current->entries[j].name_bytes_hex) == 0)
			{
				next.entries[i].selected = current->entries[j].selected;
				next.selection_count += next.entries[i].selected != 0;
				if(cursor_name != NULL && strcmp(next.entries[i].name_bytes_hex,
						cursor_name) == 0) next.cursor = (int)i;
				break;
			}
		}
		if(remembered_path != NULL && strcmp(next.entries[i].path_bytes_hex,
				remembered_path) == 0) next.cursor = (int)i;
	}
	nv_pane_snapshot_sort(&next, sort_key, sort_descending);
	next.snapshot_revision = session->next_snapshot_revision++;
	free(cursor_name);
	nv_pane_snapshot_free(current);
	*current = next;
	free(cursor_path);
	return 0;
}

static int
refresh_snapshot(nv_workspace_session_t *session, nv_pane_snapshot_t *snapshot,
		nv_session_pane_t pane, uint64_t tab_id, nv_snapshot_error_t *error)
{
	char *const path = hex_decode(snapshot->cwd_bytes_hex);
	if(path == NULL)
	{
		return set_error(error, "invalid-path", "directory identity is invalid");
	}
	const int result = replace_snapshot(session, snapshot, pane, tab_id, path,
			error);
	free(path);
	return result;
}

int
nv_workspace_session_refresh_pane(nv_workspace_session_t *session,
		nv_session_pane_t pane, nv_snapshot_error_t *error)
{
	if(session == NULL || error == NULL ||
			(pane != NV_SESSION_LEFT && pane != NV_SESSION_RIGHT))
	{
		return -1;
	}
	nv_snapshot_error_free(error);
	nv_pane_snapshot_t *const snapshot = pane == NV_SESSION_LEFT ?
		&session->left : &session->right;
	if(ensure_tabs(session, error) != 0) return -1;
	return refresh_snapshot(session, snapshot, pane,
		nv_workspace_session_tab_id(session, pane,
		nv_workspace_session_active_tab_index(session, pane)), error);
}

int
nv_workspace_session_refresh_tab(nv_workspace_session_t *session,
		nv_session_pane_t pane, uint64_t tab_id, nv_snapshot_error_t *error)
{
	if(session == NULL || error == NULL || !valid_pane(pane)) return -1;
	nv_snapshot_error_free(error);
	if(ensure_tabs(session, error) != 0) return -1;
	nv_session_tabs_t *const tabs = pane_tabs(session, pane);
	size_t index = 0U;
	if(find_tab_index(tabs, tab_id, &index) != 0)
		return set_error(error, "invalid-tab", "tab does not exist");
	nv_pane_snapshot_t *const snapshot = index == tabs->active ?
		pane_snapshot(session, pane) : &tabs->items[index].snapshot;
	return refresh_snapshot(session, snapshot, pane, tab_id, error);
}

int
nv_workspace_session_attach_resource(nv_workspace_session_t *session,
		nv_session_pane_t pane, uint64_t tab_id, nv_session_resource_kind_t kind,
		const char origin_directory[], const char origin_cwd_bytes_hex[],
		const char origin_entry_path_bytes_hex[], const char remote[],
		const char mount_point[], const char unmount_path[],
		nv_snapshot_error_t *error)
{
	if(session == NULL || error == NULL || !valid_pane(pane) || tab_id == 0U ||
			(kind != NV_SESSION_RESOURCE_ARCHIVE && kind != NV_SESSION_RESOURCE_SSH) ||
			origin_directory == NULL || origin_cwd_bytes_hex == NULL ||
			mount_point == NULL || mount_point[0] != '/' || unmount_path == NULL ||
			unmount_path[0] != '/')
	{
		return set_error(error, "invalid-resource", "resource attachment is invalid");
	}
	if(kind == NV_SESSION_RESOURCE_ARCHIVE && origin_entry_path_bytes_hex == NULL)
		return set_error(error, "invalid-resource", "archive origin is missing");
	if(kind == NV_SESSION_RESOURCE_SSH && remote == NULL)
		return set_error(error, "invalid-resource", "ssh origin is missing");
	nv_snapshot_error_free(error);
	if(ensure_tabs(session, error) != 0) return -1;
	nv_session_tabs_t *const tabs = pane_tabs(session, pane);
	size_t index = 0U;
	if(find_tab_index(tabs, tab_id, &index) != 0)
		return set_error(error, "invalid-tab", "resource tab does not exist");
	nv_pane_snapshot_t *const snapshot = tab_snapshot(session, pane, index);
	nv_session_resource_t *const resource = tab_resource(session, pane, index);
	if(snapshot == NULL || resource == NULL || resource->active ||
			strcmp(snapshot->cwd_bytes_hex, origin_cwd_bytes_hex) != 0)
		return set_error(error, "stale-resource", "resource origin is no longer active");
	nv_session_resource_t next = {
		.active = 1,
		.kind = kind,
		.origin_directory = strdup(origin_directory),
		.origin_cwd_bytes_hex = strdup(origin_cwd_bytes_hex),
		.origin_entry_path_bytes_hex = origin_entry_path_bytes_hex == NULL ? NULL :
			strdup(origin_entry_path_bytes_hex),
		.remote = remote == NULL ? NULL : strdup(remote),
		.mount_point = strdup(mount_point),
		.unmount_path = strdup(unmount_path),
	};
	if(next.origin_directory == NULL || next.origin_cwd_bytes_hex == NULL ||
			(next.origin_entry_path_bytes_hex == NULL &&
				kind == NV_SESSION_RESOURCE_ARCHIVE) ||
			(next.remote == NULL && kind == NV_SESSION_RESOURCE_SSH) ||
			next.mount_point == NULL || next.unmount_path == NULL)
	{
		resource_free(&next);
		return set_error(error, "out-of-memory", "failed to store resource origin");
	}
	if(replace_snapshot(session, snapshot, pane, tab_id, mount_point, error) != 0)
	{
		resource_free(&next);
		return -1;
	}
	resource_free(resource);
	*resource = next;
	return 0;
}

int
nv_workspace_session_detach_resource(nv_workspace_session_t *session,
		nv_session_pane_t pane, uint64_t tab_id, nv_snapshot_error_t *error)
{
	if(session == NULL || error == NULL || !valid_pane(pane) || tab_id == 0U)
		return set_error(error, "invalid-resource", "resource detachment is invalid");
	nv_snapshot_error_free(error);
	if(ensure_tabs(session, error) != 0) return -1;
	nv_session_tabs_t *const tabs = pane_tabs(session, pane);
	size_t index = 0U;
	if(find_tab_index(tabs, tab_id, &index) != 0)
		return set_error(error, "invalid-tab", "resource tab does not exist");
	nv_pane_snapshot_t *const snapshot = tab_snapshot(session, pane, index);
	nv_session_resource_t *const resource = tab_resource(session, pane, index);
	if(snapshot == NULL || resource == NULL || !resource->active ||
			resource->origin_directory == NULL)
		return set_error(error, "resource-not-mounted", "tab has no mounted resource");
	char *const origin_directory = strdup(resource->origin_directory);
	char *const origin_entry = resource->origin_entry_path_bytes_hex == NULL ? NULL :
		strdup(resource->origin_entry_path_bytes_hex);
	if(origin_directory == NULL ||
			(resource->origin_entry_path_bytes_hex != NULL && origin_entry == NULL))
	{
		free(origin_directory);
		free(origin_entry);
		return set_error(error, "out-of-memory", "failed to restore resource origin");
	}
	if(replace_snapshot(session, snapshot, pane, tab_id, origin_directory,
			error) != 0)
	{
		free(origin_directory);
		free(origin_entry);
		return -1;
	}
	if(origin_entry != NULL)
	{
		for(size_t i = 0U; i < snapshot->entry_count; ++i)
		{
			if(strcmp(snapshot->entries[i].path_bytes_hex, origin_entry) == 0)
			{
				snapshot->cursor = (int)i;
				break;
			}
		}
	}
	free(origin_directory);
	free(origin_entry);
	resource_free(resource);
	return 0;
}

static const char *
persisted_sort_key_name(nv_pane_sort_key_t key)
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

static int
persisted_sort_key(const char name[], nv_pane_sort_key_t *key)
{
	if(name == NULL || key == NULL) return -1;
	static const struct
	{
		const char *name;
		nv_pane_sort_key_t key;
	} keys[] = {
		{ "name", NV_SORT_NAME }, { "extension", NV_SORT_EXTENSION },
		{ "size", NV_SORT_SIZE }, { "ctime", NV_SORT_CTIME },
		{ "mtime", NV_SORT_MTIME }, { "mode", NV_SORT_MODE },
		{ "type", NV_SORT_TYPE }, { "other", NV_SORT_OTHER },
	};
	for(size_t i = 0U; i < sizeof(keys)/sizeof(keys[0]); ++i)
	{
		if(strcmp(name, keys[i].name) == 0)
		{
			*key = keys[i].key;
			return 0;
		}
	}
	return -1;
}

static void
persisted_tab_free(nv_persisted_tab_t *tab)
{
	if(tab == NULL) return;
	free(tab->cwd_bytes_hex);
	free(tab->cursor_path_bytes_hex);
	*tab = (nv_persisted_tab_t){};
}

static void
persisted_state_free(nv_persisted_state_t *state)
{
	if(state == NULL) return;
	for(size_t pane = 0U; pane < 2U; ++pane)
	{
		for(size_t i = 0U; i < state->panes[pane].count; ++i)
			persisted_tab_free(&state->panes[pane].items[i]);
		state->panes[pane] = (nv_persisted_tabs_t){};
	}
	*state = (nv_persisted_state_t){};
}

static int
write_state_json_atomic(const JSON_Value *value, const char path[])
{
	char *const serialized = json_serialize_to_string_pretty(value);
	if(serialized == NULL) return -1;
	char *temporary = NULL;
	const int descriptor = nv_session_open_temporary(path, &temporary);
	FILE *const file = descriptor < 0 ? NULL :
#ifndef _WIN32
		fdopen(descriptor, "wb");
#else
		_fdopen(descriptor, "wb");
#endif
	if(file == NULL)
	{
		if(descriptor >= 0)
#ifndef _WIN32
			close(descriptor);
#else
			_close(descriptor);
#endif
		if(descriptor >= 0) nv_session_remove_file(temporary);
		free(temporary);
		json_free_serialized_string(serialized);
		return -1;
	}
	const size_t length = strlen(serialized);
	int result = fwrite(serialized, 1U, length, file) == length &&
		fflush(file) == 0;
#ifndef _WIN32
	if(result && fsync(fileno(file)) != 0) result = 0;
#else
	if(result && _commit(_fileno(file)) != 0) result = 0;
#endif
	if(fclose(file) != 0) result = 0;
	if(result && nv_session_replace_file(temporary, path) != 0) result = 0;
	if(!result) nv_session_remove_file(temporary);
	free(temporary);
	json_free_serialized_string(serialized);
	return result ? 0 : -1;
}

static int
persisted_hex_is_valid(const char value[])
{
	if(value == NULL || value[0] == '\0' || strlen(value) > NV_PANE_SNAPSHOT_MAX_HEX_BYTES ||
			strlen(value) % 2U != 0U)
		return 0;
	for(size_t i = 0U; value[i] != '\0'; ++i)
	{
		if(hex_digit(value[i]) < 0) return 0;
	}
	return 1;
}

static int
persisted_directory_exists(const char bytes_hex[])
{
	char *const path = hex_decode(bytes_hex);
	if(path == NULL) return 0;
	const int result = nv_session_directory_exists(path);
	free(path);
	return result;
}

static int
parse_persisted_tabs(JSON_Object *root, const char field[],
		nv_persisted_tabs_t *tabs)
{
	JSON_Array *const array = json_object_get_array(root, field);
	if(array == NULL || json_array_get_count(array) == 0U ||
			json_array_get_count(array) > NV_SESSION_MAX_TABS)
		return -1;
	for(size_t i = 0U; i < json_array_get_count(array); ++i)
	{
		JSON_Object *const object = json_array_get_object(array, i);
		if(object == NULL) return -1;
		if(json_object_has_value_of_type(object, "resource", JSONBoolean) &&
				json_object_get_boolean(object, "resource") == 1)
		{
			tabs->items[tabs->count++].resource = 1;
			continue;
		}
		const char *const cwd = json_object_get_string(object, "cwd_bytes_hex");
		if(!persisted_hex_is_valid(cwd)) return -1;
		nv_persisted_tab_t *const tab = &tabs->items[tabs->count];
		tab->cwd_bytes_hex = strdup(cwd);
		if(tab->cwd_bytes_hex == NULL) return -2;
		tab->sort_key = NV_SORT_NAME;
		const char *const sort = json_object_get_string(object, "sort_key");
		if(sort != NULL && persisted_sort_key(sort, &tab->sort_key) != 0)
		{
			persisted_tab_free(tab);
			return -1;
		}
		if(json_object_has_value(object, "sort_descending"))
		{
			if(!json_object_has_value_of_type(object, "sort_descending", JSONBoolean))
			{
				persisted_tab_free(tab);
				return -1;
			}
			tab->sort_descending = json_object_get_boolean(object,
					"sort_descending");
		}
		const char *const cursor = json_object_get_string(object,
				"cursor_path_bytes_hex");
		if(cursor != NULL)
		{
			if(!persisted_hex_is_valid(cursor))
			{
				persisted_tab_free(tab);
				return -1;
			}
			tab->cursor_path_bytes_hex = strdup(cursor);
			if(tab->cursor_path_bytes_hex == NULL)
			{
				persisted_tab_free(tab);
				return -2;
			}
		}
		++tabs->count;
	}
	if(tabs->count == 0U) return -1;
	return 0;
}

static int
parse_persisted_state(const char path[], nv_persisted_state_t *state)
{
	FILE *const file = nv_session_fopen(path, "rb");
	if(file == NULL) return 1;
	struct stat info = {};
#ifndef _WIN32
	const int descriptor = fileno(file);
#else
	const int descriptor = _fileno(file);
#endif
	if(descriptor < 0 || fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) ||
			info.st_size < 0 || (uintmax_t)info.st_size > NV_SESSION_STATE_MAX_BYTES)
	{
		fclose(file);
		return 1;
	}
	const size_t length = (size_t)info.st_size;
	char *const contents = malloc(length + 1U);
	if(contents == NULL)
	{
		fclose(file);
		return -1;
	}
	const int complete = fread(contents, 1U, length, file) == length &&
		fgetc(file) == EOF && !ferror(file);
	if(fclose(file) != 0 || !complete)
	{
		free(contents);
		return 1;
	}
	contents[length] = '\0';
	JSON_Value *const value = json_parse_string(contents);
	free(contents);
	JSON_Object *const root = value == NULL ? NULL : json_value_get_object(value);
	if(root == NULL || !json_object_has_value_of_type(root, "version", JSONNumber) ||
			json_object_get_number(root, "version") != NV_SESSION_STATE_VERSION ||
			!json_object_has_value_of_type(root, "active_pane", JSONString))
	{
		json_value_free(value);
		return 1;
	}
	const char *const active = json_object_get_string(root, "active_pane");
	if(strcmp(active, "left") == 0) state->active_pane = NV_SESSION_LEFT;
	else if(strcmp(active, "right") == 0) state->active_pane = NV_SESSION_RIGHT;
	else
	{
		json_value_free(value);
		return 1;
	}
	const int left_result = parse_persisted_tabs(root, "left_tabs", &state->panes[0]);
	const int right_result = parse_persisted_tabs(root, "right_tabs", &state->panes[1]);
	if(left_result != 0 || right_result != 0)
	{
		json_value_free(value);
		return left_result == -2 || right_result == -2 ? -1 : 1;
	}
	for(size_t pane = 0U; pane < 2U; ++pane)
	{
		const char *const field = pane == 0U ? "left_active" : "right_active";
		if(!json_object_has_value_of_type(root, field, JSONNumber))
		{
			json_value_free(value);
			return 1;
		}
		const double active_index = json_object_get_number(root, field);
		if(active_index < 0.0 ||
				active_index >= (double)state->panes[pane].count ||
				active_index != (double)(size_t)active_index)
		{
			json_value_free(value);
			return 1;
		}
		state->panes[pane].active = (size_t)active_index;
	}
	json_value_free(value);
	for(size_t pane = 0U; pane < 2U; ++pane)
	{
		nv_persisted_tabs_t source = state->panes[pane];
		nv_persisted_tabs_t filtered = {};
		const size_t source_active = source.active;
		state->panes[pane] = (nv_persisted_tabs_t){};
		for(size_t i = 0U; i < source.count; ++i)
		{
			nv_persisted_tab_t *const tab = &source.items[i];
			if(tab->resource || !persisted_directory_exists(tab->cwd_bytes_hex))
			{
				persisted_tab_free(tab);
				continue;
			}
			const size_t output = filtered.count++;
			filtered.items[output] = *tab;
			*tab = (nv_persisted_tab_t){};
			if(i == source_active)
				filtered.active = output;
		}
		for(size_t i = 0U; i < source.count; ++i)
			persisted_tab_free(&source.items[i]);
		state->panes[pane] = filtered;
		if(state->panes[pane].count == 0U) return 1;
		if(state->panes[pane].active >= state->panes[pane].count)
			state->panes[pane].active = 0U;
	}
	return 0;
}

static int
restore_persisted_tab(nv_workspace_session_t *session, nv_session_pane_t pane,
		size_t index, const nv_persisted_tab_t *stored, nv_snapshot_error_t *error)
{
	nv_session_tabs_t *const tabs = pane_tabs(session, pane);
	if(index >= tabs->count || stored == NULL) return -1;
	nv_pane_snapshot_t *const snapshot = tab_snapshot(session, pane, index);
	const uint64_t id = tabs->items[index].id;
	char *const path = hex_decode(stored->cwd_bytes_hex);
	if(snapshot == NULL || path == NULL)
	{
		free(path);
		return set_error(error, "invalid-path",
				"persisted directory identity is invalid");
	}
	if(replace_snapshot(session, snapshot, pane, id, path, error) != 0)
	{
		free(path);
		return -1;
	}
	free(path);
	nv_pane_snapshot_sort(snapshot, stored->sort_key, stored->sort_descending);
	if(stored->cursor_path_bytes_hex != NULL)
	{
		for(size_t i = 0U; i < snapshot->entry_count; ++i)
		{
			if(strcmp(snapshot->entries[i].path_bytes_hex,
					stored->cursor_path_bytes_hex) == 0)
			{
				snapshot->cursor = (int)i;
				break;
			}
		}
	}
	return 0;
}

static int
restore_persisted_pane(nv_workspace_session_t *session, nv_session_pane_t pane,
		const nv_persisted_tabs_t *stored, nv_snapshot_error_t *error)
{
	if(stored == NULL || stored->count == 0U) return -1;
	if(restore_persisted_tab(session, pane, 0U, &stored->items[0], error) != 0)
		return -1;
	for(size_t i = 1U; i < stored->count; ++i)
	{
		if(new_tab(session, pane, error) != 0 ||
				restore_persisted_tab(session, pane,
					nv_workspace_session_active_tab_index(session, pane),
					&stored->items[i], error) != 0)
			return -1;
	}
	return activate_tab_at(session, pane, stored->active, 0, error);
}

int
nv_workspace_session_save_state(const nv_workspace_session_t *session,
		const char path[], nv_snapshot_error_t *error)
{
	if(session == NULL || path == NULL || path[0] == '\0' || error == NULL)
		return -1;
	nv_snapshot_error_free(error);
	JSON_Value *const root_value = json_value_init_object();
	if(root_value == NULL) return set_error(error, "out-of-memory",
			"failed to allocate session state");
	JSON_Object *const root = json_value_get_object(root_value);
	const char *const active = nv_workspace_session_active_name(session);
	if(json_object_set_number(root, "version", NV_SESSION_STATE_VERSION) != JSONSuccess ||
			json_object_set_string(root, "active_pane", active) != JSONSuccess)
		goto failed;
	for(nv_session_pane_t pane = NV_SESSION_LEFT; pane <= NV_SESSION_RIGHT; ++pane)
	{
		const char *const field = pane == NV_SESSION_LEFT ? "left_tabs" : "right_tabs";
		const char *const active_field = pane == NV_SESSION_LEFT ? "left_active" :
			"right_active";
		JSON_Value *const array_value = json_value_init_array();
		if(array_value == NULL) goto failed;
		JSON_Array *const array = json_value_get_array(array_value);
		const size_t count = nv_workspace_session_tab_count(session, pane);
		for(size_t i = 0U; i < count; ++i)
		{
			JSON_Value *const tab_value = json_value_init_object();
			if(tab_value == NULL) { json_value_free(array_value); goto failed; }
			JSON_Object *const tab = json_value_get_object(tab_value);
			const nv_pane_snapshot_t *const snapshot =
				nv_workspace_session_tab_snapshot(session, pane, i);
			const nv_session_resource_t *const resource =
				nv_workspace_session_tab_resource(session, pane, i);
			const int is_resource = resource != NULL && resource->active;
			const char *const cursor = !is_resource && snapshot != NULL &&
					snapshot->cursor >= 0 && (size_t)snapshot->cursor < snapshot->entry_count ?
				snapshot->entries[snapshot->cursor].path_bytes_hex : NULL;
			if((is_resource && json_object_set_boolean(tab, "resource", 1) != JSONSuccess) ||
					(!is_resource && (snapshot == NULL ||
						json_object_set_string(tab, "cwd_bytes_hex", snapshot->cwd_bytes_hex) != JSONSuccess ||
						json_object_set_string(tab, "sort_key",
							persisted_sort_key_name(snapshot->sort_key)) != JSONSuccess ||
						json_object_set_boolean(tab, "sort_descending",
							snapshot->sort_descending) != JSONSuccess ||
						(cursor != NULL && json_object_set_string(tab,
							"cursor_path_bytes_hex", cursor) != JSONSuccess))) ||
					json_array_append_value(array, tab_value) != JSONSuccess)
			{
				if(json_value_get_parent(tab_value) == NULL) json_value_free(tab_value);
				json_value_free(array_value);
				goto failed;
			}
		}
		if(json_object_set_value(root, field, array_value) != JSONSuccess ||
			json_object_set_number(root, active_field,
				nv_workspace_session_active_tab_index(session, pane)) != JSONSuccess)
		{
			if(json_value_get_parent(array_value) == NULL) json_value_free(array_value);
			goto failed;
		}
	}
	if(json_serialization_size_pretty(root_value) == 0U ||
			json_serialization_size_pretty(root_value) - 1U > NV_SESSION_STATE_MAX_BYTES)
		goto failed;
	if(write_state_json_atomic(root_value, path) != 0) goto failed;
	json_value_free(root_value);
	return 0;

failed:
	json_value_free(root_value);
	return set_error(error, "session-state-write-failed",
			"failed to save workspace state");
}

int
nv_workspace_session_load_state(nv_workspace_session_t *session,
		const char path[], nv_snapshot_error_t *error)
{
	if(session == NULL || path == NULL || path[0] == '\0' || error == NULL)
		return -1;
	nv_snapshot_error_free(error);
	nv_persisted_state_t stored = {};
	const int parsed = parse_persisted_state(path, &stored);
	if(parsed != 0)
	{
		persisted_state_free(&stored);
		return parsed;
	}
	char *const left_path = hex_decode(stored.panes[0].items[0].cwd_bytes_hex);
	char *const right_path = hex_decode(stored.panes[1].items[0].cwd_bytes_hex);
	if(left_path == NULL || right_path == NULL)
	{
		free(left_path);
		free(right_path);
		persisted_state_free(&stored);
		return 1;
	}
	nv_workspace_session_t next = {};
	if(nv_workspace_session_init(left_path, right_path, &next, error) != 0)
	{
		free(left_path);
		free(right_path);
		persisted_state_free(&stored);
		return 1;
	}
	free(left_path);
	free(right_path);
	if(restore_persisted_pane(&next, NV_SESSION_LEFT, &stored.panes[0], error) != 0 ||
			restore_persisted_pane(&next, NV_SESSION_RIGHT, &stored.panes[1], error) != 0)
	{
		nv_workspace_session_free(&next);
		persisted_state_free(&stored);
		nv_snapshot_error_free(error);
		return 1;
	}
	next.active_pane = stored.active_pane;
	nv_workspace_session_free(session);
	*session = next;
	persisted_state_free(&stored);
	return 0;
}

static int
hex_digit(char character)
{
	if(character >= '0' && character <= '9') return character - '0';
	if(character >= 'a' && character <= 'f') return character - 'a' + 10;
	return -1;
}

static char *
hex_decode(const char hex[])
{
	if(hex == NULL)
	{
		return NULL;
	}
	const size_t length = strlen(hex);
	if(length % 2U != 0U || length/2U > NV_PANE_SNAPSHOT_MAX_HEX_BYTES/2U)
	{
		return NULL;
	}
	char *const decoded = malloc(length/2U + 1U);
	if(decoded == NULL)
	{
		return NULL;
	}
	for(size_t i = 0U; i < length; i += 2U)
	{
		const int high = hex_digit(hex[i]);
		const int low = hex_digit(hex[i + 1U]);
		if(high < 0 || low < 0 || (high == 0 && low == 0))
		{
			free(decoded);
			return NULL;
		}
		decoded[i/2U] = (char)((high << 4U) | low);
	}
	decoded[length/2U] = '\0';
	return decoded;
}

static char *
parent_path(const char path[])
{
	char *const result = strdup(path);
	if(result == NULL)
	{
		return NULL;
	}
	char *end = result + strlen(result);
	while(end > result + 1 && end[-1] == '/') --end;
	*end = '\0';
	char *const slash = strrchr(result, '/');
	if(slash == NULL)
	{
		free(result);
		return strdup("..");
	}
	if(slash == result)
	{
		result[1] = '\0';
	}
	else
	{
		*slash = '\0';
	}
	return result;
}

static const nv_pane_entry_t *
find_entry_by_path(const nv_pane_snapshot_t *snapshot,
		const char path_bytes_hex[])
{
	if(path_bytes_hex == NULL) return NULL;
	for(size_t i = 0U; i < snapshot->entry_count; ++i)
	{
		if(strcmp(snapshot->entries[i].path_bytes_hex, path_bytes_hex) == 0)
		{
			return &snapshot->entries[i];
		}
	}
	return NULL;
}

static int
valid_name(const char name[])
{
	if(name == NULL) return 0;
	const char *const end = memchr(name, '\0', NV_SESSION_MAX_NAME_BYTES + 1U);
	if(end == NULL) return 0;
	const size_t length = (size_t)(end - name);
	return length != 0U && length <= NV_SESSION_MAX_NAME_BYTES &&
			strcmp(name, ".") != 0 && strcmp(name, "..") != 0 &&
			strchr(name, '/') == NULL && strchr(name, '\\') == NULL;
}

void
nv_session_prepared_action_free(nv_session_prepared_action_t *action)
{
	if(action == NULL) return;
	free(action->source_directory);
	free(action->destination_directory);
	free(action->name);
	for(size_t i = 0U; i < action->target_count; ++i)
	{
		free(action->targets[i].path);
		free(action->targets[i].name);
	}
	free(action->targets);
	*action = (nv_session_prepared_action_t){};
}

static int
snapshot_identity_matches(const nv_pane_snapshot_t *snapshot,
		const nv_session_command_t *command, int destination)
{
	return snapshot->has_cwd_stat &&
			(destination ? command->action_destination_snapshot_revision :
				command->action_snapshot_revision) == snapshot->snapshot_revision &&
			(destination ? command->action_destination_cwd_device :
				command->action_cwd_device) == snapshot->cwd_device &&
			(destination ? command->action_destination_cwd_inode :
				command->action_cwd_inode) == snapshot->cwd_inode &&
			(destination ? command->action_destination_cwd_ctime_unix_ns :
				command->action_cwd_ctime_unix_ns) == snapshot->cwd_ctime_unix_ns;
}

static int
find_tab_index(const nv_session_tabs_t *tabs, uint64_t id, size_t *index)
{
	if(id == 0U) return -1;
	for(size_t i = 0U; i < tabs->count; ++i)
	{
		if(tabs->items[i].id == id)
		{
			*index = i;
			return 0;
		}
	}
	return -1;
}

static int
activate_tab_at(nv_workspace_session_t *session, nv_session_pane_t pane,
		size_t index, int focus_pane, nv_snapshot_error_t *error)
{
	nv_session_tabs_t *const tabs = pane_tabs(session, pane);
	if(index >= tabs->count)
	{
		return set_error(error, "invalid-tab", "tab does not exist");
	}
	if(index != tabs->active)
	{
		nv_pane_snapshot_t *const current = pane_snapshot(session, pane);
		nv_session_resource_t active_resource = tabs->items[tabs->active].resource;
		tabs->items[tabs->active].snapshot = *current;
		tabs->items[tabs->active].resource = tabs->items[index].resource;
		*current = tabs->items[index].snapshot;
		tabs->items[index].resource = active_resource;
		tabs->items[index].snapshot = (nv_pane_snapshot_t){};
		tabs->active = index;
	}
	if(focus_pane) session->active_pane = pane;
	return 0;
}

static int
new_tab(nv_workspace_session_t *session, nv_session_pane_t pane,
		nv_snapshot_error_t *error)
{
	if(!valid_pane(pane))
		return set_error(error, "invalid-command", "invalid pane");
	if(ensure_tabs(session, error) != 0) return -1;
	nv_session_tabs_t *const tabs = pane_tabs(session, pane);
	if(tabs->count == NV_SESSION_MAX_TABS)
	{
		return set_error(error, "tab-limit", "pane tab limit reached");
	}
	if(tabs->items[tabs->active].resource.active)
	{
		return set_error(error, "resource-tab-unsupported",
				"open a new tab after returning from the mounted resource");
	}
	nv_pane_snapshot_t clone = {};
	if(clone_snapshot(pane_snapshot(session, pane), &clone, error) != 0) return -1;
	const uint64_t id = next_tab_id(session);
	if(id == 0U)
	{
		nv_pane_snapshot_free(&clone);
		return set_error(error, "tab-id-exhausted",
				"no more tab identifiers are available");
	}
	clone.snapshot_revision = session->next_snapshot_revision++;
	const size_t inserted = tabs->active + 1U;
	for(size_t i = tabs->count; i > inserted; --i)
	{
		tabs->items[i] = tabs->items[i - 1U];
	}
	tabs->items[tabs->active].snapshot = *pane_snapshot(session, pane);
	*pane_snapshot(session, pane) = clone;
	tabs->items[inserted] = (nv_session_tab_t){ .id = id };
	++tabs->count;
	tabs->active = inserted;
	session->active_pane = pane;
	return 0;
}

static void
remove_tab_slot(nv_session_tabs_t *tabs, size_t index)
{
	for(size_t i = index; i + 1U < tabs->count; ++i)
	{
		tabs->items[i] = tabs->items[i + 1U];
	}
	--tabs->count;
	tabs->items[tabs->count] = (nv_session_tab_t){};
}

static int
close_tab(nv_workspace_session_t *session, nv_session_pane_t pane,
		uint64_t id, nv_snapshot_error_t *error)
{
	if(!valid_pane(pane))
		return set_error(error, "invalid-command", "invalid pane");
	if(ensure_tabs(session, error) != 0) return -1;
	nv_session_tabs_t *const tabs = pane_tabs(session, pane);
	size_t index = 0U;
	if(find_tab_index(tabs, id, &index) != 0)
		return set_error(error, "invalid-tab", "tab does not exist");
	if(tabs->items[index].resource.active)
		return set_error(error, "resource-tab-mounted",
				"return from the mounted resource before closing its tab");
	if(tabs->count == 1U)
		return set_error(error, "last-tab", "cannot close the last pane tab");
	if(index != tabs->active)
	{
		nv_pane_snapshot_free(&tabs->items[index].snapshot);
		resource_free(&tabs->items[index].resource);
		remove_tab_slot(tabs, index);
		if(index < tabs->active) --tabs->active;
		return 0;
	}

	const size_t replacement = index + 1U < tabs->count ? index + 1U : index - 1U;
	nv_pane_snapshot_t next = tabs->items[replacement].snapshot;
	nv_session_resource_t next_resource = tabs->items[replacement].resource;
	tabs->items[replacement].snapshot = (nv_pane_snapshot_t){};
	tabs->items[replacement].resource = (nv_session_resource_t){};
	nv_pane_snapshot_free(pane_snapshot(session, pane));
	*pane_snapshot(session, pane) = next;
	remove_tab_slot(tabs, index);
	tabs->active = replacement > index ? index : replacement;
	tabs->items[tabs->active].resource = next_resource;
	return 0;
}

int
nv_workspace_session_prepare_action(const nv_workspace_session_t *session,
		const nv_session_command_t *command,
		nv_session_prepared_action_t *action, nv_snapshot_error_t *error)
{
	if(session == NULL || command == NULL || action == NULL || error == NULL)
		return -1;
	nv_snapshot_error_free(error);
	nv_session_prepared_action_t next = {};
	if(command->pane != NV_SESSION_LEFT && command->pane != NV_SESSION_RIGHT)
		return set_error(error, "invalid-command", "invalid action pane");
	if(command->kind != NV_SESSION_COPY && command->kind != NV_SESSION_MOVE_FILES &&
			command->kind != NV_SESSION_MKDIR && command->kind != NV_SESSION_DELETE &&
			command->kind != NV_SESSION_RENAME)
		return set_error(error, "invalid-command", "command is not a file action");
	const nv_pane_snapshot_t *const source = command->pane == NV_SESSION_LEFT ?
		&session->left : &session->right;
	if(command->action_cwd_bytes_hex == NULL ||
			strcmp(command->action_cwd_bytes_hex, source->cwd_bytes_hex) != 0 ||
			!snapshot_identity_matches(source, command, 0))
	{
		return set_error(error, "stale-action", "file action target is stale");
	}
	next.kind = command->kind;
	next.pane = command->pane;
	next.source_directory = hex_decode(source->cwd_bytes_hex);
	next.source_directory_identity = (nv_fs_identity_t){
		.device = source->cwd_device, .inode = source->cwd_inode,
		.ctime_unix_ns = source->cwd_ctime_unix_ns,
	};
	if(next.source_directory == NULL)
		goto invalid_path;
	if(command->kind == NV_SESSION_MKDIR || command->kind == NV_SESSION_RENAME)
	{
		if(!valid_name(command->name))
		{
			nv_session_prepared_action_free(&next);
			return set_error(error, "invalid-name", "directory name is invalid");
		}
		next.name = strdup(command->name);
		if(next.name == NULL) goto out_of_memory;
	}
	if(command->kind == NV_SESSION_RENAME &&
			command->action_target_count != 1U)
	{
		nv_session_prepared_action_free(&next);
		return set_error(error, "invalid-command",
				"rename targets exactly one entry");
	}
	if(command->kind == NV_SESSION_COPY || command->kind == NV_SESSION_MOVE_FILES)
	{
		const nv_pane_snapshot_t *const destination = command->pane == NV_SESSION_LEFT ?
			&session->right : &session->left;
		if(command->action_destination_cwd_bytes_hex == NULL ||
				strcmp(command->action_destination_cwd_bytes_hex,
					destination->cwd_bytes_hex) != 0 ||
				!snapshot_identity_matches(destination, command, 1))
		{
			nv_session_prepared_action_free(&next);
			return set_error(error, "stale-action",
					"file action destination is stale");
		}
		next.destination_directory = hex_decode(destination->cwd_bytes_hex);
		next.destination_directory_identity = (nv_fs_identity_t){
			.device = destination->cwd_device, .inode = destination->cwd_inode,
			.ctime_unix_ns = destination->cwd_ctime_unix_ns,
		};
		if(next.destination_directory == NULL) goto invalid_path;
	}
	if(command->kind == NV_SESSION_RENAME)
	{
		/* A rename stays inside its own directory: the destination is the
		 * source directory itself, only the basename changes. */
		next.destination_directory = strdup(next.source_directory);
		next.destination_directory_identity = next.source_directory_identity;
		if(next.destination_directory == NULL) goto out_of_memory;
	}
	if(command->kind != NV_SESSION_MKDIR)
	{
		if(command->action_targets == NULL || command->action_target_count == 0U ||
				command->action_target_count > NV_SESSION_MAX_ACTION_PATHS)
		{
			nv_session_prepared_action_free(&next);
			return set_error(error, "stale-action", "file action target is stale");
		}
		next.targets = calloc(command->action_target_count, sizeof(*next.targets));
		if(next.targets == NULL) goto out_of_memory;
		next.target_count = command->action_target_count;
		for(size_t i = 0U; i < next.target_count; ++i)
		{
			const nv_pane_entry_t *const entry = find_entry_by_path(source,
				command->action_targets[i].path_bytes_hex);
			if(entry == NULL || !entry->has_stat ||
					entry->device != command->action_targets[i].device ||
					entry->inode != command->action_targets[i].inode ||
					entry->ctime_unix_ns != command->action_targets[i].ctime_unix_ns ||
					entry->kind != command->action_targets[i].kind)
			{
				nv_session_prepared_action_free(&next);
				return set_error(error, "stale-action", "file action entry is stale");
			}
			next.targets[i].path = hex_decode(entry->path_bytes_hex);
			next.targets[i].name = hex_decode(entry->name_bytes_hex);
			next.targets[i].identity = (nv_fs_identity_t){
				.device = entry->device, .inode = entry->inode,
				.ctime_unix_ns = entry->ctime_unix_ns,
			};
			next.targets[i].kind = entry->kind;
			next.targets[i].size_bytes = entry->size_bytes;
			next.targets[i].size_known = entry->kind == NV_ENTRY_FILE ||
				entry->kind == NV_ENTRY_EXECUTABLE;
			if(next.targets[i].path == NULL || next.targets[i].name == NULL)
				goto invalid_path;
		}
	}
	if(command->kind == NV_SESSION_RENAME)
	{
		if(strcmp(next.name, next.targets[0].name) == 0)
		{
			nv_session_prepared_action_free(&next);
			return set_error(error, "invalid-name", "name is unchanged");
		}
		/* The worker builds the destination as directory + target name, so a
		 * rename installs the new basename on the single prepared target. */
		char *const renamed = strdup(next.name);
		if(renamed == NULL) goto out_of_memory;
		free(next.targets[0].name);
		next.targets[0].name = renamed;
	}
	nv_session_prepared_action_free(action);
	*action = next;
	return 0;

invalid_path:
	nv_session_prepared_action_free(&next);
	return set_error(error, "invalid-path", "file action identity is invalid");
out_of_memory:
	nv_session_prepared_action_free(&next);
	return set_error(error, "out-of-memory", "failed to prepare file action");
}

int
nv_workspace_session_apply(nv_workspace_session_t *session,
		const nv_session_command_t *command, nv_snapshot_error_t *error)
{
	if(session == NULL || command == NULL || error == NULL)
	{
		return -1;
	}
	nv_snapshot_error_free(error);
	if(ensure_tabs(session, error) != 0) return -1;
	if(command->kind == NV_SESSION_SEARCH ||
			command->kind == NV_SESSION_SEARCH_NEXT)
	{
		char **const stored_query = pane_search_query(session, session->active_pane);
		int *const stored_direction = pane_search_direction(session,
				session->active_pane);
		if(command->kind == NV_SESSION_SEARCH)
		{
			if(command->search_query == NULL || command->search_query[0] == '\0' ||
					strlen(command->search_query) > NV_SESSION_MAX_SEARCH_BYTES ||
					(command->search_direction != -1 && command->search_direction != 1))
				return set_error(error, "invalid-search", "search query is invalid");
			char *const replacement = strdup(command->search_query);
			if(replacement == NULL)
				return set_error(error, "out-of-memory", "failed to store search query");
			free(*stored_query);
			*stored_query = replacement;
			*stored_direction = command->search_direction;
		}
		else if(*stored_query == NULL || (*stored_query)[0] == '\0')
		{
			return set_error(error, "search-not-set", "no previous search query");
		}
		const int direction = command->kind == NV_SESSION_SEARCH ?
			command->search_direction : command->search_direction != 0 ?
			command->search_direction : *stored_direction;
		if(direction != -1 && direction != 1)
			return set_error(error, "invalid-search", "search direction is invalid");
		if(command->kind == NV_SESSION_SEARCH_NEXT) *stored_direction = direction;
		return search_snapshot(active_snapshot(session), *stored_query, direction,
				error);
	}
	if(command->kind == NV_SESSION_FOCUS)
	{
		if(command->pane != NV_SESSION_LEFT && command->pane != NV_SESSION_RIGHT)
		{
			return set_error(error, "invalid-command", "invalid pane");
		}
		session->active_pane = command->pane;
		return 0;
	}
	if(command->kind == NV_SESSION_FOCUS_NEXT)
	{
		session->active_pane = session->active_pane == NV_SESSION_LEFT ?
			NV_SESSION_RIGHT : NV_SESSION_LEFT;
		return 0;
	}
	if(command->kind == NV_SESSION_SORT_BY)
	{
		if(command->pane != NV_SESSION_LEFT && command->pane != NV_SESSION_RIGHT)
		{
			return set_error(error, "invalid-command", "invalid pane");
		}
		if(command->sort_key < NV_SORT_NAME || command->sort_key > NV_SORT_OTHER)
		{
			return set_error(error, "invalid-command", "invalid sort key");
		}
		session->active_pane = command->pane;
		nv_pane_snapshot_t *const target = active_snapshot(session);
		const int descending = target->sort_key == command->sort_key ?
			!target->sort_descending : 0;
		nv_pane_snapshot_sort(target, command->sort_key, descending);
		return 0;
	}
	if(command->kind == NV_SESSION_SELECT_ENTRY)
	{
		if(!valid_pane(command->pane))
			return set_error(error, "invalid-command", "invalid pane");
		if(command->toggle_selection != 0 && command->toggle_selection != 1)
			return set_error(error, "invalid-command", "invalid selection mode");
		nv_pane_snapshot_t *const target = pane_snapshot(session, command->pane);
		if(command->entry_index >= target->entry_count)
			return set_error(error, "invalid-entry", "entry does not exist");
		session->active_pane = command->pane;
		target->cursor = (int)command->entry_index;
		if(command->toggle_selection)
		{
			target->entries[command->entry_index].selected =
				!target->entries[command->entry_index].selected;
			if(target->entries[command->entry_index].selected)
				++target->selection_count;
			else
				--target->selection_count;
		}
		return 0;
	}
	if(command->kind == NV_SESSION_NEW_TAB)
		return new_tab(session, command->pane, error);
	if(command->kind == NV_SESSION_ACTIVATE_TAB)
	{
		if(!valid_pane(command->pane))
			return set_error(error, "invalid-command", "invalid pane");
		if(ensure_tabs(session, error) != 0) return -1;
		size_t index = 0U;
		if(find_tab_index(pane_tabs(session, command->pane), command->tab_id,
				&index) != 0)
			return set_error(error, "invalid-tab", "tab does not exist");
		return activate_tab_at(session, command->pane, index, 1, error);
	}
	if(command->kind == NV_SESSION_CLOSE_TAB)
		return close_tab(session, command->pane, command->tab_id, error);
	if(command->kind == NV_SESSION_TAB_CYCLE)
	{
		if(command->delta == 0 || command->delta < -NV_SESSION_MAX_TAB_CYCLE ||
				command->delta > NV_SESSION_MAX_TAB_CYCLE)
			return set_error(error, "invalid-command", "invalid tab direction");
		if(ensure_tabs(session, error) != 0) return -1;
		nv_session_tabs_t *const tabs = pane_tabs(session, session->active_pane);
		const int count = (int)tabs->count;
		int index = ((int)tabs->active + command->delta)%count;
		if(index < 0) index += count;
		return activate_tab_at(session, session->active_pane, (size_t)index, 1,
				error);
	}
	if(command->kind == NV_SESSION_SORT_CYCLE && command->has_pane)
	{
		if(!valid_pane(command->pane))
			return set_error(error, "invalid-command", "invalid pane");
		if(command->delta != -1 && command->delta != 1)
			return set_error(error, "invalid-command", "invalid sort direction");
		session->active_pane = command->pane;
	}

	nv_pane_snapshot_t *const snapshot = active_snapshot(session);
	switch(command->kind)
	{
		case NV_SESSION_MOVE_FIRST:
			if(snapshot->entry_count != 0U) snapshot->cursor = 0;
			return 0;
		case NV_SESSION_MOVE_LAST:
			if(snapshot->entry_count != 0U) snapshot->cursor = (int)snapshot->entry_count - 1;
			return 0;
		case NV_SESSION_SEARCH:
		case NV_SESSION_SEARCH_NEXT:
			break;
		case NV_SESSION_MOVE_CURSOR:
			if(snapshot->entry_count == 0U) return 0;
			if(command->delta < 0 && snapshot->cursor > 0) --snapshot->cursor;
			if(command->delta > 0 && (size_t)snapshot->cursor + 1U < snapshot->entry_count) ++snapshot->cursor;
			return 0;
		case NV_SESSION_TOGGLE_SELECTION:
			if(snapshot->cursor < 0) return set_error(error, "empty-pane", "cannot select in an empty pane");
			snapshot->entries[snapshot->cursor].selected = !snapshot->entries[snapshot->cursor].selected;
			if(snapshot->entries[snapshot->cursor].selected)
			{
				++snapshot->selection_count;
			}
			else
			{
				--snapshot->selection_count;
			}
			return 0;
		case NV_SESSION_ENTER:
			if(snapshot->cursor < 0)
			{
				return set_error(error, "empty-pane", "cannot enter an empty pane");
			}
			if(snapshot->entries[snapshot->cursor].resource_kind == NV_ENTRY_RESOURCE_ARCHIVE)
			{
				return set_error(error, "archive-mount-unavailable",
						"archive resource mounting is unavailable");
			}
			if(snapshot->entries[snapshot->cursor].kind != NV_ENTRY_DIRECTORY)
			{
				return set_error(error, "not-directory", "current entry is not a directory");
			}
			{
				char *const path = hex_decode(snapshot->entries[snapshot->cursor].path_bytes_hex);
				if(path == NULL) return set_error(error, "invalid-path", "directory identity is invalid");
				const int result = replace_snapshot(session, snapshot,
					session->active_pane,
					nv_workspace_session_tab_id(session, session->active_pane,
						nv_workspace_session_active_tab_index(session,
							session->active_pane)), path, error);
				free(path);
				return result;
			}
		case NV_SESSION_PARENT:
			{
				char *const path = hex_decode(snapshot->cwd_bytes_hex);
				if(path == NULL) return set_error(error, "invalid-path", "directory identity is invalid");
				char *const parent = parent_path(path);
				free(path);
				if(parent == NULL) return set_error(error, "out-of-memory", "failed to build parent path");
				const int result = replace_snapshot(session, snapshot,
						session->active_pane,
						nv_workspace_session_tab_id(session, session->active_pane,
							nv_workspace_session_active_tab_index(session,
								session->active_pane)), parent, error);
				free(parent);
				return result;
			}
		case NV_SESSION_REFRESH:
			return refresh_snapshot(session, snapshot, session->active_pane,
				nv_workspace_session_tab_id(session, session->active_pane,
					nv_workspace_session_active_tab_index(session,
						session->active_pane)), error);
		case NV_SESSION_SORT_CYCLE:
			{
				static const nv_pane_sort_key_t keys[] = {
					NV_SORT_NAME, NV_SORT_SIZE, NV_SORT_CTIME, NV_SORT_MTIME,
					NV_SORT_MODE,
				};
				size_t index = 0U;
				while(index + 1U < sizeof(keys)/sizeof(keys[0]) &&
						keys[index] != snapshot->sort_key) ++index;
				const size_t count = sizeof(keys)/sizeof(keys[0]);
				if(command->delta != -1 && command->delta != 1)
				{
					return set_error(error, "invalid-command", "invalid sort direction");
				}
				const size_t next = command->delta < 0 ?
					(index + count - 1U)%count : (index + 1U)%count;
				nv_pane_snapshot_sort(snapshot, keys[next], 0);
				return 0;
			}
		case NV_SESSION_COPY:
		case NV_SESSION_MOVE_FILES:
		case NV_SESSION_DELETE:
		case NV_SESSION_MKDIR:
		case NV_SESSION_RENAME:
			return set_error(error, "async-action-required",
					"file actions must run through the action queue");
		case NV_SESSION_UNDO:
			return set_error(error, "core-command-required",
					"undo must run through the core session command path");
		case NV_SESSION_MOUNT_SSH:
		case NV_SESSION_CANCEL_RESOURCE:
			return set_error(error, "core-command-required",
					"resource tasks must run through the core session command path");
		case NV_SESSION_FOCUS:
		case NV_SESSION_FOCUS_NEXT:
		case NV_SESSION_SORT_BY:
		case NV_SESSION_SELECT_ENTRY:
		case NV_SESSION_NEW_TAB:
		case NV_SESSION_ACTIVATE_TAB:
		case NV_SESSION_CLOSE_TAB:
		case NV_SESSION_TAB_CYCLE:
		case NV_SESSION_PREVIEW:
		case NV_SESSION_OPEN:
		case NV_SESSION_CANCEL_ACTION:
		case NV_SESSION_RETRY_ACTION:
			break;
	}
	return set_error(error, "invalid-command", "unsupported session command");
}

void
nv_session_command_free(nv_session_command_t *command)
{
	if(command == NULL) return;
	if(command->owns_action_fields)
	{
		free(command->action_cwd_bytes_hex);
		free(command->action_destination_cwd_bytes_hex);
		for(size_t i = 0U; i < command->action_target_count; ++i)
		{
			free(command->action_targets[i].path_bytes_hex);
		}
		free(command->action_targets);
	}
	if(command->owns_preview_fields)
	{
		free(command->preview_cwd_bytes_hex);
		free(command->preview_path_bytes_hex);
	}
	if(command->owns_open_fields)
	{
		free(command->open_cwd_bytes_hex);
		free(command->open_path_bytes_hex);
		for(size_t i = 0U; i < command->open_association_argc; ++i)
			free(command->open_association_argv[i]);
		free(command->open_association_argv);
	}
	if(command->owns_resource_fields)
		free(command->resource_remote);
	if(command->owns_search_fields)
		free(command->search_query);
	memset(command, 0, sizeof(*command));
}

void
nv_workspace_session_free(nv_workspace_session_t *session)
{
	if(session == NULL)
	{
		return;
	}
	nv_pane_snapshot_free(&session->left);
	nv_pane_snapshot_free(&session->right);
	for(size_t i = 0U; i < session->left_tabs.count; ++i)
	{
		resource_free(&session->left_tabs.items[i].resource);
		if(i != session->left_tabs.active)
			nv_pane_snapshot_free(&session->left_tabs.items[i].snapshot);
	}
	for(size_t i = 0U; i < session->right_tabs.count; ++i)
	{
		resource_free(&session->right_tabs.items[i].resource);
		if(i != session->right_tabs.active)
			nv_pane_snapshot_free(&session->right_tabs.items[i].snapshot);
	}
	cursor_history_free(&session->left_cursor_history);
	cursor_history_free(&session->right_cursor_history);
	free(session->left_search_query);
	free(session->right_search_query);
	memset(session, 0, sizeof(*session));
}
