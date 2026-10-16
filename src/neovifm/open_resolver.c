/* vifm
 * Copyright (C) 2026 NeoVifm contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 or 3 of the License.
 */

#include "open_resolver.h"

#include "open_config.h"

#include <ctype.h> /* isspace() */
#include <stdlib.h> /* calloc free malloc */
#include <string.h> /* memcpy strlen strdup */

#ifdef _WIN32
#include "../utils/utf8.h"

#include <windows.h> /* GetModuleFileNameW */
#include <wchar.h> /* wchar_t wcsrchr wcslen */
#endif

static int set_error(nv_open_error_t *error, const char code[],
		const char message[]);
static char *platform_opener(void);
static int bounded_length(const char value[], size_t maximum, size_t *length);
static int association_kind_valid(nv_open_association_kind_t kind);
static int association_kind_accepts(nv_open_intent_t intent,
		nv_open_association_kind_t kind);
static int glob_pattern_valid(const char pattern[]);
static int glob_class_match(const char pattern[], size_t start,
		unsigned char character, size_t *next);
static int glob_matches(const char pattern[], const char value[]);
static int append_token_character(char token[], size_t *length,
		unsigned char character);
static int expand_token(const char token[], const char path[], char output[],
		size_t *length, int *used_path_macro);
static int parse_association_command(const char command[], const char path[],
		char **argv[], size_t *argc, nv_open_error_t *error);
static int build_rule_resolution(nv_open_intent_t intent, const char path[],
		const nv_open_association_rule_t *rule,
		nv_open_resolution_t *resolution, nv_open_error_t *error);
static int resolve_open_argv(const char path[],
		const char *const association_argv[], size_t association_argc,
		nv_open_resolution_t *resolution, nv_open_error_t *error);

const char *
nv_open_intent_name(nv_open_intent_t intent)
{
	switch(intent)
	{
		case NV_OPEN_INTENT_OPEN: return "open";
		case NV_OPEN_INTENT_EDIT: return "edit";
		case NV_OPEN_INTENT_PREVIEW: return "preview";
	}
	return NULL;
}

const char *
nv_open_source_name(nv_open_source_t source)
{
	switch(source)
	{
		case NV_OPEN_SOURCE_ASSOCIATION: return "association";
		case NV_OPEN_SOURCE_PLATFORM: return "platform";
	}
	return NULL;
}

static int
set_error(nv_open_error_t *error, const char code[], const char message[])
{
	if(error == NULL) return -1;
	nv_open_error_free(error);
	error->code = strdup(code);
	error->message = strdup(message);
	if(error->code == NULL || error->message == NULL)
	{
		nv_open_error_free(error);
	}
	return -1;
}

static int
argument_valid(const char argument[])
{
	if(argument == NULL || argument[0] == '\0') return 0;
	for(size_t i = 0U; i <= NV_OPEN_MAX_ARG_BYTES; ++i)
	{
		if(argument[i] == '\0') return 1;
	}
	return 0;
}

static char *
platform_opener(void)
{
#ifdef __APPLE__
	return strdup("/usr/bin/open");
#elif defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
		defined(__NetBSD__) || defined(__sun__) || defined(_AIX)
	return strdup("xdg-open");
#elif defined(_WIN32)
	DWORD capacity = 512U;
	wchar_t *module = NULL;
	for(;;)
	{
		module = calloc(capacity, sizeof(*module));
		if(module == NULL) return NULL;
		const DWORD length = GetModuleFileNameW(NULL, module, capacity);
		if(length == 0U)
		{
			free(module);
			return NULL;
		}
		if(length < capacity - 1U) break;
		free(module);
		module = NULL;
		if(capacity >= 32768U) return NULL;
		capacity *= 2U;
	}
	wchar_t *slash = wcsrchr(module, L'\\');
	wchar_t *const forward_slash = wcsrchr(module, L'/');
	if(forward_slash != NULL && (slash == NULL || forward_slash > slash))
	{
		slash = forward_slash;
	}
	if(slash == NULL)
	{
		free(module);
		return NULL;
	}
	static const wchar_t helper[] = L"neovifm-win-open.exe";
	const size_t prefix = (size_t)(slash - module) + 1U;
	const size_t helper_length = sizeof(helper)/sizeof(helper[0]);
	if(prefix + helper_length > capacity)
	{
		wchar_t *const resized = realloc(module,
				(prefix + helper_length)*sizeof(*module));
		if(resized == NULL)
		{
			free(module);
			return NULL;
		}
		module = resized;
	}
	memcpy(module + prefix, helper, helper_length*sizeof(*module));
	char *const result = utf8_from_utf16(module);
	free(module);
	return result;
#else
	return NULL;
#endif
}

int
nv_open_resolve(nv_open_intent_t intent, const char path[],
		const char *const association_argv[], size_t association_argc,
		nv_open_resolution_t *resolution, nv_open_error_t *error)
{
	if(resolution == NULL || error == NULL)
	{
		return -1;
	}
	nv_open_resolution_free(resolution);
	nv_open_error_free(error);
	if(nv_open_intent_name(intent) == NULL)
	{
		return set_error(error, "invalid-intent", "open intent is invalid");
	}
	if(path == NULL || !argument_valid(path))
	{
		return set_error(error, "invalid-path", "open path is empty or too long");
	}
	if(intent != NV_OPEN_INTENT_OPEN)
	{
		return set_error(error, "unsupported-intent",
				"only the open intent has a platform resolver");
	}
	if(association_argc > NV_OPEN_MAX_ARGS - 1U ||
			(association_argc != 0U && association_argv == NULL))
	{
		return set_error(error, "association-too-large",
				"open association has too many arguments");
	}
	for(size_t i = 0U; i < association_argc; ++i)
	{
		if(!argument_valid(association_argv[i]))
		{
			return set_error(error, "invalid-association",
					"open association contains an invalid argument");
		}
	}
	if(association_argc == 0U)
	{
		const char *const config_path = getenv("MYVIFMRC");
		if(config_path != NULL && config_path[0] != '\0')
		{
			nv_open_config_t config = {};
			if(nv_open_config_load_env(&config, error) != 0)
			{
				return -1;
			}
			const int result = nv_open_resolve_rules(intent, path,
					config.rules, config.rule_count, resolution, error);
			nv_open_config_free(&config);
			return result;
		}
	}
	return resolve_open_argv(path, association_argv, association_argc,
			resolution, error);
}

static int
resolve_open_argv(const char path[], const char *const association_argv[],
		size_t association_argc, nv_open_resolution_t *resolution,
		nv_open_error_t *error)
{
	char *opener = association_argc == 0U ? platform_opener() : NULL;
	if(association_argc == 0U && opener == NULL)
	{
		return set_error(error, "unsupported-platform",
				"no platform opener is available");
	}
	const size_t prefix_argc = association_argc == 0U ? 1U : association_argc;
	const size_t argc = prefix_argc + 1U;
	char **const argv = calloc(argc + 1U, sizeof(*argv));
	if(argv == NULL)
	{
		free(opener);
		return set_error(error, "out-of-memory", "failed to allocate open argv");
	}
	if(association_argc == 0U)
	{
		argv[0] = opener;
		opener = NULL;
	}
	for(size_t i = 0U; i < association_argc; ++i)
	{
		argv[i] = strdup(association_argv[i]);
		if(argv[i] == NULL) goto allocation_failed;
	}
	argv[prefix_argc] = strdup(path);
	if(argv[prefix_argc] == NULL) goto allocation_failed;
	resolution->intent = NV_OPEN_INTENT_OPEN;
	resolution->source = association_argc == 0U ? NV_OPEN_SOURCE_PLATFORM :
		NV_OPEN_SOURCE_ASSOCIATION;
	resolution->argv = argv;
	resolution->argc = argc;
	return 0;

allocation_failed:
	free(opener);
	for(size_t i = 0U; i < argc; ++i) free(argv[i]);
	free(argv);
	return set_error(error, "out-of-memory", "failed to copy open argv");
}

static int
bounded_length(const char value[], size_t maximum, size_t *length)
{
	if(value == NULL)
	{
		return 0;
	}
	for(size_t i = 0U; i <= maximum; ++i)
	{
		if(value[i] == '\0')
		{
			if(length != NULL) *length = i;
			return 1;
		}
	}
	return 0;
}

static int
association_kind_valid(nv_open_association_kind_t kind)
{
	return kind == NV_OPEN_ASSOC_FILETYPE || kind == NV_OPEN_ASSOC_FILEXTYPE ||
		kind == NV_OPEN_ASSOC_FILEVIEWER;
}

