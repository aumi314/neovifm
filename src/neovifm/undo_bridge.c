/* vifm
 * Copyright (C) 2026 NeoVifm contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "undo_bridge.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../undo.h"

#if defined(ESTALE)
#define NV_IDENTITY_STALE_ERRNO ESTALE
#else
#define NV_IDENTITY_STALE_ERRNO EIO
#endif

typedef enum
{
	NV_UNDO_RECORD_MKDIR,
	NV_UNDO_RECORD_COPY,
	NV_UNDO_RECORD_MOVE,
} nv_undo_record_kind_t;

typedef struct nv_undo_record_t nv_undo_record_t;

struct nv_undo_record_t
{
	nv_undo_record_kind_t kind;
	char *source_path;
	char *destination_path;
	nv_fs_identity_t source_parent_identity;
	nv_fs_identity_t destination_parent_identity;
	nv_fs_identity_t source_identity;
	nv_fs_identity_t child_identity;
	nv_undo_bridge_location_t location;
	uint64_t group_id;
	nv_undo_record_t *next;
};

static nv_undo_record_t *records;
static int initialized;
static int undo_levels = 64;
static uint64_t next_group_id = 1U;

static int
identity_equal(nv_fs_identity_t left, nv_fs_identity_t right)
{
	return left.device == right.device && left.inode == right.inode &&
		left.ctime_unix_ns == right.ctime_unix_ns;
}

static nv_undo_record_t *
find_record_for_undo(OPS op, const char src[], const char dst[])
{
	for(nv_undo_record_t *record = records; record != NULL;
			record = record->next)
	{
		if(op == OP_RMDIR && record->kind == NV_UNDO_RECORD_MKDIR &&
				strcmp(record->destination_path, src) == 0) return record;
		if(op == OP_MKDIR && record->kind == NV_UNDO_RECORD_MKDIR &&
				strcmp(record->destination_path, src) == 0) return record;
		if(op == OP_REMOVE && record->kind == NV_UNDO_RECORD_COPY &&
				strcmp(record->destination_path, src) == 0) return record;
		if(op == OP_MOVE && record->kind == NV_UNDO_RECORD_MOVE &&
				strcmp(record->destination_path, src) == 0 &&
				dst != NULL && strcmp(record->source_path, dst) == 0) return record;
	}
	return NULL;
}

static int
current_identity(const char path[], nv_fs_identity_t *identity)
{
	struct stat st = {};
	int is_symlink = 0;
	if(nv_lstat(path, &st, &is_symlink, identity) != 0)
		return -1;
	return 0;
}

static void
free_record(nv_undo_record_t *record)
{
	if(record == NULL) return;
	free(record->source_path);
	free(record->destination_path);
	free(record);
}

static OpsResult
perform_undo_operation(OPS op, void *data, const char src[], const char dst[])
{
	(void)data;
	if(src == NULL) return OPS_FAILED;
	nv_undo_record_t *const record = find_record_for_undo(op, src, dst);
	if(record == NULL)
	{
		errno = ENOENT;
		return OPS_FAILED;
	}

	if(op == OP_RMDIR || op == OP_REMOVE)
	{
		nv_fs_identity_t current = {};
		if(current_identity(src, &current) != 0 ||
				!identity_equal(current, record->child_identity))
		{
			if(errno == 0) errno = NV_IDENTITY_STALE_ERRNO;
			return OPS_FAILED;
		}
		return nv_fs_remove(src, record->destination_parent_identity,
				record->child_identity, NULL, NULL) == 0 ? OPS_SUCCEEDED :
			OPS_FAILED;
	}

	if(op == OP_MOVE)
	{
		nv_fs_identity_t current = {};
		if(current_identity(src, &current) != 0 ||
				!identity_equal(current, record->child_identity))
		{
			if(errno == 0) errno = NV_IDENTITY_STALE_ERRNO;
			return OPS_FAILED;
		}
		return nv_fs_move(src, dst, record->destination_parent_identity,
				record->source_parent_identity, record->child_identity, NULL,
				NULL) == 0 ? OPS_SUCCEEDED : OPS_FAILED;
	}

	if(op == OP_MKDIR)
	{
		if(nv_fs_mkdir(src, 0777, record->destination_parent_identity) != 0)
			return OPS_FAILED;
		if(current_identity(src, &record->child_identity) != 0)
		{
			(void)nv_fs_remove(src, record->destination_parent_identity,
					record->child_identity, NULL, NULL);
			errno = EIO;
			return OPS_FAILED;
		}
		return OPS_SUCCEEDED;
	}

	errno = ENOTSUP;
	return OPS_FAILED;
}

static int
operation_available(OPS op)
{
	return op == OP_MKDIR || op == OP_RMDIR || op == OP_REMOVE ||
		op == OP_MOVE ? 0 : -1;
}

int
nv_undo_bridge_init(void)
{
	if(initialized) return 0;
	un_init(perform_undo_operation, operation_available, NULL, &undo_levels);
	initialized = 1;
	return 0;
}

void
nv_undo_bridge_reset(void)
{
	if(!initialized) return;
	un_reset();
	while(records != NULL)
	{
		nv_undo_record_t *const next = records->next;
		free_record(records);
		records = next;
	}
	next_group_id = 1U;
	initialized = 0;
}

int
nv_undo_bridge_record_mkdir(const char path[], nv_fs_identity_t parent_identity,
		nv_undo_bridge_location_t location)
{
	if(!initialized || path == NULL || path[0] == '\0')
	{
		errno = EINVAL;
		return -1;
	}
	struct stat st = {};
	int is_symlink = 0;
	nv_fs_identity_t identity = {};
	if(nv_lstat(path, &st, &is_symlink, &identity) != 0 || is_symlink != 0 ||
			!S_ISDIR(st.st_mode))
	{
		if(errno == 0) errno = EINVAL;
		return -1;
	}
	nv_undo_record_t *const record = calloc(1U, sizeof(*record));
	if(record == NULL || (record->destination_path = strdup(path)) == NULL)
	{
		free_record(record);
		errno = ENOMEM;
		return -1;
	}
	record->kind = NV_UNDO_RECORD_MKDIR;
	record->destination_parent_identity = parent_identity;
	record->location = location;
	record->child_identity = identity;
	record->group_id = next_group_id++;
	un_group_open("mkdir");
	if(un_group_add_op(OP_MKDIR, NULL, NULL, path, "") != 0)
	{
		un_group_close();
		free_record(record);
		errno = ENOMEM;
		return -1;
	}
	un_group_close();
	record->next = records;
	records = record;
	return 0;
}

static int
record_transfer_group(const nv_undo_bridge_transfer_t transfers[], size_t count,
		nv_undo_record_kind_t kind, nv_fs_identity_t source_parent,
		nv_fs_identity_t destination_parent, nv_undo_bridge_location_t location)
{
	if(!initialized || transfers == NULL || count == 0U || count > 64U ||
			(kind != NV_UNDO_RECORD_COPY && kind != NV_UNDO_RECORD_MOVE))
	{
		errno = EINVAL;
		return -1;
	}
	nv_undo_record_t *group = NULL;
	for(size_t i = 0U; i < count; ++i)
	{
		if(transfers[i].source_path == NULL || transfers[i].source_path[0] == '\0' ||
				transfers[i].destination_path == NULL ||
				transfers[i].destination_path[0] == '\0')
		{
			errno = EINVAL;
			goto failed;
		}
		nv_fs_identity_t child_identity = {};
		if(current_identity(transfers[i].destination_path, &child_identity) != 0)
			goto failed;
		nv_undo_record_t *const record = calloc(1U, sizeof(*record));
		if(record == NULL || (record->source_path = strdup(transfers[i].source_path)) == NULL ||
				(record->destination_path = strdup(transfers[i].destination_path)) == NULL)
		{
			free_record(record);
			errno = ENOMEM;
			goto failed;
		}
		record->kind = kind;
		record->source_parent_identity = source_parent;
		record->destination_parent_identity = destination_parent;
		record->source_identity = transfers[i].source_identity;
		record->child_identity = child_identity;
		record->location = location;
		record->next = group;
		group = record;
	}
	un_group_open(kind == NV_UNDO_RECORD_COPY ? "copy" : "move");
	for(nv_undo_record_t *record = group; record != NULL; record = record->next)
	{
		const OPS operation = kind == NV_UNDO_RECORD_COPY ? OP_COPY : OP_MOVE;
		if(un_group_add_op(operation, NULL, NULL, record->source_path,
				record->destination_path) != 0)
		{
			un_group_close();
			errno = ENOMEM;
			goto failed;
		}
	}
	un_group_close();
	const uint64_t group_id = next_group_id++;
	for(nv_undo_record_t *record = group; record != NULL; record = record->next)
		record->group_id = group_id;
	while(group != NULL)
	{
		nv_undo_record_t *const next = group->next;
		group->next = records;
		records = group;
		group = next;
	}
	return 0;

failed:
	while(group != NULL)
	{
		nv_undo_record_t *const next = group->next;
		free_record(group);
		group = next;
	}
	return -1;
}

int
nv_undo_bridge_record_copy_group(const nv_undo_bridge_transfer_t transfers[],
		size_t count, nv_fs_identity_t destination_parent,
		nv_undo_bridge_location_t location)
{
	return record_transfer_group(transfers, count, NV_UNDO_RECORD_COPY,
			(nv_fs_identity_t){}, destination_parent, location);
}

int
nv_undo_bridge_record_move_group(const nv_undo_bridge_transfer_t transfers[],
		size_t count, nv_fs_identity_t source_parent,
		nv_fs_identity_t destination_parent,
		nv_undo_bridge_location_t location)
{
	return record_transfer_group(transfers, count, NV_UNDO_RECORD_MOVE,
			source_parent, destination_parent, location);
}

nv_undo_bridge_result_t
nv_undo_bridge_undo(nv_undo_bridge_location_t *location)
{
	if(location != NULL) *location = (nv_undo_bridge_location_t){};
	if(!initialized) return NV_UNDO_BRIDGE_FAILED;
	const UnErrCode result = un_group_undo();
	if(result == UN_ERR_SUCCESS && location != NULL && records != NULL)
		*location = records->location;
	if(result == UN_ERR_SUCCESS && records != NULL)
	{
		const uint64_t group_id = records->group_id;
		while(records != NULL && records->group_id == group_id)
		{
			nv_undo_record_t *const record = records;
			records = record->next;
			free_record(record);
		}
	}
	switch(result)
	{
		case UN_ERR_SUCCESS: return NV_UNDO_BRIDGE_SUCCESS;
		case UN_ERR_NONE: return NV_UNDO_BRIDGE_NONE;
		default: return NV_UNDO_BRIDGE_FAILED;
	}
}

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
