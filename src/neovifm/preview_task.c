/* vifm
 * Copyright (C) 2026 NeoVifm contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef _WIN32
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#else
#include <io.h>
#include <windows.h>
#include <wchar.h>
#include "../utils/utf8.h"
#endif

#include "preview_task.h"
#include "../compat/pthread.h"

#if defined(_WIN32)
#define NV_PREVIEW_READ_FLAGS (O_RDONLY | O_BINARY)
#elif defined(O_NONBLOCK)
#define NV_PREVIEW_READ_FLAGS (O_RDONLY | O_NONBLOCK)
#else
#define NV_PREVIEW_READ_FLAGS O_RDONLY
#endif

typedef struct nv_preview_task_t nv_preview_task_t;
typedef struct nv_preview_event_node_t nv_preview_event_node_t;

struct nv_preview_task_t
{
	uint64_t id;
	nv_preview_request_t request;
	char *cwd;
	char *path;
	char *cwd_hex;
	char *path_hex;
	char **viewer_argv;
	size_t viewer_argc;
	int cancelled;
	int terminal_emitted;
	uint64_t deadline_ms;
	nv_preview_task_t *next;
};

struct nv_preview_event_node_t
{
	nv_preview_event_t event;
	nv_preview_event_node_t *next;
};

struct nv_preview_queue_t
{
	pthread_mutex_t mutex;
	pthread_cond_t ready;
	pthread_t worker;
	int started;
	int stopping;
	uint64_t next_id;
	nv_preview_task_t *tasks_head;
	nv_preview_task_t *tasks_tail;
	nv_preview_task_t *running_task;
	nv_preview_event_node_t *events_head;
	nv_preview_event_node_t *events_tail;
};

static int queue_start_worker(nv_preview_queue_t *queue);
static void *preview_worker(void *data);
static int request_valid(const nv_preview_request_t *request);
static int hex_decode(const char hex[], char **decoded);
static int hex_digit(char character);
static void task_free(nv_preview_task_t *task);
static void queue_event_locked(nv_preview_queue_t *queue,
		const nv_preview_task_t *task, nv_preview_task_state_t state,
		const char content[], int truncated, const char error_code[], int os_error);
static void cancel_task_locked(nv_preview_queue_t *queue, nv_preview_task_t *task);
static int task_cancelled(nv_preview_queue_t *queue, const nv_preview_task_t *task);
static int task_expired(const nv_preview_task_t *task);
static uint64_t now_ms(void);
static int preview_text(nv_preview_queue_t *queue, nv_preview_task_t *task,
		char **content, int *truncated, const char **error_code, int *os_error);
static int preview_pdf(nv_preview_queue_t *queue, nv_preview_task_t *task,
		char **content, int *truncated, const char **error_code, int *os_error);
static int preview_archive(nv_preview_queue_t *queue, nv_preview_task_t *task,
		char **content, int *truncated, const char **error_code, int *os_error);
static int preview_binary(nv_preview_queue_t *queue, nv_preview_task_t *task,
		char **content, int *truncated, const char **error_code, int *os_error);
static int preview_image(nv_preview_queue_t *queue, nv_preview_task_t *task,
		char **content, int *truncated, const char **error_code, int *os_error);
static int preview_chafa(nv_preview_queue_t *queue, nv_preview_task_t *task,
		char **content, int *truncated, const char **error_code, int *os_error);
static int preview_audio(nv_preview_queue_t *queue, nv_preview_task_t *task,
		char **content, int *truncated, const char **error_code, int *os_error);
static int preview_video(nv_preview_queue_t *queue, nv_preview_task_t *task,
		char **content, int *truncated, const char **error_code, int *os_error);
static int preview_directory(nv_preview_queue_t *queue, nv_preview_task_t *task,
		char **content, int *truncated, const char **error_code, int *os_error);
static int preview_builtin(nv_preview_queue_t *queue, nv_preview_task_t *task,
		char **content, int *truncated, const char **error_code, int *os_error);
static void sanitize_preview_text(char content[], size_t length);
#ifdef _WIN32
static wchar_t *
preview_wide_path(const char path[])
{
	wchar_t *wide = utf8_to_utf16(path);
	if(wide == NULL) return NULL;
	for(wchar_t *cursor = wide; *cursor != L'\0'; ++cursor)
		if(*cursor == L'/') *cursor = L'\\';
	const size_t length = wcslen(wide);
	if(length < MAX_PATH - 1U || wcsncmp(wide, L"\\\\?\\", 4U) == 0)
		return wide;
	const int drive = length >= 3U && wide[1] == L':' && wide[2] == L'\\';
	const int unc = length >= 2U && wide[0] == L'\\' && wide[1] == L'\\';
	if(!drive && !unc) return wide;
	const wchar_t prefix[] = L"\\\\?\\";
	const wchar_t unc_prefix[] = L"\\\\?\\UNC\\";
	const size_t prefix_length = unc ? 8U : 4U;
	const size_t skipped = unc ? 2U : 0U;
	wchar_t *const extended = malloc((prefix_length + length - skipped + 1U)*
			sizeof(*extended));
	if(extended == NULL)
	{
		free(wide);
		return NULL;
	}
	memcpy(extended, unc ? unc_prefix : prefix,
			prefix_length*sizeof(*extended));
	memcpy(extended + prefix_length, wide + skipped,
			(length - skipped + 1U)*sizeof(*extended));
	free(wide);
	return extended;
}

static int open_preview_file(const char path[], int flags)
{
	wchar_t *const wide_path = preview_wide_path(path);
	if(wide_path == NULL) return -1;
	const int descriptor = _wopen(wide_path, flags);
	free(wide_path);
	return descriptor;
}
#else
static int open_preview_file(const char path[], int flags)
{
	return open(path, flags);
}
#endif
#ifndef _WIN32
static int preview_external(nv_preview_queue_t *queue, nv_preview_task_t *task,
		const char helper[], char *const argv[], char **content,
		int *truncated, const char **error_code, int *os_error);
static int preview_chafa_path(nv_preview_queue_t *queue, nv_preview_task_t *task,
		const char image_path[], char **content, int *truncated,
		const char **error_code, int *os_error);
static int preview_pdf_frame(nv_preview_queue_t *queue, nv_preview_task_t *task,
		char **content, int *truncated, const char **error_code, int *os_error);
static int preview_video_frame(nv_preview_queue_t *queue, nv_preview_task_t *task,
		char **content, int *truncated, const char **error_code, int *os_error);
static int preview_ffprobe_metadata(nv_preview_queue_t *queue,
		nv_preview_task_t *task, char **content, int *truncated,
		const char **error_code, int *os_error);
#endif

int
nv_preview_hex_encode(const char input[], char output[], size_t output_size)
{
	static const char digits[] = "0123456789abcdef";
	if(input == NULL || output == NULL) return -1;
	const size_t length = strlen(input);
	if(length > NV_PREVIEW_MAX_PATH_BYTES || output_size < length*2U + 1U) return -1;
	for(size_t i = 0U; i < length; ++i)
	{
		const unsigned char value = (unsigned char)input[i];
		output[i*2U] = digits[value >> 4U];
		output[i*2U + 1U] = digits[value & 0x0fU];
	}
	output[length*2U] = '\0';
	return 0;
}

static int
hex_digit(char character)
{
	if(character >= '0' && character <= '9') return character - '0';
	if(character >= 'a' && character <= 'f') return character - 'a' + 10;
	if(character >= 'A' && character <= 'F') return character - 'A' + 10;
	return -1;
}

static int
hex_decode(const char hex[], char **decoded)
{
	if(hex == NULL || decoded == NULL) return -1;
	const size_t length = strlen(hex);
	if(length == 0U || length % 2U != 0U || length/2U > NV_PREVIEW_MAX_PATH_BYTES)
	{
		return -1;
	}
	char *const result = malloc(length/2U + 1U);
	if(result == NULL) return -1;
	for(size_t i = 0U; i < length; i += 2U)
	{
		const int high = hex_digit(hex[i]), low = hex_digit(hex[i + 1U]);
		if(high < 0 || low < 0 || (high == 0 && low == 0))
		{
			free(result);
			return -1;
		}
		result[i/2U] = (char)((high << 4U) | low);
	}
	result[length/2U] = '\0';
	*decoded = result;
	return 0;
}

static int
request_valid(const nv_preview_request_t *request)
{
	char *cwd = NULL, *path = NULL;
	int viewer_valid = request != NULL && request->viewer_argc <= NV_PREVIEW_MAX_VIEWER_ARGS &&
		(request->viewer_argc == 0U || request->viewer_argv != NULL);
	if(viewer_valid && request->viewer_argc != 0U)
	{
		for(size_t i = 0U; i < request->viewer_argc; ++i)
		{
			if(request->viewer_argv[i] == NULL || request->viewer_argv[i][0] == '\0')
			{
				viewer_valid = 0;
				break;
			}
			if(strlen(request->viewer_argv[i]) > NV_PREVIEW_MAX_VIEWER_ARG_BYTES)
			{
				viewer_valid = 0;
				break;
			}
		}
	}
	const int valid = request != NULL && viewer_valid && request->generation != 0U &&
			(request->pane == NV_PREVIEW_PANE_LEFT || request->pane == NV_PREVIEW_PANE_RIGHT) &&
			(request->has_target_pane == 0 || request->has_target_pane == 1) &&
			(!request->has_target_pane ||
			 (request->target_pane == NV_PREVIEW_PANE_LEFT ||
			  request->target_pane == NV_PREVIEW_PANE_RIGHT)) &&
			(request->kind == NV_PREVIEW_KIND_TEXT ||
			 request->kind == NV_PREVIEW_KIND_MARKDOWN ||
			 request->kind == NV_PREVIEW_KIND_PDF ||
			 request->kind == NV_PREVIEW_KIND_DIRECTORY ||
			 request->kind == NV_PREVIEW_KIND_ARCHIVE ||
			 request->kind == NV_PREVIEW_KIND_BINARY ||
			 request->kind == NV_PREVIEW_KIND_IMAGE ||
			 request->kind == NV_PREVIEW_KIND_AUDIO ||
			 request->kind == NV_PREVIEW_KIND_VIDEO) &&
			request->max_bytes != 0U && request->max_bytes <= NV_PREVIEW_MAX_BYTES &&
			request->timeout_ms != 0U && request->timeout_ms <= NV_PREVIEW_MAX_TIMEOUT_MS &&
			hex_decode(request->cwd_bytes_hex, &cwd) == 0 &&
			hex_decode(request->path_bytes_hex, &path) == 0;
	free(cwd);
	free(path);
	return valid;
}

static uint64_t
now_ms(void)
{
	struct timeval value;
	return gettimeofday(&value, NULL) == 0 ? (uint64_t)value.tv_sec*1000U +
		(uint64_t)value.tv_usec/1000U : 0U;
}

static int
task_expired(const nv_preview_task_t *task)
{
	return task->deadline_ms != 0U && now_ms() >= task->deadline_ms;
}

static size_t
valid_utf8_sequence_length(const unsigned char content[], size_t remaining)
{
	const unsigned char first = content[0];
	if(first >= 0xc2U && first <= 0xdfU)
		return remaining >= 2U && content[1] >= 0x80U && content[1] <= 0xbfU ? 2U : 0U;
	if(first == 0xe0U)
		return remaining >= 3U && content[1] >= 0xa0U && content[1] <= 0xbfU &&
			content[2] >= 0x80U && content[2] <= 0xbfU ? 3U : 0U;
	if((first >= 0xe1U && first <= 0xecU) || (first >= 0xeeU && first <= 0xefU))
		return remaining >= 3U && content[1] >= 0x80U && content[1] <= 0xbfU &&
			content[2] >= 0x80U && content[2] <= 0xbfU ? 3U : 0U;
	if(first == 0xedU)
		return remaining >= 3U && content[1] >= 0x80U && content[1] <= 0x9fU &&
			content[2] >= 0x80U && content[2] <= 0xbfU ? 3U : 0U;
	if(first == 0xf0U)
		return remaining >= 4U && content[1] >= 0x90U && content[1] <= 0xbfU &&
			content[2] >= 0x80U && content[2] <= 0xbfU &&
			content[3] >= 0x80U && content[3] <= 0xbfU ? 4U : 0U;
	if(first >= 0xf1U && first <= 0xf3U)
		return remaining >= 4U && content[1] >= 0x80U && content[1] <= 0xbfU &&
			content[2] >= 0x80U && content[2] <= 0xbfU &&
			content[3] >= 0x80U && content[3] <= 0xbfU ? 4U : 0U;
	if(first == 0xf4U)
		return remaining >= 4U && content[1] >= 0x80U && content[1] <= 0x8fU &&
			content[2] >= 0x80U && content[2] <= 0xbfU &&
			content[3] >= 0x80U && content[3] <= 0xbfU ? 4U : 0U;
	return 0U;
}

static void
sanitize_preview_text(char content[], size_t length)
{
	for(size_t i = 0U; i < length;)
	{
		const unsigned char value = (unsigned char)content[i];
		if(value == '\n' || value == '\r' || value == '\t' ||
			(value >= 0x20U && value <= 0x7eU))
		{
			++i;
			continue;
		}
		const size_t sequence_length = valid_utf8_sequence_length(
				(const unsigned char *)content + i, length - i);
		if(sequence_length != 0U)
		{
			i += sequence_length;
			continue;
		}
		content[i] = '?';
		++i;
	}
}

static nv_preview_queue_t *
queue_alloc(int start)
{
	nv_preview_queue_t *const queue = calloc(1U, sizeof(*queue));
	if(queue == NULL) return NULL;
	if(pthread_mutex_init(&queue->mutex, NULL) != 0 ||
			pthread_cond_init(&queue->ready, NULL) != 0)
	{
		pthread_mutex_destroy(&queue->mutex);
		free(queue);
		return NULL;
	}
	queue->next_id = 1U;
	if(start && queue_start_worker(queue) != 0)
	{
		nv_preview_queue_free(queue);
		return NULL;
	}
	return queue;
}

nv_preview_queue_t *
nv_preview_queue_alloc(void)
{
	return queue_alloc(1);
}

nv_preview_queue_t *
nv_preview_queue_alloc_paused(void)
{
	return queue_alloc(0);
}

static int
queue_start_worker(nv_preview_queue_t *queue)
{
	if(queue == NULL || queue->started || queue->stopping) return -1;
	if(pthread_create(&queue->worker, NULL, preview_worker, queue) != 0) return -1;
	queue->started = 1;
	return 0;
}

int
nv_preview_queue_start(nv_preview_queue_t *queue)
{
	if(queue == NULL) return -1;
	pthread_mutex_lock(&queue->mutex);
	const int result = queue_start_worker(queue);
	pthread_mutex_unlock(&queue->mutex);
	return result;
}

static void
task_free(nv_preview_task_t *task)
{
	if(task == NULL) return;
	free(task->cwd);
	free(task->path);
	for(size_t i = 0U; i < task->viewer_argc; ++i) free(task->viewer_argv[i]);
	free(task->viewer_argv);
	free(task->cwd_hex);
	free(task->path_hex);
	free(task);
}

void
nv_preview_event_free(nv_preview_event_t *event)
{
	if(event == NULL) return;
	free(event->cwd_bytes_hex);
	free(event->path_bytes_hex);
	free(event->content);
	free(event->error_code);
	*event = (nv_preview_event_t){};
}

static void
queue_event_locked(nv_preview_queue_t *queue, const nv_preview_task_t *task,
		nv_preview_task_state_t state, const char content[], int truncated,
		const char error_code[], int os_error)
{
	nv_preview_event_node_t *const node = calloc(1U, sizeof(*node));
	if(node == NULL) return;
	node->event = (nv_preview_event_t){
		.task_id = task->id,
		.generation = task->request.generation,
		.pane = task->request.pane,
		.target_pane = task->request.target_pane,
		.has_target_pane = 1,
		.kind = task->request.kind,
		.state = state,
		.cwd_bytes_hex = strdup(task->cwd_hex),
		.path_bytes_hex = strdup(task->path_hex),
		.content = content == NULL ? NULL : strdup(content),
		.error_code = error_code == NULL ? NULL : strdup(error_code),
		.os_error = os_error,
		.truncated = truncated,
	};
	if(node->event.cwd_bytes_hex == NULL || node->event.path_bytes_hex == NULL ||
			(content != NULL && node->event.content == NULL) ||
			(error_code != NULL && node->event.error_code == NULL))
	{
		nv_preview_event_free(&node->event);
		free(node);
		return;
	}
	if(queue->events_tail == NULL) queue->events_head = node;
	else queue->events_tail->next = node;
	queue->events_tail = node;
}

static void
cancel_task_locked(nv_preview_queue_t *queue, nv_preview_task_t *task)
{
	if(task->cancelled || task->terminal_emitted) return;
	task->cancelled = 1;
	task->terminal_emitted = 1;
	queue_event_locked(queue, task, NV_PREVIEW_TASK_CANCELLED, NULL, 0,
			"preview-cancelled", 0);
}

int
nv_preview_queue_submit(nv_preview_queue_t *queue,
		const nv_preview_request_t *request, uint64_t *task_id)
{
	if(queue == NULL || !request_valid(request)) return -1;
	nv_preview_task_t *const task = calloc(1U, sizeof(*task));
	if(task == NULL) return -1;
	task->request = *request;
	if(!task->request.has_target_pane)
	{
		task->request.target_pane = task->request.pane;
		task->request.has_target_pane = 1;
	}
	task->deadline_ms = now_ms() + request->timeout_ms;
	if(hex_decode(request->cwd_bytes_hex, &task->cwd) != 0 ||
			hex_decode(request->path_bytes_hex, &task->path) != 0 ||
			(task->cwd_hex = strdup(request->cwd_bytes_hex)) == NULL ||
			(task->path_hex = strdup(request->path_bytes_hex)) == NULL)
	{
		task_free(task);
		return -1;
	}
	if(request->viewer_argc != 0U)
	{
		task->viewer_argv = calloc(request->viewer_argc + 1U,
				sizeof(*task->viewer_argv));
		if(task->viewer_argv == NULL)
		{
			task_free(task);
			return -1;
		}
		task->viewer_argc = request->viewer_argc;
		for(size_t i = 0U; i < request->viewer_argc; ++i)
		{
			task->viewer_argv[i] = strdup(request->viewer_argv[i]);
			if(task->viewer_argv[i] == NULL)
			{
				task_free(task);
				return -1;
			}
		}
	}
	pthread_mutex_lock(&queue->mutex);
	if(queue->stopping)
	{
		pthread_mutex_unlock(&queue->mutex);
		task_free(task);
		return -1;
	}
	for(nv_preview_task_t *older = queue->tasks_head; older != NULL; older = older->next)
	{
		if(older->request.target_pane == task->request.target_pane &&
				older->request.generation < request->generation)
		{
			cancel_task_locked(queue, older);
		}
	}
	if(queue->running_task != NULL &&
			queue->running_task->request.target_pane == task->request.target_pane &&
			queue->running_task->request.generation < request->generation)
	{
		cancel_task_locked(queue, queue->running_task);
	}
	task->id = queue->next_id++;
	if(queue->tasks_tail == NULL) queue->tasks_head = task;
	else queue->tasks_tail->next = task;
	queue->tasks_tail = task;
	queue_event_locked(queue, task, NV_PREVIEW_TASK_QUEUED, NULL, 0, NULL, 0);
	pthread_cond_signal(&queue->ready);
	pthread_mutex_unlock(&queue->mutex);
	if(task_id != NULL) *task_id = task->id;
	return 0;
}

int
nv_preview_queue_pop(nv_preview_queue_t *queue, nv_preview_event_t *event)
{
	if(queue == NULL || event == NULL) return -1;
	pthread_mutex_lock(&queue->mutex);
	nv_preview_event_node_t *const node = queue->events_head;
	if(node != NULL)
	{
		queue->events_head = node->next;
		if(queue->events_head == NULL) queue->events_tail = NULL;
		*event = node->event;
		free(node);
	}
	pthread_mutex_unlock(&queue->mutex);
	return node == NULL ? 0 : 1;
}

static int
task_cancelled(nv_preview_queue_t *queue, const nv_preview_task_t *task)
{
	pthread_mutex_lock(&queue->mutex);
	const int cancelled = task->cancelled || queue->stopping;
	pthread_mutex_unlock(&queue->mutex);
	return cancelled;
}

static int
preview_text(nv_preview_queue_t *queue, nv_preview_task_t *task, char **content,
		int *truncated, const char **error_code, int *os_error)
{
	/* Do not let a FIFO/device selected by a stale snapshot block the only
	 * worker before fstat() rejects its non-regular type. */
	const int fd = open_preview_file(task->path, NV_PREVIEW_READ_FLAGS);
	if(fd < 0)
	{
		*error_code = "preview-open-failed";
		*os_error = errno;
		return -1;
	}
	struct stat st;
	if(fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
	{
		*error_code = "preview-not-regular-file";
		*os_error = errno;
		close(fd);
		return -1;
	}
	char *const result = malloc(task->request.max_bytes + 1U);
	if(result == NULL)
	{
		*error_code = "preview-out-of-memory";
		close(fd);
		return -1;
	}
	size_t used = 0U;
	while(used < task->request.max_bytes)
	{
		if(task_cancelled(queue, task))
		{
			free(result);
			close(fd);
			return 1;
		}
		if(task_expired(task))
		{
			*error_code = "preview-timeout";
			free(result);
			close(fd);
			return -1;
		}
		const ssize_t read_count = read(fd, result + used, task->request.max_bytes - used);
		if(read_count < 0)
		{
			if(errno == EINTR) continue;
			*error_code = "preview-read-failed";
			*os_error = errno;
			free(result);
			close(fd);
			return -1;
		}
		if(read_count == 0) break;
		used += (size_t)read_count;
	}
	if(used == task->request.max_bytes)
	{
		char extra;
		const ssize_t read_count = read(fd, &extra, 1U);
		*truncated = read_count > 0;
	}
	sanitize_preview_text(result, used);
	result[used] = '\0';
	close(fd);
	*content = result;
	return 0;
}