static int
association_kind_accepts(nv_open_intent_t intent,
		nv_open_association_kind_t kind)
{
	if(intent == NV_OPEN_INTENT_OPEN)
	{
		return kind == NV_OPEN_ASSOC_FILETYPE || kind == NV_OPEN_ASSOC_FILEXTYPE;
	}
	if(intent == NV_OPEN_INTENT_PREVIEW)
	{
		return kind == NV_OPEN_ASSOC_FILEVIEWER;
	}
	return 0;
}

static int
glob_pattern_valid(const char pattern[])
{
	size_t length;
	if(!bounded_length(pattern, NV_OPEN_MAX_PATTERN_BYTES, &length) ||
			length == 0U)
	{
		return 0;
	}
	for(size_t i = 0U; i < length; ++i)
	{
		const unsigned char character = (unsigned char)pattern[i];
		if(character < 0x20U || character == 0x7fU)
		{
			return 0;
		}
		if(character == '\\')
		{
			if(++i >= length)
			{
				return 0;
			}
			continue;
		}
		if(character != '[')
		{
			continue;
		}
		++i;
		if(i < length && (pattern[i] == '!' || pattern[i] == '^'))
		{
			++i;
		}
		if(i < length && pattern[i] == ']')
		{
			++i;
		}
		int closed = 0;
		for(; i < length; ++i)
		{
			if(pattern[i] == '\\')
			{
				if(++i >= length)
				{
					return 0;
				}
				continue;
			}
			if(pattern[i] == ']')
			{
				closed = 1;
				break;
			}
		}
		if(!closed)
		{
			return 0;
		}
	}
	return 1;
}

static int
glob_class_match(const char pattern[], size_t start, unsigned char character,
		size_t *next)
{
	size_t i = start + 1U;
	int negate = 0;
	int matched = 0;
	if(pattern[i] == '!' || pattern[i] == '^')
	{
		negate = 1;
		++i;
	}
	if(pattern[i] == ']')
	{
		matched = character == ']';
		++i;
	}
	while(pattern[i] != '\0' && pattern[i] != ']')
	{
		unsigned char first = (unsigned char)pattern[i++];
		if(first == '\\' && pattern[i] != '\0')
		{
			first = (unsigned char)pattern[i++];
		}
		if(pattern[i] == '-' && pattern[i + 1U] != '\0' &&
				pattern[i + 1U] != ']')
		{
			++i;
			unsigned char last = (unsigned char)pattern[i++];
			if(last == '\\' && pattern[i] != '\0')
			{
				last = (unsigned char)pattern[i++];
			}
			matched = matched || (first <= character && character <= last);
		}
		else
		{
			matched = matched || first == character;
		}
	}
	if(pattern[i] == ']')
	{
		++i;
	}
	*next = i - start;
	return negate ? !matched : matched;
}

static int
glob_matches(const char pattern[], const char value[])
{
	size_t pattern_index = 0U;
	size_t value_index = 0U;
	size_t star_index = (size_t)-1;
	size_t star_value_index = 0U;
	while(value[value_index] != '\0')
	{
		size_t consumed = 1U;
		int matched = 0;
		if(pattern[pattern_index] == '*')
		{
			star_index = pattern_index++;
			star_value_index = value_index;
			continue;
		}
		if(pattern[pattern_index] == '\\' &&
				pattern[pattern_index + 1U] != '\0')
		{
			matched = (unsigned char)pattern[pattern_index + 1U] ==
				(unsigned char)value[value_index];
			consumed = 2U;
		}
		else if(pattern[pattern_index] == '?')
		{
			matched = 1;
		}
		else if(pattern[pattern_index] == '[')
		{
			matched = glob_class_match(pattern, pattern_index,
					(unsigned char)value[value_index], &consumed);
		}
		else
		{
			matched = (unsigned char)pattern[pattern_index] ==
				(unsigned char)value[value_index];
		}
		if(matched)
		{
			pattern_index += consumed;
			++value_index;
			continue;
		}
		if(star_index != (size_t)-1)
		{
			pattern_index = star_index + 1U;
			value_index = ++star_value_index;
			continue;
		}
		return 0;
	}
	while(pattern[pattern_index] == '*')
	{
		++pattern_index;
	}
	return pattern[pattern_index] == '\0';
}

