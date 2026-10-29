#include <stic.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <test-utils.h>

#include "../../src/neovifm/action_task.h"
#include "../../src/neovifm/preview_task.h"

#if defined(__APPLE__) || defined(__linux__) || defined(_WIN32)

static nv_pane_snapshot_t *
pane_for(nv_workspace_session_t *session, nv_session_pane_t pane)
{
	return pane == NV_SESSION_LEFT ? &session->left : &session->right;
}

static nv_session_command_t
command_for(nv_workspace_session_t *session, nv_session_pane_t pane,
		nv_session_command_kind_t kind)
{
	nv_pane_snapshot_t *const source = pane_for(session, pane);
	nv_pane_snapshot_t *const destination = pane_for(session,
			pane == NV_SESSION_LEFT ? NV_SESSION_RIGHT : NV_SESSION_LEFT);
	static nv_session_action_target_t target;
	const nv_pane_entry_t *const entry = &source->entries[source->cursor];
	target = (nv_session_action_target_t){
		.path_bytes_hex = entry->path_bytes_hex,
		.device = entry->device,
		.inode = entry->inode,
		.ctime_unix_ns = entry->ctime_unix_ns,
		.kind = entry->kind,
	};
	return (nv_session_command_t){
		.kind = kind,
		.pane = pane,
		.action_cwd_bytes_hex = source->cwd_bytes_hex,
		.action_snapshot_revision = source->snapshot_revision,
		.action_cwd_device = source->cwd_device,
		.action_cwd_inode = source->cwd_inode,
		.action_cwd_ctime_unix_ns = source->cwd_ctime_unix_ns,
		.action_destination_cwd_bytes_hex = destination->cwd_bytes_hex,
		.action_destination_snapshot_revision = destination->snapshot_revision,
		.action_destination_cwd_device = destination->cwd_device,
		.action_destination_cwd_inode = destination->cwd_inode,
		.action_destination_cwd_ctime_unix_ns =
			destination->cwd_ctime_unix_ns,
		.action_targets = &target,
		.action_target_count = 1U,
	};
}

static int
pop_terminal(nv_action_queue_t *queue, nv_action_event_t *event,
		int *saw_queued, int *saw_running)
{
	for(int attempt = 0; attempt < 400; ++attempt)
	{
		if(nv_action_queue_pop(queue, event) == 1)
		{
			if(event->state == NV_ACTION_TASK_QUEUED) *saw_queued = 1;
			if(event->state == NV_ACTION_TASK_RUNNING) *saw_running = 1;
			if(event->state == NV_ACTION_TASK_DONE ||
					event->state == NV_ACTION_TASK_FAILED ||
					event->state == NV_ACTION_TASK_CANCELLED) return 1;
			nv_action_event_free(event);
		}
		else usleep(5000);
	}
	return 0;
}

static void
write_replacement(const char path[])
{
	(void)unlink(path);
	const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if(fd >= 0)
	{
		const ssize_t written = write(fd, "replacement", 11U);
		(void)written;
		(void)close(fd);
	}
}