static int
preview_directory(nv_preview_queue_t *queue, nv_preview_task_t *task,
		char **content, int *truncated, const char **error_code, int *os_error)
{
	DIR *const directory = opendir(task->path);
	if(directory == NULL)
	{
		*error_code = "preview-open-failed";
		*os_error = errno;
		return -1;
	}
	char *const result = calloc(task->request.max_bytes + 1U, 1U);
	if(result == NULL)
	{
		*error_code = "preview-out-of-memory";
		closedir(directory);
		return -1;
	}
	size_t used = 0U;
	for(;;)
	{
		if(task_cancelled(queue, task))
		{
			free(result);
			closedir(directory);
			return 1;
		}
		if(task_expired(task))
		{
			*error_code = "preview-timeout";
			free(result);
			closedir(directory);
			return -1;
		}
		errno = 0;
		struct dirent *const entry = readdir(directory);
		if(entry == NULL)
		{
			if(errno != 0)
			{
				*error_code = "preview-read-failed";
				*os_error = errno;
				free(result);
				closedir(directory);
				return -1;
			}
			break;
		}
		if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
		const size_t name_length = strlen(entry->d_name);
		if(name_length + 1U > task->request.max_bytes - used)
		{
			*truncated = 1;
			break;
		}
		memcpy(result + used, entry->d_name, name_length);
		used += name_length;
		result[used++] = '\n';
	}
	closedir(directory);
	sanitize_preview_text(result, used);
	*content = result;
	return 0;
}

