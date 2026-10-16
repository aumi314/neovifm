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
	HANDLE directory;
	HANDLE event;
	OVERLAPPED overlapped;
	BY_HANDLE_FILE_INFORMATION identity;
	unsigned char buffer[64U*1024U];
	int pending;
	wchar_t *wpath;
};

static wchar_t * wide_path(const char path[]);
static int start_watch(fswatch_t *watcher);
static int path_replaced(const fswatch_t *watcher, int *replaced);

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

	w->directory = CreateFileW(w->wpath, FILE_LIST_DIRECTORY,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
			OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED |
			FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	if(w->directory == INVALID_HANDLE_VALUE)
	{
		free(w->wpath);
		free(w);
		return NULL;
	}
	if(!GetFileInformationByHandle(w->directory, &w->identity))
	{
		CloseHandle(w->directory);
		free(w->wpath);
		free(w);
		return NULL;
	}
	w->event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if(w->event == NULL)
	{
		CloseHandle(w->directory);
		free(w->wpath);
		free(w);
		return NULL;
	}
	memset(&w->overlapped, 0, sizeof(w->overlapped));
	w->overlapped.hEvent = w->event;
	w->pending = 0;
	if(start_watch(w) != 0)
	{
		CloseHandle(w->event);
		CloseHandle(w->directory);
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
		if(w->pending)
		{
			DWORD ignored;
			CancelIo(w->directory);
			(void)GetOverlappedResult(w->directory, &w->overlapped, &ignored, TRUE);
		}
		CloseHandle(w->event);
		CloseHandle(w->directory);
		free(w->wpath);
		free(w);
	}
}

FSWatchState
fswatch_poll(fswatch_t *w)
{
	int replaced = 0;
	if(path_replaced(w, &replaced) != 0) return FSWS_ERRORED;
	if(replaced) return FSWS_REPLACED;
	const DWORD wait = WaitForSingleObject(w->event, 0U);
	if(wait == WAIT_TIMEOUT) return FSWS_UNCHANGED;
	if(wait != WAIT_OBJECT_0) return FSWS_ERRORED;
	DWORD transferred = 0U;
	if(!GetOverlappedResult(w->directory, &w->overlapped, &transferred, FALSE))
	{
		const DWORD error = GetLastError();
		if(error == ERROR_IO_INCOMPLETE) return FSWS_UNCHANGED;
		if(error != ERROR_NOTIFY_ENUM_DIR) return FSWS_ERRORED;
	}
	w->pending = 0;
	if(start_watch(w) != 0) return FSWS_ERRORED;
	return FSWS_UPDATED;
}

static int
path_replaced(const fswatch_t *watcher, int *replaced)
{
	const HANDLE current = CreateFileW(watcher->wpath, 0U,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
			OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS |
			FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	if(current == INVALID_HANDLE_VALUE) return 1;
	BY_HANDLE_FILE_INFORMATION identity = {};
	const int failed = !GetFileInformationByHandle(current, &identity);
	CloseHandle(current);
	if(failed) return 1;
	*replaced = identity.dwVolumeSerialNumber !=
			watcher->identity.dwVolumeSerialNumber ||
		identity.nFileIndexHigh != watcher->identity.nFileIndexHigh ||
		identity.nFileIndexLow != watcher->identity.nFileIndexLow;
	return 0;
}

static int
start_watch(fswatch_t *watcher)
{
	ResetEvent(watcher->event);
	memset(&watcher->overlapped, 0, sizeof(watcher->overlapped));
	watcher->overlapped.hEvent = watcher->event;
	if(!ReadDirectoryChangesW(watcher->directory, watcher->buffer,
			sizeof(watcher->buffer), TRUE,
			FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
			FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
			FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION |
			FILE_NOTIFY_CHANGE_SECURITY,
			NULL, &watcher->overlapped, NULL))
		return 1;
	watcher->pending = 1;
	return 0;
}

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 filetype=c : */
