#ifndef FILE_WATCHER_H
#define FILE_WATCHER_H

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>
extern "C"
{
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#undef WIN32_LEAN_AND_MEAN
#endif

struct file_watcher_ctx
{
	bool has_value;  // true if the rest of the contents are valid
#ifdef __linux__
	int inotify_fd, epoll_fd;
	uint32_t cookie;
	char* filename;  // null-terminated filename
	size_t filename_size;  // excludes null terminator
	unsigned char* read_data;
	size_t read_data_consumed_size, read_data_size;
#elif defined(_WIN32)
	HANDLE handle;
	const wchar_t* filename;  // null-terminated filename
	size_t filename_size;  // excludes null terminator
	bool notify_on_last_write;  // true to use FILE_NOTIFY_CHANGE_LAST_WRITE, false to use FILE_NOTIFY_CHANGE_SIZE
	bool has_cur_request;
	OVERLAPPED cur_request;
	bool moved;
	unsigned char* read_data;
	size_t read_data_offset, read_data_size;
#endif
};

// watch `filename` in `dir` for create, modify, and rename events
// @param dir  null-terminated string of directory to watch
// @param filename  null-terminated string of target file (filename only, not path)
// @param filename_size  size of filename string excluding null, or -1 if unknown
// @param other_data  additional data that the platform-specific implementation might use:
//                    on windows, this will be a bool* that holds true to use FILE_NOTIFY_CHANGE_LAST_WRITE and false to use FILE_NOTIFY_CHANGE_SIZE;
//                    on linux, it is unused
struct file_watcher_ctx file_watcher_init(const char* dir, const char* filename, size_t filename_size, void* other_data);

struct file_watcher_result
{
	// -1 on error, 0 if nothing was available to read, 1 if data was read, 2 if more should be read
	// other struct contents only valid if state == 1
	char state;
	// file was created
	bool event_create;
	// file was moved to (effectively a create with existing data)
	bool event_create_moved;
	// file was modified
	bool event_modify;
	// new file name if file was moved elsewhere (NULL otherwise)
	// MUST BE FREED BY USER WITH free()
	char* moved_to;  // null-terminated new file name
	size_t moved_to_size;  // excludes null terminator
};
// read a single event, if it exists
struct file_watcher_result file_watcher_poll(struct file_watcher_ctx* ctx);

// if failed, some file descriptors may not be closed
// @return true on success
bool file_watcher_cleanup(struct file_watcher_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif
