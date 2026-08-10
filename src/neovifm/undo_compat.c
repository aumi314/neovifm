/* vifm
 * Copyright (C) 2026 NeoVifm contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * The classic undo module is also used by the full Vifm binary, where these
 * helpers come from the normal utility/register/trash modules.  The compact
 * core-session target intentionally does not link that entire UI graph, so
 * this file supplies only the symbols needed by its mkdir-only undo slice.
 * It is excluded from the ordinary Vifm test object graph.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "../compat/fs_limits.h"
#include "../compat/neovifm_fs.h"
#include "../utils/fs.h"

int
replace_string(char **str, const char with[])
{
	char *replacement = with == NULL ? NULL : strdup(with);
	if(with != NULL && replacement == NULL) return -1;
	free(*str);
	*str = replacement;
	return 0;
}

char *
format_str(const char format[], ...)
{
	va_list args;
	va_start(args, format);
	va_list copy;
	va_copy(copy, args);
	const int length = vsnprintf(NULL, 0, format, args);
	va_end(args);
	if(length < 0)
	{
		va_end(copy);
		return NULL;
	}
	char *const result = malloc((size_t)length + 1U);
	if(result != NULL) (void)vsnprintf(result, (size_t)length + 1U, format, copy);
	va_end(copy);
	return result;
}

size_t
copy_str(char dst[], size_t dst_len, const char src[])
{
	const size_t length = strlen(src);
	if(dst_len != 0U)
	{
		const size_t copied = length < dst_len - 1U ? length : dst_len - 1U;
		memcpy(dst, src, copied);
		dst[copied] = '\0';
	}
	return length;
}

int
path_exists(const char path[], int deref)
{
	(void)deref;
	struct stat st;
	int is_symlink = 0;
	return nv_lstat(path, &st, &is_symlink, NULL) == 0;
}

int
is_case_change(const char src[], const char dst[])
{
	if(src == NULL || dst == NULL || strcmp(src, dst) == 0) return 0;
	while(*src != '\0' && *dst != '\0')
	{
		if(*src >= 'A' && *src <= 'Z')
		{
			if(*src + ('a' - 'A') != *dst) return 0;
		}
		else if(*src != *dst)
		{
			return 0;
		}
		++src;
		++dst;
	}
	return *src == '\0' && *dst == '\0';
}

#ifdef _WIN32
const char *
attr_str(uint32_t attr)
{
	static char value[6];
	size_t length = 0U;
	if((attr & FILE_ATTRIBUTE_ARCHIVE) != 0U) value[length++] = 'A';
	if((attr & FILE_ATTRIBUTE_HIDDEN) != 0U) value[length++] = 'H';
	if((attr & FILE_ATTRIBUTE_NOT_CONTENT_INDEXED) != 0U) value[length++] = 'I';
	if((attr & FILE_ATTRIBUTE_READONLY) != 0U) value[length++] = 'R';
	if((attr & FILE_ATTRIBUTE_SYSTEM) != 0U) value[length++] = 'S';
	value[length] = '\0';
	return value;
}
#endif

void
remove_last_path_component(char path[])
{
	if(path == NULL) return;
	char *const slash = strrchr(path, '/');
	if(slash == NULL) { strcpy(path, "."); return; }
	if(slash == path) { path[1] = '\0'; return; }
	*slash = '\0';
}

void
regs_rename_contents(const char old[], const char new[])
{
	(void)old;
	(void)new;
}

void
regs_sync_from_shared_memory(void)
{
}

void
regs_sync_to_shared_memory(void)
{
}

char *
trash_gen_path(const char base_path[], const char name[])
{
	(void)base_path;
	(void)name;
	return NULL;
}

const char *
trash_get_real_name_of(const char trash_path[])
{
	(void)trash_path;
	return NULL;
}

int
trash_has_path(const char path[])
{
	(void)path;
	return 0;
}

int
trash_has_path_at(const char trash_dir[], const char path[])
{
	(void)trash_dir;
	(void)path;
	return 0;
}

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
