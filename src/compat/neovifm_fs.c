/* vifm
 * Copyright (C) 2026 NeoVifm contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "neovifm_fs.h"

#include <errno.h> /* E* errno */
#include <stdint.h> /* SIZE_MAX */
#include <stdlib.h> /* free() malloc() */
#include <string.h> /* memcpy() strlen() */

#ifdef _WIN32

#define COBJMACROS
#include <windows.h> /* FindClose() FindFirstFileW() FindNextFileW() */
#include <objbase.h> /* CoCreateGuid() CoCreateInstance() */
#include <shobjidl.h> /* IFileOperation IShellItem */
#include <winioctl.h> /* IO_REPARSE_TAG_SYMLINK */
#include <winternl.h> /* RTL_OSVERSIONINFOW */

#include <errno.h> /* E* errno */
#include <stddef.h> /* size_t */
#include <stdint.h> /* INT64_MAX */
#include <stdlib.h> /* free() malloc() */
#include <string.h> /* memset() */
#include <sys/stat.h> /* S_IF* S_I* struct stat */
#include <time.h> /* time_t */
#include <stdio.h> /* swprintf() */
#include <wchar.h> /* wchar_t wcslen() */

#include "../utils/utf8.h"

struct nv_dir_t
{
	HANDLE handle;
	HANDLE directory;
	WIN32_FIND_DATAW entry;
	char *path;
	char *name;
	int has_first_entry;
};

static void set_errno_from_windows_error(DWORD error);
static wchar_t * wide_path(const char path[]);
static wchar_t * search_pattern(const char path[]);
static time_t filetime_to_time_t(FILETIME file_time);
static uint64_t filetime_to_unix_ns(FILETIME file_time);
static void stat_from_find_data(struct stat *st,
		const WIN32_FIND_DATAW *entry);
static void stat_from_handle_data(struct stat *st,
		const BY_HANDLE_FILE_INFORMATION *entry);
static int handle_identity(HANDLE handle, struct stat *st,
		nv_fs_identity_t *identity, DWORD *attributes);

static void
set_errno_from_windows_error(DWORD error)
{
	switch(error)
	{
		case ERROR_ACCESS_DENIED:
			errno = EACCES;
			break;
		case ERROR_FILE_NOT_FOUND:
		case ERROR_PATH_NOT_FOUND:
		case ERROR_DIRECTORY:
			errno = ENOENT;
			break;
		case ERROR_INVALID_NAME:
			errno = EINVAL;
			break;
		case ERROR_ALREADY_EXISTS:
		case ERROR_FILE_EXISTS:
			errno = EEXIST;
			break;
		case ERROR_DIR_NOT_EMPTY:
			errno = ENOTEMPTY;
			break;
		case ERROR_NOT_SAME_DEVICE:
			errno = EXDEV;
			break;
		case ERROR_OPERATION_ABORTED:
			errno = ECANCELED;
			break;
		case ERROR_NOT_SUPPORTED:
		case ERROR_CALL_NOT_IMPLEMENTED:
			errno = ENOTSUP;
			break;
		case ERROR_FILENAME_EXCED_RANGE:
			errno = ENAMETOOLONG;
			break;
		case ERROR_DISK_FULL:
			errno = ENOSPC;
			break;
		case ERROR_NOT_ENOUGH_MEMORY:
			errno = ENOMEM;
			break;
		default:
			errno = EIO;
			break;
	}
}

