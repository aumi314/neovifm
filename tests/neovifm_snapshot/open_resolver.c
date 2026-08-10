#include <stic.h>

#include <stdlib.h>
#include <string.h>

#include <test-utils.h>

#include "../../src/neovifm/open_config.h"
#include "../../src/neovifm/open_resolver.h"

#ifdef _WIN32
#define setenv(name, value, overwrite) _putenv_s(name, value)
#define unsetenv(name) _putenv_s(name, "")
#endif

TEST(explicit_association_precedes_platform_fallback)
{
	const char *const association[] = { "viewer", "--wait" };
	nv_open_resolution_t resolution = {};
	nv_open_error_t error = {};
	assert_success(nv_open_resolve(NV_OPEN_INTENT_OPEN, "/tmp/a b.pdf",
			association, 2U, &resolution, &error));
	assert_string_equal("open", nv_open_intent_name(resolution.intent));
	assert_string_equal("association", nv_open_source_name(resolution.source));
	assert_int_equal(3, resolution.argc);
	assert_string_equal("viewer", resolution.argv[0]);
	assert_string_equal("--wait", resolution.argv[1]);
	assert_string_equal("/tmp/a b.pdf", resolution.argv[2]);
	nv_open_resolution_free(&resolution);
	nv_open_error_free(&error);
}

TEST(empty_association_uses_platform_fallback)
{
	const char *const old_value = getenv("MYVIFMRC");
	char *const old_copy = old_value == NULL ? NULL : strdup(old_value);
	assert_int_equal(0, unsetenv("MYVIFMRC"));
	nv_open_resolution_t resolution = {};
	nv_open_error_t error = {};
#ifdef _WIN32
	assert_success(nv_open_resolve(NV_OPEN_INTENT_OPEN,
			"C:/Temp/a b-\xe4\xb8\xad\xe6\x96\x87.pdf", NULL, 0U, &resolution, &error));
	assert_string_equal("platform", nv_open_source_name(resolution.source));
	assert_int_equal(2, resolution.argc);
	assert_non_null(strstr(resolution.argv[0], "neovifm-win-open.exe"));
	assert_string_equal("C:/Temp/a b-\xe4\xb8\xad\xe6\x96\x87.pdf", resolution.argv[1]);
#else
	assert_success(nv_open_resolve(NV_OPEN_INTENT_OPEN, "/tmp/a b.pdf",
			NULL, 0U, &resolution, &error));
	assert_string_equal("platform", nv_open_source_name(resolution.source));
	assert_int_equal(2, resolution.argc);
#ifdef __APPLE__
	assert_string_equal("/usr/bin/open", resolution.argv[0]);
#else
	assert_string_equal("xdg-open", resolution.argv[0]);
	#endif
	assert_string_equal("/tmp/a b.pdf", resolution.argv[1]);
#endif
	nv_open_resolution_free(&resolution);
	nv_open_error_free(&error);
	if(old_copy == NULL)
	{
		assert_int_equal(0, unsetenv("MYVIFMRC"));
	}
	else
	{
		assert_int_equal(0, setenv("MYVIFMRC", old_copy, 1));
		free(old_copy);
	}
}

TEST(open_resolver_rejects_unsafe_inputs_and_non_open_intents)
{
	nv_open_resolution_t resolution = {};
	nv_open_error_t error = {};
	char oversized[NV_OPEN_MAX_ARG_BYTES + 2U];
	memset(oversized, 'x', sizeof(oversized));
	oversized[sizeof(oversized) - 1U] = '\0';
	assert_failure(nv_open_resolve(NV_OPEN_INTENT_OPEN, oversized, NULL,
			0U, &resolution, &error));
	assert_string_equal("invalid-path", error.code);
	nv_open_error_free(&error);
	const char *const invalid[] = { "viewer", "" };
	assert_failure(nv_open_resolve(NV_OPEN_INTENT_OPEN, "/tmp/a", invalid,
			2U, &resolution, &error));
	assert_string_equal("invalid-association", error.code);
	nv_open_error_free(&error);
	assert_failure(nv_open_resolve(NV_OPEN_INTENT_PREVIEW, "/tmp/a", NULL,
			0U, &resolution, &error));
	assert_string_equal("unsupported-intent", error.code);
	nv_open_error_free(&error);
	nv_open_resolution_free(&resolution);
}

