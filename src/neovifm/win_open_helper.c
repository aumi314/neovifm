/* vifm
 * Copyright (C) 2026 NeoVifm contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 or 3 of the License.
 */

#ifdef _WIN32

#include <stdio.h> /* fprintf fputs */
#include <stdlib.h> /* free malloc */
#include <wchar.h> /* wchar_t */
#include <windows.h> /* GetLastError */
#include <objbase.h> /* CoInitializeEx CoUninitialize */
#include <shellapi.h> /* ShellExecuteExW SHELLEXECUTEINFOW */

int
wmain(int argc, wchar_t *argv[])
{
	if(argc != 2 || argv[1] == NULL || argv[1][0] == L'\0')
	{
		fputs("neovifm-win-open: expected one non-empty target path\n", stderr);
		return 2;
	}
	const size_t target_length = wcslen(argv[1]);
	wchar_t *const target = malloc((target_length + 1U)*sizeof(*target));
	if(target == NULL)
	{
		fputs("neovifm-win-open: failed to allocate target path\n", stderr);
		return 3;
	}
	for(size_t i = 0U; i <= target_length; ++i)
	{
		target[i] = argv[1][i] == L'/' ? L'\\' : argv[1][i];
	}
	const HRESULT initialized = CoInitializeEx(NULL,
			COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	if(FAILED(initialized))
	{
		fprintf(stderr, "neovifm-win-open: COM initialization failed (error %lu)\n",
				(unsigned long)initialized);
		free(target);
		return 4;
	}
	SHELLEXECUTEINFOW execution = {
		.cbSize = sizeof(execution),
		.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC,
		.lpFile = target,
		.nShow = SW_SHOWNORMAL,
	};
	if(!ShellExecuteExW(&execution))
	{
		const DWORD error = GetLastError();
		CoUninitialize();
		free(target);
		fprintf(stderr, "neovifm-win-open: system opener failed (error %lu)\n",
				(unsigned long)error);
		return 5;
	}
	if(execution.hProcess != NULL) CloseHandle(execution.hProcess);
	CoUninitialize();
	free(target);
	return 0;
}

#endif /* _WIN32 */

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 filetype=c : */
