/* vifm
 * Copyright (C) 2026 NeoVifm contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "pane_snapshot.h"

#include <ctype.h> /* tolower() */
#include <sys/stat.h> /* S_* struct stat */

#include <errno.h> /* E* errno */
#include <inttypes.h> /* PRIu64 */
#include <limits.h> /* INT64_MAX INT64_MIN */
#include <stdint.h> /* SIZE_MAX int64_t */
#include <stdlib.h> /* free() malloc() qsort() realloc() */
#include <stdio.h> /* snprintf() */
#include <string.h> /* memcpy() memset() strcmp() strdup() strlen() strerror() */
#include <time.h> /* time_t time() */
#ifndef _WIN32
#include <sys/time.h> /* gettimeofday() struct timeval */
#endif

#include "../compat/neovifm_fs.h"
#include "../utils/utf8proc.h"

static char *bytes_to_hex(const char bytes[]);
static char *display_string(const char bytes[]);
static int raw_string_may_fit_protocol_fields(const char bytes[]);
static int protocol_field_pair_fits(const char display[], const char hex[]);
static int add_protocol_bytes(size_t *total, size_t bytes);
static int snapshot_protocol_budget(const nv_pane_snapshot_t *snapshot,
		size_t *budget);
static int entry_protocol_budget(const nv_pane_entry_t *entry, size_t *budget);
static char *join_path(const char dir[], const char name[]);
static int build_entry(nv_dir_t *dir, const char directory[], const char name[],
		nv_pane_entry_t *entry);
static int append_entry(nv_pane_snapshot_t *snapshot,
		nv_pane_entry_t *entry, size_t *capacity, size_t *protocol_bytes);
static void entry_free(nv_pane_entry_t *entry);
static int set_error(nv_snapshot_error_t *error, const char code[],
		int os_error, const char path[]);
#ifndef _WIN32
static char *identity_display(uint64_t id, int group);
#endif
static nv_entry_resource_kind_t entry_resource_kind(const char name[],
		nv_entry_kind_t kind);

enum
{
	BUILD_ENTRY_NO_MEMORY = -1,
	BUILD_ENTRY_LIMIT_REACHED = -2,
	APPEND_ENTRY_NO_MEMORY = -1,
	APPEND_ENTRY_LIMIT_REACHED = -2,
};

static char *
bytes_to_hex(const char bytes[])
{
	static const char hex[] = "0123456789abcdef";
	const size_t len = strlen(bytes);
	if(len > (SIZE_MAX - 1U)/2U)
	{
		return NULL;
	}

	char *const encoded = malloc(len*2U + 1U);
	if(encoded == NULL)
	{
		return NULL;
	}

	for(size_t i = 0U; i < len; ++i)
	{
		const unsigned char byte = (unsigned char)bytes[i];
		encoded[i*2U] = hex[byte >> 4U];
		encoded[i*2U + 1U] = hex[byte & 0x0fU];
	}
	encoded[len*2U] = '\0';
	return encoded;
}

static int
raw_string_may_fit_protocol_fields(const char bytes[])
{
	const size_t maximum_bytes = NV_PANE_SNAPSHOT_MAX_HEX_BYTES/2U;
	for(size_t i = 0U; i <= maximum_bytes; ++i)
	{
		if(bytes[i] == '\0')
		{
			return 1;
		}
	}
	return 0;
}

static int
protocol_field_pair_fits(const char display[], const char hex[])
{
	return strlen(display) <= NV_PANE_SNAPSHOT_MAX_DISPLAY_BYTES &&
			strlen(hex) <= NV_PANE_SNAPSHOT_MAX_HEX_BYTES;
}

static int
add_protocol_bytes(size_t *total, size_t bytes)
{
	if(*total > SIZE_MAX - bytes)
	{
		return -1;
	}
	*total += bytes;
	return 0;
}

static int
snapshot_protocol_budget(const nv_pane_snapshot_t *snapshot, size_t *budget)
{
	/* Includes envelope, payload keys, decimal fields, separators and quotes. */
	*budget = 512U;
	const size_t cwd_display_bytes = strlen(snapshot->cwd_display);
	if(cwd_display_bytes > SIZE_MAX/2U ||
			add_protocol_bytes(budget, cwd_display_bytes*2U) != 0 ||
			add_protocol_bytes(budget, strlen(snapshot->cwd_bytes_hex)) != 0)
	{
		return -1;
	}
	return *budget <= NV_PROTOCOL_MAX_RECORD_BYTES ? 0 : -1;
}

static int
entry_protocol_budget(const nv_pane_entry_t *entry, size_t *budget)
{
	/* Display fields can expand to two JSON bytes per stored byte. */
	*budget = 512U;
	const size_t name_display_bytes = strlen(entry->name_display);
	const size_t path_display_bytes = strlen(entry->path_display);
	if(name_display_bytes > SIZE_MAX/2U || path_display_bytes > SIZE_MAX/2U ||
			add_protocol_bytes(budget, name_display_bytes*2U) != 0 ||
			add_protocol_bytes(budget, strlen(entry->name_bytes_hex)) != 0 ||
			add_protocol_bytes(budget, path_display_bytes*2U) != 0 ||
			add_protocol_bytes(budget, strlen(entry->path_bytes_hex)) != 0)
	{
		return -1;
	}
	if((entry->owner_display != NULL &&
			add_protocol_bytes(budget, strlen(entry->owner_display)*2U) != 0) ||
		(entry->group_display != NULL &&
			add_protocol_bytes(budget, strlen(entry->group_display)*2U) != 0))
	{
		return -1;
	}
	return 0;
}

static int
is_unsafe_codepoint(utf8proc_int32_t codepoint)
{
	return codepoint < 0x20 || codepoint == 0x7f ||
			(codepoint >= 0x80 && codepoint <= 0x9f) ||
			codepoint == 0x061c || codepoint == 0x200e || codepoint == 0x200f ||
			(codepoint >= 0x2028 && codepoint <= 0x202e) ||
			(codepoint >= 0x2066 && codepoint <= 0x2069);
}

static char *
display_string(const char bytes[])
{
	static const unsigned char replacement[] = { 0xefU, 0xbfU, 0xbdU };
	const size_t len = strlen(bytes);
	if(len > (SIZE_MAX - 1U)/3U || len > (size_t)PTRDIFF_MAX)
	{
		return NULL;
	}

	char *const display = malloc(len*3U + 1U);
	if(display == NULL)
	{
		return NULL;
	}

	size_t input_pos = 0U;
	size_t output_pos = 0U;
	while(input_pos < len)
	{
		utf8proc_int32_t codepoint;
		const utf8proc_ssize_t width = utf8proc_iterate(
				(const utf8proc_uint8_t *)bytes + input_pos,
				(utf8proc_ssize_t)(len - input_pos), &codepoint);
		if(width > 0 && !is_unsafe_codepoint(codepoint))
		{
			memcpy(display + output_pos, bytes + input_pos, (size_t)width);
			input_pos += (size_t)width;
			output_pos += (size_t)width;
		}
		else
		{
			memcpy(display + output_pos, replacement, sizeof(replacement));
			input_pos += (width > 0) ? (size_t)width : 1U;
			output_pos += sizeof(replacement);
		}
	}
	display[output_pos] = '\0';
	return display;
}

static char *
join_path(const char dir[], const char name[])
{
	const size_t dir_len = strlen(dir);
	const size_t name_len = strlen(name);
	const int needs_separator = dir_len != 0U && dir[dir_len - 1U] != '/';
	if(dir_len > SIZE_MAX - name_len - (size_t)needs_separator - 1U)
	{
		return NULL;
	}

	char *const path = malloc(dir_len + (size_t)needs_separator + name_len + 1U);
	if(path == NULL)
	{
		return NULL;
	}

	memcpy(path, dir, dir_len);
	if(needs_separator)
	{
		path[dir_len] = '/';
	}
	memcpy(path + dir_len + (size_t)needs_separator, name, name_len + 1U);
	return path;
}