TEST(action_queue_emits_lifecycle_and_copies_an_immutable_request)
{
	const char *const left = SANDBOX_PATH "/action-queue-left";
	const char *const right = SANDBOX_PATH "/action-queue-right";
	create_dir(left);
	create_dir(right);
	make_file(SANDBOX_PATH "/action-queue-left/file", "content");
	nv_workspace_session_t session = {};
	nv_snapshot_error_t error = {};
	assert_success(nv_workspace_session_init(left, right, &session, &error));
	nv_session_command_t command = command_for(&session, NV_SESSION_LEFT,
			NV_SESSION_COPY);
	nv_session_prepared_action_t action = {};
	assert_success(nv_workspace_session_prepare_action(&session, &command,
			&action, &error));
	char left_hex[NV_PREVIEW_MAX_PATH_BYTES*2U + 1U];
	char right_hex[NV_PREVIEW_MAX_PATH_BYTES*2U + 1U];
	char file_hex[NV_PREVIEW_MAX_PATH_BYTES*2U + 1U];
	assert_success(nv_preview_hex_encode(left, left_hex, sizeof(left_hex)));
	assert_success(nv_preview_hex_encode(right, right_hex, sizeof(right_hex)));
	assert_success(nv_preview_hex_encode(SANDBOX_PATH "/action-queue-left/file",
			file_hex, sizeof(file_hex)));
	nv_action_queue_t *const queue = nv_action_queue_alloc();
	assert_non_null(queue);
	uint64_t task_id = 0U;
	assert_success(nv_action_queue_submit(queue, &action, 7U, &task_id));
	assert_null(action.source_directory);

	int saw_queued = 0, saw_running = 0;
	nv_action_event_t event = {};
	assert_true(pop_terminal(queue, &event, &saw_queued, &saw_running));
	assert_true(saw_queued);
	assert_true(saw_running);
	assert_int_equal(NV_ACTION_TASK_DONE, event.state);
	assert_int_equal(task_id, event.task_id);
	assert_int_equal(7, event.command_sequence);
	assert_int_equal(1, event.completed_count);
	assert_int_equal(1, event.total_count);
	assert_true(event.bytes_known);
	assert_int_equal(7, event.bytes_total);
	assert_int_equal(7, event.bytes_completed);
	assert_string_equal(left_hex, event.source_path_bytes_hex);
	assert_string_equal(right_hex, event.destination_path_bytes_hex);
	assert_string_equal(file_hex, event.current_path_bytes_hex);
	assert_true(event.started_at_unix_ms > 0U);
	assert_true(event.finished_at_unix_ms >= event.started_at_unix_ms);
	assert_false(event.partial);
	assert_false(event.retryable);
	assert_int_equal(0, event.os_error);
	assert_true(nv_action_queue_busy(queue));
	nv_action_queue_ack_terminal(queue, event.task_id);
	assert_false(nv_action_queue_busy(queue));
	assert_success(access(SANDBOX_PATH "/action-queue-right/file", F_OK));

	nv_action_event_free(&event);
	nv_action_queue_free(queue);
	nv_workspace_session_free(&session);
	nv_snapshot_error_free(&error);
	remove_file(SANDBOX_PATH "/action-queue-left/file");
	remove_file(SANDBOX_PATH "/action-queue-right/file");
	remove_dir(left);
	remove_dir(right);
}

TEST(action_queue_publishes_item_progress_before_terminal_completion)
{
	const char *const left = SANDBOX_PATH "/action-progress-left";
	const char *const right = SANDBOX_PATH "/action-progress-right";
	create_dir(left);
	create_dir(right);
	make_file(SANDBOX_PATH "/action-progress-left/file", "content");
	nv_workspace_session_t session = {};
	nv_snapshot_error_t error = {};
	assert_success(nv_workspace_session_init(left, right, &session, &error));
	nv_session_command_t command = command_for(&session, NV_SESSION_LEFT,
			NV_SESSION_COPY);
	nv_session_prepared_action_t action = {};
	assert_success(nv_workspace_session_prepare_action(&session, &command,
			&action, &error));
	char progress_file_hex[NV_PREVIEW_MAX_PATH_BYTES*2U + 1U];
	assert_success(nv_preview_hex_encode(SANDBOX_PATH "/action-progress-left/file",
			progress_file_hex, sizeof(progress_file_hex)));
	nv_action_queue_t *const queue = nv_action_queue_alloc();
	assert_non_null(queue);
	assert_success(nv_action_queue_submit(queue, &action, 11U, NULL));
	int saw_progress = 0, saw_terminal = 0;
	for(int attempt = 0; attempt < 400 && !saw_terminal; ++attempt)
	{
		nv_action_event_t event = {};
		if(nv_action_queue_pop(queue, &event) == 0)
		{
			usleep(5000);
			continue;
		}
		if(event.state == NV_ACTION_TASK_RUNNING && event.completed_count == 1U)
		{
			saw_progress = 1;
			assert_true(event.bytes_known);
			assert_int_equal(7, event.bytes_completed);
			assert_string_equal(progress_file_hex, event.current_path_bytes_hex);
		}
		if(event.state == NV_ACTION_TASK_DONE) saw_terminal = 1;
		nv_action_event_free(&event);
	}
	assert_true(saw_progress);
	assert_true(saw_terminal);
	nv_action_queue_free(queue);
	nv_workspace_session_free(&session);
	nv_snapshot_error_free(&error);
	remove_file(SANDBOX_PATH "/action-progress-left/file");
	remove_file(SANDBOX_PATH "/action-progress-right/file");
	remove_dir(left);
	remove_dir(right);
}