static int
preview_binary(nv_preview_queue_t *queue, nv_preview_task_t *task, char **content,
		int *truncated, const char **error_code, int *os_error)
{
	const int fd = open_preview_file(task->path, NV_PREVIEW_READ_FLAGS);
	if(fd < 0)
	{
		*error_code = "preview-open-failed";
		*os_error = errno;
		return -1;
	}
	struct stat st;
	if(fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
	{
		*error_code = "preview-not-regular-file";
		*os_error = errno;
		close(fd);
		return -1;
	}
	char *const result = malloc(task->request.max_bytes + 1U);
	if(result == NULL)
	{
		*error_code = "preview-out-of-memory";
		close(fd);
		return -1;
	}
	size_t used = 0U;
	size_t offset = 0U;
	for(;;)
	{
		if(task_cancelled(queue, task))
		{
			free(result);
			close(fd);
			return 1;
		}
		if(task_expired(task))
		{
			*error_code = "preview-timeout";
			free(result);
			close(fd);
			return -1;
		}
		unsigned char bytes[16];
		const ssize_t read_count = read(fd, bytes, sizeof(bytes));
		if(read_count < 0)
		{
			if(errno == EINTR) continue;
			*error_code = "preview-read-failed";
			*os_error = errno;
			free(result);
			close(fd);
			return -1;
		}
		if(read_count == 0) break;
		char line[128];
		size_t line_length = (size_t)snprintf(line, sizeof(line), "%08zx  ", offset);
		if(line_length >= sizeof(line))
		{
			*error_code = "preview-format-failed";
			free(result);
			close(fd);
			return -1;
		}
		for(size_t i = 0U; i < 16U; ++i)
		{
			const int written = snprintf(line + line_length, sizeof(line) - line_length,
					i < (size_t)read_count ? "%02x " : "   ",
				i < (size_t)read_count ? bytes[i] : 0U);
			if(written < 0 || (size_t)written >= sizeof(line) - line_length)
			{
				*error_code = "preview-format-failed";
				free(result);
				close(fd);
				return -1;
			}
			line_length += (size_t)written;
		}
		if(line_length + 2U >= sizeof(line))
		{
			*error_code = "preview-format-failed";
			free(result);
			close(fd);
			return -1;
		}
		line[line_length++] = ' ';
		line[line_length++] = '|';
		for(size_t i = 0U; i < (size_t)read_count; ++i)
		{
			const unsigned char value = bytes[i];
			line[line_length++] = value >= 0x20U && value <= 0x7eU ? (char)value : '.';
		}
		line[line_length++] = '|';
		line[line_length++] = '\n';
		if(line_length > task->request.max_bytes - used)
		{
			*truncated = 1;
			break;
		}
		memcpy(result + used, line, line_length);
		used += line_length;
		offset += (size_t)read_count;
	}
	close(fd);
	result[used] = '\0';
	*content = result;
	return 0;
}

static uint32_t
read_u32_be(const unsigned char bytes[])
{
	return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) |
		((uint32_t)bytes[2] << 8U) | (uint32_t)bytes[3];
}