static int64_t
milliseconds_from_parts(long double seconds, long nanoseconds)
{
	const long double value = seconds*1000.0L + nanoseconds/1000000.0L;
	if(value >= 0x1p63L)
	{
		return INT64_MAX;
	}
	if(value <= -0x1p63L)
	{
		return INT64_MIN;
	}
	return (int64_t)value;
}

static int64_t
stat_mtime_ms(const struct stat *st)
{
#if defined(__APPLE__)
	return milliseconds_from_parts(st->st_mtimespec.tv_sec,
			st->st_mtimespec.tv_nsec);
#elif defined(HAVE_STRUCT_STAT_ST_MTIM)
	return milliseconds_from_parts(st->st_mtim.tv_sec, st->st_mtim.tv_nsec);
#else
	return milliseconds_from_parts(st->st_mtime, 0L);
#endif
}

static nv_entry_kind_t
entry_kind(const struct stat *st)
{
	if(S_ISDIR(st->st_mode))
		return NV_ENTRY_DIRECTORY;
#ifdef S_ISLNK
	if(S_ISLNK(st->st_mode))
		return NV_ENTRY_SYMLINK;
#endif
	if(S_ISREG(st->st_mode))
		return (st->st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))
			? NV_ENTRY_EXECUTABLE
			: NV_ENTRY_FILE;
#ifdef S_ISFIFO
	if(S_ISFIFO(st->st_mode))
		return NV_ENTRY_FIFO;
#endif
#ifdef S_ISSOCK
	if(S_ISSOCK(st->st_mode))
		return NV_ENTRY_SOCKET;
#endif
#ifdef S_ISCHR
	if(S_ISCHR(st->st_mode))
		return NV_ENTRY_CHAR_DEVICE;
#endif
#ifdef S_ISBLK
	if(S_ISBLK(st->st_mode))
		return NV_ENTRY_BLOCK_DEVICE;
#endif
	return NV_ENTRY_UNKNOWN;
}

static int
has_suffix_ci(const char name[], const char suffix[])
{
	const size_t name_length = strlen(name);
	const size_t suffix_length = strlen(suffix);
	if(name_length < suffix_length) return 0;
	for(size_t i = 0U; i < suffix_length; ++i)
	{
		if(tolower((unsigned char)name[name_length - suffix_length + i]) !=
				tolower((unsigned char)suffix[i])) return 0;
	}
	return 1;
}

static nv_entry_resource_kind_t
entry_resource_kind(const char name[], nv_entry_kind_t kind)
{
	if(kind != NV_ENTRY_FILE && kind != NV_ENTRY_EXECUTABLE)
		return NV_ENTRY_RESOURCE_NONE;
	static const char *const archive_suffixes[] = {
		".zip", ".tar", ".tgz", ".tar.gz", ".tbz", ".tbz2",
		".tar.bz2", ".txz", ".tar.xz", ".7z", ".rar", ".jar",
	};
	for(size_t i = 0U; i < sizeof(archive_suffixes)/sizeof(archive_suffixes[0]); ++i)
	{
		if(has_suffix_ci(name, archive_suffixes[i]))
			return NV_ENTRY_RESOURCE_ARCHIVE;
	}
	return NV_ENTRY_RESOURCE_NONE;
}

#ifndef _WIN32
static char *
identity_display(uint64_t id, int group)
{
	char numeric[32];
	snprintf(numeric, sizeof(numeric), "%" PRIu64, id);
	/* Avoid NSS/Directory Services in snapshot generation. A bounded local
	 * file lookup is deterministic; remote or unavailable identities use the
	 * numeric fallback below. */
	const char *const path = group ? "/etc/group" : "/etc/passwd";
	FILE *const file = fopen(path, "r");
	if(file != NULL)
	{
		char line[1024];
		while(fgets(line, sizeof(line), file) != NULL)
		{
			if(strchr(line, '\n') == NULL && !feof(file))
			{
				int character;
				while((character = fgetc(file)) != '\n' && character != EOF) { }
				continue;
			}
			char *const newline = strchr(line, '\n');
			if(newline != NULL) *newline = '\0';
			char *const first_separator = strchr(line, ':');
			if(first_separator == NULL || first_separator == line) continue;
			*first_separator = '\0';
			char *field = first_separator + 1;
			const char *const uid_separator = strchr(field, ':');
			if(uid_separator == NULL) continue;
			field = (char *)uid_separator + 1;
			char *end = NULL;
			errno = 0;
			const unsigned long long parsed = strtoull(field, &end, 10);
			if(errno != 0 || end == field || *end != ':' || parsed != id) continue;
			if(strlen(line) > NV_PANE_SNAPSHOT_MAX_OWNER_BYTES) continue;
			fclose(file);
			return display_string(line);
		}
		fclose(file);
	}
	return strdup(numeric);
}
#endif

