/* vifm
 * Copyright (C) 2026 NeoVifm contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "action_task.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <sys/time.h>
#else
#include <objbase.h>
#endif

#include "../compat/pthread.h"

#define NV_ACTION_EVENT_LIMIT 256U
#define NV_ACTION_QUEUE_LIMIT 64U

typedef struct nv_action_task_t nv_action_task_t;
typedef struct nv_action_event_node_t nv_action_event_node_t;

struct nv_action_task_t
{
	uint64_t id;
	unsigned int command_sequence;
	nv_session_prepared_action_t action;
	int cancelled;
	uint64_t started_at_unix_ms;
	uint64_t finished_at_unix_ms;
	nv_action_task_t *next;
};

struct nv_action_event_node_t
{
	nv_action_event_t event;
	nv_action_event_node_t *next;
};

struct nv_action_queue_t
{
	pthread_mutex_t mutex;
	pthread_cond_t ready;
	pthread_t worker;
	int started;
	int stopping;
	int terminal_event_lost;
	uint64_t next_id;
	nv_action_task_t *pending;
	nv_action_task_t *pending_tail;
	size_t pending_count;
	nv_action_task_t *running;
	nv_action_task_t *finishing;
	nv_action_task_t *finishing_tail;
	size_t task_count;
	nv_action_event_node_t *events_head;
	nv_action_event_node_t *events_tail;
	size_t event_count;
};

static void *action_worker(void *data);

static uint64_t
now_unix_ms(void)
{
#ifndef _WIN32
	struct timeval value;
	if(gettimeofday(&value, NULL) == 0)
		return (uint64_t)value.tv_sec*1000U + (uint64_t)value.tv_usec/1000U;
#endif
	const time_t seconds = time(NULL);
	return seconds < 0 ? 0U : (uint64_t)seconds*1000U;
}

static int
action_kind_valid(nv_session_command_kind_t kind)
{
	return kind == NV_SESSION_COPY || kind == NV_SESSION_MOVE_FILES ||
		kind == NV_SESSION_MKDIR || kind == NV_SESSION_DELETE ||
		kind == NV_SESSION_RENAME;
}

static size_t
action_total(const nv_session_prepared_action_t *action)
{
	return action->kind == NV_SESSION_MKDIR ? 1U : action->target_count;
}

static int
action_valid(const nv_session_prepared_action_t *action)
{
	if(action == NULL || !action_kind_valid(action->kind) ||
			(action->pane != NV_SESSION_LEFT && action->pane != NV_SESSION_RIGHT) ||
			action->source_directory == NULL) return 0;
	if(action->kind == NV_SESSION_MKDIR) return action->name != NULL;
	if(action->kind == NV_SESSION_RENAME && action->name == NULL) return 0;
	if(action->targets == NULL || action->target_count == 0U ||
			action->target_count > NV_SESSION_MAX_ACTION_PATHS) return 0;
	if((action->kind == NV_SESSION_COPY || action->kind == NV_SESSION_MOVE_FILES ||
			action->kind == NV_SESSION_RENAME) &&
			action->destination_directory == NULL) return 0;
	for(size_t i = 0U; i < action->target_count; ++i)
	{
		if(action->targets[i].path == NULL || action->targets[i].name == NULL)
			return 0;
	}
	return 1;
}

static void
task_free(nv_action_task_t *task)
{
	if(task == NULL) return;
	nv_session_prepared_action_free(&task->action);
	free(task);
}

void
nv_action_event_free(nv_action_event_t *event)
{
	if(event == NULL) return;
	free(event->source_path_bytes_hex);
	free(event->destination_path_bytes_hex);
	free(event->current_path_bytes_hex);
	free(event->error_code);
	*event = (nv_action_event_t){};
}

static char *
path_hex_dup(const char path[])
{
	if(path == NULL) return NULL;
	const size_t length = strlen(path);
	if(length > NV_PANE_SNAPSHOT_MAX_HEX_BYTES/2U ||
			length > (SIZE_MAX - 1U)/2U)
	{
		errno = EOVERFLOW;
		return NULL;
	}
	static const char digits[] = "0123456789abcdef";
	char *const result = malloc(length*2U + 1U);
	if(result == NULL) { errno = ENOMEM; return NULL; }
	for(size_t i = 0U; i < length; ++i)
	{
		const unsigned char byte = (unsigned char)path[i];
		result[i*2U] = digits[byte >> 4U];
		result[i*2U + 1U] = digits[byte & 0x0fU];
	}
	result[length*2U] = '\0';
	return result;
}

static uint64_t
action_total_bytes(const nv_session_prepared_action_t *action, int *known)
{
	if(known != NULL) *known = 0;
	if(action == NULL || (action->kind != NV_SESSION_COPY &&
			action->kind != NV_SESSION_MOVE_FILES)) return 0U;
	uint64_t total = 0U;
	for(size_t i = 0U; i < action->target_count; ++i)
	{
		if(!action->targets[i].size_known ||
				total > UINT64_MAX - action->targets[i].size_bytes) return 0U;
		total += action->targets[i].size_bytes;
	}
	if(known != NULL) *known = 1;
	return total;
}

static int
queue_event_locked(nv_action_queue_t *queue, const nv_action_task_t *task,
		nv_action_task_state_t state, size_t completed_count,
		uint64_t completed_bytes, const char current_path[],
		int has_failed_index, size_t failed_index, const char error_code[],
		int os_error, int partial)
{
	if(queue->event_count >= NV_ACTION_EVENT_LIMIT) return -1;
	nv_action_event_node_t *const node = calloc(1U, sizeof(*node));
	if(node == NULL) return -1;
	int bytes_known = 0;
	const uint64_t total_bytes = action_total_bytes(&task->action, &bytes_known);
	node->event = (nv_action_event_t){
		.task_id = task->id,
		.command_sequence = task->command_sequence,
		.kind = task->action.kind,
		.pane = task->action.pane,
		.state = state,
		.completed_count = completed_count,
		.total_count = action_total(&task->action),
		.failed_index = failed_index,
		.has_failed_index = has_failed_index,
		.partial = partial,
		.retryable = state == NV_ACTION_TASK_FAILED ||
			state == NV_ACTION_TASK_CANCELLED ?
			(task->action.kind != NV_SESSION_MKDIR) : 0,
		.bytes_completed = bytes_known ? completed_bytes : 0U,
		.bytes_total = bytes_known ? total_bytes : 0U,
		.bytes_known = bytes_known,
		.started_at_unix_ms = task->started_at_unix_ms,
		.finished_at_unix_ms = task->finished_at_unix_ms,
		.error_code = error_code == NULL ? NULL : strdup(error_code),
		.os_error = os_error,
	};
	node->event.source_path_bytes_hex = path_hex_dup(task->action.source_directory);
	node->event.destination_path_bytes_hex = path_hex_dup(
		task->action.destination_directory);
	node->event.current_path_bytes_hex = path_hex_dup(current_path);
	if((task->action.source_directory != NULL &&
				node->event.source_path_bytes_hex == NULL) ||
			(task->action.destination_directory != NULL &&
				node->event.destination_path_bytes_hex == NULL) ||
			(current_path != NULL && node->event.current_path_bytes_hex == NULL) ||
			(error_code != NULL && node->event.error_code == NULL))
	{
		nv_action_event_free(&node->event);
		free(node);
		return -1;
	}
	if(queue->events_tail == NULL) queue->events_head = node;
	else queue->events_tail->next = node;
	queue->events_tail = node;
	++queue->event_count;
	return 0;
}

static void
publish_action_progress(nv_action_queue_t *queue, const nv_action_task_t *task,
		size_t completed_count, uint64_t completed_bytes,
		const char current_path[])
{
	pthread_mutex_lock(&queue->mutex);
	if(queue_event_locked(queue, task, NV_ACTION_TASK_RUNNING, completed_count,
			completed_bytes, current_path, 0, 0U, NULL, 0, 0) != 0)
		queue->terminal_event_lost = 1;
	pthread_mutex_unlock(&queue->mutex);
}

static nv_action_queue_t *
queue_alloc(int start)
{
	nv_action_queue_t *const queue = calloc(1U, sizeof(*queue));
	if(queue == NULL) return NULL;
	if(pthread_mutex_init(&queue->mutex, NULL) != 0 ||
			pthread_cond_init(&queue->ready, NULL) != 0)
	{
		pthread_mutex_destroy(&queue->mutex);
		free(queue);
		return NULL;
	}
	queue->next_id = 1U;
	if(start && nv_action_queue_start(queue) != 0)
	{
		nv_action_queue_free(queue);
		return NULL;
	}
	return queue;
}

nv_action_queue_t *
nv_action_queue_alloc(void)
{
	return queue_alloc(1);
}

nv_action_queue_t *
nv_action_queue_alloc_paused(void)
{
	return queue_alloc(0);
}

int
nv_action_queue_start(nv_action_queue_t *queue)
{
	if(queue == NULL) return -1;
	pthread_mutex_lock(&queue->mutex);
	if(queue->started || queue->stopping ||
			pthread_create(&queue->worker, NULL, action_worker, queue) != 0)
	{
		pthread_mutex_unlock(&queue->mutex);
		return -1;
	}
	queue->started = 1;
	pthread_mutex_unlock(&queue->mutex);
	return 0;
}

int
nv_action_queue_submit(nv_action_queue_t *queue,
		nv_session_prepared_action_t *action, unsigned int command_sequence,
		uint64_t *task_id)
{
	if(queue == NULL || command_sequence == 0U || !action_valid(action))
	{
		errno = EINVAL;
		return -1;
	}
	nv_action_task_t *const task = calloc(1U, sizeof(*task));
	if(task == NULL) { errno = ENOMEM; return -1; }
	task->command_sequence = command_sequence;
	task->action = *action;
	pthread_mutex_lock(&queue->mutex);
	if(queue->stopping || queue->task_count >= NV_ACTION_QUEUE_LIMIT)
	{
		pthread_mutex_unlock(&queue->mutex);
		free(task);
		errno = queue->stopping ? ECANCELED : EBUSY;
		return -1;
	}
	task->id = queue->next_id++;
	if(queue_event_locked(queue, task, NV_ACTION_TASK_QUEUED, 0U, 0U, NULL,
			0, 0U, NULL, 0, 0) != 0)
	{
		pthread_mutex_unlock(&queue->mutex);
		free(task);
		errno = ENOMEM;
		return -1;
	}
	if(queue->pending_tail == NULL) queue->pending = task;
	else queue->pending_tail->next = task;
	queue->pending_tail = task;
	++queue->pending_count;
	++queue->task_count;
	*action = (nv_session_prepared_action_t){};
	const uint64_t submitted_id = task->id;
	pthread_cond_signal(&queue->ready);
	pthread_mutex_unlock(&queue->mutex);
	if(task_id != NULL) *task_id = submitted_id;
	return 0;
}

void
nv_action_queue_cancel_all(nv_action_queue_t *queue)
{
	if(queue == NULL) return;
	pthread_mutex_lock(&queue->mutex);
	for(nv_action_task_t *task = queue->pending; task != NULL; task = task->next)
		task->cancelled = 1;
	if(queue->running != NULL) queue->running->cancelled = 1;
	pthread_cond_broadcast(&queue->ready);
	pthread_mutex_unlock(&queue->mutex);
}

int
nv_action_queue_cancel(nv_action_queue_t *queue, uint64_t task_id)
{
	if(queue == NULL || task_id == 0U)
	{
		errno = EINVAL;
		return -1;
	}
	pthread_mutex_lock(&queue->mutex);
	nv_action_task_t *task = queue->pending;
	while(task != NULL && task->id != task_id) task = task->next;
	if(task == NULL && queue->running != NULL && queue->running->id == task_id)
		task = queue->running;
	if(task == NULL)
	{
		pthread_mutex_unlock(&queue->mutex);
		errno = ENOENT;
		return -1;
	}
	task->cancelled = 1;
	pthread_cond_broadcast(&queue->ready);
	pthread_mutex_unlock(&queue->mutex);
	return 0;
}

int
nv_action_queue_busy(nv_action_queue_t *queue)
{
	if(queue == NULL) return 0;
	pthread_mutex_lock(&queue->mutex);
	const int busy = queue->task_count != 0U;
	pthread_mutex_unlock(&queue->mutex);
	return busy;
}

int
nv_action_queue_failed(nv_action_queue_t *queue)
{
	if(queue == NULL) return 0;
	pthread_mutex_lock(&queue->mutex);
	const int failed = queue->terminal_event_lost;
	pthread_mutex_unlock(&queue->mutex);
	return failed;
}

void
nv_action_queue_ack_terminal(nv_action_queue_t *queue, uint64_t task_id)
{
	if(queue == NULL || task_id == 0U) return;
	pthread_mutex_lock(&queue->mutex);
	nv_action_task_t *previous = NULL;
	nv_action_task_t *task = queue->finishing;
	while(task != NULL && task->id != task_id)
	{
		previous = task;
		task = task->next;
	}
	if(task != NULL)
	{
		if(previous == NULL) queue->finishing = task->next;
		else previous->next = task->next;
		if(queue->finishing_tail == task) queue->finishing_tail = previous;
		--queue->task_count;
	}
	pthread_mutex_unlock(&queue->mutex);
	task_free(task);
}

int
nv_action_queue_take_terminal_action(nv_action_queue_t *queue, uint64_t task_id,
		nv_session_prepared_action_t *action)
{
	if(queue == NULL || task_id == 0U || action == NULL)
	{
		errno = EINVAL;
		return -1;
	}
	*action = (nv_session_prepared_action_t){};
	pthread_mutex_lock(&queue->mutex);
	nv_action_task_t *previous = NULL;
	nv_action_task_t *task = queue->finishing;
	while(task != NULL && task->id != task_id)
	{
		previous = task;
		task = task->next;
	}
	if(task == NULL)
	{
		pthread_mutex_unlock(&queue->mutex);
		errno = ENOENT;
		return -1;
	}
	if(previous == NULL) queue->finishing = task->next;
	else previous->next = task->next;
	if(queue->finishing_tail == task) queue->finishing_tail = previous;
	--queue->task_count;
	*action = task->action;
	task->action = (nv_session_prepared_action_t){};
	pthread_mutex_unlock(&queue->mutex);
	task_free(task);
	return 0;
}

int
nv_action_queue_pop(nv_action_queue_t *queue, nv_action_event_t *event)
{
	if(queue == NULL || event == NULL) return -1;
	pthread_mutex_lock(&queue->mutex);
	nv_action_event_node_t *const node = queue->events_head;
	if(node != NULL)
	{
		queue->events_head = node->next;
		if(queue->events_head == NULL) queue->events_tail = NULL;
		--queue->event_count;
		*event = node->event;
		free(node);
	}
	pthread_mutex_unlock(&queue->mutex);
	return node == NULL ? 0 : 1;
}

typedef struct
{
	nv_action_queue_t *queue;
	nv_action_task_t *task;
} nv_action_cancel_context_t;

static int
task_cancelled(void *data)
{
	nv_action_cancel_context_t *const context = data;
	pthread_mutex_lock(&context->queue->mutex);
	const int cancelled = context->task->cancelled || context->queue->stopping;
	pthread_mutex_unlock(&context->queue->mutex);
	return cancelled;
}

static char *
join_path(const char directory[], const char name[])
{
	const size_t directory_length = strlen(directory);
	const size_t name_length = strlen(name);
	const int separator = directory_length != 0U &&
		directory[directory_length - 1U] != '/';
	if(directory_length > SIZE_MAX - name_length - (size_t)separator - 1U)
	{
		errno = ENOMEM;
		return NULL;
	}
	char *const path = malloc(directory_length + (size_t)separator +
		name_length + 1U);
	if(path == NULL) { errno = ENOMEM; return NULL; }
	memcpy(path, directory, directory_length);
	if(separator) path[directory_length] = '/';
	memcpy(path + directory_length + (size_t)separator, name, name_length + 1U);
	return path;
}

static const char *
failure_code(nv_session_command_kind_t kind, int os_error)
{
	if(os_error == ECANCELED) return "action-cancelled";
	if(os_error == NV_FS_STALE_ERRNO) return "stale-action";
	if(kind == NV_SESSION_MOVE_FILES && os_error == EXDEV)
		return "cross-filesystem-move-unsupported";
	if(os_error == EEXIST) return "destination-exists";
	return kind == NV_SESSION_COPY ? "copy-failed" :
		kind == NV_SESSION_MOVE_FILES ? "move-failed" :
		kind == NV_SESSION_RENAME ? "rename-failed" :
		kind == NV_SESSION_MKDIR ? "mkdir-failed" : "delete-failed";
}

static int
execute_action(nv_action_queue_t *queue, nv_action_task_t *task,
		size_t *completed_count, uint64_t *completed_bytes,
		size_t *failed_index, const char **last_path,
		const char **error_code, int *os_error)
{
	nv_action_cancel_context_t cancel = { .queue = queue, .task = task };
	int bytes_known = 0;
	(void)action_total_bytes(&task->action, &bytes_known);
	if(task_cancelled(&cancel)) { *os_error = ECANCELED; goto failed; }
	if(task->action.kind == NV_SESSION_MKDIR)
	{
		char *const path = join_path(task->action.source_directory,
				task->action.name);
		if(path == NULL) { *os_error = errno; goto failed; }
		publish_action_progress(queue, task, 0U, 0U, path);
		const int result = nv_fs_mkdir(path, 0777,
				task->action.source_directory_identity);
		*os_error = result == 0 ? 0 : errno;
		if(result != 0)
		{
			free(path);
			goto failed;
		}
		*completed_count = 1U;
		publish_action_progress(queue, task, *completed_count, 0U, path);
		free(path);
		return 0;
	}
	for(size_t i = 0U; i < task->action.target_count; ++i)
	{
		if(task_cancelled(&cancel))
		{
			*os_error = ECANCELED;
			*failed_index = i;
			goto failed;
		}
		const nv_session_prepared_target_t *const target = &task->action.targets[i];
		*last_path = target->path;
		publish_action_progress(queue, task, *completed_count,
				*completed_bytes, target->path);
		char *destination = NULL;
		if(task->action.destination_directory != NULL)
		{
			destination = join_path(task->action.destination_directory, target->name);
			if(destination == NULL)
			{
				*os_error = errno;
				*failed_index = i;
				goto failed;
			}
		}
		const int result = task->action.kind == NV_SESSION_COPY ?
			nv_fs_copy(target->path, destination,
				task->action.source_directory_identity,
				task->action.destination_directory_identity, target->identity,
				task_cancelled, &cancel) :
			task->action.kind == NV_SESSION_MOVE_FILES ||
					task->action.kind == NV_SESSION_RENAME ?
				nv_fs_move(target->path, destination,
					task->action.source_directory_identity,
					task->action.destination_directory_identity, target->identity,
					task_cancelled, &cancel) :
				nv_fs_remove(target->path,
					task->action.source_directory_identity, target->identity,
					task_cancelled, &cancel);
		*os_error = result == 0 ? 0 : errno;
		free(destination);
		if(result != 0)
		{
			*failed_index = i;
			goto failed;
		}
		++*completed_count;
		if(bytes_known) *completed_bytes += target->size_bytes;
		publish_action_progress(queue, task, *completed_count,
				*completed_bytes, target->path);
	}
	return 0;

failed:
	*error_code = failure_code(task->action.kind, *os_error);
	return *os_error == ECANCELED ? 1 : -1;
}

static void *
action_worker(void *data)
{
	nv_action_queue_t *const queue = data;
#ifdef _WIN32
	const HRESULT com_result = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	const int com_initialized = SUCCEEDED(com_result);
#endif
	for(;;)
	{
		pthread_mutex_lock(&queue->mutex);
		while(queue->pending == NULL && !queue->stopping)
			pthread_cond_wait(&queue->ready, &queue->mutex);
		if(queue->stopping)
		{
			pthread_mutex_unlock(&queue->mutex);
			break;
		}
		nv_action_task_t *const task = queue->pending;
		queue->pending = task->next;
		if(queue->pending == NULL) queue->pending_tail = NULL;
		--queue->pending_count;
		task->next = NULL;
		queue->running = task;
		task->started_at_unix_ms = now_unix_ms();
		(void)queue_event_locked(queue, task, NV_ACTION_TASK_RUNNING, 0U, 0U,
				NULL, 0, 0U, NULL, 0, 0);
		pthread_mutex_unlock(&queue->mutex);

		size_t completed = 0U, failed_index = 0U;
		uint64_t completed_bytes = 0U;
		int os_error = 0;
		const char *last_path = NULL;
		const char *error_code = NULL;
		const int result = execute_action(queue, task, &completed, &completed_bytes,
				&failed_index, &last_path, &error_code, &os_error);

		pthread_mutex_lock(&queue->mutex);
		task->finished_at_unix_ms = now_unix_ms();
		if(task->finished_at_unix_ms < task->started_at_unix_ms)
			task->finished_at_unix_ms = task->started_at_unix_ms;
		queue->running = NULL;
		if(queue->finishing_tail == NULL) queue->finishing = task;
		else queue->finishing_tail->next = task;
		queue->finishing_tail = task;
		if(queue_event_locked(queue, task,
				result == 0 ? NV_ACTION_TASK_DONE :
				result > 0 ? NV_ACTION_TASK_CANCELLED : NV_ACTION_TASK_FAILED,
				completed, completed_bytes, last_path, result == 0 ? 0 : 1, failed_index,
				error_code, os_error,
				(completed != 0U && completed < action_total(&task->action)) ||
				(result != 0 && task->action.kind == NV_SESSION_COPY)) != 0)
		{
			queue->terminal_event_lost = 1;
		}
		pthread_mutex_unlock(&queue->mutex);
	}
#ifdef _WIN32
	if(com_initialized) CoUninitialize();
#endif
	return NULL;
}

void
nv_action_queue_free(nv_action_queue_t *queue)
{
	if(queue == NULL) return;
	pthread_mutex_lock(&queue->mutex);
	queue->stopping = 1;
	if(queue->pending != NULL) queue->pending->cancelled = 1;
	if(queue->running != NULL) queue->running->cancelled = 1;
	pthread_cond_broadcast(&queue->ready);
	pthread_mutex_unlock(&queue->mutex);
	if(queue->started) pthread_join(queue->worker, NULL);
	while(queue->pending != NULL)
	{
		nv_action_task_t *const next = queue->pending->next;
		task_free(queue->pending);
		queue->pending = next;
	}
	while(queue->finishing != NULL)
	{
		nv_action_task_t *const next = queue->finishing->next;
		task_free(queue->finishing);
		queue->finishing = next;
	}
	while(queue->events_head != NULL)
	{
		nv_action_event_node_t *const next = queue->events_head->next;
		nv_action_event_free(&queue->events_head->event);
		free(queue->events_head);
		queue->events_head = next;
	}
	pthread_cond_destroy(&queue->ready);
	pthread_mutex_destroy(&queue->mutex);
	free(queue);
}

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 filetype=c : */
