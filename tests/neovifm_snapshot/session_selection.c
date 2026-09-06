#include <stic.h>

#include <string.h>
#include <unistd.h>

#include <test-utils.h>

#include "../../src/neovifm/workspace_session.h"

#if defined(__APPLE__) || defined(__linux__)

static int
entry_selected(const nv_pane_snapshot_t *pane, const char name[])
{
	for(size_t i = 0U; i < pane->entry_count; ++i)
	{
		if(strcmp(pane->entries[i].name_display, name) == 0)
			return pane->entries[i].selected;
	}
	return -1;
}

TEST(session_select_all_marks_every_entry_in_the_active_pane)
{
	const char *const left = SANDBOX_PATH "/select-all-left";
	const char *const right = SANDBOX_PATH "/select-all-right";
	create_dir(left);
	create_dir(right);
	make_file(SANDBOX_PATH "/select-all-left/a", "a");
	make_file(SANDBOX_PATH "/select-all-left/b", "b");
	make_file(SANDBOX_PATH "/select-all-left/c", "c");
	make_file(SANDBOX_PATH "/select-all-right/r", "r");
	nv_workspace_session_t session = {};
	nv_snapshot_error_t error = {};
	assert_success(nv_workspace_session_init(left, right, &session, &error));

	assert_success(nv_workspace_session_apply(&session, &(nv_session_command_t){
		.kind = NV_SESSION_SELECT_ALL,
	}, &error));
	assert_int_equal(3, session.left.selection_count);
	assert_int_equal(1, entry_selected(&session.left, "a"));
	assert_int_equal(1, entry_selected(&session.left, "b"));
	assert_int_equal(1, entry_selected(&session.left, "c"));
	assert_int_equal(0, session.left.cursor);
	assert_int_equal(0, session.right.selection_count);
	assert_int_equal(0, entry_selected(&session.right, "r"));

	/* Selecting everything again is stable, and clearing empties the set. */
	assert_success(nv_workspace_session_apply(&session, &(nv_session_command_t){
		.kind = NV_SESSION_SELECT_ALL,
	}, &error));
	assert_int_equal(3, session.left.selection_count);
	assert_success(nv_workspace_session_apply(&session, &(nv_session_command_t){
		.kind = NV_SESSION_CLEAR_SELECTION,
	}, &error));
	assert_int_equal(0, session.left.selection_count);
	assert_int_equal(0, entry_selected(&session.left, "a"));
	assert_int_equal(0, entry_selected(&session.left, "b"));
	assert_int_equal(0, entry_selected(&session.left, "c"));

	/* Clearing an empty selection stays a successful no-op. */
	assert_success(nv_workspace_session_apply(&session, &(nv_session_command_t){
		.kind = NV_SESSION_CLEAR_SELECTION,
	}, &error));
	assert_int_equal(0, session.left.selection_count);

	nv_workspace_session_free(&session);
	nv_snapshot_error_free(&error);
	remove_file(SANDBOX_PATH "/select-all-left/a");
	remove_file(SANDBOX_PATH "/select-all-left/b");
	remove_file(SANDBOX_PATH "/select-all-left/c");
	remove_file(SANDBOX_PATH "/select-all-right/r");
	remove_dir(left);
	remove_dir(right);
}