static void
set_stat(nv_pane_entry_t *entry, const struct stat *st, int is_symlink,
		nv_fs_identity_t identity)
{
	entry->kind = is_symlink ? NV_ENTRY_SYMLINK : entry_kind(st);
	entry->size_bytes = (st->st_size > 0) ? (uint64_t)st->st_size : 0U;
	entry->mtime_unix_ms = stat_mtime_ms(st);
	entry->device = identity.device;
	entry->inode = identity.inode;
	entry->ctime_unix_ns = identity.ctime_unix_ns;
	entry->mode = (uint32_t)st->st_mode;
	entry->has_stat = 1;
#ifndef _WIN32
	entry->owner_display = identity_display((uint64_t)st->st_uid, 0);
	entry->group_display = identity_display((uint64_t)st->st_gid, 1);
#endif
}

static int
build_entry(nv_dir_t *dir, const char directory[], const char name[],
		nv_pane_entry_t *entry)
{
	memset(entry, 0, sizeof(*entry));
	entry->kind = NV_ENTRY_UNKNOWN;
	entry->hidden = name[0] == '.';

	char *const raw_path = join_path(directory, name);
	if(raw_path == NULL)
	{
		return BUILD_ENTRY_NO_MEMORY;
	}
	if(!raw_string_may_fit_protocol_fields(name) ||
			!raw_string_may_fit_protocol_fields(raw_path))
	{
		free(raw_path);
		return BUILD_ENTRY_LIMIT_REACHED;
	}

	entry->name_display = display_string(name);
	entry->name_bytes_hex = bytes_to_hex(name);
	entry->path_display = display_string(raw_path);
	entry->path_bytes_hex = bytes_to_hex(raw_path);
	if(entry->name_display == NULL || entry->name_bytes_hex == NULL ||
			entry->path_display == NULL || entry->path_bytes_hex == NULL)
	{
		free(raw_path);
		entry_free(entry);
		return BUILD_ENTRY_NO_MEMORY;
	}
	if(!protocol_field_pair_fits(entry->name_display, entry->name_bytes_hex) ||
			!protocol_field_pair_fits(entry->path_display,
					entry->path_bytes_hex))
	{
		free(raw_path);
		entry_free(entry);
		return BUILD_ENTRY_LIMIT_REACHED;
	}

	struct stat st;
	int is_symlink = 0;
	nv_fs_identity_t identity = {};
	if(nv_dir_lstat(dir, name, &st, &is_symlink, &identity) == 0)
	{
		set_stat(entry, &st, is_symlink, identity);
		entry->resource_kind = entry_resource_kind(name, entry->kind);
#ifndef _WIN32
		if(entry->owner_display == NULL || entry->group_display == NULL)
		{
			free(raw_path);
			entry_free(entry);
			return BUILD_ENTRY_NO_MEMORY;
		}
#endif
	}
	else
	{
		entry->stat_error = errno;
		if(is_symlink)
		{
			entry->kind = NV_ENTRY_SYMLINK;
		}
	}
	free(raw_path);
	return 0;
}