static uint32_t
read_u32_le(const unsigned char bytes[])
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
		((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
}

static int
preview_header(nv_preview_queue_t *queue, nv_preview_task_t *task,
		unsigned char header[], size_t header_size, size_t *header_length,
		struct stat *stat_value, const char **error_code, int *os_error)
{
	const int fd = open_preview_file(task->path, NV_PREVIEW_READ_FLAGS);
	if(fd < 0)
	{
		*error_code = "preview-open-failed";
		*os_error = errno;
		return -1;
	}
	if(fstat(fd, stat_value) != 0 || !S_ISREG(stat_value->st_mode))
	{
		*error_code = "preview-not-regular-file";
		*os_error = errno;
		close(fd);
		return -1;
	}
	size_t used = 0U;
	while(used < header_size)
	{
		if(task_cancelled(queue, task))
		{
			close(fd);
			return 1;
		}
		if(task_expired(task))
		{
			*error_code = "preview-timeout";
			close(fd);
			return -1;
		}
		const ssize_t count = read(fd, header + used, header_size - used);
		if(count < 0)
		{
			if(errno == EINTR) continue;
			*error_code = "preview-read-failed";
			*os_error = errno;
			close(fd);
			return -1;
		}
		if(count == 0) break;
		used += (size_t)count;
	}
	close(fd);
	*header_length = used;
	return 0;
}

static const char *
image_format(const unsigned char header[], size_t length)
{
	if(length >= 8U && memcmp(header, "\x89PNG\r\n\x1a\n", 8U) == 0) return "PNG";
	if(length >= 6U && (memcmp(header, "GIF87a", 6U) == 0 ||
			memcmp(header, "GIF89a", 6U) == 0)) return "GIF";
	if(length >= 2U && header[0] == 0xffU && header[1] == 0xd8U) return "JPEG";
	if(length >= 12U && memcmp(header, "RIFF", 4U) == 0 &&
			memcmp(header + 8U, "WEBP", 4U) == 0) return "WebP";
	if(length >= 2U && memcmp(header, "BM", 2U) == 0) return "BMP";
	if(length >= 4U && ((header[0] == 'I' && header[1] == 'I' && header[2] == 42U && header[3] == 0U) ||
			(header[0] == 'M' && header[1] == 'M' && header[2] == 0U && header[3] == 42U))) return "TIFF";
	for(size_t i = 0U; i + 4U <= length; ++i)
	{
		if(memcmp(header + i, "<svg", 4U) == 0 || memcmp(header + i, "<SVG", 4U) == 0)
			return "SVG";
	}
	return "image";
}

static const char *
audio_format(const unsigned char header[], size_t length)
{
	if(length >= 12U && memcmp(header, "RIFF", 4U) == 0 &&
			memcmp(header + 8U, "WAVE", 4U) == 0) return "WAV";
	if(length >= 4U && memcmp(header, "fLaC", 4U) == 0) return "FLAC";
	if(length >= 3U && memcmp(header, "ID3", 3U) == 0) return "MP3";
	if(length >= 4U && memcmp(header, "OggS", 4U) == 0) return "Ogg";
	if(length >= 12U && memcmp(header, "FORM", 4U) == 0 &&
			(memcmp(header + 8U, "AIFF", 4U) == 0 || memcmp(header + 8U, "AIFC", 4U) == 0)) return "AIFF";
	return "audio";
}

static const char *
video_format(const unsigned char header[], size_t length)
{
	if(length >= 12U && memcmp(header + 4U, "ftyp", 4U) == 0) return "MP4/MOV";
	if(length >= 4U && memcmp(header, "\x1a\x45\xdf\xa3", 4U) == 0) return "WebM/Matroska";
	if(length >= 12U && memcmp(header, "RIFF", 4U) == 0 &&
			memcmp(header + 8U, "AVI ", 4U) == 0) return "AVI";
	if(length >= 4U && memcmp(header, "OggS", 4U) == 0) return "Ogg";
	return "video";
}

static int
preview_media_metadata(nv_preview_queue_t *queue, nv_preview_task_t *task,
		nv_preview_kind_t kind, char **content, int *truncated,
		const char **error_code, int *os_error)
{
	unsigned char header[256] = {};
	size_t header_length = 0U;
	struct stat stat_value = {};
	const int header_result = preview_header(queue, task, header, sizeof(header),
			&header_length, &stat_value, error_code, os_error);
	if(header_result != 0) return header_result;
	const char *format = kind == NV_PREVIEW_KIND_IMAGE ? image_format(header, header_length) :
		kind == NV_PREVIEW_KIND_AUDIO ? audio_format(header, header_length) :
		video_format(header, header_length);
	char metadata[512];
	int written = snprintf(metadata, sizeof(metadata),
			"%s metadata\nformat: %s\nbytes: %lld\n",
			kind == NV_PREVIEW_KIND_IMAGE ? "image" :
			kind == NV_PREVIEW_KIND_AUDIO ? "audio" : "video", format,
			(long long)stat_value.st_size);
	if(kind == NV_PREVIEW_KIND_IMAGE && written >= 0 && (size_t)written < sizeof(metadata))
	{
		uint32_t width = 0U, height = 0U;
		if(strcmp(format, "PNG") == 0 && header_length >= 24U)
		{
			width = read_u32_be(header + 16U);
			height = read_u32_be(header + 20U);
		}
		else if(strcmp(format, "GIF") == 0 && header_length >= 10U)
		{
			width = (uint32_t)header[6] | ((uint32_t)header[7] << 8U);
			height = (uint32_t)header[8] | ((uint32_t)header[9] << 8U);
		}
		else if(strcmp(format, "BMP") == 0 && header_length >= 26U)
		{
			width = read_u32_le(header + 18U);
			height = read_u32_le(header + 22U);
		}
		else if(strcmp(format, "WebP") == 0 && header_length >= 30U &&
				memcmp(header + 12U, "VP8X", 4U) == 0)
		{
			width = 1U + (uint32_t)header[24] + ((uint32_t)header[25] << 8U) +
				((uint32_t)header[26] << 16U);
			height = 1U + (uint32_t)header[27] + ((uint32_t)header[28] << 8U) +
				((uint32_t)header[29] << 16U);
		}
		if(width != 0U && height != 0U)
			written += snprintf(metadata + written, sizeof(metadata) - (size_t)written,
					"size: %ux%u\n", width, height);
	}
	if(written < 0 || (size_t)written >= sizeof(metadata))
	{
		*error_code = "preview-format-failed";
		return -1;
	}
	written += snprintf(metadata + written, sizeof(metadata) - (size_t)written,
			"mode: metadata-only; use an external viewer for full media rendering\n");
	if(written < 0)
	{
		*error_code = "preview-format-failed";
		return -1;
	}
	const size_t available = task->request.max_bytes;
	const size_t length = (size_t)written < available ? (size_t)written : available;
	char *const result = malloc(length + 1U);
	if(result == NULL)
	{
		*error_code = "preview-out-of-memory";
		return -1;
	}
	memcpy(result, metadata, length);
	result[length] = '\0';
	*truncated = (size_t)written > length;
	*content = result;
	return 0;
}

static int
preview_image(nv_preview_queue_t *queue, nv_preview_task_t *task, char **content,
		int *truncated, const char **error_code, int *os_error)
{
	const int rendered = preview_chafa(queue, task, content, truncated,
			error_code, os_error);
	if(rendered >= 0) return rendered;
	if(*error_code == NULL || strncmp(*error_code, "preview-helper-",
			strlen("preview-helper-")) != 0)
	{
		return rendered;
	}
	/* A helper is an optional capability.  Its absence or non-zero exit must
	 * not turn an otherwise readable image into a hard preview failure. */
	*error_code = NULL;
	*os_error = 0;
	*truncated = 0;
	return preview_media_metadata(queue, task, NV_PREVIEW_KIND_IMAGE, content,
			truncated, error_code, os_error);
}

static int
preview_audio(nv_preview_queue_t *queue, nv_preview_task_t *task, char **content,
		int *truncated, const char **error_code, int *os_error)
{
#ifndef _WIN32
	const int probed = preview_ffprobe_metadata(queue, task, content, truncated,
			error_code, os_error);
	if(probed >= 0) return probed;
	if(*error_code == NULL || strncmp(*error_code, "preview-helper-",
			strlen("preview-helper-")) != 0)
	{
		return probed;
	}
	*error_code = NULL;
	*os_error = 0;
	*truncated = 0;
#endif
	return preview_media_metadata(queue, task, NV_PREVIEW_KIND_AUDIO, content,
			truncated, error_code, os_error);
}

static int
preview_video(nv_preview_queue_t *queue, nv_preview_task_t *task, char **content,
		int *truncated, const char **error_code, int *os_error)
{
#ifndef _WIN32
	const int rendered = preview_video_frame(queue, task, content, truncated,
			error_code, os_error);
	if(rendered >= 0) return rendered;
	if(*error_code == NULL || strncmp(*error_code, "preview-helper-",
			strlen("preview-helper-")) != 0)
	{
		return rendered;
	}
	*error_code = NULL;
	*os_error = 0;
	*truncated = 0;
	const int probed = preview_ffprobe_metadata(queue, task, content, truncated,
			error_code, os_error);
	if(probed >= 0) return probed;
	if(*error_code == NULL || strncmp(*error_code, "preview-helper-",
			strlen("preview-helper-")) != 0)
	{
		return probed;
	}
	*error_code = NULL;
	*os_error = 0;
	*truncated = 0;
#endif
	return preview_media_metadata(queue, task, NV_PREVIEW_KIND_VIDEO, content,
			truncated, error_code, os_error);
}

#ifndef _WIN32
extern char **environ;

static int
preview_external(nv_preview_queue_t *queue, nv_preview_task_t *task,
		const char helper[], char *const argv[], char **content,
		int *truncated, const char **error_code, int *os_error)
{
	int output_pipe[2];
	if(pipe(output_pipe) != 0)
	{
		*error_code = "preview-helper-pipe-failed";
		*os_error = errno;
		return -1;
	}
	posix_spawn_file_actions_t actions;
	if(posix_spawn_file_actions_init(&actions) != 0 ||
			posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO) != 0 ||
			posix_spawn_file_actions_addclose(&actions, output_pipe[0]) != 0 ||
			posix_spawn_file_actions_addclose(&actions, output_pipe[1]) != 0)
	{
		close(output_pipe[0]);
		close(output_pipe[1]);
		*error_code = "preview-helper-setup-failed";
		*os_error = errno;
		return -1;
	}
	pid_t child = 0;
	/* Vifm fileviewer rules conventionally name helpers through PATH (for
	 * example, `zip` or `pdftotext`).  `posix_spawnp` preserves the shell-free
	 * argv boundary while retaining that native command lookup behavior. */
	const int spawned = posix_spawnp(&child, helper, &actions, NULL, argv, environ);
	(void)posix_spawn_file_actions_destroy(&actions);
	close(output_pipe[1]);
	if(spawned != 0)
	{
		close(output_pipe[0]);
		*error_code = "preview-helper-spawn-failed";
		*os_error = spawned;
		return -1;
	}
	const int flags = fcntl(output_pipe[0], F_GETFL, 0);
	if(flags < 0 || fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK) != 0)
	{
		(void)kill(child, SIGTERM);
		(void)waitpid(child, NULL, 0);
		close(output_pipe[0]);
		*error_code = "preview-helper-pipe-failed";
		*os_error = errno;
		return -1;
	}
	char *const result = malloc(task->request.max_bytes + 1U);
	if(result == NULL)
	{
		(void)kill(child, SIGTERM);
		(void)waitpid(child, NULL, 0);
		close(output_pipe[0]);
		*error_code = "preview-out-of-memory";
		return -1;
	}
	size_t used = 0U;
	int child_status = 0, child_done = 0, pipe_done = 0;
	while(!child_done || !pipe_done)
	{
		if(task_cancelled(queue, task))
		{
			(void)kill(child, SIGTERM);
			(void)waitpid(child, &child_status, 0);
			free(result);
			close(output_pipe[0]);
			return 1;
		}
		if(task_expired(task))
		{
			(void)kill(child, SIGTERM);
			(void)waitpid(child, &child_status, 0);
			free(result);
			close(output_pipe[0]);
			*error_code = "preview-timeout";
			return -1;
		}
		struct pollfd descriptor = { .fd = output_pipe[0], .events = POLLIN };
		(void)poll(&descriptor, 1U, 50);
		if(!pipe_done && (descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0)
		{
			char buffer[4096];
			for(;;)
			{
				const ssize_t count = read(output_pipe[0], buffer, sizeof(buffer));
				if(count > 0)
				{
					const size_t available = task->request.max_bytes - used;
					const size_t copied = (size_t)count < available ? (size_t)count : available;
					if(copied != 0U) memcpy(result + used, buffer, copied);
					used += copied;
					if((size_t)count > copied) *truncated = 1;
					if(used == task->request.max_bytes) *truncated = 1;
					continue;
				}
				if(count == 0) pipe_done = 1;
				if(count < 0 && errno != EAGAIN && errno != EINTR) pipe_done = 1;
				break;
			}
		}
		if(!child_done)
		{
			const pid_t waited = waitpid(child, &child_status, WNOHANG);
			if(waited == child) child_done = 1;
			else if(waited < 0)
			{
				pipe_done = 1;
				*error_code = "preview-helper-wait-failed";
				*os_error = errno;
			}
		}
	}
	close(output_pipe[0]);
	if(!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)
	{
		free(result);
		*error_code = "preview-helper-failed";
		*os_error = WIFEXITED(child_status) ? WEXITSTATUS(child_status) : ECHILD;
		return -1;
	}
	sanitize_preview_text(result, used);
	result[used] = '\0';
	*content = result;
	return 0;
}