TEST(action_queue_accepts_multiple_pending_actions_in_submission_order)
{
	const char *const left = SANDBOX_PATH "/action-capacity-left";
	const char *const right = SANDBOX_PATH "/action-capacity-right";
	create_dir(left);
	create_dir(right);
	make_file(SANDBOX_PATH "/action-capacity-left/file", "content");
	nv_workspace_session_t session = {};
	nv_snapshot_error_t error = {};
	assert_success(nv_workspace_session_init(left, right, &session, &error));
	nv_session_command_t command = command_for(&session, NV_SESSION_LEFT,
			NV_SESSION_COPY);
	nv_session_prepared_action_t first = {}, second = {};
	assert_success(nv_workspace_session_prepare_action(&session, &command,
			&first, &error));
	assert_success(nv_workspace_session_prepare_action(&session, &command,
			&second, &error));
	nv_action_queue_t *const queue = nv_action_queue_alloc_paused();
	assert_non_null(queue);
	uint64_t first_id = 0U, second_id = 0U;
	assert_success(nv_action_queue_submit(queue, &first, 1U, &first_id));
	assert_success(nv_action_queue_submit(queue, &second, 2U, &second_id));
	assert_true(first_id < second_id);
	nv_action_event_t event = {};
	assert_int_equal(1, nv_action_queue_pop(queue, &event));
	assert_int_equal(NV_ACTION_TASK_QUEUED, event.state);
	assert_int_equal(first_id, event.task_id);
	nv_action_event_free(&event);
	assert_int_equal(1, nv_action_queue_pop(queue, &event));
	assert_int_equal(NV_ACTION_TASK_QUEUED, event.state);
	assert_int_equal(second_id, event.task_id);
	nv_action_event_free(&event);
	assert_int_equal(0, nv_action_queue_pop(queue, &event));
	nv_action_queue_free(queue);
	nv_workspace_session_free(&session);
	nv_snapshot_error_free(&error);
	remove_file(SANDBOX_PATH "/action-capacity-left/file");
	remove_dir(left);
	remove_dir(right);
}

TEST(action_queue_cancels_a_pending_action_before_it_touches_disk)
{
	const char *const left = SANDBOX_PATH "/action-cancel-left";
	const char *const right = SANDBOX_PATH "/action-cancel-right";
	create_dir(left);
	create_dir(right);
	make_file(SANDBOX_PATH "/action-cancel-left/file", "content");
	nv_workspace_session_t session = {};
	nv_snapshot_error_t error = {};
	assert_success(nv_workspace_session_init(left, right, &session, &error));
	nv_session_command_t command = command_for(&session, NV_SESSION_LEFT,
			NV_SESSION_MOVE_FILES);
	nv_session_prepared_action_t action = {};
	assert_success(nv_workspace_session_prepare_action(&session, &command,
			&action, &error));
	nv_action_queue_t *const queue = nv_action_queue_alloc_paused();
	assert_non_null(queue);
	assert_success(nv_action_queue_submit(queue, &action, 1U, NULL));
	nv_action_queue_cancel_all(queue);
	assert_success(nv_action_queue_start(queue));
	int saw_queued = 0, saw_running = 0;
	nv_action_event_t event = {};
	assert_true(pop_terminal(queue, &event, &saw_queued, &saw_running));
	assert_int_equal(NV_ACTION_TASK_CANCELLED, event.state);
	assert_success(access(SANDBOX_PATH "/action-cancel-left/file", F_OK));
	assert_failure(access(SANDBOX_PATH "/action-cancel-right/file", F_OK));

	nv_action_event_free(&event);
	nv_action_queue_free(queue);
	nv_workspace_session_free(&session);
	nv_snapshot_error_free(&error);
	remove_file(SANDBOX_PATH "/action-cancel-left/file");
	remove_dir(left);
	remove_dir(right);
}