static int
append_entry(nv_pane_snapshot_t *snapshot, nv_pane_entry_t *entry,
		size_t *capacity, size_t *protocol_bytes)
{
	if(snapshot->entry_count >= NV_PANE_SNAPSHOT_MAX_ENTRIES)
	{
		return APPEND_ENTRY_LIMIT_REACHED;
	}
	size_t entry_bytes;
	if(entry_protocol_budget(entry, &entry_bytes) != 0 ||
			entry_bytes > NV_PROTOCOL_MAX_RECORD_BYTES - *protocol_bytes)
	{
		return APPEND_ENTRY_LIMIT_REACHED;
	}
	if(snapshot->entry_count == *capacity)
	{
		const size_t next_capacity = (*capacity == 0U)
			? 16U
			: *capacity + *capacity/2U;
		if(next_capacity <= *capacity ||
				next_capacity > SIZE_MAX/sizeof(*snapshot->entries))
		{
			return APPEND_ENTRY_NO_MEMORY;
		}

		nv_pane_entry_t *const entries = realloc(snapshot->entries,
				next_capacity*sizeof(*snapshot->entries));
		if(entries == NULL)
		{
			return APPEND_ENTRY_NO_MEMORY;
		}
		snapshot->entries = entries;
		*capacity = next_capacity;
	}

	snapshot->entries[snapshot->entry_count] = *entry;
	++snapshot->entry_count;
	*protocol_bytes += entry_bytes;
	memset(entry, 0, sizeof(*entry));
	return 0;
}

static void
entry_free(nv_pane_entry_t *entry)
{
	free(entry->name_display);
	free(entry->name_bytes_hex);
	free(entry->path_display);
	free(entry->path_bytes_hex);
	free(entry->owner_display);
	free(entry->group_display);
	memset(entry, 0, sizeof(*entry));
}

static int
compare_name(const nv_pane_entry_t *left, const nv_pane_entry_t *right)
{
	return strcmp(left->name_bytes_hex, right->name_bytes_hex);
}

static const char *
entry_extension(const nv_pane_entry_t *entry)
{
	const char *const dot = strrchr(entry->name_display, '.');
	return dot == NULL || dot == entry->name_display ? "" : dot + 1;
}

static int
compare_uint64(uint64_t left, uint64_t right)
{
	return left < right ? -1 : left > right ? 1 : 0;
}

static int
compare_int64(int64_t left, int64_t right)
{
	return left < right ? -1 : left > right ? 1 : 0;
}

static int
compare_entry_group(const nv_pane_entry_t *left, const nv_pane_entry_t *right)
{
	/* Keep directories together regardless of the direction of the selected
	 * secondary sort key.  Symlinks remain in the non-directory group because
	 * this snapshot does not resolve link targets for presentation sorting. */
	const int left_group = left != NULL && left->kind == NV_ENTRY_DIRECTORY ? 0 : 1;
	const int right_group = right != NULL && right->kind == NV_ENTRY_DIRECTORY ? 0 : 1;
	return compare_int64(left_group, right_group);
}

static int
compare_entry_key(const nv_pane_entry_t *left, const nv_pane_entry_t *right,
		nv_pane_sort_key_t key)
{
	int result = 0;
	switch(key)
	{
		case NV_SORT_NAME:
			break;
		case NV_SORT_EXTENSION:
			result = strcmp(entry_extension(left), entry_extension(right));
			break;
		case NV_SORT_SIZE:
			result = compare_uint64(left->size_bytes, right->size_bytes);
			break;
		case NV_SORT_CTIME:
			result = compare_uint64(left->ctime_unix_ns, right->ctime_unix_ns);
			break;
		case NV_SORT_MTIME:
			result = compare_int64(left->mtime_unix_ms, right->mtime_unix_ms);
			break;
		case NV_SORT_MODE:
			result = compare_uint64(left->mode, right->mode);
			break;
		case NV_SORT_TYPE:
			result = compare_int64(left->kind, right->kind);
			break;
		case NV_SORT_OTHER:
			break;
	}
	return result == 0 ? compare_name(left, right) : result;
}

static int
compare_entries(const nv_pane_entry_t *left, const nv_pane_entry_t *right,
		nv_pane_sort_key_t key, int descending)
{
	const int group = compare_entry_group(left, right);
	if(group != 0) return group;
	return descending ? compare_entry_key(right, left, key) :
		compare_entry_key(left, right, key);
}

