#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "file_watcher.h"

#if defined(__linux__) || defined(_WIN32)

int main()
{
	const std::string file = "latest.log";
#ifdef _WIN32
	bool notify_on_last_write = false;
#define FILE_WATCHER_USER_DATA &notify_on_last_write
#else
#define FILE_WATCHER_USER_DATA nullptr
#endif
	auto ctx = file_watcher_init(".", file.c_str(), file.size(), FILE_WATCHER_USER_DATA);
#undef FILE_WATCHER_USER_DATA
	if (!ctx.has_value)
		{ return -1; }
	
	std::uintmax_t prev_size = 0;

	while (true)
	{
		auto res = file_watcher_poll(&ctx);
		if (res.state == -1)
			{ return -1; }
		if (res.state == 1)
		{
			std::cout << std::format("{:%D %T} - {}{}{}", std::chrono::system_clock::now(), res.event_create ? "CREATE" : "", res.event_modify ? "MODIFY" : "", (res.moved_to != nullptr) ? "MOVE" : "") << std::endl;
			if (res.event_create || res.moved_to != nullptr)
				{ prev_size = 0; }
			if (res.event_create_moved)
			{
				const auto size = std::filesystem::exists(file) ? std::filesystem::file_size(file) : 0;
				if (size > 0)
				{
					std::ifstream fin(file, std::ios::binary);
					std::string s;
					s.resize_and_overwrite(size, [&fin](char* buf, std::size_t buf_size)
					{
						fin.read(buf, buf_size);
						return buf_size;
					});
					fin.close();
					std::cout << std::format("--BEGIN MOVED FILE CONTENTS--\n{}\n--END MOVED FILE CONTENTS--", s) << std::endl;
				}
				prev_size = size;
			}
			if (res.event_modify)
			{				
				const auto size = std::filesystem::exists(file) ? std::filesystem::file_size(file) : 0;
				if (size < prev_size)
				{
					std::cerr << "WARNING: File shrunk somehow, reading from start" << std::endl;
					prev_size = 0;
				}
				if (size > 0 && size > prev_size)
				{
					std::ifstream fin(file, std::ios::binary);
					fin.seekg(prev_size, std::ios::beg);
					std::string s;
					s.resize_and_overwrite(size - prev_size, [&fin](char* buf, std::size_t buf_size)
					{
						fin.read(buf, buf_size);
						return buf_size;
					});
					fin.close();
					std::cout << s << std::endl;
				}
				else
					{ std::cout << "(empty)" << std::endl; }
				prev_size = size;
			}
			if (res.moved_to != nullptr)
			{
				std::cout << std::string_view(res.moved_to, res.moved_to_size) << std::endl;
				std::free(res.moved_to);
			}
		}
		else if (res.state == 0)
			{ std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
	}

	if (!file_watcher_cleanup(&ctx))
		{ return -1; }
}

#else

int main()
{
	std::cerr << "not implemented" << std::endl;
}

#endif