static const char *
select_preview_helper(const char environment_name[],
		const char *const candidates[])
{
	const char *const configured = getenv(environment_name);
	if(configured != NULL && configured[0] == '/' && configured[1] != '\0' &&
			access(configured, X_OK) == 0)
	{
		return configured;
	}
	for(size_t i = 0U; candidates != NULL && candidates[i] != NULL; ++i)
	{
		if(access(candidates[i], X_OK) == 0) return candidates[i];
	}
	return NULL;
}

static const char *
select_chafa_helper(void)
{
	static const char *const candidates[] = {
		"/usr/local/bin/chafa", "/opt/homebrew/bin/chafa",
		"/usr/bin/chafa", "/bin/chafa", NULL,
	};
	return select_preview_helper("NEOVIFM_CHAFA_EXECUTABLE", candidates);
}

static int
preview_chafa_path(nv_preview_queue_t *queue, nv_preview_task_t *task,
		const char image_path[], char **content, int *truncated,
		const char **error_code, int *os_error)
{
	const char *const helper = select_chafa_helper();
	if(helper == NULL)
	{
		*error_code = "preview-helper-unavailable";
		*os_error = ENOENT;
		return -1;
	}
	/* Unicode block symbols and no colors keep this optional renderer inside
	 * the line-safe protocol while avoiding the dense '@' output of the ASCII
	 * symbol set.  Raw Kitty/Sixel/iTerm escape sequences are not allowed to
	 * cross the core/client boundary. */
	char *const argv[] = {
		(char *)helper, "--format", "symbols", "--symbols", "block",
		"--colors", "none", "--polite", "on", "--relative", "off",
		"--animate", "off", "--size", "40x16", (char *)image_path, NULL,
	};
	const int result = preview_external(queue, task, helper, argv, content, truncated,
			error_code, os_error);
	if(result == 0 && (content == NULL || *content == NULL || (*content)[0] == '\0'))
	{
		free(content == NULL ? NULL : *content);
		if(content != NULL) *content = NULL;
		*error_code = "preview-helper-failed";
		*os_error = EINVAL;
		return -1;
	}
	return result;
}