#define DEFINE_ENTRY_COMPARATOR(name, key, descending) \
	static int name(const void *left, const void *right) \
	{ \
		const nv_pane_entry_t *const lhs = left; \
		const nv_pane_entry_t *const rhs = right; \
		return compare_entries(lhs, rhs, key, descending); \
	}

DEFINE_ENTRY_COMPARATOR(compare_name_asc, NV_SORT_NAME, 0)
DEFINE_ENTRY_COMPARATOR(compare_name_desc, NV_SORT_NAME, 1)
DEFINE_ENTRY_COMPARATOR(compare_extension_asc, NV_SORT_EXTENSION, 0)
DEFINE_ENTRY_COMPARATOR(compare_extension_desc, NV_SORT_EXTENSION, 1)
DEFINE_ENTRY_COMPARATOR(compare_size_asc, NV_SORT_SIZE, 0)
DEFINE_ENTRY_COMPARATOR(compare_size_desc, NV_SORT_SIZE, 1)
DEFINE_ENTRY_COMPARATOR(compare_ctime_asc, NV_SORT_CTIME, 0)
DEFINE_ENTRY_COMPARATOR(compare_ctime_desc, NV_SORT_CTIME, 1)
DEFINE_ENTRY_COMPARATOR(compare_mtime_asc, NV_SORT_MTIME, 0)
DEFINE_ENTRY_COMPARATOR(compare_mtime_desc, NV_SORT_MTIME, 1)
DEFINE_ENTRY_COMPARATOR(compare_mode_asc, NV_SORT_MODE, 0)
DEFINE_ENTRY_COMPARATOR(compare_mode_desc, NV_SORT_MODE, 1)
DEFINE_ENTRY_COMPARATOR(compare_type_asc, NV_SORT_TYPE, 0)
DEFINE_ENTRY_COMPARATOR(compare_type_desc, NV_SORT_TYPE, 1)
DEFINE_ENTRY_COMPARATOR(compare_other_asc, NV_SORT_OTHER, 0)
DEFINE_ENTRY_COMPARATOR(compare_other_desc, NV_SORT_OTHER, 1)

#undef DEFINE_ENTRY_COMPARATOR

typedef int (*entry_comparator_t)(const void *, const void *);

static entry_comparator_t
entry_comparator(nv_pane_sort_key_t key, int descending)
{
	switch(key)
	{
		case NV_SORT_NAME: return descending ? compare_name_desc : compare_name_asc;
		case NV_SORT_EXTENSION: return descending ? compare_extension_desc : compare_extension_asc;
		case NV_SORT_SIZE: return descending ? compare_size_desc : compare_size_asc;
		case NV_SORT_CTIME: return descending ? compare_ctime_desc : compare_ctime_asc;
		case NV_SORT_MTIME: return descending ? compare_mtime_desc : compare_mtime_asc;
		case NV_SORT_MODE: return descending ? compare_mode_desc : compare_mode_asc;
		case NV_SORT_TYPE: return descending ? compare_type_desc : compare_type_asc;
		case NV_SORT_OTHER: return descending ? compare_other_desc : compare_other_asc;
	}
	return NULL;
}

void
nv_pane_snapshot_sort(nv_pane_snapshot_t *snapshot, nv_pane_sort_key_t key,
		int descending)
{
	if(snapshot == NULL)
	{
		return;
	}
	entry_comparator_t const comparator = entry_comparator(key, descending != 0);
	if(comparator == NULL)
	{
		return;
	}
	const char *const cursor_identity = snapshot->cursor < 0 ? NULL :
		snapshot->entries[snapshot->cursor].path_bytes_hex;
	if(snapshot->entry_count > 1U)
	{
		qsort(snapshot->entries, snapshot->entry_count,
				sizeof(*snapshot->entries), comparator);
	}
	if(cursor_identity != NULL)
	{
		for(size_t i = 0U; i < snapshot->entry_count; ++i)
		{
			if(snapshot->entries[i].path_bytes_hex == cursor_identity)
			{
				snapshot->cursor = (int)i;
				break;
			}
		}
	}
	snapshot->sort_key = key;
	snapshot->sort_descending = descending != 0;
}