TEST(vifm_rules_match_in_configuration_order_and_expand_path_macro)
{
	const nv_open_association_rule_t rules[] = {
		{ NV_OPEN_ASSOC_FILETYPE, "*.md", "editor --wait %f" },
		{ NV_OPEN_ASSOC_FILEVIEWER, "*.md", "markdown %f" },
	};
	nv_open_resolution_t resolution = {};
	nv_open_error_t error = {};
	assert_success(nv_open_resolve_rules(NV_OPEN_INTENT_OPEN,
			"/tmp/readme.md", rules, sizeof(rules)/sizeof(rules[0]),
			&resolution, &error));
	assert_string_equal("association", nv_open_source_name(resolution.source));
	assert_int_equal(3, resolution.argc);
	assert_string_equal("editor", resolution.argv[0]);
	assert_string_equal("--wait", resolution.argv[1]);
	assert_string_equal("/tmp/readme.md", resolution.argv[2]);
	nv_open_resolution_free(&resolution);
	nv_open_error_free(&error);
}

TEST(vifm_fileviewer_rules_support_bounded_globs_and_quoted_argv)
{
	const nv_open_association_rule_t rules[] = {
		{ NV_OPEN_ASSOC_FILEVIEWER, "*.[mM][dD]", "markdown viewer --title 'Read me' %c" },
	};
	nv_open_resolution_t resolution = {};
	nv_open_error_t error = {};
	assert_success(nv_open_resolve_rules(NV_OPEN_INTENT_PREVIEW,
			"/tmp/readme.md", rules, 1U, &resolution, &error));
	assert_string_equal("preview", nv_open_intent_name(resolution.intent));
	assert_string_equal("association", nv_open_source_name(resolution.source));
	assert_int_equal(5, resolution.argc);
	assert_string_equal("markdown", resolution.argv[0]);
	assert_string_equal("viewer", resolution.argv[1]);
	assert_string_equal("--title", resolution.argv[2]);
	assert_string_equal("Read me", resolution.argv[3]);
	/* A path macro suppresses the implicit trailing target argument. */
	assert_string_equal("/tmp/readme.md", resolution.argv[4]);
	nv_open_resolution_free(&resolution);
	nv_open_error_free(&error);
}

TEST(vifm_rules_reject_shell_syntax_and_unsupported_macros)
{
	nv_open_resolution_t resolution = {};
	nv_open_error_t error = {};
	const nv_open_association_rule_t shell_rule = {
		NV_OPEN_ASSOC_FILETYPE, "*.md", "editor %f | less",
	};
	assert_failure(nv_open_resolve_rules(NV_OPEN_INTENT_OPEN,
			"/tmp/readme.md", &shell_rule, 1U, &resolution, &error));
	assert_string_equal("shell-syntax", error.code);
	nv_open_error_free(&error);

	const nv_open_association_rule_t macro_rule = {
		NV_OPEN_ASSOC_FILETYPE, "*.md", "editor %i",
	};
	assert_failure(nv_open_resolve_rules(NV_OPEN_INTENT_OPEN,
			"/tmp/readme.md", &macro_rule, 1U, &resolution, &error));
	assert_string_equal("unsupported-macro", error.code);
	nv_open_error_free(&error);

	const nv_open_association_rule_t pattern_rule = {
		NV_OPEN_ASSOC_FILETYPE, "*.md[", "editor %f",
	};
	assert_failure(nv_open_resolve_rules(NV_OPEN_INTENT_OPEN,
			"/tmp/readme.md", &pattern_rule, 1U, &resolution, &error));
	assert_string_equal("invalid-association", error.code);
	nv_open_error_free(&error);
	nv_open_resolution_free(&resolution);
}

TEST(vifm_rules_fall_back_only_for_open_and_bound_rule_count)
{
	const nv_open_association_rule_t viewer_rule = {
		NV_OPEN_ASSOC_FILEVIEWER, "*.md", "markdown %f",
	};
	nv_open_resolution_t resolution = {};
	nv_open_error_t error = {};
	const int fallback = nv_open_resolve_rules(NV_OPEN_INTENT_OPEN,
			"/tmp/unknown.bin", &viewer_rule, 1U, &resolution, &error);
#ifdef _WIN32
	assert_success(fallback);
	assert_string_equal("platform", nv_open_source_name(resolution.source));
	assert_non_null(strstr(resolution.argv[0], "neovifm-win-open.exe"));
#else
	assert_success(fallback);
	assert_string_equal("platform", nv_open_source_name(resolution.source));
#endif
	nv_open_resolution_free(&resolution);
	nv_open_error_free(&error);
	assert_failure(nv_open_resolve_rules(NV_OPEN_INTENT_PREVIEW,
			"/tmp/unknown.bin", &viewer_rule, 1U, &resolution, &error));
	assert_string_equal("no-association", error.code);
	nv_open_error_free(&error);

	nv_open_association_rule_t too_many[NV_OPEN_MAX_ASSOCIATIONS + 1U] = {};
	for(size_t i = 0U; i < sizeof(too_many)/sizeof(too_many[0]); ++i)
	{
		too_many[i] = viewer_rule;
	}
	assert_failure(nv_open_resolve_rules(NV_OPEN_INTENT_OPEN,
			"/tmp/unknown.bin", too_many,
			sizeof(too_many)/sizeof(too_many[0]), &resolution, &error));
	assert_string_equal("association-too-large", error.code);
	nv_open_error_free(&error);
	nv_open_resolution_free(&resolution);
}

