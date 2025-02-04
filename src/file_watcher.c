// wrapper for monitoring when files are updated
// mainly exists because of flexible array member
// in linux inotify_event
// some platform implementations in file_watcher.cpp

#include "file_watcher.h"

#ifdef __linux__

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/types.h>
#include <unistd.h>

static const size_t file_watcher_buf_size = sizeof(struct inotify_event) + NAME_MAX + 1;

struct file_watcher_ctx file_watcher_init(const char* dir, const char* filename, size_t filename_size, void* /* unused */)
{
	int inotify_fd = inotify_init();
	if (inotify_fd == -1)
	{
		perror("inotify_init() error");
		struct file_watcher_ctx ret = { .has_value = 0 };
		return ret;
	}

	int watch_desc = inotify_add_watch(inotify_fd, dir, IN_CREATE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO);
	if (watch_desc == -1)
	{
		perror("inotify_add_watch() error");
		struct file_watcher_ctx ret = { .has_value = 0 };
		return ret;
	}

	int epoll_fd = epoll_create(1);
	if (epoll_fd == -1)
	{
		perror("epoll_create() error");
		struct file_watcher_ctx ret = { .has_value = 0 };
		return ret;
	}

	struct epoll_event event = { .events = EPOLLIN, .data = { .fd = inotify_fd } };
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, inotify_fd, &event) == -1)
	{
		perror("epoll_ctl() error");
		struct file_watcher_ctx ret = { .has_value = 0 };
		return ret;
	}

	if (filename_size == -1)
		{ filename_size = strlen(filename); }
	
	char* filename_ = strndup(filename, filename_size);
	if (filename_ == NULL)
	{
		perror("strndup() error");  // for C23 strndup, setting errno is not required, but it is in POSIX
		struct file_watcher_ctx ret = { .has_value = 0 };
		return ret;
	}
	unsigned char* read_data = malloc(file_watcher_buf_size);
	if (read_data == NULL)
	{
		perror("malloc() error");  // malloc setting errno is not required in standard C, but is in POSIX
		struct file_watcher_ctx ret = { .has_value = 0 };
		return ret;
	}
	struct file_watcher_ctx ret = { .has_value = 1, .inotify_fd = inotify_fd, .epoll_fd = epoll_fd, .cookie = 0,
		.filename = filename_, .filename_size = filename_size,
		.read_data = read_data, .read_data_consumed_size = 0, .read_data_size = 0 };
	return ret;
}

// expects previous read_data to have been fully consumed
// ctx->read_data, read_data_consumed_size, read_data_size will be updated only if data was available to read
// @return -1 on error, 0 if nothing was available to read, 1 if data was read successfully (subset of file_watcher_result.state)
static char read_more_events(struct file_watcher_ctx* ctx)
{
	// no ctx->has_value check because this isn't public and
	// everything that calls this should have checked already

	struct epoll_event epoll_event_out;
	int res = epoll_wait(ctx->epoll_fd, &epoll_event_out, 1, 0);
	if (res == -1)
	{
		perror("epoll_wait() error");
		return -1;
	}
	if (res > 0)
	{
		// overwrite previous contenst of ctx->read_data
		// this isn't c++, no need for destructors or anything
		ctx->read_data_consumed_size = 0;
		// alternatively use epoll_event_out.data.fd
		ssize_t read_amt = read(ctx->inotify_fd, ctx->read_data, file_watcher_buf_size);
		if (read_amt == -1)
		{
			perror("inotify read() error");
			return -1;
		}
		ctx->read_data_size = read_amt;
		return 1;
	}
	return 0;
}

// does not validate. use with caution
static struct inotify_event* get_next_event(struct file_watcher_ctx* ctx)
{
	struct inotify_event* ptr = (struct inotify_event*)(ctx->read_data + ctx->read_data_consumed_size);
	size_t size = sizeof(struct inotify_event) + ptr->len;
	ctx->read_data_consumed_size += size;
	return ptr;
}

struct file_watcher_result file_watcher_poll(struct file_watcher_ctx* ctx)
{
	if (!ctx->has_value)
	{
		struct file_watcher_result ret = { .state = -1 };
		return ret;
	}

	if (ctx->read_data == NULL || ctx->read_data_consumed_size >= ctx->read_data_size)
	{
		char read_res = read_more_events(ctx);
		if (read_res != 1)
		{
			struct file_watcher_result ret = { .state = read_res };
			return ret;
		}
	}

	struct inotify_event* event = get_next_event(ctx);

	if ((event->mask & IN_MOVED_TO) && ctx->cookie != 0 && ctx->cookie == event->cookie)
	{
		ctx->cookie = 0;
		char* new_filename = strndup(event->name, event->len);
		if (new_filename == NULL)
		{
			perror("strndup() error");
			struct file_watcher_result ret = { .state = -1 };
			return ret;
		}
		size_t new_filename_size = strlen(new_filename);
		struct file_watcher_result ret = { .state = 1, .event_create = false, .event_modify = false,
			.moved_to = new_filename, .moved_to_size = new_filename_size };
		return ret;
	}

	// note: event->len does not give the exact size of event->name, only an upper bound
	bool strs_eq = (event->len > ctx->filename_size &&
			strncmp(event->name, ctx->filename, ctx->filename_size) == 0);

	if (!strs_eq)
	{
		struct file_watcher_result ret = { .state = 2 };
		return ret;
	}
	if (event->mask & IN_MOVED_FROM)
	{
		ctx->cookie = event->cookie;
		struct file_watcher_result ret = { .state = 2 };
		return ret;
	}
	// same file but different event
	ctx->cookie = 0;

	bool created = event->mask & IN_CREATE;
	bool created_moved = event->mask & IN_MOVED_TO;
	bool modified = event->mask & IN_MODIFY;
	
	if (!created && !created_moved && !modified)
	{
		struct file_watcher_result ret = { .state = 2 };
		return ret;
	}

	struct file_watcher_result ret = { .state = 1, .event_create = created, .event_create_moved = created_moved, .event_modify = modified, .moved_to = NULL, .moved_to_size = 0 };
	return ret;
}

bool file_watcher_cleanup(struct file_watcher_ctx* ctx)
{
	if (!ctx->has_value)
		{ return false; }

	free(ctx->read_data);
	free(ctx->filename);
	bool b1 = close(ctx->epoll_fd) != -1;
	bool b2 = close(ctx->inotify_fd) != -1;
	return (b1 && b2);
}

#endif