static int
append_token_character(char token[], size_t *length, unsigned char character)
{
	if(*length >= NV_OPEN_MAX_ARG_BYTES)
	{
		return -1;
	}
	token[(*length)++] = (char)character;
	token[*length] = '\0';
	return 0;
}

static int
expand_token(const char token[], const char path[], char output[],
		size_t *length, int *used_path_macro)
{
	for(size_t i = 0U; token[i] != '\0'; ++i)
	{
		if(token[i] != '%')
		{
			if(append_token_character(output, length,
					(unsigned char)token[i]) != 0)
			{
				return -1;
			}
			continue;
		}
		const char macro = token[++i];
		if(macro == '%')
		{
			if(append_token_character(output, length, '%') != 0)
			{
				return -1;
			}
		}
		else if(macro == 'f' || macro == 'c')
		{
			for(size_t j = 0U; path[j] != '\0'; ++j)
			{
				if(append_token_character(output, length,
						(unsigned char)path[j]) != 0)
				{
					return -1;
				}
			}
			*used_path_macro = 1;
		}
		else
		{
			return -2;
		}
	}
	return 0;
}

static int
parse_association_command(const char command[], const char path[], char **argv[],
		size_t *argc, nv_open_error_t *error)
{
	size_t command_length;
	if(!bounded_length(command, NV_OPEN_MAX_ARG_BYTES, &command_length) ||
			command_length == 0U || argv == NULL || argc == NULL)
	{
		return set_error(error, "invalid-association",
				"association command is empty or too long");
	}
	char **const result = calloc(NV_OPEN_MAX_ARGS + 1U, sizeof(*result));
	if(result == NULL)
	{
		return set_error(error, "out-of-memory",
				"failed to allocate association argv");
	}
	char token[NV_OPEN_MAX_ARG_BYTES + 1U];
	char expanded[NV_OPEN_MAX_ARG_BYTES + 1U];
	size_t result_count = 0U;
	size_t token_length = 0U;
	int token_started = 0;
	char quote = '\0';
	int used_path_macro = 0;
	for(size_t i = 0U; i <= command_length; ++i)
	{
		const unsigned char character = (unsigned char)command[i];
		if(character == '\n' || character == '\r')
		{
			goto invalid_command;
		}
		if(character == '\0' || (quote == '\0' && isspace(character)))
		{
			if(!token_started)
			{
				if(character == '\0') break;
				continue;
			}
			if(token_length == 0U)
			{
				goto invalid_command;
			}
			if(result_count >= NV_OPEN_MAX_ARGS - 1U)
			{
				goto too_many_arguments;
			}
			expanded[0] = '\0';
			size_t expanded_length = 0U;
			const int expansion = expand_token(token, path, expanded,
					&expanded_length, &used_path_macro);
			if(expansion == -2)
			{
				goto unsupported_macro;
			}
			if(expansion != 0 || expanded_length == 0U)
			{
				goto invalid_command;
			}
			result[result_count] = strdup(expanded);
			if(result[result_count] == NULL)
			{
				goto allocation_failed;
			}
			++result_count;
			token_length = 0U;
			token[0] = '\0';
			token_started = 0;
			if(character == '\0') break;
			continue;
		}
		if(quote == '\0' && (character == ';' || character == '|' ||
				character == '&' || character == '<' || character == '>' ||
				character == '`' || character == '$'))
		{
			goto shell_syntax;
		}
		if(character < 0x20U || character == 0x7fU)
		{
			goto invalid_command;
		}
		if(character == '\\')
		{
			if(command[i + 1U] == '\0') goto invalid_command;
			if(append_token_character(token, &token_length,
					(unsigned char)command[++i]) != 0)
			{
				goto invalid_command;
			}
			token_started = 1;
			continue;
		}
		if(quote != '\0')
		{
			if(character == (unsigned char)quote)
			{
				quote = '\0';
				token_started = 1;
				continue;
			}
		}
		else if(character == '\'' || character == '"')
		{
			quote = (char)character;
			token_started = 1;
			continue;
		}
		if(append_token_character(token, &token_length, character) != 0)
		{
			goto invalid_command;
		}
		token_started = 1;
	}
	if(quote != '\0' || result_count == 0U)
	{
		goto invalid_command;
	}
	if(!used_path_macro)
	{
		if(result_count >= NV_OPEN_MAX_ARGS - 1U ||
				(result[result_count] = strdup(path)) == NULL)
		{
			goto allocation_failed;
		}
		++result_count;
	}
	*argv = result;
	*argc = result_count;
	return 0;

shell_syntax:
	set_error(error, "shell-syntax", "shell operators are not allowed in associations");
	goto free_result;
unsupported_macro:
	set_error(error, "unsupported-macro", "association macro is not supported");
	goto free_result;
too_many_arguments:
	set_error(error, "association-too-large", "association has too many arguments");
	goto free_result;
allocation_failed:
	set_error(error, "out-of-memory", "failed to copy association argv");
	goto free_result;
invalid_command:
	set_error(error, "invalid-association", "association command is not a safe argv specification");
free_result:
	for(size_t i = 0U; i < result_count; ++i) free(result[i]);
	free(result);
	return -1;
}

