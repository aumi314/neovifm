/* vifm
 * Copyright (C) 2026 NeoVifm contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "session_watcher.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
# include <windows.h>
#elif defined(__APPLE__)
# include <fcntl.h>
# include <sys/event.h>
# include <time.h>
# include <unistd.h>
#else
# include <time.h>
#endif

#ifndef __APPLE__
# include "../utils/fswatch.h"
#endif

#define NV_SESSION_WATCH_INTERVAL_MS 50U

typedef struct
{
	char *cwd_bytes_hex;
	uint64_t device;
	uint64_t inode;
	int has_identity;
#ifdef __APPLE__
	int descriptor;
	int preview_descriptor;
	char *preview_path_bytes_hex;
#else
	fswatch_t *watch;
#endif
} nv_session_pane_watcher_t;

struct nv_session_watcher_t
{
	nv_session_pane_watcher_t panes[2];
	uint64_t next_poll_ms;
#ifdef __APPLE__
	int queue;
#endif
};

static unsigned int pane_mask(nv_session_pane_t pane);
static nv_session_pane_watcher_t *pane_watcher(nv_session_watcher_t *watcher,
		nv_session_pane_t pane);
static const nv_pane_snapshot_t *pane_snapshot(
		const nv_workspace_session_t *session, nv_session_pane_t pane);
static int binding_matches(const nv_session_pane_watcher_t *pane,
		const nv_pane_snapshot_t *snapshot);
static int hex_digit(char character);
static char *hex_decode(const char hex[]);
static uint64_t monotonic_ms(void);
#ifdef __APPLE__
static void stop_preview(nv_session_pane_watcher_t *pane);
static void sync_preview(nv_session_watcher_t *watcher,
		nv_session_pane_watcher_t *pane, const nv_pane_snapshot_t *snapshot);
#endif
static void stop_pane(nv_session_pane_watcher_t *pane);
static int bind_pane(nv_session_watcher_t *watcher, nv_session_pane_t pane,
		const nv_pane_snapshot_t *snapshot);

static unsigned int
pane_mask(nv_session_pane_t pane)
{
	return 1U << (unsigned int)pane;
}

static nv_session_pane_watcher_t *
pane_watcher(nv_session_watcher_t *watcher, nv_session_pane_t pane)
{
	return &watcher->panes[pane == NV_SESSION_LEFT ? 0 : 1];
}

static const nv_pane_snapshot_t *
pane_snapshot(const nv_workspace_session_t *session, nv_session_pane_t pane)
{
	return pane == NV_SESSION_LEFT ? &session->left : &session->right;
}

static int
binding_matches(const nv_session_pane_watcher_t *pane,
		const nv_pane_snapshot_t *snapshot)
{
	return pane->cwd_bytes_hex != NULL && snapshot->cwd_bytes_hex != NULL &&
		strcmp(pane->cwd_bytes_hex, snapshot->cwd_bytes_hex) == 0 &&
		pane->has_identity == snapshot->has_cwd_stat &&
		(!pane->has_identity || (pane->device == snapshot->cwd_device &&
		 pane->inode == snapshot->cwd_inode));
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
	if(hex == NULL) return NULL;
	const size_t length = strlen(hex);
	if(length % 2U != 0U || length/2U > NV_PANE_SNAPSHOT_MAX_HEX_BYTES/2U)
		return NULL;
	char *const decoded = malloc(length/2U + 1U);
	if(decoded == NULL) return NULL;
	for(size_t i = 0U; i < length; i += 2U)
	{
		const int high = hex_digit(hex[i]), low = hex_digit(hex[i + 1U]);
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

static uint64_t
monotonic_ms(void)
{
#ifdef _WIN32
	return (uint64_t)GetTickCount64();
#else
	struct timespec now = {};
	if(clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0U;
	return (uint64_t)now.tv_sec*1000U + (uint64_t)now.tv_nsec/1000000U;
#endif
}

#ifdef __APPLE__
static void
stop_preview(nv_session_pane_watcher_t *pane)
{
	if(pane->preview_descriptor >= 0) close(pane->preview_descriptor);
	pane->preview_descriptor = -1;
	free(pane->preview_path_bytes_hex);
	pane->preview_path_bytes_hex = NULL;
}

static void
sync_preview(nv_session_watcher_t *watcher,
		nv_session_pane_watcher_t *pane, const nv_pane_snapshot_t *snapshot)
{
	const nv_pane_entry_t *entry = NULL;
	if(snapshot->cursor >= 0 && (size_t)snapshot->cursor < snapshot->entry_count)
	{
		const nv_pane_entry_t *const selected = &snapshot->entries[snapshot->cursor];
		if(selected->kind == NV_ENTRY_FILE || selected->kind == NV_ENTRY_EXECUTABLE)
			entry = selected;
	}
	const char *const path_hex = entry == NULL ? NULL : entry->path_bytes_hex;
	if(pane->preview_descriptor >= 0 && path_hex != NULL &&
			pane->preview_path_bytes_hex != NULL &&
			strcmp(pane->preview_path_bytes_hex, path_hex) == 0)
		return;
	stop_preview(pane);
	if(path_hex == NULL) return;
	pane->preview_path_bytes_hex = strdup(path_hex);
	char *const path = hex_decode(path_hex);
	if(pane->preview_path_bytes_hex == NULL || path == NULL)
	{
		free(path);
		stop_preview(pane);
		return;
	}
	const int descriptor = open(path, O_EVTONLY);
	free(path);
	if(descriptor < 0)
	{
		stop_preview(pane);
		return;
	}
	struct kevent change;
	EV_SET(&change, (uintptr_t)descriptor, EVFILT_VNODE,
		EV_ADD | EV_ENABLE | EV_CLEAR,
		NOTE_WRITE | NOTE_EXTEND | NOTE_DELETE | NOTE_RENAME | NOTE_ATTRIB,
		0, NULL);
	if(kevent(watcher->queue, &change, 1, NULL, 0, NULL) == -1)
	{
		close(descriptor);
		stop_preview(pane);
		return;
	}
	pane->preview_descriptor = descriptor;
}
#endif

static void
stop_pane(nv_session_pane_watcher_t *pane)
{
#ifdef __APPLE__
	stop_preview(pane);
	if(pane->descriptor >= 0) close(pane->descriptor);
	pane->descriptor = -1;
#else
	fswatch_free(pane->watch);
	pane->watch = NULL;
#endif
}

static int
bind_pane(nv_session_watcher_t *watcher, nv_session_pane_t pane,
		const nv_pane_snapshot_t *snapshot)
{
	nv_session_pane_watcher_t *const target = pane_watcher(watcher, pane);
	stop_pane(target);
	free(target->cwd_bytes_hex);
	target->cwd_bytes_hex = snapshot->cwd_bytes_hex == NULL ? NULL :
		strdup(snapshot->cwd_bytes_hex);
	target->has_identity = snapshot->has_cwd_stat;
	target->device = snapshot->cwd_device;
	target->inode = snapshot->cwd_inode;
	if(target->cwd_bytes_hex == NULL) { errno = ENOMEM; return -1; }
	char *const path = hex_decode(snapshot->cwd_bytes_hex);
	if(path == NULL) { errno = EINVAL; return -1; }
#ifdef __APPLE__
	const int descriptor = open(path, O_EVTONLY);
	free(path);
	if(descriptor < 0) return -1;
	struct kevent change;
	EV_SET(&change, (uintptr_t)descriptor, EVFILT_VNODE,
		EV_ADD | EV_ENABLE | EV_CLEAR,
		NOTE_WRITE | NOTE_EXTEND | NOTE_DELETE | NOTE_RENAME | NOTE_ATTRIB,
		0, NULL);
	if(kevent(watcher->queue, &change, 1, NULL, 0, NULL) == -1)
	{
		close(descriptor);
		return -1;
	}
	target->descriptor = descriptor;
#else
	target->watch = fswatch_create(path);
	free(path);
	if(target->watch == NULL)
	{
		if(errno == 0) errno = EIO;
		return -1;
	}
#endif
	return 0;
}

nv_session_watcher_t *
nv_session_watcher_alloc(void)
{
	nv_session_watcher_t *const watcher = calloc(1U, sizeof(*watcher));
	if(watcher == NULL) return NULL;
#ifdef __APPLE__
	watcher->queue = kqueue();
	watcher->panes[0].descriptor = -1;
	watcher->panes[1].descriptor = -1;
	watcher->panes[0].preview_descriptor = -1;
	watcher->panes[1].preview_descriptor = -1;
	if(watcher->queue < 0)
	{
		free(watcher);
		return NULL;
	}
#endif
	return watcher;
}

void
nv_session_watcher_free(nv_session_watcher_t *watcher)
{
	if(watcher == NULL) return;
	for(size_t i = 0U; i < 2U; ++i)
	{
		stop_pane(&watcher->panes[i]);
		free(watcher->panes[i].cwd_bytes_hex);
	}
#ifdef __APPLE__
	if(watcher->queue >= 0) close(watcher->queue);
#endif
	free(watcher);
}

void
nv_session_watcher_sync(nv_session_watcher_t *watcher,
		const nv_workspace_session_t *session, unsigned int *failed_panes)
{
	if(failed_panes != NULL) *failed_panes = 0U;
	if(watcher == NULL || session == NULL) return;
	for(nv_session_pane_t pane = NV_SESSION_LEFT; pane <= NV_SESSION_RIGHT; ++pane)
	{
		const nv_pane_snapshot_t *const snapshot = pane_snapshot(session, pane);
		nv_session_pane_watcher_t *const target = pane_watcher(watcher, pane);
		if(!binding_matches(target, snapshot) &&
				bind_pane(watcher, pane, snapshot) != 0 && failed_panes != NULL)
			*failed_panes |= pane_mask(pane);
#ifdef __APPLE__
		if(binding_matches(target, snapshot)) sync_preview(watcher, target, snapshot);
#endif
	}
}

void
nv_session_watcher_poll(nv_session_watcher_t *watcher,
		unsigned int *changed_panes, unsigned int *failed_panes)
{
	if(changed_panes != NULL) *changed_panes = 0U;
	if(failed_panes != NULL) *failed_panes = 0U;
	if(watcher == NULL) return;
	const uint64_t now = monotonic_ms();
	if(now != 0U && now < watcher->next_poll_ms) return;
	watcher->next_poll_ms = now + NV_SESSION_WATCH_INTERVAL_MS;
#ifdef __APPLE__
	struct kevent events[4];
	const struct timespec timeout = {};
	const int count = kevent(watcher->queue, NULL, 0, events,
		sizeof(events)/sizeof(events[0]), &timeout);
	if(count < 0)
	{
		if(errno == EINTR) return;
		for(nv_session_pane_t pane = NV_SESSION_LEFT;
				pane <= NV_SESSION_RIGHT; ++pane)
		{
			if(pane_watcher(watcher, pane)->descriptor < 0) continue;
			if(failed_panes != NULL) *failed_panes |= pane_mask(pane);
			stop_pane(pane_watcher(watcher, pane));
		}
		return;
	}
	for(int i = 0; i < count; ++i)
	{
		for(nv_session_pane_t pane = NV_SESSION_LEFT;
				pane <= NV_SESSION_RIGHT; ++pane)
		{
			nv_session_pane_watcher_t *const target = pane_watcher(watcher, pane);
			if(target->preview_descriptor >= 0 &&
				events[i].ident == (uintptr_t)target->preview_descriptor)
			{
				if((events[i].flags & EV_ERROR) == 0 &&
					events[i].filter == EVFILT_VNODE && changed_panes != NULL)
					*changed_panes |= pane_mask(pane);
				stop_preview(target);
				continue;
			}
			if(target->descriptor < 0 ||
					events[i].ident != (uintptr_t)target->descriptor) continue;
			if((events[i].flags & EV_ERROR) != 0)
			{
				if(failed_panes != NULL) *failed_panes |= pane_mask(pane);
				stop_pane(target);
			}
			else if(events[i].filter == EVFILT_VNODE && changed_panes != NULL)
				*changed_panes |= pane_mask(pane);
		}
	}
#else
	for(nv_session_pane_t pane = NV_SESSION_LEFT; pane <= NV_SESSION_RIGHT; ++pane)
	{
		nv_session_pane_watcher_t *const target = pane_watcher(watcher, pane);
		if(target->watch == NULL) continue;
		const FSWatchState state = fswatch_poll(target->watch);
		if(state == FSWS_UPDATED || state == FSWS_REPLACED)
		{
			if(changed_panes != NULL) *changed_panes |= pane_mask(pane);
		}
		else if(state == FSWS_ERRORED)
		{
			if(failed_panes != NULL) *failed_panes |= pane_mask(pane);
			stop_pane(target);
		}
	}
#endif
}

void
nv_session_watcher_disable(nv_session_watcher_t *watcher,
		nv_session_pane_t pane)
{
	if(watcher != NULL) stop_pane(pane_watcher(watcher, pane));
}

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 filetype=c : */