TEST(action_move_never_falls_back_to_copy_delete_across_filesystems)
{
	const char *const left = SANDBOX_PATH "/action-exdev-left";
	const char *const right = SANDBOX_PATH "/action-exdev-right";
	create_dir(left);
	create_dir(right);
	make_file(SANDBOX_PATH "/action-exdev-left/file", "content");
	nv_workspace_session_t session = {};
	nv_snapshot_error_t error = {};
	assert_success(nv_workspace_session_init(left, right, &session, &error));
	nv_session_command_t command = command_for(&session, NV_SESSION_LEFT,
			NV_SESSION_MOVE_FILES);
	nv_session_prepared_action_t action = {};
	assert_success(nv_workspace_session_prepare_action(&session, &command,
			&action, &error));
	nv_fs_test_force_cross_device_move(1);
	nv_action_queue_t *const queue = nv_action_queue_alloc();
	assert_non_null(queue);
	assert_success(nv_action_queue_submit(queue, &action, 1U, NULL));
	int saw_queued = 0, saw_running = 0;
	nv_action_event_t event = {};
	assert_true(pop_terminal(queue, &event, &saw_queued, &saw_running));
	nv_fs_test_force_cross_device_move(0);
	assert_int_equal(NV_ACTION_TASK_FAILED, event.state);
	assert_string_equal("cross-filesystem-move-unsupported", event.error_code);
	assert_success(access(SANDBOX_PATH "/action-exdev-left/file", F_OK));
	assert_failure(access(SANDBOX_PATH "/action-exdev-right/file", F_OK));

	nv_action_event_free(&event);
	nv_action_queue_free(queue);
	nv_workspace_session_free(&session);
	nv_snapshot_error_free(&error);
	remove_file(SANDBOX_PATH "/action-exdev-left/file");
	remove_dir(left);
	remove_dir(right);
}

TEST(action_queue_retains_a_failed_identity_for_safe_retry)
{
	const char *const left = SANDBOX_PATH "/action-retry-left";
	const char *const right = SANDBOX_PATH "/action-retry-right";
	const char *const source = SANDBOX_PATH "/action-retry-left/file";
	const char *const destination = SANDBOX_PATH "/action-retry-right/file";
	create_dir(left);
	create_dir(right);
	make_file(source, "content");
	make_file(destination, "existing");
	nv_workspace_session_t session = {};
	nv_snapshot_error_t error = {};
	assert_success(nv_workspace_session_init(left, right, &session, &error));
	nv_session_command_t command = command_for(&session, NV_SESSION_LEFT,
			NV_SESSION_COPY);
	nv_session_prepared_action_t action = {};
	assert_success(nv_workspace_session_prepare_action(&session, &command,
			&action, &error));
	nv_action_queue_t *const queue = nv_action_queue_alloc();
	assert_non_null(queue);
	uint64_t task_id = 0U;
	assert_success(nv_action_queue_submit(queue, &action, 9U, &task_id));
	int saw_queued = 0, saw_running = 0;
	nv_action_event_t event = {};
	assert_true(pop_terminal(queue, &event, &saw_queued, &saw_running));
	assert_int_equal(NV_ACTION_TASK_FAILED, event.state);
	assert_true(event.retryable);
	nv_action_event_free(&event);

	nv_session_prepared_action_t retry = {};
	assert_success(nv_action_queue_take_terminal_action(queue, task_id, &retry));
	assert_int_equal(NV_SESSION_COPY, retry.kind);
	assert_non_null(retry.source_directory);
	assert_non_null(retry.destination_directory);
	assert_int_equal(1, retry.target_count);
	assert_string_equal(source, retry.targets[0].path);
	nv_session_prepared_action_free(&retry);
	nv_action_queue_free(queue);
	nv_workspace_session_free(&session);
	nv_snapshot_error_free(&error);
	remove_file(source);
	remove_file(destination);
	remove_dir(left);
	remove_dir(right);
}

