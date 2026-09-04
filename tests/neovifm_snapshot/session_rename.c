#include <stic.h>

#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <test-utils.h>

#include "../../src/neovifm/workspace_session.h"
#include "../../src/neovifm/action_task.h"

#if defined(__APPLE__) || defined(__linux__)

static int
run_action(nv_workspace_session_t *session,
		const nv_session_command_t *command, nv_snapshot_error_t *error)
{
	nv_session_prepared_action_t action = {};
	if(nv_workspace_session_prepare_action(session, command, &action, error) != 0)
		return -1;
	nv_action_queue_t *const queue = nv_action_queue_alloc();
	assert_non_null(queue);
	if(nv_action_queue_submit(queue, &action, 1U, NULL) != 0)
	{
		nv_session_prepared_action_free(&action);
		nv_action_queue_free(queue);
		return -1;
	}
	nv_action_event_t event = {};
	for(int attempt = 0; attempt < 400; ++attempt)
	{
		if(nv_action_queue_pop(queue, &event) == 1)
		{
			if(event.state == NV_ACTION_TASK_DONE ||
					event.state == NV_ACTION_TASK_FAILED ||
					event.state == NV_ACTION_TASK_CANCELLED) break;
			nv_action_event_free(&event);
		}
		else usleep(5000);
	}
	nv_action_queue_free(queue);
	nv_snapshot_error_t refresh_error = {};
	assert_success(nv_workspace_session_refresh_pane(session, NV_SESSION_LEFT,
			&refresh_error));
	assert_success(nv_workspace_session_refresh_pane(session, NV_SESSION_RIGHT,
			&refresh_error));
	nv_snapshot_error_free(&refresh_error);
	if(event.state == NV_ACTION_TASK_DONE)
	{
		nv_action_event_free(&event);
		return 0;
	}
	nv_snapshot_error_free(error);
	error->code = strdup(event.error_code == NULL ? "action-failed" :
			event.error_code);
	error->message = strdup("file action failed");
	error->os_error = event.os_error;
	nv_action_event_free(&event);
	return -1;
}

static int
apply_rename(nv_workspace_session_t *session, nv_session_pane_t pane,
		const char name[], nv_snapshot_error_t *error)
{
	nv_pane_snapshot_t *const source = pane == NV_SESSION_LEFT ?
		&session->left : &session->right;
	nv_session_action_target_t target = {};
	if(source->cursor >= 0)
	{
		const nv_pane_entry_t *const entry = &source->entries[source->cursor];
		target = (nv_session_action_target_t){
			.path_bytes_hex = entry->path_bytes_hex,
			.device = entry->device,
			.inode = entry->inode,
			.ctime_unix_ns = entry->ctime_unix_ns,
			.kind = entry->kind,
		};
	}
	nv_session_command_t command = {
		.kind = NV_SESSION_RENAME,
		.pane = pane,
		.action_cwd_bytes_hex = source->cwd_bytes_hex,
		.action_snapshot_revision = source->snapshot_revision,
		.action_cwd_device = source->cwd_device,
		.action_cwd_inode = source->cwd_inode,
		.action_cwd_ctime_unix_ns = source->cwd_ctime_unix_ns,
		.action_targets = &target,
		.action_target_count = source->cursor >= 0 ? 1U : 0U,
	};
	strcpy(command.name, name);
	return run_action(session, &command, error);
}