static wchar_t *
wide_path(const char path[])
{
	wchar_t *absolute = utf8_to_utf16(path);
	if(absolute == NULL) { errno = ENOMEM; return NULL; }
	if(wcsncmp(absolute, L"\\\\?\\", 4U) == 0) return absolute;
	const size_t initial_length = wcslen(absolute);
	if(initial_length < 3U || !((absolute[0] >= L'A' && absolute[0] <= L'Z') ||
			(absolute[0] >= L'a' && absolute[0] <= L'z')) || absolute[1] != L':' ||
			(absolute[2] != L'\\' && absolute[2] != L'/'))
	{
		const DWORD required = GetFullPathNameW(absolute, 0U, NULL, NULL);
		if(required == 0U)
		{
			const DWORD error = GetLastError();
			free(absolute);
			set_errno_from_windows_error(error);
			return NULL;
		}
		wchar_t *const full = malloc((size_t)required*sizeof(*full));
		if(full == NULL)
		{
			free(absolute);
			errno = ENOMEM;
			return NULL;
		}
		if(GetFullPathNameW(absolute, required, full, NULL) == 0U)
		{
			const DWORD error = GetLastError();
			free(full);
			free(absolute);
			set_errno_from_windows_error(error);
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
	if(extended == NULL)
	{
		free(absolute);
		errno = ENOMEM;
		return NULL;
	}
	memcpy(extended, unc ? unc_prefix : prefix,
			prefix_length*sizeof(*extended));
	memcpy(extended + prefix_length, absolute + skipped,
			(length - skipped + 1U)*sizeof(*extended));
	for(wchar_t *p = extended; *p != L'\0'; ++p)
		if(*p == L'/') *p = L'\\';
	free(absolute);
	return extended;
}

static wchar_t *
search_pattern(const char path[])
{
	wchar_t *const directory = wide_path(path);
	if(directory == NULL)
	{
		errno = ENOMEM;
		return NULL;
	}

	const size_t length = wcslen(directory);
	const int needs_separator = length != 0U && directory[length - 1U] != L'/' &&
			directory[length - 1U] != L'\\';
	wchar_t *const pattern = malloc((length + (size_t)needs_separator + 2U)*
			sizeof(*pattern));
	if(pattern == NULL)
	{
		free(directory);
		errno = ENOMEM;
		return NULL;
	}

	memcpy(pattern, directory, length*sizeof(*pattern));
	if(needs_separator)
	{
		pattern[length] = L'\\';
	}
	pattern[length + (size_t)needs_separator] = L'*';
	pattern[length + (size_t)needs_separator + 1U] = L'\0';
	free(directory);
	return pattern;
}

nv_dir_t *
nv_dir_open(const char path[])
{
	wchar_t *const native_path = wide_path(path);
	if(native_path == NULL) return NULL;
	const HANDLE directory = CreateFileW(native_path,
			FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
			OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS |
			FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	free(native_path);
	if(directory == INVALID_HANDLE_VALUE)
	{
		set_errno_from_windows_error(GetLastError());
		return NULL;
	}
	DWORD attributes = 0U;
	if(handle_identity(directory, NULL, NULL, &attributes) != 0 ||
			(attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
			(attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
	{
		const int saved = errno == 0 ? ENOTDIR : errno;
		CloseHandle(directory);
		errno = saved;
		return NULL;
	}
	wchar_t *const pattern = search_pattern(path);
	if(pattern == NULL)
	{
		CloseHandle(directory);
		return NULL;
	}

	WIN32_FIND_DATAW entry;
	const HANDLE handle = FindFirstFileW(pattern, &entry);
	free(pattern);
	if(handle == INVALID_HANDLE_VALUE)
	{
		CloseHandle(directory);
		set_errno_from_windows_error(GetLastError());
		return NULL;
	}

	nv_dir_t *const dir = malloc(sizeof(*dir));
	if(dir == NULL)
	{
		(void)FindClose(handle);
		CloseHandle(directory);
		errno = ENOMEM;
		return NULL;
	}

	*dir = (nv_dir_t){ .handle = handle, .directory = directory,
		.entry = entry, .path = strdup(path),
		.has_first_entry = 1 };
	if(dir->path == NULL)
	{
		(void)FindClose(handle);
		CloseHandle(directory);
		free(dir);
		errno = ENOMEM;
		return NULL;
	}
	return dir;
}

const char *
nv_dir_read(nv_dir_t *dir)
{
	if(dir->has_first_entry)
	{
		dir->has_first_entry = 0;
	}
	else if(!FindNextFileW(dir->handle, &dir->entry))
	{
		const DWORD error = GetLastError();
		if(error == ERROR_NO_MORE_FILES)
		{
			errno = 0;
		}
		else
		{
			set_errno_from_windows_error(error);
		}
		return NULL;
	}

	char *const name = utf8_from_utf16(dir->entry.cFileName);
	if(name == NULL)
	{
		errno = ENOMEM;
		return NULL;
	}
	free(dir->name);
	dir->name = name;
	return dir->name;
}

int
nv_dir_close(nv_dir_t *dir)
{
	const int find_result = FindClose(dir->handle) ? 0 : -1;
	const int directory_result = CloseHandle(dir->directory) ? 0 : -1;
	const int result = find_result == 0 && directory_result == 0 ? 0 : -1;
	if(result != 0)
	{
		set_errno_from_windows_error(GetLastError());
	}
	free(dir->name);
	free(dir->path);
	free(dir);
	return result;
}

int
nv_dir_fstat(nv_dir_t *dir, struct stat *st, nv_fs_identity_t *identity)
{
	if(dir == NULL || st == NULL) { errno = EINVAL; return -1; }
	return handle_identity(dir->directory, st, identity, NULL);
}

int
nv_dir_lstat(nv_dir_t *dir, const char name[], struct stat *st,
		int *is_symlink, nv_fs_identity_t *identity)
{
	if(dir == NULL || name == NULL || st == NULL) { errno = EINVAL; return -1; }
	const size_t length = strlen(dir->path), name_length = strlen(name);
	const int slash = length != 0U && dir->path[length - 1U] != '/' &&
		dir->path[length - 1U] != '\\';
	char *const path = malloc(length + (size_t)slash + name_length + 1U);
	if(path == NULL) { errno = ENOMEM; return -1; }
	memcpy(path, dir->path, length);
	if(slash) path[length] = '/';
	memcpy(path + length + (size_t)slash, name, name_length + 1U);
	const int result = nv_lstat(path, st, is_symlink, identity);
	free(path);
	return result;
}

static time_t
filetime_to_time_t(FILETIME file_time)
{
	ULARGE_INTEGER ticks = {
		.LowPart = file_time.dwLowDateTime,
		.HighPart = file_time.dwHighDateTime,
	};
	static const ULONGLONG WINDOWS_EPOCH_OFFSET = 116444736000000000ULL;
	static const ULONGLONG TICKS_PER_SECOND = 10000000ULL;
	if(ticks.QuadPart <= WINDOWS_EPOCH_OFFSET)
	{
		return 0;
	}

	const ULONGLONG seconds = (ticks.QuadPart - WINDOWS_EPOCH_OFFSET)/
		TICKS_PER_SECOND;
	return seconds > (ULONGLONG)INT64_MAX ? (time_t)INT64_MAX :
		(time_t)seconds;
}

static uint64_t
filetime_to_unix_ns(FILETIME file_time)
{
	ULARGE_INTEGER ticks = {
		.LowPart = file_time.dwLowDateTime,
		.HighPart = file_time.dwHighDateTime,
	};
	static const ULONGLONG WINDOWS_EPOCH_OFFSET = 116444736000000000ULL;
	if(ticks.QuadPart <= WINDOWS_EPOCH_OFFSET) return 0U;
	const ULONGLONG unix_ticks = ticks.QuadPart - WINDOWS_EPOCH_OFFSET;
	return unix_ticks > UINT64_MAX/100U ? UINT64_MAX : (uint64_t)unix_ticks*100U;
}

static void
stat_from_find_data(struct stat *st, const WIN32_FIND_DATAW *entry)
{
	ULARGE_INTEGER size = {
		.LowPart = entry->nFileSizeLow,
		.HighPart = entry->nFileSizeHigh,
	};
	memset(st, 0, sizeof(*st));
	st->st_mode = (entry->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
		? S_IFDIR
		: S_IFREG;
	st->st_mode |= S_IREAD;
	if((entry->dwFileAttributes & FILE_ATTRIBUTE_READONLY) == 0)
	{
		st->st_mode |= S_IWRITE;
	}
	st->st_size = size.QuadPart > (ULONGLONG)INT64_MAX
		? (off_t)INT64_MAX
		: (off_t)size.QuadPart;
	st->st_atime = filetime_to_time_t(entry->ftLastAccessTime);
	st->st_mtime = filetime_to_time_t(entry->ftLastWriteTime);
	st->st_ctime = filetime_to_time_t(entry->ftCreationTime);
}

static void
stat_from_handle_data(struct stat *st, const BY_HANDLE_FILE_INFORMATION *entry)
{
	WIN32_FIND_DATAW data = {
		.dwFileAttributes = entry->dwFileAttributes,
		.ftCreationTime = entry->ftCreationTime,
		.ftLastAccessTime = entry->ftLastAccessTime,
		.ftLastWriteTime = entry->ftLastWriteTime,
		.nFileSizeHigh = entry->nFileSizeHigh,
		.nFileSizeLow = entry->nFileSizeLow,
	};
	stat_from_find_data(st, &data);
}

static int
handle_identity(HANDLE handle, struct stat *st, nv_fs_identity_t *identity,
		DWORD *attributes)
{
	BY_HANDLE_FILE_INFORMATION info;
	if(!GetFileInformationByHandle(handle, &info))
	{
		set_errno_from_windows_error(GetLastError());
		return -1;
	}
	if(st != NULL) stat_from_handle_data(st, &info);
	if(identity != NULL)
	{
		*identity = (nv_fs_identity_t){
			.device = (uint64_t)info.dwVolumeSerialNumber,
			.inode = ((uint64_t)info.nFileIndexHigh << 32U) |
				(uint64_t)info.nFileIndexLow,
			.ctime_unix_ns = filetime_to_unix_ns(info.ftCreationTime),
		};
	}
	if(attributes != NULL) *attributes = info.dwFileAttributes;
	return 0;
}

int
nv_lstat(const char path[], struct stat *st, int *is_symlink,
		nv_fs_identity_t *identity)
{
	if(path == NULL || st == NULL) { errno = EINVAL; return -1; }
	if(is_symlink != NULL) *is_symlink = 0;
	wchar_t *const native_path = wide_path(path);
	if(native_path == NULL) return -1;
	const HANDLE handle = CreateFileW(native_path, FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
			OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS |
			FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	free(native_path);
	if(handle == INVALID_HANDLE_VALUE)
	{
		set_errno_from_windows_error(GetLastError());
		return -1;
	}
	DWORD attributes = 0U;
	const int result = handle_identity(handle, st, identity, &attributes);
	const int saved = errno;
	if(is_symlink != NULL)
		*is_symlink = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
	if(!CloseHandle(handle) && result == 0)
	{
		set_errno_from_windows_error(GetLastError());
		return -1;
	}
	if(result != 0) errno = saved;
	return result;
}

int
nv_fs_actions_supported(void)
{
	typedef LONG (WINAPI *rtl_get_version_fn)(PRTL_OSVERSIONINFOW);
	const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	if(ntdll == NULL) return 0;
	rtl_get_version_fn get_version = (rtl_get_version_fn)(void *)
		GetProcAddress(ntdll, "RtlGetVersion");
	if(get_version == NULL) return 0;
	RTL_OSVERSIONINFOW version = { .dwOSVersionInfoSize = sizeof(version) };
	return get_version(&version) == 0 && version.dwMajorVersion >= 10U;
}

#ifndef FOFX_RECYCLEONDELETE
#define FOFX_RECYCLEONDELETE 0x00080000U
#endif
#ifndef FOFX_EARLYFAILURE
#define FOFX_EARLYFAILURE 0x00100000U
#endif
#ifndef FOFX_NOCOPYHOOKS
#define FOFX_NOCOPYHOOKS 0x00800000U
#endif

typedef struct
{
	HANDLE handle;
	wchar_t *path;
	wchar_t *name;
} nv_parent_entry_t;

static nv_fs_test_before_atomic_hook test_before_atomic_hook;
static int test_cross_device_move;

void
nv_fs_test_set_before_atomic_hook(nv_fs_test_before_atomic_hook hook)
{
	test_before_atomic_hook = hook;
}

void
nv_fs_test_force_cross_device_move(int enabled)
{
	test_cross_device_move = enabled;
}

static int
identity_equal(nv_fs_identity_t left, nv_fs_identity_t right)
{
	return left.device == right.device && left.inode == right.inode &&
		left.ctime_unix_ns == right.ctime_unix_ns;
}

static wchar_t *
wide_dup(const wchar_t value[])
{
	const size_t length = wcslen(value);
	wchar_t *const copy = malloc((length + 1U)*sizeof(*copy));
	if(copy != NULL) memcpy(copy, value, (length + 1U)*sizeof(*copy));
	else errno = ENOMEM;
	return copy;
}

static wchar_t *
wide_join(const wchar_t parent[], const wchar_t name[])
{
	const size_t parent_length = wcslen(parent), name_length = wcslen(name);
	const int separator = parent_length != 0U && parent[parent_length - 1U] != L'\\';
	if(parent_length > SIZE_MAX - name_length - (size_t)separator - 1U)
	{
		errno = ENOMEM;
		return NULL;
	}
	wchar_t *const path = malloc((parent_length + (size_t)separator +
			name_length + 1U)*sizeof(*path));
	if(path == NULL) { errno = ENOMEM; return NULL; }
	memcpy(path, parent, parent_length*sizeof(*path));
	if(separator) path[parent_length] = L'\\';
	memcpy(path + parent_length + (size_t)separator, name,
			(name_length + 1U)*sizeof(*path));
	return path;
}

static HANDLE
open_nofollow(const wchar_t path[], DWORD access, DWORD creation)
{
	return CreateFileW(path, access,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, creation,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
}

static void
parent_entry_free(nv_parent_entry_t *entry)
{
	if(entry->handle != INVALID_HANDLE_VALUE) CloseHandle(entry->handle);
	free(entry->path);
	free(entry->name);
	*entry = (nv_parent_entry_t){ .handle = INVALID_HANDLE_VALUE };
}

static int
open_parent_entry(const char path[], nv_parent_entry_t *entry)
{
	*entry = (nv_parent_entry_t){ .handle = INVALID_HANDLE_VALUE };
	wchar_t *const native = wide_path(path);
	if(native == NULL) return -1;
	size_t length = wcslen(native);
	while(length > 7U && native[length - 1U] == L'\\') native[--length] = L'\0';
	wchar_t *const slash = wcsrchr(native, L'\\');
	if(slash == NULL || slash[1] == L'\0')
	{
		free(native);
		errno = EINVAL;
		return -1;
	}
	entry->name = wide_dup(slash + 1U);
	if(slash == native + 6U && native[4] != L'U') slash[1] = L'\0';
	else *slash = L'\0';
	entry->path = native;
	entry->handle = open_nofollow(entry->path, MAXIMUM_ALLOWED, OPEN_EXISTING);
	if(entry->name == NULL || entry->handle == INVALID_HANDLE_VALUE)
	{
		const int saved = entry->name == NULL ? ENOMEM :
			(set_errno_from_windows_error(GetLastError()), errno);
		parent_entry_free(entry);
		errno = saved;
		return -1;
	}
	DWORD attributes = 0U;
	if(handle_identity(entry->handle, NULL, NULL, &attributes) != 0 ||
			(attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
			(attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
	{
		const int saved = errno == 0 ? ENOTDIR : errno;
		parent_entry_free(entry);
		errno = saved;
		return -1;
	}
	return 0;
}

static int
parent_matches(const nv_parent_entry_t *entry, nv_fs_identity_t expected)
{
	nv_fs_identity_t current = {};
	if(handle_identity(entry->handle, NULL, &current, NULL) != 0) return 0;
	if(identity_equal(current, expected)) return 1;
	errno = NV_FS_STALE_ERRNO;
	return 0;
}

static int
handle_matches(HANDLE handle, nv_fs_identity_t expected)
{
	nv_fs_identity_t current = {};
	if(handle_identity(handle, NULL, &current, NULL) != 0) return 0;
	if(identity_equal(current, expected)) return 1;
	errno = NV_FS_STALE_ERRNO;
	return 0;
}

static int
is_cancelled(nv_fs_cancel_hook hook, void *arg)
{
	if(hook != NULL && hook(arg))
	{
		errno = ECANCELED;
		return 1;
	}
	return 0;
}

static int
dispose_created_handle(HANDLE handle)
{
	FILE_DISPOSITION_INFO disposition = { .DeleteFile = TRUE };
	if(SetFileInformationByHandle(handle, FileDispositionInfo, &disposition,
			sizeof(disposition))) return 0;
	set_errno_from_windows_error(GetLastError());
	return -1;
}

static int
same_content(const BY_HANDLE_FILE_INFORMATION *before,
		const BY_HANDLE_FILE_INFORMATION *after)
{
	return before->dwVolumeSerialNumber == after->dwVolumeSerialNumber &&
		before->nFileIndexHigh == after->nFileIndexHigh &&
		before->nFileIndexLow == after->nFileIndexLow &&
		before->nFileSizeHigh == after->nFileSizeHigh &&
		before->nFileSizeLow == after->nFileSizeLow &&
		before->ftLastWriteTime.dwHighDateTime ==
			after->ftLastWriteTime.dwHighDateTime &&
		before->ftLastWriteTime.dwLowDateTime ==
			after->ftLastWriteTime.dwLowDateTime;
}

static int
copy_file_handles(HANDLE input, HANDLE output, nv_fs_cancel_hook hook, void *arg)
{
	char buffer[64U*1024U];
	for(;;)
	{
		if(is_cancelled(hook, arg)) return -1;
		DWORD count = 0U;
		if(!ReadFile(input, buffer, sizeof(buffer), &count, NULL))
		{
			set_errno_from_windows_error(GetLastError());
			return -1;
		}
		if(count == 0U) return 0;
		DWORD used = 0U;
		while(used < count)
		{
			DWORD written = 0U;
			if(!WriteFile(output, buffer + used, count - used, &written, NULL) ||
					written == 0U)
			{
				if(written == 0U) errno = EIO;
				else set_errno_from_windows_error(GetLastError());
				return -1;
			}
			used += written;
		}
	}
}

static int copy_entry_w(const wchar_t source[], const wchar_t destination[],
		const nv_fs_identity_t *expected, nv_fs_cancel_hook hook, void *arg);

static int
copy_directory_w(HANDLE source_handle, const wchar_t source[],
		const BY_HANDLE_FILE_INFORMATION *before, const wchar_t destination[],
		nv_fs_cancel_hook hook, void *arg)
{
	if(!CreateDirectoryW(destination, NULL))
	{
		set_errno_from_windows_error(GetLastError());
		return -1;
	}
	HANDLE destination_handle = open_nofollow(destination,
			FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | DELETE, OPEN_EXISTING);
	if(destination_handle == INVALID_HANDLE_VALUE)
	{
		const DWORD error = GetLastError();
		(void)RemoveDirectoryW(destination);
		set_errno_from_windows_error(error);
		return -1;
	}
	wchar_t *const pattern = wide_join(source, L"*");
	if(pattern == NULL)
	{
		dispose_created_handle(destination_handle);
		CloseHandle(destination_handle);
		return -1;
	}
	WIN32_FIND_DATAW data;
	HANDLE search = FindFirstFileW(pattern, &data);
	free(pattern);
	if(search == INVALID_HANDLE_VALUE)
	{
		const DWORD error = GetLastError();
		if(error != ERROR_FILE_NOT_FOUND)
		{
			dispose_created_handle(destination_handle);
			CloseHandle(destination_handle);
			set_errno_from_windows_error(error);
			return -1;
		}
	}
	int result = 0;
	int saved = 0;
	while(search != INVALID_HANDLE_VALUE)
	{
		if(wcscmp(data.cFileName, L".") != 0 && wcscmp(data.cFileName, L"..") != 0)
		{
			if(is_cancelled(hook, arg) ||
					(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
			{
				if(errno == 0) errno = ENOTSUP;
				result = -1;
				saved = errno;
				break;
			}
			wchar_t *const child_source = wide_join(source, data.cFileName);
			wchar_t *const child_destination = wide_join(destination, data.cFileName);
			if(child_source == NULL || child_destination == NULL ||
					copy_entry_w(child_source, child_destination, NULL, hook, arg) != 0)
			{
				result = -1;
				saved = errno;
				free(child_destination);
				free(child_source);
				break;
			}
			free(child_destination);
			free(child_source);
		}
		if(!FindNextFileW(search, &data))
		{
			const DWORD error = GetLastError();
			if(error != ERROR_NO_MORE_FILES)
			{
				result = -1;
				set_errno_from_windows_error(error);
				saved = errno;
			}
			break;
		}
	}
	if(search != INVALID_HANDLE_VALUE) FindClose(search);
	BY_HANDLE_FILE_INFORMATION finished;
	if(result == 0 && !GetFileInformationByHandle(source_handle, &finished))
	{
		result = -1;
		set_errno_from_windows_error(GetLastError());
		saved = errno;
	}
	else if(result == 0 && !same_content(before, &finished))
	{
		result = -1;
		saved = NV_FS_STALE_ERRNO;
	}
	if(result != 0) (void)dispose_created_handle(destination_handle);
	CloseHandle(destination_handle);
	if(result != 0) errno = saved == 0 ? EIO : saved;
	return result;
}

static int
copy_entry_w(const wchar_t source[], const wchar_t destination[],
		const nv_fs_identity_t *expected, nv_fs_cancel_hook hook, void *arg)
{
	if(is_cancelled(hook, arg)) return -1;
	HANDLE input = open_nofollow(source, GENERIC_READ | FILE_READ_ATTRIBUTES,
			OPEN_EXISTING);
	if(input == INVALID_HANDLE_VALUE)
	{
		set_errno_from_windows_error(GetLastError());
		return -1;
	}
	BY_HANDLE_FILE_INFORMATION before;
	nv_fs_identity_t identity = {};
	DWORD attributes = 0U;
	if(!GetFileInformationByHandle(input, &before) ||
			handle_identity(input, NULL, &identity, &attributes) != 0)
	{
		const int saved = errno == 0 ? EIO : errno;
		CloseHandle(input);
		errno = saved;
		return -1;
	}
	if((expected != NULL && !identity_equal(identity, *expected)) ||
			(attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
	{
		CloseHandle(input);
		errno = expected != NULL && !identity_equal(identity, *expected) ?
			NV_FS_STALE_ERRNO :
			ENOTSUP;
		return -1;
	}
	if((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U)
	{
		const int result = copy_directory_w(input, source, &before, destination,
				hook, arg);
		const int saved = errno;
		CloseHandle(input);
		if(result != 0) errno = saved;
		return result;
	}
	HANDLE output = open_nofollow(destination,
			GENERIC_WRITE | FILE_READ_ATTRIBUTES | DELETE, CREATE_NEW);
	if(output == INVALID_HANDLE_VALUE)
	{
		const DWORD error = GetLastError();
		CloseHandle(input);
		set_errno_from_windows_error(error);
		return -1;
	}
	int result = copy_file_handles(input, output, hook, arg);
	int saved = errno;
	if(result == 0 && !FlushFileBuffers(output))
	{
		result = -1;
		set_errno_from_windows_error(GetLastError());
		saved = errno;
	}
	BY_HANDLE_FILE_INFORMATION after;
	if(result == 0 && !GetFileInformationByHandle(input, &after))
	{
		result = -1;
		set_errno_from_windows_error(GetLastError());
		saved = errno;
	}
	else if(result == 0 && !same_content(&before, &after))
	{
		result = -1;
		saved = NV_FS_STALE_ERRNO;
	}
	if(result != 0) (void)dispose_created_handle(output);
	CloseHandle(output);
	CloseHandle(input);
	if(result != 0) errno = saved == 0 ? EIO : saved;
	return result;
}

static int
destination_inside_source(const wchar_t source[], const wchar_t destination[])
{
	const size_t source_length = wcslen(source);
	return _wcsnicmp(source, destination, source_length) == 0 &&
		(destination[source_length] == L'\0' ||
		 destination[source_length] == L'\\');
}

static int
set_rename_info(HANDLE source, HANDLE root, const wchar_t destination_name[])
{
	const DWORD name_bytes = (DWORD)(wcslen(destination_name)*sizeof(wchar_t));
	const size_t size = offsetof(FILE_RENAME_INFO, FileName) + name_bytes +
		sizeof(wchar_t);
	FILE_RENAME_INFO *const info = calloc(1U, size);
	if(info == NULL) { errno = ENOMEM; return -1; }
	info->ReplaceIfExists = FALSE;
	info->RootDirectory = root;
	info->FileNameLength = name_bytes;
	memcpy(info->FileName, destination_name, name_bytes);
	const BOOL renamed = SetFileInformationByHandle(source, FileRenameInfo, info,
			(DWORD)size);
	const DWORD error = renamed ? ERROR_SUCCESS : GetLastError();
	free(info);
	if(renamed) return 0;
	set_errno_from_windows_error(error);
	return -1;
}

static int
rename_handle_no_replace(HANDLE source, HANDLE destination_directory,
		const wchar_t destination_name[])
{
	const DWORD required = GetFinalPathNameByHandleW(destination_directory, NULL,
			0U, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	if(required == 0U)
	{
		set_errno_from_windows_error(GetLastError());
		return -1;
	}
	wchar_t *const parent = malloc(((size_t)required + 1U)*sizeof(*parent));
	if(parent == NULL) { errno = ENOMEM; return -1; }
	if(GetFinalPathNameByHandleW(destination_directory, parent, required + 1U,
			FILE_NAME_NORMALIZED | VOLUME_NAME_DOS) == 0U)
	{
		const DWORD error = GetLastError();
		free(parent);
		set_errno_from_windows_error(error);
		return -1;
	}
	wchar_t *const destination = wide_join(parent, destination_name);
	free(parent);
	if(destination == NULL) return -1;
	const int result = set_rename_info(source, NULL, destination);
	const int saved = errno;
	free(destination);
	if(result != 0) errno = saved;
	return result;
}

static int
run_helper(const char executable_utf8[], const wchar_t path[],
		nv_fs_cancel_hook hook, void *arg)
{
	wchar_t *const executable = wide_path(executable_utf8);
	if(executable == NULL) return -1;
	const size_t length = wcslen(executable) + wcslen(path) + 8U;
	wchar_t *const command = malloc(length*sizeof(*command));
	if(command == NULL)
	{
		free(executable);
		errno = ENOMEM;
		return -1;
	}
	(void)swprintf(command, length, L"\"%ls\" \"%ls\"", executable, path);
	STARTUPINFOW startup = { .cb = sizeof(startup) };
	PROCESS_INFORMATION process = {};
	const BOOL started = CreateProcessW(executable, command, NULL, NULL, FALSE,
			CREATE_NO_WINDOW, NULL, NULL, &startup, &process);
	const DWORD start_error = started ? ERROR_SUCCESS : GetLastError();
	free(command);
	free(executable);
	if(!started)
	{
		set_errno_from_windows_error(start_error);
		return -1;
	}
	CloseHandle(process.hThread);
	for(;;)
	{
		const DWORD waited = WaitForSingleObject(process.hProcess, 10U);
		if(waited == WAIT_OBJECT_0)
		{
			DWORD code = 1U;
			const BOOL read = GetExitCodeProcess(process.hProcess, &code);
			CloseHandle(process.hProcess);
			if(read && code == 0U) return 0;
			errno = EIO;
			return -1;
		}
		if(waited == WAIT_FAILED)
		{
			const DWORD error = GetLastError();
			CloseHandle(process.hProcess);
			set_errno_from_windows_error(error);
			return -1;
		}
		if(is_cancelled(hook, arg))
		{
			TerminateProcess(process.hProcess, 1U);
			WaitForSingleObject(process.hProcess, INFINITE);
			CloseHandle(process.hProcess);
			errno = ECANCELED;
			return -1;
		}
	}
}

static int
run_recycle(const wchar_t path[], nv_fs_cancel_hook hook, void *arg)
{
	const char *const helper = getenv("NEOVIFM_TRASH_EXECUTABLE");
	if(helper != NULL && helper[0] != '\0') return run_helper(helper, path, hook, arg);
	if(is_cancelled(hook, arg)) return -1;
	const HRESULT initialized = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	if(FAILED(initialized)) { errno = EIO; return -1; }
	IFileOperation *operation = NULL;
	IShellItem *item = NULL;
	wchar_t *shell_path = NULL;
	if(wcsncmp(path, L"\\\\?\\UNC\\", 8U) == 0)
	{
		const size_t length = wcslen(path + 8U);
		shell_path = malloc((length + 3U)*sizeof(*shell_path));
		if(shell_path != NULL)
		{
			shell_path[0] = L'\\';
			shell_path[1] = L'\\';
			memcpy(shell_path + 2U, path + 8U, (length + 1U)*sizeof(*shell_path));
		}
	}
	else if(wcsncmp(path, L"\\\\?\\", 4U) == 0)
	{
		shell_path = wide_dup(path + 4U);
	}
	else
	{
		shell_path = wide_dup(path);
	}
	if(shell_path == NULL) { CoUninitialize(); return -1; }
	HRESULT result = CoCreateInstance(&CLSID_FileOperation, NULL,
			CLSCTX_INPROC_SERVER, &IID_IFileOperation, (void **)&operation);
	if(SUCCEEDED(result))
	{
		result = SHCreateItemFromParsingName(shell_path, NULL, &IID_IShellItem,
				(void **)&item);
	}
	if(SUCCEEDED(result))
	{
		result = IFileOperation_SetOperationFlags(operation,
				FOFX_RECYCLEONDELETE | FOF_ALLOWUNDO | FOF_NOCONFIRMATION |
				FOF_SILENT | FOF_NOERRORUI | FOFX_EARLYFAILURE |
				FOFX_NOCOPYHOOKS);
	}
	if(SUCCEEDED(result)) result = IFileOperation_DeleteItem(operation, item, NULL);
	if(SUCCEEDED(result)) result = IFileOperation_PerformOperations(operation);
	BOOL aborted = FALSE;
	if(operation != NULL)
	{
		const HRESULT checked = IFileOperation_GetAnyOperationsAborted(operation,
				&aborted);
		if(SUCCEEDED(result) && FAILED(checked)) result = checked;
	}
	if(item != NULL) IShellItem_Release(item);
	if(operation != NULL) IFileOperation_Release(operation);
	free(shell_path);
	CoUninitialize();
	if(SUCCEEDED(result) && !aborted) return 0;
	fprintf(stderr, "neovifm-core-session: recycle operation failed: 0x%08lx%s\n",
			(unsigned long)result, aborted ? " (aborted)" : "");
	const unsigned int error_code = (unsigned int)HRESULT_CODE(result);
	errno = aborted ? ECANCELED :
		(error_code == 0U ? EIO : (int)error_code);
	return -1;
}

static int
create_quarantine(const nv_parent_entry_t *parent, wchar_t **name,
		wchar_t **path, HANDLE *handle)
{
	for(int attempt = 0; attempt < 8; ++attempt)
	{
		GUID value;
		if(FAILED(CoCreateGuid(&value))) { errno = EIO; return -1; }
		wchar_t buffer[64];
		(void)swprintf(buffer, sizeof(buffer)/sizeof(buffer[0]),
				L".neovifm-trash-%08lx%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x",
				(unsigned long)value.Data1, value.Data2, value.Data3,
				value.Data4[0], value.Data4[1], value.Data4[2], value.Data4[3],
				value.Data4[4], value.Data4[5], value.Data4[6], value.Data4[7]);
		wchar_t *const candidate = wide_join(parent->path, buffer);
		if(candidate == NULL) return -1;
		if(CreateDirectoryW(candidate, NULL))
		{
			HANDLE directory = open_nofollow(candidate, MAXIMUM_ALLOWED,
					OPEN_EXISTING);
			if(directory == INVALID_HANDLE_VALUE)
			{
				const DWORD error = GetLastError();
				RemoveDirectoryW(candidate);
				free(candidate);
				set_errno_from_windows_error(error);
				return -1;
			}
			*name = wide_dup(buffer);
			if(*name == NULL)
			{
				dispose_created_handle(directory);
				CloseHandle(directory);
				free(candidate);
				return -1;
			}
			*path = candidate;
			*handle = directory;
			return 0;
		}
		const DWORD error = GetLastError();
		free(candidate);
		if(error != ERROR_ALREADY_EXISTS) { set_errno_from_windows_error(error); return -1; }
	}
	errno = EEXIST;
	return -1;
}

int
nv_fs_mkdir(const char path[], int mode,
		nv_fs_identity_t destination_directory)
{
	(void)mode;
	nv_parent_entry_t parent;
	if(open_parent_entry(path, &parent) != 0) return -1;
	wchar_t *const native = wide_path(path);
	if(native == NULL)
	{
		parent_entry_free(&parent);
		return -1;
	}
	int result = -1;
	int saved = 0;
	if(!parent_matches(&parent, destination_directory)) saved = errno;
	else if(!CreateDirectoryW(native, NULL))
	{
		set_errno_from_windows_error(GetLastError());
		saved = errno;
	}
	else
	{
		HANDLE created = open_nofollow(native,
				FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | DELETE, OPEN_EXISTING);
		if(created == INVALID_HANDLE_VALUE)
		{
			set_errno_from_windows_error(GetLastError());
			saved = errno;
			(void)RemoveDirectoryW(native);
		}
		else if(!parent_matches(&parent, destination_directory))
		{
			saved = errno;
			(void)dispose_created_handle(created);
			CloseHandle(created);
		}
		else
		{
			CloseHandle(created);
			result = 0;
		}
	}
	free(native);
	parent_entry_free(&parent);
	if(result != 0) errno = saved == 0 ? EIO : saved;
	return result;
}

int
nv_fs_remove(const char path[], nv_fs_identity_t source_directory,
		nv_fs_identity_t source_entry, nv_fs_cancel_hook cancelled,
		void *cancel_arg)
{
	nv_parent_entry_t parent;
	if(open_parent_entry(path, &parent) != 0) return -1;
	wchar_t *const native = wide_path(path);
	if(native == NULL)
	{
		parent_entry_free(&parent);
		return -1;
	}
	HANDLE source = open_nofollow(native, DELETE | FILE_READ_ATTRIBUTES,
			OPEN_EXISTING);
	if(source == INVALID_HANDLE_VALUE)
	{
		const DWORD error = GetLastError();
		free(native);
		parent_entry_free(&parent);
		set_errno_from_windows_error(error);
		return -1;
	}
	int result = -1;
	int saved = 0;
	if(!parent_matches(&parent, source_directory) ||
			!handle_matches(source, source_entry))
	{
		saved = errno;
		goto done;
	}
	if(is_cancelled(cancelled, cancel_arg)) { saved = errno; goto done; }
	wchar_t *quarantine_name = NULL;
	wchar_t *quarantine_path = NULL;
	HANDLE quarantine = INVALID_HANDLE_VALUE;
	if(create_quarantine(&parent, &quarantine_name, &quarantine_path,
			&quarantine) != 0)
	{
		saved = errno;
		goto done;
	}
	if(test_before_atomic_hook != NULL) test_before_atomic_hook(path);
	HANDLE current = open_nofollow(native, FILE_READ_ATTRIBUTES, OPEN_EXISTING);
	if(current == INVALID_HANDLE_VALUE || !handle_matches(current, source_entry))
	{
		saved = current == INVALID_HANDLE_VALUE ?
			(set_errno_from_windows_error(GetLastError()), errno) : errno;
		if(current != INVALID_HANDLE_VALUE) CloseHandle(current);
		goto quarantine_done;
	}
	CloseHandle(current);
	if(rename_handle_no_replace(source, quarantine, parent.name) != 0)
	{
		saved = errno;
		goto quarantine_done;
	}
	nv_fs_identity_t quarantined_identity = {};
	if(handle_identity(source, NULL, &quarantined_identity, NULL) != 0)
	{
		saved = errno;
		(void)rename_handle_no_replace(source, parent.handle, parent.name);
		goto quarantine_done;
	}
	wchar_t *const quarantined = wide_join(quarantine_path, parent.name);
	if(quarantined == NULL)
	{
		saved = errno;
		(void)rename_handle_no_replace(source, parent.handle, parent.name);
		goto quarantine_done;
	}
	/* Shell recycling is handle-hostile on some filesystems even when the
	 * handle was opened with FILE_SHARE_DELETE.  The object is already inside
	 * a private, pinned quarantine directory, so close the source handle and
	 * reopen by identity only if recovery is needed. */
	CloseHandle(source);
	source = INVALID_HANDLE_VALUE;
	result = run_recycle(quarantined, cancelled, cancel_arg);
	saved = errno;
	const DWORD remaining = GetFileAttributesW(quarantined);
	if(result == 0 && remaining != INVALID_FILE_ATTRIBUTES)
	{
		result = -1;
		saved = EIO;
	}
	if(result != 0 && remaining != INVALID_FILE_ATTRIBUTES)
	{
		HANDLE restore = open_nofollow(quarantined,
				DELETE | FILE_READ_ATTRIBUTES, OPEN_EXISTING);
		if(restore != INVALID_HANDLE_VALUE &&
				handle_matches(restore, quarantined_identity))
			(void)rename_handle_no_replace(restore, parent.handle, parent.name);
		if(restore != INVALID_HANDLE_VALUE) CloseHandle(restore);
	}
	free(quarantined);

quarantine_done:
	CloseHandle(quarantine);
	(void)RemoveDirectoryW(quarantine_path);
	free(quarantine_path);
	free(quarantine_name);
done:
	if(source != INVALID_HANDLE_VALUE) CloseHandle(source);
	free(native);
	parent_entry_free(&parent);
	if(result != 0) errno = saved == 0 ? EIO : saved;
	return result;
}

int
nv_fs_copy(const char source[], const char destination[],
		nv_fs_identity_t source_directory,
		nv_fs_identity_t destination_directory, nv_fs_identity_t source_entry,
		nv_fs_cancel_hook cancelled, void *cancel_arg)
{
	nv_parent_entry_t from, to;
	if(open_parent_entry(source, &from) != 0) return -1;
	if(open_parent_entry(destination, &to) != 0)
	{
		const int saved = errno;
		parent_entry_free(&from);
		errno = saved;
		return -1;
	}
	wchar_t *const native_source = wide_path(source);
	wchar_t *const native_destination = wide_path(destination);
	int result = -1;
	int saved = 0;
	if(native_source == NULL || native_destination == NULL) saved = errno;
	else if(!parent_matches(&from, source_directory) ||
			!parent_matches(&to, destination_directory)) saved = errno;
	else if(destination_inside_source(native_source, native_destination)) saved = EINVAL;
	else
	{
		result = copy_entry_w(native_source, native_destination, &source_entry,
				cancelled, cancel_arg);
		saved = errno;
		if(result == 0 && !parent_matches(&to, destination_directory))
		{
			result = -1;
			saved = errno;
		}
	}
	free(native_destination);
	free(native_source);
	parent_entry_free(&to);
	parent_entry_free(&from);
	if(result != 0) errno = saved == 0 ? EIO : saved;
	return result;
}

int
nv_fs_move(const char source[], const char destination[],
		nv_fs_identity_t source_directory,
		nv_fs_identity_t destination_directory, nv_fs_identity_t source_entry,
		nv_fs_cancel_hook cancelled, void *cancel_arg)
{
	nv_parent_entry_t from, to;
	if(open_parent_entry(source, &from) != 0) return -1;
	if(open_parent_entry(destination, &to) != 0)
	{
		const int saved = errno;
		parent_entry_free(&from);
		errno = saved;
		return -1;
	}
	wchar_t *const native_source = wide_path(source);
	wchar_t *const native_destination = wide_path(destination);
	HANDLE source_handle = INVALID_HANDLE_VALUE;
	int result = -1;
	int saved = 0;
	if(native_source == NULL || native_destination == NULL) saved = errno;
	else if(!parent_matches(&from, source_directory) ||
			!parent_matches(&to, destination_directory)) saved = errno;
	else if(test_cross_device_move || source_directory.device !=
			destination_directory.device) saved = EXDEV;
	else if(destination_inside_source(native_source, native_destination)) saved = EINVAL;
	else
	{
		source_handle = open_nofollow(native_source,
				DELETE | FILE_READ_ATTRIBUTES, OPEN_EXISTING);
		if(source_handle == INVALID_HANDLE_VALUE)
		{
			set_errno_from_windows_error(GetLastError());
			saved = errno;
		}
		else if(!handle_matches(source_handle, source_entry))
		{
			saved = errno;
		}
		else if(is_cancelled(cancelled, cancel_arg)) saved = errno;
		else
		{
			if(test_before_atomic_hook != NULL) test_before_atomic_hook(source);
			HANDLE current = open_nofollow(native_source, FILE_READ_ATTRIBUTES,
					OPEN_EXISTING);
			if(current == INVALID_HANDLE_VALUE || !handle_matches(current, source_entry))
			{
				saved = current == INVALID_HANDLE_VALUE ?
					(set_errno_from_windows_error(GetLastError()), errno) : errno;
				if(current != INVALID_HANDLE_VALUE) CloseHandle(current);
			}
			else
			{
				CloseHandle(current);
				if(rename_handle_no_replace(source_handle, to.handle, to.name) != 0)
					saved = errno;
				else
				{
					HANDLE moved = open_nofollow(native_destination,
							FILE_READ_ATTRIBUTES, OPEN_EXISTING);
					nv_fs_identity_t source_after = {}, moved_identity = {};
					if(moved != INVALID_HANDLE_VALUE &&
							handle_identity(source_handle, NULL, &source_after, NULL) == 0 &&
							handle_identity(moved, NULL, &moved_identity, NULL) == 0 &&
							identity_equal(moved_identity, source_after)) result = 0;
					else
					{
						saved = moved == INVALID_HANDLE_VALUE ? EIO :
							(errno == 0 ? NV_FS_STALE_ERRNO : errno);
						(void)rename_handle_no_replace(source_handle, from.handle,
								from.name);
					}
					if(moved != INVALID_HANDLE_VALUE) CloseHandle(moved);
				}
			}
		}
	}
	if(source_handle != INVALID_HANDLE_VALUE) CloseHandle(source_handle);
	free(native_destination);
	free(native_source);
	parent_entry_free(&to);
	parent_entry_free(&from);
	if(result != 0) errno = saved == 0 ? EIO : saved;
	return result;
}

#else

#include <dirent.h> /* DIR struct dirent */

#include <fcntl.h> /* O_* open() */
#include <signal.h> /* SIGTERM kill() */
#include <spawn.h> /* posix_spawn() */
#include <stdio.h> /* rename() */
#ifdef __linux__
#include <linux/fs.h> /* RENAME_NOREPLACE */
#include <sys/syscall.h> /* SYS_renameat2 */
#endif
#include <sys/wait.h> /* waitpid() */
#include <time.h> /* nanosleep() */
#include <unistd.h> /* close() link() read() readlink() symlink() unlink() write() */

#include "os.h"

extern char **environ;

static nv_fs_test_before_atomic_hook test_before_atomic_hook;
static int test_cross_device_move;
static uint64_t stat_ctime_ns(const struct stat *st);

static void
stat_identity(const struct stat *st, nv_fs_identity_t *identity)
{
	if(identity == NULL) return;
	*identity = (nv_fs_identity_t){
		.device = (uint64_t)st->st_dev,
		.inode = (uint64_t)st->st_ino,
		.ctime_unix_ns = stat_ctime_ns(st),
	};
}

void
nv_fs_test_set_before_atomic_hook(nv_fs_test_before_atomic_hook hook)
{
	test_before_atomic_hook = hook;
}

void
nv_fs_test_force_cross_device_move(int enabled)
{
	test_cross_device_move = enabled;
}

#if defined(__APPLE__) || defined(__linux__)
static void
run_test_before_atomic_hook(const char path[])
{
	if(test_before_atomic_hook != NULL) test_before_atomic_hook(path);
}
#endif

struct nv_dir_t
{
	DIR *dir;
};

nv_dir_t *
nv_dir_open(const char path[])
{
	DIR *const native_dir = os_opendir(path);
	if(native_dir == NULL)
	{
		return NULL;
	}

	nv_dir_t *const dir = malloc(sizeof(*dir));
	if(dir == NULL)
	{
		(void)os_closedir(native_dir);
		errno = ENOMEM;
		return NULL;
	}
	dir->dir = native_dir;
	return dir;
}

const char *
nv_dir_read(nv_dir_t *dir)
{
	struct dirent *const entry = os_readdir(dir->dir);
	return entry == NULL ? NULL : entry->d_name;
}

int
nv_dir_close(nv_dir_t *dir)
{
	const int result = os_closedir(dir->dir);
	free(dir);
	return result;
}

int
nv_dir_fstat(nv_dir_t *dir, struct stat *st, nv_fs_identity_t *identity)
{
	if(dir == NULL || st == NULL) { errno = EINVAL; return -1; }
	const int result = fstat(dirfd(dir->dir), st);
	if(result == 0) stat_identity(st, identity);
	return result;
}

int
nv_dir_lstat(nv_dir_t *dir, const char name[], struct stat *st,
		int *is_symlink, nv_fs_identity_t *identity)
{
	if(dir == NULL || name == NULL || st == NULL) { errno = EINVAL; return -1; }
	if(is_symlink != NULL) *is_symlink = 0;
	const int result = fstatat(dirfd(dir->dir), name, st, AT_SYMLINK_NOFOLLOW);
	if(result == 0 && is_symlink != NULL) *is_symlink = S_ISLNK(st->st_mode);
	if(result == 0) stat_identity(st, identity);
	return result;
}

int
nv_lstat(const char path[], struct stat *st, int *is_symlink,
		nv_fs_identity_t *identity)
{
	if(is_symlink != NULL)
	{
		*is_symlink = 0;
	}
	const int result = os_lstat(path, st);
#ifdef S_ISLNK
	if(result == 0 && is_symlink != NULL)
	{
		*is_symlink = S_ISLNK(st->st_mode);
	}
#endif
	if(result == 0) stat_identity(st, identity);
	return result;
}

int
nv_fs_actions_supported(void)
{
#if defined(__APPLE__) || defined(__linux__)
	return 1;
#else
	return 0;
#endif
}

typedef struct
{
	int fd;
	char *path;
	char *name;
} nv_parent_entry_t;

static void
parent_entry_free(nv_parent_entry_t *entry)
{
	if(entry->fd >= 0) close(entry->fd);
	free(entry->path);
	free(entry->name);
	entry->fd = -1;
	entry->path = NULL;
	entry->name = NULL;
}

static int
open_parent_entry(const char path[], nv_parent_entry_t *entry)
{
	*entry = (nv_parent_entry_t){ .fd = -1 };
	if(path == NULL || path[0] == '\0') { errno = EINVAL; return -1; }
	char *const copy = strdup(path);
	if(copy == NULL) { errno = ENOMEM; return -1; }
	size_t length = strlen(copy);
	while(length > 1U && copy[length - 1U] == '/') copy[--length] = '\0';
	char *const slash = strrchr(copy, '/');
	const char *parent = ".";
	const char *name = copy;
	if(slash != NULL)
	{
		name = slash + 1;
		if(slash == copy)
		{
			parent = "/";
		}
		else
		{
			*slash = '\0';
			parent = copy;
		}
	}
	if(name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
	{
		free(copy);
		errno = EINVAL;
		return -1;
	}
	entry->path = strdup(parent);
	entry->name = strdup(name);
	entry->fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	free(copy);
	if(entry->path == NULL || entry->name == NULL || entry->fd < 0)
	{
		const int saved = entry->path == NULL || entry->name == NULL ? ENOMEM : errno;
		parent_entry_free(entry);
		errno = saved;
		return -1;
	}
	return 0;
}

static int
same_object(const struct stat *st, nv_fs_identity_t identity)
{
	return (uint64_t)st->st_dev == identity.device &&
			(uint64_t)st->st_ino == identity.inode;
}

static uint64_t
stat_ctime_ns(const struct stat *st)
{
#if defined(__APPLE__)
	const time_t seconds = st->st_ctimespec.tv_sec;
	const long nanoseconds = st->st_ctimespec.tv_nsec;
#elif defined(__linux__) || defined(HAVE_STRUCT_STAT_ST_CTIM)
	const time_t seconds = st->st_ctim.tv_sec;
	const long nanoseconds = st->st_ctim.tv_nsec;
#else
	const time_t seconds = st->st_ctime;
	const long nanoseconds = 0L;
#endif
	if(seconds < 0) return 0U;
	const uint64_t value = (uint64_t)seconds;
	if(value > (UINT64_MAX - (uint64_t)nanoseconds)/1000000000U)
		return UINT64_MAX;
	return value*1000000000U + (uint64_t)nanoseconds;
}

static int
identity_matches(const struct stat *st, nv_fs_identity_t identity)
{
	return same_object(st, identity) && stat_ctime_ns(st) == identity.ctime_unix_ns;
}

static int
parent_matches(const nv_parent_entry_t *entry, nv_fs_identity_t identity)
{
	struct stat st;
	if(fstat(entry->fd, &st) != 0) return 0;
	/* The snapshot ctime guards command admission.  File actions themselves
	 * change a directory's ctime, so subsequent targets must validate the
	 * held directory object without treating their own prior work as stale. */
	if(same_object(&st, identity)) return 1;
	errno = ESTALE;
	return 0;
}

static int
is_cancelled(nv_fs_cancel_hook cancelled, void *cancel_arg)
{
	if(cancelled != NULL && cancelled(cancel_arg))
	{
		errno = ECANCELED;
		return 1;
	}
	return 0;
}

static int
copy_file_data(int input, int output, nv_fs_cancel_hook cancelled,
		void *cancel_arg)
{
	char buffer[64U*1024U];
	for(;;)
	{
		if(is_cancelled(cancelled, cancel_arg)) return -1;
		ssize_t count;
		do { count = read(input, buffer, sizeof(buffer)); }
		while(count < 0 && errno == EINTR);
		if(count == 0) return 0;
		if(count < 0) return -1;
		ssize_t written = 0;
		while(written < count)
		{
			ssize_t result;
			do { result = write(output, buffer + written,
					(size_t)(count - written)); }
			while(result < 0 && errno == EINTR);
			if(result <= 0)
			{
				if(result == 0) errno = EIO;
				return -1;
			}
			written += result;
		}
	}
}

static int
copy_symbolic_link_at(int source_fd, const char source_name[], int destination_fd,
		const char destination_name[], off_t size)
{
	size_t capacity = size > 0 && (uintmax_t)size < SIZE_MAX - 2U ?
		(size_t)size + 2U : 4096U;
	for(;;)
	{
		char *const target = malloc(capacity);
		if(target == NULL) { errno = ENOMEM; return -1; }
		const ssize_t length = readlinkat(source_fd, source_name, target,
				capacity - 1U);
		if(length >= 0 && (size_t)length < capacity - 1U)
		{
			target[length] = '\0';
			const int result = symlinkat(target, destination_fd,
					destination_name);
			free(target);
			return result;
		}
		const int saved = length < 0 ? errno : ENAMETOOLONG;
		free(target);
		if(length < 0 || capacity > SIZE_MAX/2U)
		{
			errno = saved;
			return -1;
		}
		capacity *= 2U;
	}
}

static int
directory_is_ancestor(int ancestor_fd, int directory_fd)
{
	struct stat ancestor;
	if(fstat(ancestor_fd, &ancestor) != 0) return -1;
	int current = dup(directory_fd);
	if(current < 0) return -1;
	for(;;)
	{
		struct stat here;
		if(fstat(current, &here) != 0)
		{
			const int saved = errno;
			close(current);
			errno = saved;
			return -1;
		}
		if(here.st_dev == ancestor.st_dev && here.st_ino == ancestor.st_ino)
		{
			close(current);
			return 1;
		}
		const int parent = openat(current, "..",
				O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if(parent < 0)
		{
			const int saved = errno;
			close(current);
			errno = saved;
			return -1;
		}
		struct stat above;
		if(fstat(parent, &above) != 0)
		{
			const int saved = errno;
			close(parent);
			close(current);
			errno = saved;
			return -1;
		}
		if(above.st_dev == here.st_dev && above.st_ino == here.st_ino)
		{
			close(parent);
			close(current);
			return 0;
		}
		close(current);
		current = parent;
	}
}

static int
stat_content_same(const struct stat *before, const struct stat *after)
{
	if(before->st_size != after->st_size || before->st_mtime != after->st_mtime ||
			stat_ctime_ns(before) != stat_ctime_ns(after))
		return 0;
#ifdef __APPLE__
	return before->st_mtimespec.tv_nsec == after->st_mtimespec.tv_nsec;
#else
	return 1;
#endif
}

static int
copy_entry_at(int source_fd, const char source_name[], int destination_fd,
		const char destination_name[], const nv_fs_identity_t *expected,
		nv_fs_cancel_hook cancelled, void *cancel_arg)
{
	if(is_cancelled(cancelled, cancel_arg)) return -1;
	struct stat st;
	if(fstatat(source_fd, source_name, &st, AT_SYMLINK_NOFOLLOW) != 0) return -1;
	if(expected != NULL && !identity_matches(&st, *expected))
	{
		errno = ESTALE;
		return -1;
	}
	if(S_ISLNK(st.st_mode))
	{
		if(copy_symbolic_link_at(source_fd, source_name, destination_fd,
				destination_name, st.st_size) != 0) return -1;
		struct stat current;
		const int current_result = fstatat(source_fd, source_name, &current,
				AT_SYMLINK_NOFOLLOW);
		if(current_result == 0 &&
				same_object(&current, (nv_fs_identity_t){
					.device = (uint64_t)st.st_dev, .inode = (uint64_t)st.st_ino }))
		{
			return 0;
		}
		const int saved = current_result == 0 ? ESTALE : errno;
		errno = saved;
		return -1;
	}
	if(S_ISREG(st.st_mode))
	{
		const int input = openat(source_fd, source_name,
				O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
		if(input < 0) return -1;
		struct stat opened;
		if(fstat(input, &opened) != 0)
		{
			const int saved = errno;
			close(input);
			errno = saved;
			return -1;
		}
		if(!S_ISREG(opened.st_mode) || opened.st_dev != st.st_dev ||
				opened.st_ino != st.st_ino)
		{
			close(input);
			errno = ESTALE;
			return -1;
		}
		const int output = openat(destination_fd, destination_name,
				O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
				opened.st_mode & 0777);
		if(output < 0)
		{
			const int saved = errno;
			close(input);
			errno = saved;
			return -1;
		}
		int result = copy_file_data(input, output, cancelled, cancel_arg);
		int saved = errno;
		struct stat finished;
		if(result == 0 && (fstat(input, &finished) != 0 ||
				!stat_content_same(&opened, &finished)))
		{
			result = -1;
			saved = errno == 0 ? ESTALE : errno;
		}
		if(close(input) != 0 && result == 0) { result = -1; saved = errno; }
		if(close(output) != 0 && result == 0) { result = -1; saved = errno; }
		if(result != 0)
		{
			errno = saved == 0 ? EIO : saved;
		}
		return result;
	}
	if(!S_ISDIR(st.st_mode)) { errno = ENOTSUP; return -1; }

	const int input = openat(source_fd, source_name,
			O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if(input < 0) return -1;
	struct stat opened;
	if(fstat(input, &opened) != 0)
	{
		const int saved = errno;
		close(input);
		errno = saved;
		return -1;
	}
	if(opened.st_dev != st.st_dev || opened.st_ino != st.st_ino)
	{
		close(input);
		errno = ESTALE;
		return -1;
	}
	if(mkdirat(destination_fd, destination_name, opened.st_mode & 0777) != 0)
	{
		const int saved = errno;
		close(input);
		errno = saved;
		return -1;
	}
	const int output = openat(destination_fd, destination_name,
			O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if(output < 0)
	{
		const int saved = errno;
		close(input);
		errno = saved;
		return -1;
	}
	const int traversal_fd = dup(input);
	DIR *const directory = traversal_fd < 0 ? NULL : fdopendir(traversal_fd);
	if(directory == NULL)
	{
		const int saved = errno;
		if(traversal_fd >= 0) close(traversal_fd);
		close(output);
		close(input);
		errno = saved;
		return -1;
	}
	int result = 0;
	int saved = 0;
	for(;;)
	{
		if(is_cancelled(cancelled, cancel_arg))
		{
			result = -1;
			saved = errno;
			break;
		}
		errno = 0;
		struct dirent *const entry = readdir(directory);
		if(entry == NULL)
		{
			if(errno != 0) { result = -1; saved = errno; }
			break;
		}
		if(strcmp(entry->d_name, ".") == 0 ||
				strcmp(entry->d_name, "..") == 0) continue;
		if(copy_entry_at(input, entry->d_name, output, entry->d_name, NULL,
				cancelled, cancel_arg) != 0)
		{
			result = -1;
			saved = errno;
			break;
		}
	}
	struct stat finished;
	if(result == 0 && (fstat(input, &finished) != 0 ||
			!stat_content_same(&opened, &finished)))
	{
		result = -1;
		saved = errno == 0 ? ESTALE : errno;
	}
	if(closedir(directory) != 0 && result == 0) { result = -1; saved = errno; }
	if(close(output) != 0 && result == 0) { result = -1; saved = errno; }
	if(close(input) != 0 && result == 0) { result = -1; saved = errno; }
	if(result != 0) errno = saved == 0 ? EIO : saved;
	return result;
}

#ifdef __linux__
static int
rename_no_replace(int source_fd, const char source_name[], int destination_fd,
		const char destination_name[])
{
#ifdef SYS_renameat2
	return (int)syscall(SYS_renameat2, source_fd, source_name, destination_fd,
			destination_name, RENAME_NOREPLACE);
#else
	(void)source_fd;
	(void)source_name;
	(void)destination_fd;
	(void)destination_name;
	errno = ENOTSUP;
	return -1;
#endif
}
#endif

#if defined(__APPLE__) || defined(__linux__)
static char *
join_parent_name(const char parent[], const char name[])
{
	const size_t parent_length = strlen(parent), name_length = strlen(name);
	const int separator = parent_length != 0U && parent[parent_length - 1U] != '/';
	if(parent_length > SIZE_MAX - name_length - (size_t)separator - 1U)
	{
		errno = ENOMEM;
		return NULL;
	}
	char *const path = malloc(parent_length + (size_t)separator + name_length + 1U);
	if(path == NULL) { errno = ENOMEM; return NULL; }
	memcpy(path, parent, parent_length);
	if(separator) path[parent_length] = '/';
	memcpy(path + parent_length + (size_t)separator, name, name_length + 1U);
	return path;
}

static int
random_quarantine_name(char name[], size_t size)
{
	unsigned char bytes[16];
	const int random_fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if(random_fd < 0) return -1;
	size_t used = 0U;
	while(used < sizeof(bytes))
	{
		const ssize_t count = read(random_fd, bytes + used, sizeof(bytes) - used);
		if(count < 0 && errno == EINTR) continue;
		if(count <= 0)
		{
			const int saved = count < 0 ? errno : EIO;
			close(random_fd);
			errno = saved;
			return -1;
		}
		used += (size_t)count;
	}
	if(close(random_fd) != 0) return -1;
	static const char hex[] = "0123456789abcdef";
	if(size < sizeof(".neovifm-trash-") + 2U*sizeof(bytes))
	{
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(name, ".neovifm-trash-", sizeof(".neovifm-trash-") - 1U);
	size_t offset = sizeof(".neovifm-trash-") - 1U;
	for(size_t i = 0U; i < sizeof(bytes); ++i)
	{
		name[offset++] = hex[bytes[i] >> 4U];
		name[offset++] = hex[bytes[i] & 0x0fU];
	}
	name[offset] = '\0';
	return 0;
}

static int
create_quarantine_directory(int parent_fd, char name[], size_t size,
		int *directory_fd)
{
	for(int attempt = 0; attempt < 8; ++attempt)
	{
		if(random_quarantine_name(name, size) != 0) return -1;
		if(mkdirat(parent_fd, name, 0700) == 0)
		{
			*directory_fd = openat(parent_fd, name,
					O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			if(*directory_fd >= 0) return 0;
			const int saved = errno;
			(void)unlinkat(parent_fd, name, AT_REMOVEDIR);
			errno = saved;
			return -1;
		}
		if(errno != EEXIST) return -1;
	}
	errno = EEXIST;
	return -1;
}

static void
restore_no_replace(int source_fd, const char source_name[], int parent_fd,
		const char original_name[], const struct stat *moved)
{
	struct stat current;
	if(fstatat(source_fd, source_name, &current, AT_SYMLINK_NOFOLLOW) == 0 &&
		current.st_dev == moved->st_dev && current.st_ino == moved->st_ino)
	{
#ifdef __APPLE__
		(void)renameatx_np(source_fd, source_name, parent_fd, original_name,
				RENAME_EXCL);
#else
		(void)rename_no_replace(source_fd, source_name, parent_fd, original_name);
#endif
	}
}

static int
run_trash(const char path[], nv_fs_cancel_hook cancelled, void *cancel_arg)
{
	pid_t child;
	const char *const configured = getenv("NEOVIFM_TRASH_EXECUTABLE");
	const char *const executable = configured == NULL || configured[0] != '/' ?
#ifdef __APPLE__
		"/usr/bin/trash" : configured;
	char *argv[] = { (char *)executable, (char *)path, NULL };
#else
		"/usr/bin/gio" : configured;
	char *argv[] = { (char *)executable, NULL, (char *)path, NULL };
	if(configured == NULL || configured[0] != '/')
	{
		argv[1] = (char *)"trash";
	}
	else
	{
		argv[1] = (char *)path;
		argv[2] = NULL;
	}
#endif
	posix_spawn_file_actions_t actions;
	int spawn_error = posix_spawn_file_actions_init(&actions);
	const int actions_initialized = spawn_error == 0;
	if(spawn_error == 0)
		spawn_error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO,
				"/dev/null", O_RDONLY, 0);
	if(spawn_error == 0)
		spawn_error = posix_spawn_file_actions_adddup2(&actions, STDERR_FILENO,
				STDOUT_FILENO);
	if(spawn_error == 0)
		spawn_error = posix_spawn(&child, argv[0], &actions, NULL, argv, environ);
	if(actions_initialized) (void)posix_spawn_file_actions_destroy(&actions);
	if(spawn_error != 0) { errno = spawn_error; return -1; }
	for(;;)
	{
		int status;
		const pid_t result = waitpid(child, &status, WNOHANG);
		if(result == child)
		{
			if(WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
			errno = EIO;
			return -1;
		}
		if(result < 0 && errno != EINTR) return -1;
		if(is_cancelled(cancelled, cancel_arg))
		{
			(void)kill(child, SIGTERM);
			for(int attempt = 0; attempt < 50; ++attempt)
			{
				const pid_t waited = waitpid(child, &status, WNOHANG);
				if(waited == child) { errno = ECANCELED; return -1; }
				if(waited < 0 && errno != EINTR) return -1;
				const struct timespec delay = { .tv_sec = 0,
					.tv_nsec = 10L*1000L*1000L };
				(void)nanosleep(&delay, NULL);
			}
			(void)kill(child, SIGKILL);
			while(waitpid(child, &status, 0) < 0 && errno == EINTR) { }
			errno = ECANCELED;
			return -1;
		}
		const struct timespec delay = { .tv_sec = 0, .tv_nsec = 10L*1000L*1000L };
		(void)nanosleep(&delay, NULL);
	}
}
#endif

static int
copy_between_entries(const nv_parent_entry_t *from,
		const nv_parent_entry_t *to, nv_fs_identity_t source_directory,
		nv_fs_identity_t destination_directory, nv_fs_identity_t source_entry,
		nv_fs_cancel_hook cancelled, void *cancel_arg)
{
	if(!parent_matches(from, source_directory) ||
			!parent_matches(to, destination_directory)) return -1;
	struct stat source_stat;
	if(fstatat(from->fd, from->name, &source_stat, AT_SYMLINK_NOFOLLOW) != 0)
	{
		return -1;
	}
	if(!identity_matches(&source_stat, source_entry))
	{
		errno = ESTALE;
		return -1;
	}
	if(S_ISDIR(source_stat.st_mode))
	{
		const int source_fd = openat(from->fd, from->name,
				O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if(source_fd < 0) return -1;
		const int contained = directory_is_ancestor(source_fd, to->fd);
		const int saved = errno;
		close(source_fd);
		if(contained != 0)
		{
			errno = contained > 0 ? EINVAL : saved;
			return -1;
		}
	}
	return copy_entry_at(from->fd, from->name, to->fd, to->name,
			&source_entry, cancelled, cancel_arg);
}

int
nv_fs_mkdir(const char path[], int mode,
		nv_fs_identity_t destination_directory)
{
	nv_parent_entry_t entry;
	if(open_parent_entry(path, &entry) != 0) return -1;
	const int result = parent_matches(&entry, destination_directory) ?
		mkdirat(entry.fd, entry.name, (mode_t)mode) : -1;
	const int saved = errno;
	parent_entry_free(&entry);
	if(result != 0) errno = saved;
	return result;
}

int
nv_fs_copy(const char source[], const char destination[],
		nv_fs_identity_t source_directory,
		nv_fs_identity_t destination_directory, nv_fs_identity_t source_entry,
		nv_fs_cancel_hook cancelled, void *cancel_arg)
{
	nv_parent_entry_t from, to;
	if(open_parent_entry(source, &from) != 0) return -1;
	if(open_parent_entry(destination, &to) != 0)
	{
		const int saved = errno;
		parent_entry_free(&from);
		errno = saved;
		return -1;
	}
	const int result = copy_between_entries(&from, &to, source_directory,
			destination_directory, source_entry, cancelled, cancel_arg);
	const int saved = errno;
	parent_entry_free(&to);
	parent_entry_free(&from);
	if(result != 0) errno = saved;
	return result;
}

int
nv_fs_remove(const char path[], nv_fs_identity_t source_directory,
		nv_fs_identity_t source_entry, nv_fs_cancel_hook cancelled,
		void *cancel_arg)
{
	nv_parent_entry_t entry;
	if(open_parent_entry(path, &entry) != 0) return -1;
	int result = -1;
	int saved = 0;
	struct stat current;
	if(!parent_matches(&entry, source_directory) ||
			fstatat(entry.fd, entry.name, &current, AT_SYMLINK_NOFOLLOW) != 0)
	{
		saved = errno;
		goto done;
	}
	if(!identity_matches(&current, source_entry))
	{
		saved = ESTALE;
		goto done;
	}
	if(is_cancelled(cancelled, cancel_arg))
	{
		saved = errno;
		goto done;
	}
#if defined(__APPLE__) || defined(__linux__)
	char quarantine[64];
	int quarantine_fd = -1;
	if(create_quarantine_directory(entry.fd, quarantine, sizeof(quarantine),
			&quarantine_fd) != 0)
	{
		saved = errno;
		goto done;
	}
	run_test_before_atomic_hook(path);
	struct stat before_move;
	if(fstatat(entry.fd, entry.name, &before_move, AT_SYMLINK_NOFOLLOW) != 0 ||
			!identity_matches(&before_move, source_entry))
	{
		saved = errno == 0 ? ESTALE : errno;
		close(quarantine_fd);
		(void)unlinkat(entry.fd, quarantine, AT_REMOVEDIR);
		goto done;
	}
#ifdef __APPLE__
	if(renameatx_np(entry.fd, entry.name, quarantine_fd, entry.name,
			RENAME_EXCL) != 0)
#else
	if(rename_no_replace(entry.fd, entry.name, quarantine_fd, entry.name) != 0)
#endif
	{
		saved = errno;
		close(quarantine_fd);
		(void)unlinkat(entry.fd, quarantine, AT_REMOVEDIR);
		goto done;
	}
	struct stat quarantined;
	const int quarantined_result = fstatat(quarantine_fd, entry.name,
			&quarantined, AT_SYMLINK_NOFOLLOW);
	if(quarantined_result != 0 ||
			!same_object(&quarantined, source_entry))
	{
		saved = errno == 0 ? ESTALE : errno;
		if(quarantined_result == 0)
			restore_no_replace(quarantine_fd, entry.name, entry.fd, entry.name,
					&quarantined);
		close(quarantine_fd);
		(void)unlinkat(entry.fd, quarantine, AT_REMOVEDIR);
		goto done;
	}
	char *const quarantine_path = join_parent_name(entry.path, quarantine);
	char *const trash_path = quarantine_path == NULL ? NULL :
		join_parent_name(quarantine_path, entry.name);
	free(quarantine_path);
	if(trash_path == NULL)
	{
		saved = errno;
		restore_no_replace(quarantine_fd, entry.name, entry.fd, entry.name,
				&quarantined);
		close(quarantine_fd);
		(void)unlinkat(entry.fd, quarantine, AT_REMOVEDIR);
		goto done;
	}
	result = run_trash(trash_path, cancelled, cancel_arg);
	saved = errno;
	free(trash_path);
	if(result != 0)
	{
		struct stat remaining;
		if(fstatat(quarantine_fd, entry.name, &remaining, AT_SYMLINK_NOFOLLOW) != 0 &&
				errno == ENOENT)
		{
			result = 0;
		}
		else
		{
			restore_no_replace(quarantine_fd, entry.name, entry.fd, entry.name,
					&quarantined);
		}
	}
	close(quarantine_fd);
	(void)unlinkat(entry.fd, quarantine, AT_REMOVEDIR);
#else
	(void)cancelled;
	(void)cancel_arg;
	saved = ENOTSUP;
#endif

done:
	parent_entry_free(&entry);
	if(result != 0) errno = saved == 0 ? EIO : saved;
	return result;
}

int
nv_fs_move(const char source[], const char destination[],
		nv_fs_identity_t source_directory,
		nv_fs_identity_t destination_directory, nv_fs_identity_t source_entry,
		nv_fs_cancel_hook cancelled, void *cancel_arg)
{
	nv_parent_entry_t from, to;
	if(open_parent_entry(source, &from) != 0) return -1;
	if(open_parent_entry(destination, &to) != 0)
	{
		const int saved = errno;
		parent_entry_free(&from);
		errno = saved;
		return -1;
	}
	int result = -1;
	int saved = 0;
	struct stat current;
	if(!parent_matches(&from, source_directory) ||
			!parent_matches(&to, destination_directory) ||
			fstatat(from.fd, from.name, &current, AT_SYMLINK_NOFOLLOW) != 0)
	{
		saved = errno;
		goto done;
	}
	if(!identity_matches(&current, source_entry))
	{
		saved = ESTALE;
		goto done;
	}
	if(is_cancelled(cancelled, cancel_arg))
	{
		saved = errno;
		goto done;
	}
	if(S_ISDIR(current.st_mode))
	{
		const int source_fd = openat(from.fd, from.name,
				O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		const int contained = source_fd < 0 ? -1 :
			directory_is_ancestor(source_fd, to.fd);
		saved = errno;
		if(source_fd >= 0) close(source_fd);
		if(contained != 0)
		{
			if(contained > 0) saved = EINVAL;
			goto done;
		}
	}
#if defined(__APPLE__) || defined(__linux__)
	run_test_before_atomic_hook(source);
	struct stat before_move;
	if(fstatat(from.fd, from.name, &before_move, AT_SYMLINK_NOFOLLOW) != 0 ||
			!identity_matches(&before_move, source_entry))
	{
		saved = errno == 0 ? ESTALE : errno;
		goto done;
	}
#ifdef __APPLE__
	if(test_cross_device_move)
	{
		saved = EXDEV;
		goto done;
	}
	if(renameatx_np(from.fd, from.name, to.fd, to.name, RENAME_EXCL) != 0)
#else
	if(test_cross_device_move)
	{
		saved = EXDEV;
		goto done;
	}
	if(rename_no_replace(from.fd, from.name, to.fd, to.name) != 0)
#endif
	{
		saved = errno;
		goto done;
	}
	struct stat moved;
	const int moved_result = fstatat(to.fd, to.name, &moved,
			AT_SYMLINK_NOFOLLOW);
	if(moved_result == 0 &&
			same_object(&moved, source_entry))
	{
		result = 0;
	}
	else
	{
		saved = moved_result == 0 ? ESTALE : errno;
		if(moved_result == 0)
			restore_no_replace(to.fd, to.name, from.fd, from.name, &moved);
	}
	#else
	saved = ENOTSUP;
	#endif

done:
	parent_entry_free(&to);
	parent_entry_free(&from);
	if(result != 0) errno = saved == 0 ? EIO : saved;
	return result;
}

#endif

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 filetype=c : */