static int
preview_chafa(nv_preview_queue_t *queue, nv_preview_task_t *task, char **content,
		int *truncated, const char **error_code, int *os_error)
{
	return preview_chafa_path(queue, task, task->path, content, truncated,
			error_code, os_error);
}

static int
create_preview_image_path(char prefix[], size_t prefix_size, char image_path[],
		size_t image_path_size, const char **error_code, int *os_error)
{
	const char *base = getenv("TMPDIR");
	if(base == NULL || base[0] != '/' || strlen(base) > NV_PREVIEW_MAX_PATH_BYTES/2U)
		base = "/tmp";
	const size_t base_length = strlen(base);
	const int separator = base_length != 0U && base[base_length - 1U] != '/';
	const int written = snprintf(prefix, prefix_size, "%s%sneovifm-preview-XXXXXX",
			base, separator ? "/" : "");
	if(written < 0 || (size_t)written >= prefix_size)
	{
		*error_code = "preview-helper-temp-failed";
		*os_error = ENAMETOOLONG;
		return -1;
	}
	const int fd = mkstemp(prefix);
	if(fd < 0)
	{
		*error_code = "preview-helper-temp-failed";
		*os_error = errno;
		return -1;
	}
	const int close_error = close(fd);
	const int unlink_error = unlink(prefix);
	if(close_error != 0 || unlink_error != 0)
	{
		const int error = errno;
		(void)unlink(prefix);
		*error_code = "preview-helper-temp-failed";
		*os_error = error;
		return -1;
	}
	const int image_written = snprintf(image_path, image_path_size, "%s.png", prefix);
	if(image_written < 0 || (size_t)image_written >= image_path_size)
	{
		*error_code = "preview-helper-temp-failed";
		*os_error = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

static void
cleanup_preview_image(const char prefix[], const char image_path[])
{
	if(prefix != NULL && prefix[0] != '\0') (void)unlink(prefix);
	if(image_path != NULL && image_path[0] != '\0') (void)unlink(image_path);
}

static int
preview_pdf_frame(nv_preview_queue_t *queue, nv_preview_task_t *task, char **content,
		int *truncated, const char **error_code, int *os_error)
{
	static const char *const candidates[] = {
		"/usr/local/bin/pdftoppm", "/opt/homebrew/bin/pdftoppm",
		"/usr/bin/pdftoppm", NULL,
	};
	const char *const raster = select_preview_helper("NEOVIFM_PDF_RENDER_EXECUTABLE",
			candidates);
	if(raster == NULL || select_chafa_helper() == NULL)
	{
		*error_code = "preview-helper-unavailable";
		*os_error = ENOENT;
		return -1;
	}
	char prefix[NV_PREVIEW_MAX_PATH_BYTES + 1U] = {};
	char image_path[NV_PREVIEW_MAX_PATH_BYTES + 8U] = {};
	if(create_preview_image_path(prefix, sizeof(prefix), image_path,
			sizeof(image_path), error_code, os_error) != 0)
		return -1;
	char *const argv[] = {
		(char *)raster, "-f", "1", "-l", "1", "-singlefile", "-png",
		"-r", "72", "-scale-to", "1200",
		task->path, prefix, NULL,
	};
	char *raster_output = NULL;
	int raster_truncated = 0;
	int result = preview_external(queue, task, raster, argv, &raster_output,
			&raster_truncated, error_code, os_error);
	free(raster_output);
	if(result == 0 && access(image_path, R_OK) == 0)
	{
		result = preview_chafa_path(queue, task, image_path, content, truncated,
				error_code, os_error);
	}
	else if(result == 0)
	{
		*error_code = "preview-helper-failed";
		*os_error = ENOENT;
		result = -1;
	}
	cleanup_preview_image(prefix, image_path);
	return result;
}

static int
preview_video_frame(nv_preview_queue_t *queue, nv_preview_task_t *task, char **content,
		int *truncated, const char **error_code, int *os_error)
{
	static const char *const candidates[] = {
		"/usr/local/bin/ffmpeg", "/opt/homebrew/bin/ffmpeg",
		"/usr/bin/ffmpeg", NULL,
	};
	const char *const ffmpeg = select_preview_helper("NEOVIFM_FFMPEG_EXECUTABLE",
			candidates);
	if(ffmpeg == NULL || select_chafa_helper() == NULL)
	{
		*error_code = "preview-helper-unavailable";
		*os_error = ENOENT;
		return -1;
	}
	char prefix[NV_PREVIEW_MAX_PATH_BYTES + 1U] = {};
	char image_path[NV_PREVIEW_MAX_PATH_BYTES + 8U] = {};
	if(create_preview_image_path(prefix, sizeof(prefix), image_path,
			sizeof(image_path), error_code, os_error) != 0)
		return -1;
	char *const argv[] = {
		(char *)ffmpeg, "-v", "error", "-y", "-i", task->path,
		"-frames:v", "1", "-f", "image2", image_path, NULL,
	};
	char *ffmpeg_output = NULL;
	int ffmpeg_truncated = 0;
	int result = preview_external(queue, task, ffmpeg, argv, &ffmpeg_output,
			&ffmpeg_truncated, error_code, os_error);
	free(ffmpeg_output);
	if(result == 0 && access(image_path, R_OK) == 0)
	{
		result = preview_chafa_path(queue, task, image_path, content, truncated,
				error_code, os_error);
	}
	else if(result == 0)
	{
		*error_code = "preview-helper-failed";
		*os_error = ENOENT;
		result = -1;
	}
	cleanup_preview_image(prefix, image_path);
	return result;
}

static int
preview_ffprobe_metadata(nv_preview_queue_t *queue, nv_preview_task_t *task,
		char **content, int *truncated, const char **error_code, int *os_error)
{
	static const char *const candidates[] = {
		"/usr/local/bin/ffprobe", "/opt/homebrew/bin/ffprobe",
		"/usr/bin/ffprobe", NULL,
	};
	const char *const helper = select_preview_helper("NEOVIFM_FFPROBE_EXECUTABLE",
			candidates);
	if(helper == NULL)
	{
		*error_code = "preview-helper-unavailable";
		*os_error = ENOENT;
		return -1;
	}
	char *const argv[] = {
		(char *)helper, "-v", "error", "-show_entries",
		"format=format_name,duration:stream=index,codec_type,codec_name,width,height,sample_rate,channels",
		"-of", "default=noprint_wrappers=1:nokey=0", task->path, NULL,
	};
	const int result = preview_external(queue, task, helper, argv, content, truncated,
			error_code, os_error);
	if(result == 0 && (content == NULL || *content == NULL || (*content)[0] == '\0'))
	{
		free(content == NULL ? NULL : *content);
		if(content != NULL) *content = NULL;
		*error_code = "preview-helper-failed";
		*os_error = EINVAL;
		return -1;
	}
	return result;
}

static int
preview_pdf_text(nv_preview_queue_t *queue, nv_preview_task_t *task, char **content,
		int *truncated, const char **error_code, int *os_error)
{
	static const char *const candidates[] = {
		"/usr/local/bin/pdftotext", "/opt/homebrew/bin/pdftotext",
		"/usr/bin/pdftotext", NULL,
	};
	const char *const helper = select_preview_helper("NEOVIFM_PDF_TEXT_EXECUTABLE",
			candidates);
	if(helper == NULL)
	{
		*error_code = "preview-helper-unavailable";
		*os_error = ENOENT;
		return -1;
	}
	char *const argv[] = {
		(char *)helper, "-f", "1", "-l", "1", task->path, "-", NULL,
	};
	return preview_external(queue, task, helper, argv, content, truncated,
			error_code, os_error);
}

static int
preview_pdf(nv_preview_queue_t *queue, nv_preview_task_t *task, char **content,
		int *truncated, const char **error_code, int *os_error)
{
	const int rendered = preview_pdf_frame(queue, task, content, truncated,
			error_code, os_error);
	if(rendered >= 0) return rendered;
	if(*error_code == NULL || strncmp(*error_code, "preview-helper-",
			strlen("preview-helper-")) != 0)
	{
		return rendered;
	}
	*error_code = NULL;
	*os_error = 0;
	*truncated = 0;
	return preview_pdf_text(queue, task, content, truncated, error_code, os_error);
}

static int
archive_path_is_zip(const char path[])
{
	const size_t length = path == NULL ? 0U : strlen(path);
	if(length < 4U) return 0;
	const char *const suffix = path + length - 4U;
	return tolower((unsigned char)suffix[0]) == '.' &&
		tolower((unsigned char)suffix[1]) == 'z' &&
		tolower((unsigned char)suffix[2]) == 'i' &&
		tolower((unsigned char)suffix[3]) == 'p';
}

static int
preview_archive(nv_preview_queue_t *queue, nv_preview_task_t *task, char **content,
		int *truncated, const char **error_code, int *os_error)
{
	const char *helper = NULL;
	const char *first = NULL;
	const char *second = NULL;
	const char *const *common = NULL;
	if(archive_path_is_zip(task->path))
	{
		first = "/usr/local/bin/unzip";
		second = "/opt/homebrew/bin/unzip";
		static const char *const zip_common[] = {
			"/usr/bin/unzip", "/bin/unzip", "/usr/bin/bsdtar", "/bin/bsdtar",
		};
		common = zip_common;
	}
	else
	{
		first = "/usr/local/bin/bsdtar";
		second = "/opt/homebrew/bin/bsdtar";
		static const char *const tar_common[] = {
			"/usr/bin/bsdtar", "/usr/bin/tar", "/bin/bsdtar", "/bin/tar",
		};
		common = tar_common;
	}
	const char *const candidates[] = { first, second, common[0], common[1], common[2], common[3] };
	for(size_t i = 0U; i < sizeof(candidates)/sizeof(candidates[0]); ++i)
	{
		if(access(candidates[i], X_OK) == 0)
		{
			helper = candidates[i];
			break;
		}
	}
	if(helper == NULL)
	{
		*error_code = "preview-helper-unavailable";
		*os_error = ENOENT;
		return -1;
	}
	char *argv[4] = { (char *)helper, NULL, task->path, NULL };
	argv[1] = archive_path_is_zip(task->path) && strstr(helper, "unzip") != NULL
		? "-Z1" : "-tf";
	return preview_external(queue, task, helper, argv, content, truncated,
			error_code, os_error);
}
#else
static int
preview_chafa(nv_preview_queue_t *queue, nv_preview_task_t *task, char **content,
		int *truncated, const char **error_code, int *os_error)
{
	(void)queue; (void)task; (void)content; (void)truncated;
	*error_code = "preview-helper-unavailable";
	*os_error = ENOSYS;
	return -1;
}

static int
preview_pdf(nv_preview_queue_t *queue, nv_preview_task_t *task, char **content,
		int *truncated, const char **error_code, int *os_error)
{
	(void)queue; (void)task; (void)content; (void)truncated;
	*error_code = "preview-helper-unavailable";
	*os_error = ENOSYS;
	return -1;
}

static int
preview_archive(nv_preview_queue_t *queue, nv_preview_task_t *task, char **content,
		int *truncated, const char **error_code, int *os_error)
{
	(void)queue; (void)task; (void)content; (void)truncated;
	*error_code = "preview-helper-unavailable";
	*os_error = ENOSYS;
	return -1;
}
#endif

static int
preview_builtin(nv_preview_queue_t *queue, nv_preview_task_t *task,
		char **content, int *truncated, const char **error_code, int *os_error)
{
	switch(task->request.kind)
	{
		case NV_PREVIEW_KIND_DIRECTORY:
			return preview_directory(queue, task, content, truncated, error_code, os_error);
		case NV_PREVIEW_KIND_ARCHIVE:
			return preview_archive(queue, task, content, truncated, error_code, os_error);
		case NV_PREVIEW_KIND_BINARY:
			return preview_binary(queue, task, content, truncated, error_code, os_error);
		case NV_PREVIEW_KIND_IMAGE:
			return preview_image(queue, task, content, truncated, error_code, os_error);
		case NV_PREVIEW_KIND_AUDIO:
			return preview_audio(queue, task, content, truncated, error_code, os_error);
		case NV_PREVIEW_KIND_VIDEO:
			return preview_video(queue, task, content, truncated, error_code, os_error);
		case NV_PREVIEW_KIND_PDF:
			return preview_pdf(queue, task, content, truncated, error_code, os_error);
		case NV_PREVIEW_KIND_TEXT:
		case NV_PREVIEW_KIND_MARKDOWN:
			return preview_text(queue, task, content, truncated, error_code, os_error);
	}
	*error_code = "preview-kind-invalid";
	return -1;
}

#ifndef _WIN32
static int
preview_content_blank(const char content[])
{
	if(content == NULL) return 1;
	for(size_t i = 0U; content[i] != '\0'; ++i)
	{
		if(content[i] != ' ' && content[i] != '\n' && content[i] != '\r' &&
				content[i] != '\t')
			return 0;
	}
	return 1;
}

static int
preview_content_metadata_only(const char content[])
{
	return content == NULL || strstr(content, "mode: metadata-only") != NULL;
}

static int
preview_helper_error(const char error_code[])
{
	return error_code != NULL && strncmp(error_code, "preview-helper-",
			strlen("preview-helper-")) == 0;
}

static int
preview_media_can_fallback_from_viewer(nv_preview_kind_t kind)
{
	return kind == NV_PREVIEW_KIND_IMAGE || kind == NV_PREVIEW_KIND_AUDIO ||
		kind == NV_PREVIEW_KIND_VIDEO || kind == NV_PREVIEW_KIND_PDF;
}
#endif

static void *
preview_worker(void *data)
{
	nv_preview_queue_t *const queue = data;
	for(;;)
	{
		pthread_mutex_lock(&queue->mutex);
		while(queue->tasks_head == NULL && !queue->stopping)
		{
			pthread_cond_wait(&queue->ready, &queue->mutex);
		}
		if(queue->stopping)
		{
			pthread_mutex_unlock(&queue->mutex);
			break;
		}
		nv_preview_task_t *const task = queue->tasks_head;
		queue->tasks_head = task->next;
		if(queue->tasks_head == NULL) queue->tasks_tail = NULL;
		task->next = NULL;
		queue->running_task = task;
		if(!task->cancelled) queue_event_locked(queue, task,
				NV_PREVIEW_TASK_RUNNING, NULL, 0, NULL, 0);
		pthread_mutex_unlock(&queue->mutex);

		char *content = NULL;
		int truncated = 0, os_error = 0;
		const char *error_code = NULL;
		int outcome = task->cancelled ? 1 : 0;
#ifndef _WIN32
		if(!task->cancelled && task->viewer_argc != 0U)
		{
			/* Common Vifm image associations invoke `convert -identify` or a
			 * desktop viewer.  Those commands produce useful metadata but not
			 * terminal pixels, so prefer the line-safe built-in renderer when it
			 * can provide an actual chafa frame. */
			if(task->request.kind == NV_PREVIEW_KIND_IMAGE)
			{
				outcome = preview_builtin(queue, task, &content, &truncated,
						&error_code, &os_error);
				const int builtin_visual = outcome == 0 &&
						!preview_content_blank(content) &&
						!preview_content_metadata_only(content);
				if(!builtin_visual)
				{
					free(content);
					content = NULL;
					truncated = 0;
					if(outcome >= 0 || preview_helper_error(error_code))
					{
						error_code = NULL;
						os_error = 0;
					}
					if(outcome >= 0 || error_code == NULL ||
							strcmp(error_code, "preview-timeout") != 0)
						outcome = preview_external(queue, task, task->viewer_argv[0],
								task->viewer_argv, &content, &truncated, &error_code,
								&os_error);
				}
			}
			else
			{
				outcome = preview_external(queue, task, task->viewer_argv[0],
						task->viewer_argv, &content, &truncated, &error_code, &os_error);
			}
			if(preview_media_can_fallback_from_viewer(task->request.kind) &&
					((outcome < 0 && preview_helper_error(error_code)) ||
					 (outcome == 0 && preview_content_blank(content))))
			{
				free(content);
				content = NULL;
				truncated = 0;
				error_code = NULL;
				os_error = 0;
				outcome = preview_builtin(queue, task, &content, &truncated,
						&error_code, &os_error);
			}
		}
		else if(!task->cancelled)
		{
			outcome = preview_builtin(queue, task, &content, &truncated,
					&error_code, &os_error);
		}
#else
		if(!task->cancelled)
			outcome = preview_builtin(queue, task, &content, &truncated,
					&error_code, &os_error);
#endif
		if(outcome == 0 && task_expired(task))
		{
			free(content);
			content = NULL;
			outcome = -1;
			error_code = "preview-timeout";
		}
		pthread_mutex_lock(&queue->mutex);
		if(!task->terminal_emitted)
		{
			task->terminal_emitted = 1;
			if(task->cancelled || outcome > 0)
			{
				queue_event_locked(queue, task, NV_PREVIEW_TASK_CANCELLED, NULL, 0,
						"preview-cancelled", 0);
			}
			else if(outcome == 0)
			{
				queue_event_locked(queue, task, NV_PREVIEW_TASK_DONE, content, truncated,
						NULL, 0);
			}
			else
			{
				queue_event_locked(queue, task, NV_PREVIEW_TASK_FAILED, NULL, 0,
						error_code == NULL ? "preview-failed" : error_code, os_error);
			}
		}
		queue->running_task = NULL;
		pthread_mutex_unlock(&queue->mutex);
		free(content);
		task_free(task);
	}
	return NULL;
}

void
nv_preview_queue_free(nv_preview_queue_t *queue)
{
	if(queue == NULL) return;
	pthread_mutex_lock(&queue->mutex);
	queue->stopping = 1;
	pthread_cond_broadcast(&queue->ready);
	pthread_mutex_unlock(&queue->mutex);
	if(queue->started) pthread_join(queue->worker, NULL);
	while(queue->tasks_head != NULL)
	{
		nv_preview_task_t *const next = queue->tasks_head->next;
		task_free(queue->tasks_head);
		queue->tasks_head = next;
	}
	while(queue->events_head != NULL)
	{
		nv_preview_event_node_t *const next = queue->events_head->next;
		nv_preview_event_free(&queue->events_head->event);
		free(queue->events_head);
		queue->events_head = next;
	}
	pthread_cond_destroy(&queue->ready);
	pthread_mutex_destroy(&queue->mutex);
	free(queue);
}

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 filetype=c : */
