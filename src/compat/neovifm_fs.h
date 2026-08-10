/* vifm
 * Copyright (C) 2026 NeoVifm contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef VIFM__COMPAT__NEOVIFM_FS_H__
#define VIFM__COMPAT__NEOVIFM_FS_H__

#include <sys/stat.h> /* struct stat */
#include <stdint.h> /* uint64_t */
#include <errno.h> /* EIO ESTALE */

#if defined(ESTALE)
#define NV_FS_STALE_ERRNO ESTALE
#else
#define NV_FS_STALE_ERRNO EBUSY
#endif

typedef struct nv_dir_t nv_dir_t;

typedef struct
{
	uint64_t device;
	uint64_t inode;
	uint64_t ctime_unix_ns;
} nv_fs_identity_t;

nv_dir_t * nv_dir_open(const char path[]);
const char * nv_dir_read(nv_dir_t *dir);
int nv_dir_close(nv_dir_t *dir);
int nv_dir_fstat(nv_dir_t *dir, struct stat *st, nv_fs_identity_t *identity);
int nv_dir_lstat(nv_dir_t *dir, const char name[], struct stat *st,
		int *is_symlink, nv_fs_identity_t *identity);

/*
 * Gets no-follow metadata and reports symbolic-link identity separately.
 * On Windows this uses FindFirstFileW() so dangling links retain their
 * identity even though struct stat has no portable S_IFLNK representation.
 */
int nv_lstat(const char path[], struct stat *st, int *is_symlink,
		nv_fs_identity_t *identity);

/* Whether this runtime can uphold the platform file-action safety contract. */
int nv_fs_actions_supported(void);

typedef int (*nv_fs_cancel_hook)(void *arg);
typedef void (*nv_fs_test_before_atomic_hook)(const char path[]);

int nv_fs_copy(const char source[], const char destination[],
		nv_fs_identity_t source_directory,
		nv_fs_identity_t destination_directory, nv_fs_identity_t source_entry,
		nv_fs_cancel_hook cancelled, void *cancel_arg);
int nv_fs_move(const char source[], const char destination[],
		nv_fs_identity_t source_directory,
		nv_fs_identity_t destination_directory, nv_fs_identity_t source_entry,
		nv_fs_cancel_hook cancelled, void *cancel_arg);
int nv_fs_remove(const char path[], nv_fs_identity_t source_directory,
		nv_fs_identity_t source_entry, nv_fs_cancel_hook cancelled,
		void *cancel_arg);
int nv_fs_mkdir(const char path[], int mode,
		nv_fs_identity_t destination_directory);

/* Deterministic race seams used only by the C test suite. */
void nv_fs_test_set_before_atomic_hook(nv_fs_test_before_atomic_hook hook);
void nv_fs_test_force_cross_device_move(int enabled);

#endif /* VIFM__COMPAT__NEOVIFM_FS_H__ */

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 filetype=c : */