TEST(session_selection_commands_can_target_a_pane_without_stealing_focus)
{
	const char *const left = SANDBOX_PATH "/select-pane-left";
	const char *const right = SANDBOX_PATH "/select-pane-right";
	create_dir(left);
	create_dir(right);
	make_file(SANDBOX_PATH "/select-pane-left/a", "a");
	make_file(SANDBOX_PATH "/select-pane-right/r", "r");
	nv_workspace_session_t session = {};
	nv_snapshot_error_t error = {};
	assert_success(nv_workspace_session_init(left, right, &session, &error));
	assert_int_equal(NV_SESSION_LEFT, session.active_pane);

	assert_success(nv_workspace_session_apply(&session, &(nv_session_command_t){
		.kind = NV_SESSION_SELECT_ALL, .pane = NV_SESSION_RIGHT, .has_pane = 1,
	}, &error));
	assert_int_equal(NV_SESSION_LEFT, session.active_pane);
	assert_int_equal(1, session.right.selection_count);
	assert_int_equal(1, entry_selected(&session.right, "r"));
	assert_int_equal(0, session.left.selection_count);

	assert_success(nv_workspace_session_apply(&session, &(nv_session_command_t){
		.kind = NV_SESSION_CLEAR_SELECTION, .pane = NV_SESSION_RIGHT,
		.has_pane = 1,
	}, &error));
	assert_int_equal(NV_SESSION_LEFT, session.active_pane);
	assert_int_equal(0, session.right.selection_count);
	assert_int_equal(0, entry_selected(&session.right, "r"));

	assert_failure(nv_workspace_session_apply(&session, &(nv_session_command_t){
		.kind = NV_SESSION_SELECT_ALL, .pane = (nv_session_pane_t)2,
		.has_pane = 1,
	}, &error));
	assert_string_equal("invalid-command", error.code);
	nv_snapshot_error_free(&error);
	assert_failure(nv_workspace_session_apply(&session, &(nv_session_command_t){
		.kind = NV_SESSION_CLEAR_SELECTION, .pane = (nv_session_pane_t)2,
		.has_pane = 1,
	}, &error));
	assert_string_equal("invalid-command", error.code);

	nv_workspace_session_free(&session);
	nv_snapshot_error_free(&error);
	remove_file(SANDBOX_PATH "/select-pane-left/a");
	remove_file(SANDBOX_PATH "/select-pane-right/r");
	remove_dir(left);
	remove_dir(right);
}

TEST(session_selection_commands_coexist_with_refresh_retention)
{
	const char *const left = SANDBOX_PATH "/select-refresh-left";
	const char *const right = SANDBOX_PATH "/select-refresh-right";
	create_dir(left);
	create_dir(right);
	make_file(SANDBOX_PATH "/select-refresh-left/a", "a");
	make_file(SANDBOX_PATH "/select-refresh-left/b", "b");
	make_file(SANDBOX_PATH "/select-refresh-left/c", "c");
	nv_workspace_session_t session = {};
	nv_snapshot_error_t error = {};
	assert_success(nv_workspace_session_init(left, right, &session, &error));

	assert_success(nv_workspace_session_apply(&session, &(nv_session_command_t){
		.kind = NV_SESSION_SELECT_ALL,
	}, &error));
	assert_int_equal(3, session.left.selection_count);

	/* A refresh that adds an entry keeps the old marks and does not extend
	 * select-all to the newcomer. */
	make_file(SANDBOX_PATH "/select-refresh-left/m", "m");
	assert_success(nv_workspace_session_apply(&session, &(nv_session_command_t){
		.kind = NV_SESSION_REFRESH,
	}, &error));
	assert_int_equal(4, session.left.entry_count);
	assert_int_equal(3, session.left.selection_count);
	assert_int_equal(1, entry_selected(&session.left, "a"));
	assert_int_equal(1, entry_selected(&session.left, "b"));
	assert_int_equal(1, entry_selected(&session.left, "c"));
	assert_int_equal(0, entry_selected(&session.left, "m"));

	/* A refresh that drops an entry shrinks the set by exactly that entry. */
	remove_file(SANDBOX_PATH "/select-refresh-left/b");
	assert_success(nv_workspace_session_apply(&session, &(nv_session_command_t){
		.kind = NV_SESSION_REFRESH,
	}, &error));
	assert_int_equal(3, session.left.entry_count);
	assert_int_equal(2, session.left.selection_count);
	assert_int_equal(-1, entry_selected(&session.left, "b"));

	assert_success(nv_workspace_session_apply(&session, &(nv_session_command_t){
		.kind = NV_SESSION_CLEAR_SELECTION,
	}, &error));
	assert_int_equal(0, session.left.selection_count);
	assert_int_equal(0, entry_selected(&session.left, "a"));
	assert_int_equal(0, entry_selected(&session.left, "c"));

	nv_workspace_session_free(&session);
	nv_snapshot_error_free(&error);
	remove_file(SANDBOX_PATH "/select-refresh-left/a");
	remove_file(SANDBOX_PATH "/select-refresh-left/c");
	remove_file(SANDBOX_PATH "/select-refresh-left/m");
	remove_dir(left);
	remove_dir(right);
}

#endif

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions:=(0 : */
