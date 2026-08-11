/* vifm
 * Copyright (C) 2015 xaizek.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "fswatch.h"

#include <windows.h>

#include <stdlib.h> /* free() malloc() */
#include <string.h> /* memcpy strdup */
#include <wchar.h> /* wchar_t wcslen() wcsncmp() */

#include "utf8.h"

/* Watcher data. */
struct fswatch_t
{
	FILETIME dir_mtime;
	HANDLE dir_watcher;
	wchar_t *wpath;
};

static int get_dir_mtime(const wchar_t dir_path[], FILETIME *ft);
static wchar_t * wide_path(const char path[]);

static wchar_t *
wide_path(const char path[])
{
	wchar_t *absolute = utf8_to_utf16(path);
	if(absolute == NULL) return NULL;
	/* Preserve the classic watcher path for ordinary paths.  Some Win32 change
	 * notification behavior differs for extended paths, while the prefix is
	 * only needed once the path can exceed MAX_PATH. */
	if(wcslen(absolute) < MAX_PATH - 2U) return absolute;
	if(wcsncmp(absolute, L"\\\\?\\", 4U) != 0)
	{
		const size_t initial_length = wcslen(absolute);
		if(initial_length < 3U ||
				!((absolute[0] >= L'A' && absolute[0] <= L'Z') ||
				  (absolute[0] >= L'a' && absolute[0] <= L'z')) ||
				absolute[1] != L':' ||
				(absolute[2] != L'\\' && absolute[2] != L'/'))
		{
			const DWORD required = GetFullPathNameW(absolute, 0U, NULL, NULL);
			if(required == 0U) { free(absolute); return NULL; }
			wchar_t *const full = malloc((size_t)required*sizeof(*full));
			if(full == NULL) { free(absolute); return NULL; }
			if(GetFullPathNameW(absolute, required, full, NULL) == 0U)
			{
				free(full);
				free(absolute);
				return NULL;
			}
			free(absolute);
			absolute = full;
		}
		const int unc = absolute[0] == L'\\' && absolute[1] == L'\\';
		const size_t length = wcslen(absolute);
		const wchar_t prefix[] = L"\\\\?\\";
		const wchar_t unc_prefix[] = L"\\\\?\\UNC\\";
		const size_t prefix_length = unc ? 8U : 4U;
		const size_t skipped = unc ? 2U : 0U;
		wchar_t *const extended = malloc((prefix_length + length - skipped + 1U)*
				sizeof(*extended));
		if(extended == NULL) { free(absolute); return NULL; }
		memcpy(extended, unc ? unc_prefix : prefix,
				prefix_length*sizeof(*extended));
		memcpy(extended + prefix_length, absolute + skipped,
				(length - skipped + 1U)*sizeof(*extended));
		free(absolute);
		absolute = extended;
	}
	for(wchar_t *cursor = absolute; *cursor != L'\0'; ++cursor)
		if(*cursor == L'/') *cursor = L'\\';
	return absolute;
}

fswatch_t *
fswatch_create(const char path[])
{
	fswatch_t *const w = malloc(sizeof(*w));
	if(w == NULL)
	{
		return NULL;
	}

	w->wpath = wide_path(path);
	if(w->wpath == NULL)
	{
		free(w);
		return NULL;
	}

	if(get_dir_mtime(w->wpath, &w->dir_mtime) != 0)
	{
		free(w->wpath);
		free(w);
		return NULL;
	}

	w->dir_watcher = FindFirstChangeNotificationW(w->wpath, 1,
			FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
			FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
			FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SECURITY);
	if(w->dir_watcher == INVALID_HANDLE_VALUE)
	{
		free(w->wpath);
		free(w);
		return NULL;
	}

	return w;
}

void
fswatch_free(fswatch_t *w)
{
	if(w != NULL)
	{
		FindCloseChangeNotification(w->dir_watcher);
		free(w->wpath);
		free(w);
	}
}

FSWatchState
fswatch_poll(fswatch_t *w)
{
	FILETIME ft;
	if(get_dir_mtime(w->wpath, &ft) != 0)
	{
		return FSWS_ERRORED;
	}

	int changed = CompareFileTime(&w->dir_mtime, &ft) != 0;
	w->dir_mtime = ft;

	if(WaitForSingleObject(w->dir_watcher, 0) == WAIT_OBJECT_0)
	{
		FindNextChangeNotification(w->dir_watcher);
		changed = 1;
	}

	return (changed ? FSWS_UPDATED : FSWS_UNCHANGED);
}

/* Gets last directory modification time.  Returns non-zero on error, otherwise
 * zero is returned. */
static int
get_dir_mtime(const wchar_t dir_path[], FILETIME *ft)
{
	const size_t length = wcslen(dir_path);
	const int needs_separator = length != 0U && dir_path[length - 1U] != L'\\';
	wchar_t *const selfref_path = malloc((length + (size_t)needs_separator + 2U)*
			sizeof(*selfref_path));
	if(selfref_path == NULL) return 1;
	memcpy(selfref_path, dir_path, length*sizeof(*selfref_path));
	if(needs_separator) selfref_path[length] = L'\\';
	selfref_path[length + (size_t)needs_separator] = L'.';
	selfref_path[length + (size_t)needs_separator + 1U] = L'\0';

	const HANDLE hfile = CreateFileW(selfref_path, 0,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
			OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	free(selfref_path);

	if(hfile == INVALID_HANDLE_VALUE)
	{
		return 1;
	}

	const int error = GetFileTime(hfile, NULL, NULL, ft) == FALSE;
	CloseHandle(hfile);

	return error;
}

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 filetype=c : */