TEST(action_move_rolls_back_a_same_name_replacement_instead_of_moving_it)
{
	const char *const left = SANDBOX_PATH "/action-race-left";
	const char *const right = SANDBOX_PATH "/action-race-right";
	const char *const path = SANDBOX_PATH "/action-race-left/file";
	create_dir(left);
	create_dir(right);
	make_file(path, "original");
	nv_workspace_session_t session = {};
	nv_snapshot_error_t error = {};
	assert_success(nv_workspace_session_init(left, right, &session, &error));
	nv_session_command_t command = command_for(&session, NV_SESSION_LEFT,
			NV_SESSION_MOVE_FILES);
	nv_session_prepared_action_t action = {};
	assert_success(nv_workspace_session_prepare_action(&session, &command,
			&action, &error));
	nv_fs_test_set_before_atomic_hook(write_replacement);
	nv_action_queue_t *const queue = nv_action_queue_alloc();
	assert_non_null(queue);
	assert_success(nv_action_queue_submit(queue, &action, 1U, NULL));
	int saw_queued = 0, saw_running = 0;
	nv_action_event_t event = {};
	assert_true(pop_terminal(queue, &event, &saw_queued, &saw_running));
	nv_fs_test_set_before_atomic_hook(NULL);
	assert_int_equal(NV_ACTION_TASK_FAILED, event.state);
	assert_string_equal("stale-action", event.error_code);
	assert_success(access(path, F_OK));
	assert_failure(access(SANDBOX_PATH "/action-race-right/file", F_OK));

	nv_action_event_free(&event);
	nv_action_queue_free(queue);
	nv_workspace_session_free(&session);
	nv_snapshot_error_free(&error);
	remove_file(path);
	remove_dir(left);
	remove_dir(right);
}

TEST(action_delete_restores_source_when_trash_helper_fails)
{
#ifndef _WIN32
	const char *const helper = SANDBOX_PATH "/neovifm-test-trash-failure";
	const char *const left = SANDBOX_PATH "/delete-failure-left";
	const char *const right = SANDBOX_PATH "/delete-failure-right";
	const char *const path = SANDBOX_PATH "/delete-failure-left/file";
	create_dir(left);
	create_dir(right);
	make_file(path, "content");
	make_file(helper, "#!/bin/sh\nexit 1\n");
	assert_success(chmod(helper, 0700));
	assert_success(setenv("NEOVIFM_TRASH_EXECUTABLE", helper, 1));
	nv_workspace_session_t session = {};
	nv_snapshot_error_t error = {};
	assert_success(nv_workspace_session_init(left, right, &session, &error));
	nv_session_command_t command = command_for(&session, NV_SESSION_LEFT,
			NV_SESSION_DELETE);
	nv_session_prepared_action_t action = {};
	assert_success(nv_workspace_session_prepare_action(&session, &command,
			&action, &error));
	nv_action_queue_t *const queue = nv_action_queue_alloc();
	assert_non_null(queue);
	assert_success(nv_action_queue_submit(queue, &action, 1U, NULL));
	int saw_queued = 0, saw_running = 0;
	nv_action_event_t event = {};
	assert_true(pop_terminal(queue, &event, &saw_queued, &saw_running));
	assert_int_equal(NV_ACTION_TASK_FAILED, event.state);
	assert_string_equal("delete-failed", event.error_code);
	assert_success(access(path, F_OK));

	nv_action_event_free(&event);
	nv_action_queue_free(queue);
	nv_workspace_session_free(&session);
	nv_snapshot_error_free(&error);
	assert_success(unsetenv("NEOVIFM_TRASH_EXECUTABLE"));
	remove_file(path);
	remove_dir(left);
	remove_dir(right);
	remove_file(helper);
#endif
}

#endif /* __APPLE__ || __linux__ || _WIN32 */

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 filetype=c : */
