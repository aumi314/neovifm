/* vifm
 * Copyright (C) 2026 NeoVifm contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "classic_pane_adapter.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "../filtering.h"
#include "../ui/ui.h"
#include "../utils/utf8proc.h"

static char *to_hex(const char text[]);
static char *to_display(const char text[]);
static char *join_path(const char dir[], const char name[]);
static int set_error(nv_snapshot_error_t *error, const char code[],
		const char message[]);
static nv_entry_kind_t entry_kind(FileType type);
static nv_pane_sort_key_t sort_key(signed char key);
static void free_entry(nv_pane_entry_t *entry);

static char *
to_hex(const char text[])
{
	static const char alphabet[] = "0123456789abcdef";
	const size_t length = strlen(text);
	if(length > NV_PANE_SNAPSHOT_MAX_HEX_BYTES/2U)
	{
		return NULL;
	}
	char *const result = malloc(length*2U + 1U);
	if(result == NULL) return NULL;
	for(size_t i = 0U; i < length; ++i)
	{
		const unsigned char byte = (unsigned char)text[i];
		result[i*2U] = alphabet[byte >> 4U];
		result[i*2U + 1U] = alphabet[byte & 0x0fU];
	}
	result[length*2U] = '\0';
	return result;
}

static int
unsafe_codepoint(utf8proc_int32_t codepoint)
{
	return codepoint < 0x20 || codepoint == 0x7f ||
		(codepoint >= 0x80 && codepoint <= 0x9f) || codepoint == 0x061c ||
		codepoint == 0x200e || codepoint == 0x200f ||
		(codepoint >= 0x2028 && codepoint <= 0x202e) ||
		(codepoint >= 0x2066 && codepoint <= 0x2069);
}

static char *
to_display(const char text[])
{
	static const char replacement[] = "\357\277\275";
	const size_t length = strlen(text);
	if(length > NV_PANE_SNAPSHOT_MAX_DISPLAY_BYTES) return NULL;
	char *const result = malloc(length*3U + 1U);
	if(result == NULL) return NULL;
	size_t input = 0U, output = 0U;
	while(input < length)
	{
		utf8proc_int32_t codepoint;
		const utf8proc_ssize_t width = utf8proc_iterate(
				(const utf8proc_uint8_t *)text + input,
				(utf8proc_ssize_t)(length - input), &codepoint);
		if(width > 0 && !unsafe_codepoint(codepoint))
		{
			memcpy(result + output, text + input, (size_t)width);
			input += (size_t)width;
			output += (size_t)width;
		}
		else
		{
			memcpy(result + output, replacement, sizeof(replacement) - 1U);
			output += sizeof(replacement) - 1U;
			input += width > 0 ? (size_t)width : 1U;
		}
		if(output > NV_PANE_SNAPSHOT_MAX_DISPLAY_BYTES)
		{
			free(result);
			return NULL;
		}
	}
	result[output] = '\0';
	return result;
}

static char *
join_path(const char dir[], const char name[])
{
	const size_t dir_length = strlen(dir), name_length = strlen(name);
	if(dir_length > NV_PANE_SNAPSHOT_MAX_HEX_BYTES/2U ||
			name_length > NV_PANE_SNAPSHOT_MAX_HEX_BYTES/2U ||
			dir_length + name_length + 2U > NV_PANE_SNAPSHOT_MAX_HEX_BYTES/2U)
	{
		return NULL;
	}
	char *const path = malloc(dir_length + name_length + 2U);
	if(path == NULL) return NULL;
	memcpy(path, dir, dir_length);
	path[dir_length] = '/';
	path[dir_length + 1U] = '\0';
	if(dir_length != 0U && dir[dir_length - 1U] == '/') path[dir_length] = '\0';
	strcpy(path + strlen(path), name);
	return path;
}

static int
set_error(nv_snapshot_error_t *error, const char code[], const char message[])
{
	nv_snapshot_error_free(error);
	error->code = strdup(code);
	error->message = strdup(message);
	if(error->code == NULL || error->message == NULL) nv_snapshot_error_free(error);
	return -1;
}

static nv_entry_kind_t
entry_kind(FileType type)
{
	switch(type)
	{
		case FT_DIR: return NV_ENTRY_DIRECTORY;
		case FT_LINK: return NV_ENTRY_SYMLINK;
		case FT_EXEC: return NV_ENTRY_EXECUTABLE;
		case FT_FIFO: return NV_ENTRY_FIFO;
		case FT_CHAR_DEV: return NV_ENTRY_CHAR_DEVICE;
		case FT_BLOCK_DEV: return NV_ENTRY_BLOCK_DEVICE;
#ifndef _WIN32
		case FT_SOCK: return NV_ENTRY_SOCKET;
#endif
		case FT_REG: return NV_ENTRY_FILE;
		case FT_UNK: return NV_ENTRY_UNKNOWN;
		case FT_COUNT: break;
	}
	return NV_ENTRY_UNKNOWN;
}

static nv_pane_sort_key_t
sort_key(signed char key)
{
	const int absolute_key = key < 0 ? -key : key;
	switch(absolute_key)
	{
		case SK_BY_NAME:
		case SK_BY_INAME: return NV_SORT_NAME;
		case SK_BY_EXTENSION:
		case SK_BY_FILEEXT: return NV_SORT_EXTENSION;
		case SK_BY_SIZE: return NV_SORT_SIZE;
		case SK_BY_TIME_MODIFIED: return NV_SORT_MTIME;
		case SK_BY_TYPE: return NV_SORT_TYPE;
	}
	return NV_SORT_OTHER;
}

static void
free_entry(nv_pane_entry_t *entry)
{
	free(entry->name_display);
	free(entry->name_bytes_hex);
	free(entry->path_display);
	free(entry->path_bytes_hex);
	memset(entry, 0, sizeof(*entry));
}

int
nv_pane_snapshot_from_classic_view(const view_t *view,
		nv_pane_snapshot_t *snapshot, nv_snapshot_error_t *error)
{
	if(view == NULL || snapshot == NULL || error == NULL || view->list_rows < 0 ||
			view->list_rows > (int)NV_PANE_SNAPSHOT_MAX_ENTRIES ||
			(view->list_rows != 0 && view->dir_entry == NULL) ||
			(view->list_rows == 0 && view->list_pos != -1) ||
			(view->list_rows != 0 && (view->list_pos < 0 || view->list_pos >= view->list_rows)))
	{
		return set_error(error, "invalid-classic-pane", "classic pane has invalid list state");
	}
	nv_pane_snapshot_t next = {};
	next.cwd_display = to_display(view->curr_dir);
	next.cwd_bytes_hex = to_hex(view->curr_dir);
	if(next.cwd_display == NULL || next.cwd_bytes_hex == NULL)
	{
		nv_pane_snapshot_free(&next);
		return set_error(error, "snapshot-too-large", "classic pane path exceeds protocol limit");
	}
	next.entry_count = (size_t)view->list_rows;
	next.cursor = view->list_pos;
	next.filtered_count = view->filtered < 0 ? 0U : (size_t)view->filtered;
	next.sort_key = sort_key(view->sort[0]);
	next.sort_descending = view->sort[0] < 0;
	next.filter_active = view->local_filter.matchers != NULL &&
			!local_filter_is_empty(view);
	if(next.entry_count != 0U)
	{
		next.entries = calloc(next.entry_count, sizeof(*next.entries));
		if(next.entries == NULL)
		{
			nv_pane_snapshot_free(&next);
			return set_error(error, "out-of-memory", "failed to copy classic pane entries");
		}
	}
	for(size_t i = 0U; i < next.entry_count; ++i)
	{
		const dir_entry_t *const source = &view->dir_entry[i];
		char *const path = join_path(source->origin == NULL ? view->curr_dir : source->origin,
				source->name == NULL ? "" : source->name);
		nv_pane_entry_t *const entry = &next.entries[i];
		entry->name_display = to_display(source->name == NULL ? "" : source->name);
		entry->name_bytes_hex = to_hex(source->name == NULL ? "" : source->name);
		entry->path_display = path == NULL ? NULL : to_display(path);
		entry->path_bytes_hex = path == NULL ? NULL : to_hex(path);
		free(path);
		if(entry->name_display == NULL || entry->name_bytes_hex == NULL ||
				entry->path_display == NULL || entry->path_bytes_hex == NULL)
		{
			for(size_t j = 0U; j <= i; ++j) free_entry(&next.entries[j]);
			nv_pane_snapshot_free(&next);
			return set_error(error, "snapshot-too-large", "classic pane entry exceeds protocol limit");
		}
		entry->kind = entry_kind(source->type);
		entry->size_bytes = source->size;
		entry->mtime_unix_ms = (int64_t)source->mtime*1000;
#ifndef _WIN32
		entry->inode = source->inode;
		entry->mode = source->mode;
		entry->has_stat = 1;
#endif
		entry->selected = source->selected;
		next.selection_count += entry->selected != 0;
		entry->hidden = source->name != NULL && source->name[0] == '.';
	}
	struct timeval now;
	next.generated_at_unix_ms = gettimeofday(&now, NULL) == 0
		? (int64_t)now.tv_sec*1000 + now.tv_usec/1000 : 0;
	nv_pane_snapshot_free(snapshot);
	nv_snapshot_error_free(error);
	*snapshot = next;
	return 0;
}

void
nv_classic_workspace_snapshot_free(nv_classic_workspace_snapshot_t *workspace)
{
	if(workspace == NULL) return;
	nv_pane_snapshot_free(&workspace->left);
	nv_pane_snapshot_free(&workspace->right);
	memset(workspace, 0, sizeof(*workspace));
}

int
nv_classic_workspace_snapshot_from_views(const view_t *left,
		const view_t *right, nv_classic_pane_t active_pane,
		nv_classic_workspace_snapshot_t *workspace, nv_snapshot_error_t *error)
{
	if(workspace == NULL || error == NULL ||
			(active_pane != NV_CLASSIC_PANE_LEFT &&
					active_pane != NV_CLASSIC_PANE_RIGHT))
	{
		return set_error(error, "invalid-classic-workspace",
				"classic workspace has invalid pane state");
	}
	nv_classic_workspace_snapshot_t next = {};
	nv_snapshot_error_t next_error = {};
	if(nv_pane_snapshot_from_classic_view(left, &next.left, &next_error) != 0 ||
			nv_pane_snapshot_from_classic_view(right, &next.right, &next_error) != 0)
	{
		nv_classic_workspace_snapshot_free(&next);
		nv_snapshot_error_free(error);
		*error = next_error;
		return -1;
	}
	next.active_pane = active_pane;
	nv_classic_workspace_snapshot_free(workspace);
	nv_snapshot_error_free(error);
	*workspace = next;
	return 0;
}

int
nv_workspace_session_init_from_classic_views(const view_t *left,
		const view_t *right, nv_classic_pane_t active_pane,
		nv_workspace_session_t *session, nv_snapshot_error_t *error)
{
	if(session == NULL || error == NULL)
	{
		return set_error(error, "invalid-classic-workspace",
				"classic session destination is invalid");
	}
	nv_classic_workspace_snapshot_t workspace = {};
	if(nv_classic_workspace_snapshot_from_views(left, right, active_pane,
			&workspace, error) != 0)
	{
		return -1;
	}
	nv_workspace_session_t next = {
		.left = workspace.left,
		.right = workspace.right,
		.active_pane = active_pane == NV_CLASSIC_PANE_LEFT
			? NV_SESSION_LEFT : NV_SESSION_RIGHT,
	};
	memset(&workspace, 0, sizeof(workspace));
	nv_workspace_session_free(session);
	nv_snapshot_error_free(error);
	*session = next;
	return 0;
}
