#include <stic.h>

#include <errno.h>
#include <stdio.h>

#ifdef _WIN32
# include <windows.h>
#else
# include <unistd.h>
#endif

#include <test-utils.h>

#include "../../src/neovifm/session_watcher.h"
#include "../../src/neovifm/workspace_session.h"

static void
pause_for_watch(void)
{
#ifdef _WIN32
	Sleep(10U);
#else
	usleep(10000U);
#endif
}

static unsigned int
wait_for_watch(nv_session_watcher_t *watcher, unsigned int expected)
{
	unsigned int observed = 0U;
	for(int attempt = 0; attempt < 300 && (observed & expected) != expected;
			++attempt)
	{
		unsigned int changed = 0U, failed = 0U;
		nv_session_watcher_poll(watcher, &changed, &failed);
		assert_int_equal(0, failed);
		observed |= changed;
		if((observed & expected) != expected) pause_for_watch();
	}
	return observed;
}

TEST(session_watcher_coalesces_external_changes_for_both_panes)
{
	const char *const left = SANDBOX_PATH "/watch-left";
	const char *const right = SANDBOX_PATH "/watch-right";
	create_dir(left);
	create_dir(right);
	nv_workspace_session_t session = {};
	nv_snapshot_error_t error = {};
	assert_success(nv_workspace_session_init(left, right, &session, &error));
	nv_session_watcher_t *const watcher = nv_session_watcher_alloc();
	assert_non_null(watcher);
	unsigned int failed = 0U;
	nv_session_watcher_sync(watcher, &session, &failed);
	assert_int_equal(0, failed);

	make_file(SANDBOX_PATH "/watch-left/a", "a");
	make_file(SANDBOX_PATH "/watch-left/b", "b");
	make_file(SANDBOX_PATH "/watch-right/c", "c");
	assert_int_equal(3U, wait_for_watch(watcher, 3U) & 3U);

	nv_session_watcher_free(watcher);
	nv_workspace_session_free(&session);
	nv_snapshot_error_free(&error);
	remove_file(SANDBOX_PATH "/watch-left/a");
	remove_file(SANDBOX_PATH "/watch-left/b");
	remove_file(SANDBOX_PATH "/watch-right/c");
	remove_dir(left);
	remove_dir(right);
}

TEST(session_watcher_rebinds_before_acknowledging_directory_navigation)
{
	const char *const left = SANDBOX_PATH "/watch-rebind-left";
	const char *const child = SANDBOX_PATH "/watch-rebind-left/child";
	const char *const right = SANDBOX_PATH "/watch-rebind-right";
	create_dir(left);
	create_dir(child);
	create_dir(right);
	nv_workspace_session_t session = {};
	nv_snapshot_error_t error = {};
	assert_success(nv_workspace_session_init(left, right, &session, &error));
	nv_session_watcher_t *const watcher = nv_session_watcher_alloc();
	assert_non_null(watcher);
	unsigned int failed = 0U;
	nv_session_watcher_sync(watcher, &session, &failed);
	assert_int_equal(0, failed);
	assert_success(nv_workspace_session_apply(&session, &(nv_session_command_t){
		.kind = NV_SESSION_ENTER,
	}, &error));
	nv_session_watcher_sync(watcher, &session, &failed);
	assert_int_equal(0, failed);

	make_file(SANDBOX_PATH "/watch-rebind-left/child/inside", "inside");
	assert_true((wait_for_watch(watcher, 1U) & 1U) != 0U);

	nv_session_watcher_free(watcher);
	nv_workspace_session_free(&session);
	nv_snapshot_error_free(&error);
	remove_file(SANDBOX_PATH "/watch-rebind-left/child/inside");
	remove_dir(child);
	remove_dir(left);
	remove_dir(right);
}

TEST(session_watcher_can_disable_one_pane_without_stopping_the_other)
{
	const char *const left = SANDBOX_PATH "/watch-disable-left";
	const char *const right = SANDBOX_PATH "/watch-disable-right";
	create_dir(left);
	create_dir(right);
	nv_workspace_session_t session = {};
	nv_snapshot_error_t error = {};
	assert_success(nv_workspace_session_init(left, right, &session, &error));
	nv_session_watcher_t *const watcher = nv_session_watcher_alloc();
	assert_non_null(watcher);
	unsigned int failed = 0U;
	nv_session_watcher_sync(watcher, &session, &failed);
	assert_int_equal(0, failed);
	nv_session_watcher_disable(watcher, NV_SESSION_LEFT);

	make_file(SANDBOX_PATH "/watch-disable-left/ignored", "ignored");
	make_file(SANDBOX_PATH "/watch-disable-right/observed", "observed");
	const unsigned int changed = wait_for_watch(watcher, 2U);
	assert_false((changed & 1U) != 0U);
	assert_true((changed & 2U) != 0U);

	nv_session_watcher_free(watcher);
	nv_workspace_session_free(&session);
	nv_snapshot_error_free(&error);
	remove_file(SANDBOX_PATH "/watch-disable-left/ignored");
	remove_file(SANDBOX_PATH "/watch-disable-right/observed");
	remove_dir(left);
	remove_dir(right);
}

TEST(session_watcher_detects_same_path_directory_replacement)
{
	const char *const left = SANDBOX_PATH "/watch-replace-left";
	const char *const old = SANDBOX_PATH "/watch-replace-old";
	const char *const right = SANDBOX_PATH "/watch-replace-right";
	create_dir(left);
	create_dir(right);
	nv_workspace_session_t session = {};
	nv_snapshot_error_t error = {};
	assert_success(nv_workspace_session_init(left, right, &session, &error));
	nv_session_watcher_t *const watcher = nv_session_watcher_alloc();
	assert_non_null(watcher);
	unsigned int failed = 0U;
	nv_session_watcher_sync(watcher, &session, &failed);
	assert_int_equal(0, failed);

	assert_success(rename(left, old));
	create_dir(left);
	make_file(SANDBOX_PATH "/watch-replace-left/new", "new");
	assert_true((wait_for_watch(watcher, 1U) & 1U) != 0U);
	assert_success(nv_workspace_session_refresh_pane(&session, NV_SESSION_LEFT,
			&error));
	nv_session_watcher_sync(watcher, &session, &failed);
	assert_int_equal(0, failed);

	nv_session_watcher_free(watcher);
	nv_workspace_session_free(&session);
	nv_snapshot_error_free(&error);
	remove_file(SANDBOX_PATH "/watch-replace-left/new");
	remove_dir(left);
	remove_dir(old);
	remove_dir(right);
}
