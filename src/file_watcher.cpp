// wrapper for monitoring when files are updated
// mainly exists because of flexible array member
// in linux inotify_event (see file_watcher.c)
// although this is the c++ implementation
// (note that our interface is still c though)

#include "file_watcher.h"

#ifdef _WIN32
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string_view>
// windows.h included in header

namespace
{
	constexpr size_t file_watcher_buf_size = 256 * sizeof(DWORD); 

	// kinda like perror for win32 errors
	static inline void print_error(std::string_view sv, DWORD error_code = GetLastError())
	{
		char* msg_out_buf;
		DWORD format_ret = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error_code, 0,
			reinterpret_cast<LPSTR>(&msg_out_buf), 0, nullptr);
		if (format_ret == 0)
		{
			std::cerr << "FormatMessageA() failed for error code " << error_code << "(" << sv << "), with error code " << GetLastError() << std::endl;
			return;
		}
		std::cerr << sv << ": " << msg_out_buf << std::endl;
		if (LocalFree(msg_out_buf) != nullptr)
			{ std::cerr << "LocalFree() for FormatMessageA() failed with error code " << GetLastError() << std::endl; }
	}
}

file_watcher_ctx file_watcher_init(const char* dir, const char* filename, size_t filename_size, void* user_data)
{
	HANDLE dir_handle = CreateFileA(dir, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
	if (dir_handle == INVALID_HANDLE_VALUE)
	{
		print_error("CreateFileA() error opening directory");
		return { .has_value = false };
	}
	void* read_data = _aligned_malloc(file_watcher_buf_size, sizeof(DWORD));
	if (read_data == nullptr)
	{
		std::perror("_aligned_malloc() error");
		return { .has_value = false };
	}
	
	// wide filename can have upto as many bytes as narrow
	wchar_t* filename_ = new wchar_t[filename_size + 1];
	filename_size = std::mbstowcs(filename_, filename, filename_size + 1);
	return { .has_value = true, .handle = dir_handle, .filename = filename_, .filename_size = filename_size,
		.notify_on_last_write = *static_cast<bool*>(user_data), .has_cur_request = false, .moved = false,
		.read_data = static_cast<unsigned char*>(read_data), .read_data_offset = 0, .read_data_size = 0 };
}

namespace
{
	static inline file_watcher_result file_watcher_read(file_watcher_ctx* ctx)
	{
		const auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(ctx->read_data + ctx->read_data_offset);
		if (info->NextEntryOffset == 0)  // this is the last entry
			{ ctx->read_data_offset = ctx->read_data_size; }
		else
			{ ctx->read_data_offset += info->NextEntryOffset; }
		
		const std::size_t filename_num_chars = info->FileNameLength / sizeof(WCHAR);
		
		if (info->Action == FILE_ACTION_RENAMED_NEW_NAME && ctx->moved)
		{
			// allocate double length so wide to multibyte string conversion never overflows
			std::size_t new_filename_size = filename_num_chars * 2;
			// use malloc() so user can free()
			char* new_filename = static_cast<char*>(std::malloc(new_filename_size + 1));
			// wcstombs requires null-terminated string but info->FileName is not
			int ret = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, info->FileName, filename_num_chars, new_filename, new_filename_size, nullptr, nullptr);
			if (ret == 0)
			{
				print_error("WideCharToMultiByte() error");
				return { .state = -1 };
			}
			new_filename_size = ret;
			new_filename[new_filename_size] = '\0';  // add null terminator
			return { .state = 1, .event_create = false, .event_modify = false, .moved_to = new_filename, .moved_to_size = new_filename_size };
		}
		ctx->moved = false;
		
		bool strs_eq;
		{
			std::wstring_view old_sv(ctx->filename, ctx->filename_size);
			std::wstring_view new_sv(info->FileName, filename_num_chars);
			strs_eq = (old_sv != new_sv);
		}
		if (strs_eq)
			{ return { .state = 2 }; }
		if (info->Action == FILE_ACTION_RENAMED_OLD_NAME)
		{
			ctx->moved = true;
			return { .state = 2 };
		}
		
		bool created = false, created_moved = false, modified = false;		
		switch (info->Action)
		{
		case FILE_ACTION_ADDED:
			created = true;
			break;
		case FILE_ACTION_RENAMED_NEW_NAME:
			created_moved = true;
			break;
		case FILE_ACTION_MODIFIED:
			modified = true;
			break;
		default:
			return { .state = 2 };
		}
		return { .state = 1, .event_create = created, .event_create_moved = created_moved, .event_modify = modified, .moved_to = nullptr, .moved_to_size = 0 };
	}
}

file_watcher_result file_watcher_poll(file_watcher_ctx* ctx)
{
	if (!ctx->has_value)
		{ return { .state = -1 }; }
	
	// still more data from previous request that haven't been read
	if (ctx->read_data_offset < ctx->read_data_size)
		{ return file_watcher_read(ctx); }
	
	
	if (!ctx->has_cur_request)
	{
		ctx->cur_request = {};
		// unlike ReadFile, this shouldn't ever run synchronously if the handle is async
		if (!ReadDirectoryChangesW(ctx->handle, ctx->read_data, file_watcher_buf_size, FALSE,
			FILE_NOTIFY_CHANGE_FILE_NAME | (ctx->notify_on_last_write ? FILE_NOTIFY_CHANGE_LAST_WRITE : FILE_NOTIFY_CHANGE_SIZE) | FILE_NOTIFY_CHANGE_CREATION,
			nullptr, &(ctx->cur_request), nullptr))
		{
			print_error("ReadDirectoryChangesW() error");
			return { .state = -1 };
		}
		ctx->has_cur_request = true;
	}
	// has_cur_request will always be true now 
	DWORD bytes_transferred;
	if (!GetOverlappedResult(ctx->handle, &(ctx->cur_request), &bytes_transferred, FALSE))
	{
		const auto last_err = GetLastError();
		if (last_err == ERROR_IO_INCOMPLETE)
			{ return { .state = 0 }; }
		else
		{
			print_error("GetOverlappedResult() error", last_err);
			return { .state = -1 };
		}
	}
	// successfully read data
	ctx->read_data_offset = 0;
	ctx->read_data_size = bytes_transferred;
	ctx->has_cur_request = false;
	return file_watcher_read(ctx);
}

bool file_watcher_cleanup(struct file_watcher_ctx* ctx)
{
	if (!ctx->has_value)
		{ return false; }
	
	delete[] ctx->filename;
	_aligned_free(ctx->read_data);
	bool ok = true;
	if (!CancelIo(ctx->handle))
		{ ok = false; print_error("CancelIo() error"); }
	if (!CloseHandle(ctx->handle))
		{ ok = false; print_error("CloseHandle() error"); }
	return ok;
}

#endif
