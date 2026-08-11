/* vifm
 * Copyright (C) 2026 NeoVifm contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef VIFM__NEOVIFM__SESSION_WATCHER_H__
#define VIFM__NEOVIFM__SESSION_WATCHER_H__

#include "workspace_session.h"

typedef struct nv_session_watcher_t nv_session_watcher_t;

nv_session_watcher_t * nv_session_watcher_alloc(void);
void nv_session_watcher_free(nv_session_watcher_t *watcher);

void nv_session_watcher_sync(nv_session_watcher_t *watcher,
		const nv_workspace_session_t *session, unsigned int *failed_panes);
void nv_session_watcher_poll(nv_session_watcher_t *watcher,
		unsigned int *changed_panes, unsigned int *failed_panes);
void nv_session_watcher_disable(nv_session_watcher_t *watcher,
		nv_session_pane_t pane);
void nv_session_watcher_reset(nv_session_watcher_t *watcher);

#endif /* VIFM__NEOVIFM__SESSION_WATCHER_H__ */

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 filetype=c : */