static int
build_rule_resolution(nv_open_intent_t intent, const char path[],
		const nv_open_association_rule_t *rule,
		nv_open_resolution_t *resolution, nv_open_error_t *error)
{
	char **argv = NULL;
	size_t argc = 0U;
	if(parse_association_command(rule->command, path, &argv, &argc, error) != 0)
	{
		return -1;
	}
	resolution->intent = intent;
	resolution->source = NV_OPEN_SOURCE_ASSOCIATION;
	resolution->argv = argv;
	resolution->argc = argc;
	return 0;
}

int
nv_open_resolve_rules(nv_open_intent_t intent, const char path[],
		const nv_open_association_rule_t rules[], size_t rule_count,
		nv_open_resolution_t *resolution, nv_open_error_t *error)
{
	if(resolution == NULL || error == NULL)
	{
		return -1;
	}
	nv_open_resolution_free(resolution);
	nv_open_error_free(error);
	if(nv_open_intent_name(intent) == NULL)
	{
		return set_error(error, "invalid-intent", "open intent is invalid");
	}
	if(path == NULL || !argument_valid(path))
	{
		return set_error(error, "invalid-path", "open path is empty or too long");
	}
	if(rule_count > NV_OPEN_MAX_ASSOCIATIONS ||
			(rule_count != 0U && rules == NULL))
	{
		return set_error(error, "association-too-large",
				"too many association rules");
	}
	if(intent == NV_OPEN_INTENT_EDIT)
	{
		return set_error(error, "unsupported-intent",
				"edit associations are not available in this resolver");
	}
	for(size_t i = 0U; i < rule_count; ++i)
	{
		size_t command_length = 0U;
		if(!association_kind_valid(rules[i].kind) ||
				!glob_pattern_valid(rules[i].pattern) ||
				!bounded_length(rules[i].command, NV_OPEN_MAX_ARG_BYTES,
					&command_length) || command_length == 0U)
		{
			return set_error(error, "invalid-association",
					"association rule has an invalid pattern or command");
		}
		/* Vifm filetype patterns normally describe the file name.  Preserve
		 * explicit path patterns, but do not make a basename rule depend on the
		 * caller's absolute cwd prefix. */
		const char *const slash = strrchr(path, '/');
		const char *const match_value = strchr(rules[i].pattern, '/') == NULL &&
				slash != NULL ? slash + 1U : path;
		if(association_kind_accepts(intent, rules[i].kind) &&
				glob_matches(rules[i].pattern, match_value))
		{
			return build_rule_resolution(intent, path, &rules[i], resolution, error);
		}
	}
	if(intent == NV_OPEN_INTENT_OPEN)
	{
		return resolve_open_argv(path, NULL, 0U, resolution, error);
	}
	return set_error(error, "no-association",
			"no fileviewer rule matches the target");
}

void
nv_open_resolution_free(nv_open_resolution_t *resolution)
{
	if(resolution == NULL) return;
	for(size_t i = 0U; i < resolution->argc; ++i) free(resolution->argv[i]);
	free(resolution->argv);
	memset(resolution, 0, sizeof(*resolution));
}

void
nv_open_error_free(nv_open_error_t *error)
{
	if(error == NULL) return;
	free(error->code);
	free(error->message);
	memset(error, 0, sizeof(*error));
}

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 filetype=c : */