static int
retryable_error(int error_number)
{
	return error_number == EINTR || error_number == EAGAIN ||
			error_number == EMFILE || error_number == ENFILE;
}

static int
set_error(nv_snapshot_error_t *error, const char code[], int os_error,
		const char path[])
{
	char *const error_code = strdup(code);
	char *const message = display_string(strerror(os_error));
	char *path_display = NULL;
	char *path_bytes_hex = NULL;
	if(path != NULL && raw_string_may_fit_protocol_fields(path))
	{
		path_display = display_string(path);
		path_bytes_hex = bytes_to_hex(path);
		if(path_display == NULL || path_bytes_hex == NULL)
		{
			free(error_code);
			free(message);
			free(path_display);
			free(path_bytes_hex);
			return -1;
		}
		if(!protocol_field_pair_fits(path_display, path_bytes_hex))
		{
			free(path_display);
			free(path_bytes_hex);
			path_display = NULL;
			path_bytes_hex = NULL;
		}
	}
	if(error_code == NULL || message == NULL)
	{
		free(error_code);
		free(message);
		free(path_display);
		free(path_bytes_hex);
		return -1;
	}

	error->code = error_code;
	error->message = message;
	error->path_display = path_display;
	error->path_bytes_hex = path_bytes_hex;
	error->os_error = os_error;
	error->retryable = retryable_error(os_error);
	return -1;
}

static int
initialize_snapshot(const char path[], nv_pane_snapshot_t *snapshot,
		nv_snapshot_error_t *error)
{
	if(!raw_string_may_fit_protocol_fields(path))
	{
		return set_error(error, "snapshot-too-large", E2BIG, path);
	}
	snapshot->cwd_display = display_string(path);
	snapshot->cwd_bytes_hex = bytes_to_hex(path);
	if(snapshot->cwd_display == NULL || snapshot->cwd_bytes_hex == NULL)
	{
		return set_error(error, "out-of-memory", ENOMEM, path);
	}
	if(!protocol_field_pair_fits(snapshot->cwd_display,
			snapshot->cwd_bytes_hex))
	{
		free(snapshot->cwd_display);
		free(snapshot->cwd_bytes_hex);
		snapshot->cwd_display = NULL;
		snapshot->cwd_bytes_hex = NULL;
		return set_error(error, "snapshot-too-large", E2BIG, path);
	}
	return 0;
}