TEST(vifm_config_loads_patterns_commands_and_continuations)
{
	const char path[] = SANDBOX_PATH "/vifmrc";
	make_file(path,
			"setl previewprg='previewer --flag %c'\n"
			"filetype {*.md,*.markdown} editor --wait %f,other %f\n"
			"fileviewer {*.md} markdown %f\n"
			"filetype {*.txt,\n"
			"             \\*.text} cat %f\n"
			"filextype {*.pdf},<application/pdf> /usr/bin/open %f\n");
	nv_open_config_t config = {};
	nv_open_error_t error = {};
	assert_success(nv_open_config_load(path, &config, &error));
	assert_int_equal(6, config.rule_count);
	assert_string_equal("previewer --flag %c", config.previewprg);
	assert_string_equal("*.md", config.rules[0].pattern);
	assert_string_equal("editor --wait %f", config.rules[0].command);
	assert_string_equal("*.markdown", config.rules[1].pattern);
	assert_string_equal("*.md", config.rules[2].pattern);
	assert_string_equal("*.txt", config.rules[3].pattern);
	assert_string_equal("*.text", config.rules[4].pattern);
	assert_string_equal("*.pdf", config.rules[5].pattern);

	nv_open_resolution_t resolution = {};
	assert_success(nv_open_resolve_rules(NV_OPEN_INTENT_PREVIEW,
			"/tmp/readme.md", config.rules, config.rule_count,
			&resolution, &error));
	assert_string_equal("markdown", resolution.argv[0]);
	nv_open_resolution_free(&resolution);
	assert_success(nv_open_resolve_rules(NV_OPEN_INTENT_OPEN,
			"/tmp/readme.markdown", config.rules, config.rule_count,
			&resolution, &error));
	assert_string_equal("editor", resolution.argv[0]);
	nv_open_resolution_free(&resolution);
	nv_open_config_free(&config);
	nv_open_error_free(&error);
	remove_file(path);
}

TEST(vifm_config_environment_is_used_only_without_explicit_association)
{
	const char path[] = SANDBOX_PATH "/env-vifmrc";
	make_file(path, "filetype {*.md} configured-editor %f\n");
	const char *const old_value = getenv("MYVIFMRC");
	char *const old_copy = old_value == NULL ? NULL : strdup(old_value);
	assert_int_equal(0, setenv("MYVIFMRC", path, 1));
	nv_open_resolution_t resolution = {};
	nv_open_error_t error = {};
	assert_success(nv_open_resolve(NV_OPEN_INTENT_OPEN,
			"/tmp/readme.md", NULL, 0U, &resolution, &error));
	assert_string_equal("association", nv_open_source_name(resolution.source));
	assert_string_equal("configured-editor", resolution.argv[0]);
	nv_open_resolution_free(&resolution);

	const char *const explicit_association[] = { "explicit-editor" };
	assert_success(nv_open_resolve(NV_OPEN_INTENT_OPEN,
			"/tmp/readme.md", explicit_association, 1U,
			&resolution, &error));
	assert_string_equal("explicit-editor", resolution.argv[0]);
	assert_string_equal("association", nv_open_source_name(resolution.source));
	nv_open_resolution_free(&resolution);
	nv_open_error_free(&error);
	nv_open_config_t config = {};
	assert_success(nv_open_config_load_env(&config, &error));
	nv_open_config_free(&config);
	if(old_copy == NULL)
	{
		assert_int_equal(0, unsetenv("MYVIFMRC"));
	}
	else
	{
		assert_int_equal(0, setenv("MYVIFMRC", old_copy, 1));
		free(old_copy);
	}
	remove_file(path);
}

TEST(vifm_config_reports_missing_files)
{
	nv_open_config_t config = {};
	nv_open_error_t error = {};
	assert_failure(nv_open_config_load(SANDBOX_PATH "/missing-vifmrc",
			&config, &error));
	assert_string_equal("config-open-failed", error.code);
	nv_open_config_free(&config);
	nv_open_error_free(&error);
}

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 filetype=c : */