TEST(rename_moves_single_target_within_its_directory)
{
	const char *const left = SANDBOX_PATH "/rename-left";
	const char *const right = SANDBOX_PATH "/rename-right";
	create_dir(left);
	create_dir(right);
	make_file(SANDBOX_PATH "/rename-left/a", "a");
	make_file(SANDBOX_PATH "/rename-left/b", "b");
	nv_workspace_session_t session = {};
	nv_snapshot_error_t error = {};
	assert_success(nv_workspace_session_init(left, right, &session, &error));

	/* Entries are name-sorted: cursor 0 is "a". */
	assert_success(apply_rename(&session, NV_SESSION_LEFT, "renamed", &error));
	assert_failure(access(SANDBOX_PATH "/rename-left/a", F_OK));
	assert_success(access(SANDBOX_PATH "/rename-left/b", F_OK));
	assert_success(access(SANDBOX_PATH "/rename-left/renamed", F_OK));

	/* Park the cursor on "renamed" (sorts after "b") for the next checks. */
	assert_success(nv_workspace_session_apply(&session, &(nv_session_command_t){
		.kind = NV_SESSION_MOVE_LAST,
	}, &error));

	/* Renaming onto an existing name keeps both files intact. */
	assert_failure(apply_rename(&session, NV_SESSION_LEFT, "b", &error));
	assert_string_equal("destination-exists", error.code);
	assert_success(access(SANDBOX_PATH "/rename-left/b", F_OK));
	assert_success(access(SANDBOX_PATH "/rename-left/renamed", F_OK));

	/* Invalid and unchanged names are rejected before touching disk. */
	assert_failure(apply_rename(&session, NV_SESSION_LEFT, "../escape", &error));
	assert_string_equal("invalid-name", error.code);
	assert_failure(apply_rename(&session, NV_SESSION_LEFT, "renamed", &error));
	assert_string_equal("invalid-name", error.code);

	nv_workspace_session_free(&session);
	nv_snapshot_error_free(&error);
	remove_file(SANDBOX_PATH "/rename-left/b");
	remove_file(SANDBOX_PATH "/rename-left/renamed");
	remove_dir(left);
	remove_dir(right);
}

TEST(rename_rejects_multi_target_and_stale_identity)
{
	const char *const left = SANDBOX_PATH "/rename-guard-left";
	const char *const right = SANDBOX_PATH "/rename-guard-right";
	create_dir(left);
	create_dir(right);
	make_file(SANDBOX_PATH "/rename-guard-left/a", "a");
	make_file(SANDBOX_PATH "/rename-guard-left/b", "b");
	nv_workspace_session_t session = {};
	nv_snapshot_error_t error = {};
	assert_success(nv_workspace_session_init(left, right, &session, &error));

	/* A rename always targets exactly one entry. */
	nv_session_action_target_t targets[2];
	for(size_t i = 0U; i < 2U; ++i)
	{
		const nv_pane_entry_t *const entry = &session.left.entries[i];
		targets[i] = (nv_session_action_target_t){
			.path_bytes_hex = entry->path_bytes_hex,
			.device = entry->device,
			.inode = entry->inode,
			.ctime_unix_ns = entry->ctime_unix_ns,
			.kind = entry->kind,
		};
	}
	nv_session_command_t command = {
		.kind = NV_SESSION_RENAME,
		.pane = NV_SESSION_LEFT,
		.action_cwd_bytes_hex = session.left.cwd_bytes_hex,
		.action_snapshot_revision = session.left.snapshot_revision,
		.action_cwd_device = session.left.cwd_device,
		.action_cwd_inode = session.left.cwd_inode,
		.action_cwd_ctime_unix_ns = session.left.cwd_ctime_unix_ns,
		.action_targets = targets,
		.action_target_count = 2U,
	};
	strcpy(command.name, "batch");
	nv_session_prepared_action_t action = {};
	assert_failure(nv_workspace_session_prepare_action(&session, &command,
			&action, &error));
	assert_string_equal("invalid-command", error.code);

	/* A stale entry identity never reaches the disk. */
	command.action_target_count = 1U;
	targets[0].inode += 1U;
	assert_failure(nv_workspace_session_prepare_action(&session, &command,
			&action, &error));
	assert_string_equal("stale-action", error.code);

	nv_workspace_session_free(&session);
	nv_snapshot_error_free(&error);
	remove_file(SANDBOX_PATH "/rename-guard-left/a");
	remove_file(SANDBOX_PATH "/rename-guard-left/b");
	remove_dir(left);
	remove_dir(right);
}

#endif

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions:=(0 : */