static int
scan_directory(nv_dir_t *dir, const char path[], nv_pane_snapshot_t *snapshot,
		nv_snapshot_error_t *error, size_t *capacity, size_t *protocol_bytes)
{
	for(;;)
	{
		errno = 0;
		const char *const name = nv_dir_read(dir);
		if(name == NULL)
		{
			return (errno == 0) ? 0 : set_error(error, "read-directory", errno,
					path);
		}
		if(strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		{
			continue;
		}

		nv_pane_entry_t entry;
		const int build_result = build_entry(dir, path, name, &entry);
		if(build_result != 0)
		{
			entry_free(&entry);
			return set_error(error,
					build_result == BUILD_ENTRY_LIMIT_REACHED
						? "snapshot-too-large"
						: "out-of-memory",
					build_result == BUILD_ENTRY_LIMIT_REACHED ? E2BIG : ENOMEM, path);
		}
		const int append_result = append_entry(snapshot, &entry, capacity,
				protocol_bytes);
		if(append_result != 0)
		{
			entry_free(&entry);
			return set_error(error,
					append_result == APPEND_ENTRY_LIMIT_REACHED
						? "snapshot-too-large"
						: "out-of-memory",
					append_result == APPEND_ENTRY_LIMIT_REACHED ? E2BIG : ENOMEM,
					path);
		}
	}
}

static int64_t
current_time_ms(void)
{
#ifndef _WIN32
	struct timeval now;
	if(gettimeofday(&now, NULL) != 0)
	{
		return 0;
	}
	return milliseconds_from_parts(now.tv_sec, now.tv_usec*1000L);
#else
	const time_t now = time(NULL);
	if(now == (time_t)-1)
	{
		return 0;
	}
	return milliseconds_from_parts(now, 0L);
#endif
}

int
nv_pane_snapshot_build(const char path[], nv_pane_snapshot_t *snapshot,
		nv_snapshot_error_t *error)
{
	if(snapshot == NULL || error == NULL)
	{
		return -1;
	}

	nv_pane_snapshot_t next_snapshot = {};
	nv_snapshot_error_t next_error = {};
	size_t capacity = 0U;
	size_t protocol_bytes = 0U;
	if(path == NULL || path[0] == '\0')
	{
		set_error(&next_error, "invalid-path", EINVAL, path);
		goto failed;
	}
	if(!raw_string_may_fit_protocol_fields(path))
	{
		set_error(&next_error, "snapshot-too-large", E2BIG, path);
		goto failed;
	}

	nv_dir_t *const dir = nv_dir_open(path);
	if(dir == NULL)
	{
		set_error(&next_error, "open-directory", errno, path);
		goto failed;
	}
	struct stat cwd_stat;
	nv_fs_identity_t cwd_identity = {};
	if(nv_dir_fstat(dir, &cwd_stat, &cwd_identity) != 0 ||
			!S_ISDIR(cwd_stat.st_mode))
	{
		const int stat_error = errno == 0 ? ENOTDIR : errno;
		nv_dir_close(dir);
		set_error(&next_error, "stat-directory", stat_error, path);
		goto failed;
	}
	next_snapshot.cwd_device = cwd_identity.device;
	next_snapshot.cwd_inode = cwd_identity.inode;
	next_snapshot.cwd_ctime_unix_ns = cwd_identity.ctime_unix_ns;
	next_snapshot.has_cwd_stat = 1;
	if(initialize_snapshot(path, &next_snapshot, &next_error) != 0 ||
			snapshot_protocol_budget(&next_snapshot, &protocol_bytes) != 0 ||
			scan_directory(dir, path, &next_snapshot, &next_error, &capacity,
					&protocol_bytes) != 0)
	{
		if(next_error.code == NULL)
		{
			set_error(&next_error, "snapshot-too-large", E2BIG, path);
		}
		nv_dir_close(dir);
		goto failed;
	}
	struct stat published_stat;
	nv_fs_identity_t published_identity = {};
	if(nv_lstat(path, &published_stat, NULL, &published_identity) != 0 ||
			published_identity.device != cwd_identity.device ||
			published_identity.inode != cwd_identity.inode)
	{
		const int stat_error = errno == 0 ? NV_FS_STALE_ERRNO : errno;
		nv_dir_close(dir);
		set_error(&next_error, "stale-directory", stat_error, path);
		goto failed;
	}
	if(nv_dir_close(dir) != 0)
	{
		const int close_error = errno;
		set_error(&next_error, "close-directory", close_error, path);
		goto failed;
	}

	next_snapshot.generated_at_unix_ms = current_time_ms();
	next_snapshot.cursor = -1;
	nv_pane_snapshot_sort(&next_snapshot, NV_SORT_NAME, 0);
	next_snapshot.cursor = (next_snapshot.entry_count == 0U) ? -1 : 0;

	nv_pane_snapshot_free(snapshot);
	nv_snapshot_error_free(error);
	*snapshot = next_snapshot;
	return 0;

failed:
	nv_pane_snapshot_free(&next_snapshot);
	nv_snapshot_error_free(error);
	*error = next_error;
	return -1;
}

void
nv_pane_snapshot_free(nv_pane_snapshot_t *snapshot)
{
	if(snapshot == NULL)
	{
		return;
	}
	for(size_t i = 0U; i < snapshot->entry_count; ++i)
	{
		entry_free(&snapshot->entries[i]);
	}
	free(snapshot->entries);
	free(snapshot->cwd_display);
	free(snapshot->cwd_bytes_hex);
	memset(snapshot, 0, sizeof(*snapshot));
}

void
nv_snapshot_error_free(nv_snapshot_error_t *error)
{
	if(error == NULL)
	{
		return;
	}
	free(error->code);
	free(error->message);
	free(error->path_display);
	free(error->path_bytes_hex);
	memset(error, 0, sizeof(*error));
}

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 filetype=c : */
