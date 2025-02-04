#ifndef PARSE_LOGS_H
#define PARSE_LOGS_H

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <ranges>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <libdeflate.h>

// for some reason rpcdce.h has this
#ifdef uuid_t
#undef uuid_t
#endif

// pair of join time (time point) and play time (duration)
using play_session = std::pair<std::chrono::system_clock::time_point, std::chrono::system_clock::duration>;
// pair of play sessions and total play time (duration)
using playtime_info = std::pair<std::vector<play_session>, std::chrono::system_clock::duration>;
struct uuid_t  // formatter specialization needs user-defined type so std::pair technically doesn't work
{
	std::uint64_t first, second;
	friend std::strong_ordering operator<=>(uuid_t lhs, uuid_t rhs)
	{
		if (auto res = lhs.first <=> rhs.first; !std::is_eq(res))
			{ return res; }
		return lhs.second <=> rhs.second;
	}
	friend bool operator==(uuid_t lhs, uuid_t rhs) = default;
};

template<>
struct std::formatter<uuid_t, char>
{
	constexpr auto parse(std::format_parse_context& ctx) const
	{
		const  auto it = ctx.begin(), end = ctx.end();
		if (it == end || *it == '}')
			{ return it; }
		return end;
	}

	auto format(uuid_t uuid, std::format_context& context) const
	{
		static constexpr std::array<char, 16> hex_digits = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };
		std::array<char, 36> chars;
		auto it = chars.begin();
		for (std::size_t i = 0; i < 8; i++)
		{
			*it = hex_digits[(uuid.first >> (i * 4)) & 0xF];
			it++;
		}
		*it = '-';
		it++;
		for (std::size_t i = 8; i < 12; i++)
		{
			*it = hex_digits[(uuid.first >> (i * 4)) & 0xF];
			it++;
		}
		*it = '-';
		it++;
		for (std::size_t i = 12; i < 16; i++)
		{
			*it = hex_digits[(uuid.first >> (i * 4)) & 0xF];
			it++;
		}
		*it = '-';
		it++;
		for (std::size_t i = 0; i < 4; i++)
		{
			*it = hex_digits[(uuid.second >> (i * 4)) & 0xF];
			it++;
		}
		*it = '-';
		it++;
		for (std::size_t i = 4; i < 16; i++)
		{
			*it = hex_digits[(uuid.second >> (i * 4)) & 0xF];
			it++;
		}
		return std::ranges::copy(chars, context.out()).out;
	}
};

namespace detail
{
	// @return value or empty optional if uuid has invalid format
	[[nodiscard]] constexpr std::optional<uuid_t> parse_uuid(std::span<char, 36> str)
	{
		uuid_t uuid{0, 0};
		constexpr auto hex_digit_to_uint64 = [](char c) -> std::uint64_t
		{
			static_assert('a' + 1 == 'b' && 'b' + 1 == 'c' && 'c' + 1 == 'd' && 'd' + 1 == 'e' && 'e' + 1 == 'f', "lowercase a-f characters not in order, panic!!!");
			static_assert('A' + 1 == 'B' && 'B' + 1 == 'C' && 'C' + 1 == 'D' && 'D' + 1 == 'E' && 'E' + 1 == 'F', "uppercase A-F characters not in order, panic!!!");
			if ('0' <= c && c <= '9')
				{ return c - '0'; }
			if ('a' <= c && c <= 'f')
				{ return c - 'a' + 10; }
			if ('A' <= c && c <= 'F')
				{ return c - 'A' + 10; }
			return -1;
		};

		// xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
		for (std::size_t i : { 8, 13, 18, 23})
		{
			if (str[i] != '-')
				{ return {}; }
		}
		std::array<char, 32> arr{};
		std::copy_n(str.begin(), 8, arr.begin());
		std::copy_n(str.begin() + 9, 4, arr.begin() + 8);
		std::copy_n(str.begin() + 14, 4, arr.begin() + 12);
		std::copy_n(str.begin() + 19, 4, arr.begin() + 16);
		std::copy_n(str.begin() + 24, 12, arr.begin() + 20);

		auto it = arr.cbegin();
		for (std::size_t i = 0; i < 16; i++)
		{
			const auto val = hex_digit_to_uint64(*it) << (i * 4);
			if (val == -1)
				{ return {}; }
			uuid.first += val;
			it++;
		}
		for (std::size_t i = 0; i < 16; i++)
		{
			const auto val = hex_digit_to_uint64(*it) << (i * 4);
			if (val == -1)
				{ return {}; }
			uuid.second += val;
			it++;
		}

		return uuid;
	}
	
	struct file_read_ctx_t
	{
		struct libdeflate_decompressor_deleter
		{
			void operator()(libdeflate_decompressor* ptr)
				{ libdeflate_free_decompressor(ptr); }
		};

		// used internally by reader:
		std::unique_ptr<std::istream> fin;  // ifstream or istringstream for gzipped
		std::vector<std::pair<std::filesystem::path, bool>> paths;  // pair of path and whether the file is a .gz
		std::size_t path_ind = 0;
		// updated/initialized by consumer, used by reader:
		std::unique_ptr<libdeflate_decompressor, libdeflate_decompressor_deleter> decompressor;
		// updated by reader, to be used by consumer:
		std::chrono::system_clock::time_point date_tp;  // date assocated with log file (midnight on the log date)
		std::size_t line = 0;  // line number in current file
	};
}

// get time for midnight of the date `p` was last modified (in local time)
inline std::chrono::system_clock::time_point file_modification_date(const std::filesystem::path& p, const std::chrono::time_zone* target_tz)
{
	auto last_write_time = std::chrono::clock_cast<std::chrono::system_clock>(std::filesystem::last_write_time(p));
	auto write_time_local = target_tz->to_local(last_write_time);
	write_time_local = std::chrono::floor<std::chrono::days>(write_time_local);
	return target_tz->to_sys(write_time_local);
}

namespace detail
{
	// @return pair of line data and whether a new file was opened. string will be empty if all lines in all files have been exhausted
	//         (empty lines will be skipped otherwise)
	template<bool skip_latest_log>
	inline std::pair<std::string, bool> get_next_line(file_read_ctx_t& ctx, const std::chrono::time_zone* target_tz)
	{
		using namespace std::string_view_literals;
		if (!ctx.fin || !*(ctx.fin))
		{
			if (ctx.path_ind == ctx.paths.size())
				{ return { {}, false }; }
			ctx.fin.reset();
			ctx.line = 0;
			const auto& [path, is_gz] = ctx.paths[ctx.path_ind];
			ctx.path_ind++;

			std::string path_filename = path.filename().string();
			if (!skip_latest_log && path_filename == "latest.log"sv)
				{ ctx.date_tp = file_modification_date(path, target_tz); }
			else
			{
				std::istringstream path_ss(path_filename);
				std::chrono::local_time<std::chrono::days> local_tp;
				if (!std::chrono::from_stream(path_ss, "%4Y-%2m-%2d", local_tp))
				{
					std::cout << "WARNING: File name " << path_filename << " has unexpected format" << std::endl;
					return get_next_line<skip_latest_log>(ctx, target_tz);  // skip to next path
				}
				// TODO: handle exception for nonexistent times (or specify non-throwing overload)
				ctx.date_tp = std::chrono::locate_zone("UTC")->to_sys(local_tp);
			}
			
			if (is_gz)
			{
				std::ifstream fin(path, std::ios::binary);
				const auto size = std::filesystem::file_size(path);
				std::vector<char> data(size);
				fin.read(data.data(), size);
				std::string out_data;
				libdeflate_result res;
				out_data.resize_and_overwrite(16777216, [&ctx, &data, &res](char* buf, std::size_t buf_size)
				{
					std::size_t out_bytes_used;
					res = libdeflate_gzip_decompress(ctx.decompressor.get(), data.data(), data.size(), buf, buf_size, &out_bytes_used);
					return (res == LIBDEFLATE_SUCCESS) ? out_bytes_used : 0;
				});
				if (res != LIBDEFLATE_SUCCESS)
				{
					switch (res)
					{
					case LIBDEFLATE_BAD_DATA:
						std::cerr << "ERROR: Libdeflate bad data error while decompressing " << path_filename << std::endl;
						break;
					case LIBDEFLATE_INSUFFICIENT_SPACE:
						std::cerr << "ERROR: Libdeflate insufficient space (>16 MiB) error while decompressing " << path_filename << std::endl;
						break;
					default:
						std::cerr << "ERROR: Libdeflate error while decompressing " << path_filename << std::endl;
						break;
					}
					return get_next_line<skip_latest_log>(ctx, target_tz);  // skip to next path
				}
				ctx.fin = std::make_unique<std::istringstream>(std::move(out_data));
			}
			else
				{ ctx.fin = std::make_unique<std::ifstream>(path); }
			return { get_next_line<skip_latest_log>(ctx, target_tz).first, true };
		}
		std::string s;
		std::getline(*(ctx.fin), s);
		ctx.line++;
		if (s.empty())
			{ return get_next_line<skip_latest_log>(ctx, target_tz); }  // skip to next line
		return { s, false };
	}
}

// return a string of the filename with .log or .log.gz extension removed
// filename MUST end with .log or .log.gz (check before calling this!)
[[nodiscard]] inline std::string log_filename_no_ext(const std::pair<std::filesystem::path, bool>& p)
{
	auto p_str = p.first.filename().string();
	// ".log.gz" -> 7, ".log" -> 4
	p_str.resize(p_str.size() - (p.second ? 7 : 4));
	return p_str;
}

namespace detail
{
	struct single_player_info
	{
		std::optional<uuid_t> uuid;
		std::optional<std::chrono::system_clock::time_point> join_time;
	};
}

// yes, this needs to be a sorted map (see create_graph)
using log_data_t = std::map<uuid_t, std::pair<std::vector<std::string>, playtime_info>>;

struct parse_ctx_t
{
	std::string cur_filename;
	std::chrono::system_clock::time_point date_tp;
	std::size_t line = 0;
	std::unordered_map<std::string, detail::single_player_info> player_info;
	bool server_stopped = false;
};

namespace detail
{
	// @tparam file_start_warn  whether to warn when clearing a player (used when clearing players on start)
	// @return whether someone left
	template<bool file_start_warn = false>
	inline bool clear_all_players(parse_ctx_t& ctx, log_data_t& data, std::chrono::system_clock::time_point leave_time)
	{
		bool any = false;
		for (auto& [cur_name, cur_info] : ctx.player_info)
		{
			auto& [uuid, join_time] = cur_info;
			if (uuid && join_time)
			{
				auto& [names, play_info] = data[uuid.value()];
				if (names.empty() || names.back() != cur_name)
					{ names.emplace_back(cur_name); }
				const auto playtime = leave_time - join_time.value();
				auto& [play_sessions, total_playtime] = play_info;
				play_sessions.emplace_back(join_time.value(), playtime);
				total_playtime += playtime;
				join_time = {};

				if constexpr (file_start_warn)
				{
					std::cout << std::format("WARNING: Player {} never left before server started in file {}, assuming leave time is {:%F %T}",
						cur_name, ctx.cur_filename, std::chrono::round<std::chrono::seconds>(leave_time)) << std::endl;
				}
				any = true;
			}
		}
		return any;
	}
}

struct line_parse_results
{
	// whether a valid line with a timestamp was found. line may or may not have been parsed
	bool read_valid_line;
	// whether any player has joined or left (could be multiple)
	bool player_join_left;
};

// @tparam strip_ending_cr  whether to remove all trailing CR (\r) characters
// @param line  string containing line data
// @param ctx  parse context from previous parsing
// @param data  data output where new data will be added to
// @param clear_before  whether to clear all players on first timestamp before parsing contents (used when parsing a new file)
// @return whether a valid line with a timestamp was found. line may or may not have been parsed
template<bool strip_ending_cr = true>
inline line_parse_results parse_line(std::string line, parse_ctx_t& ctx, log_data_t& data, bool clear_before = false)
{
	using namespace std::string_view_literals;
	if constexpr (strip_ending_cr)
	{
		while (line.ends_with('\r'))
			{ line = line.substr(0, line.size() - 1); }
	}
	std::istringstream ss(std::move(line));
	std::chrono::system_clock::duration time_offset;
	if (!std::chrono::from_stream(ss, "[%2H:%2M:%2S]", time_offset))
		{ return { false, false }; }
	const auto cur_time = ctx.date_tp + time_offset;
		
	char c;
	ss >> c;
	if (c != '[' || !ss.good())
		{ return { false, false }; }
	ss.ignore(std::numeric_limits<std::streamsize>::max(), ']');
	ss >> std::noskipws >> c >> std::skipws;
	if (c != ':' || !ss.good())
		{ return { false, false }; }

	bool players_changed = false;
	if (clear_before)
		{ players_changed = detail::clear_all_players<true>(ctx, data, cur_time); }
		
	std::string str1, str2, str3, str4;
	ss >> str1 >> str2;

	if (!ss)
		{ return { true, players_changed }; }
	// sometimes this is issued but not "Stopping the server" if the server crashes
	if (ss.eof() && str1 == "Stopping"sv && str2 == "server"sv)
	{
		bool players_changed2 = detail::clear_all_players(ctx, data, cur_time);
		ctx.server_stopped = true;
		return { true, players_changed || players_changed2 };
	}

	ss >> str3;
		
	if (!ss)
		{ return { true, players_changed }; }
	if (ctx.server_stopped)
	{
		if (str1 == "Starting"sv && str2 == "minecraft"sv && str3 == "server"sv)
		{
			ss >> str1 >> str2;
			// not going to verify version string
			if (ss.eof() && str1 == "version"sv)
				{ ctx.server_stopped = false; }
		}
		return { true, players_changed };
	}
	if (ss.eof() && str1 == "Stopping"sv && str2 == "the"sv && str3 == "server"sv)
	{
		bool players_changed2 = detail::clear_all_players(ctx, data, cur_time);
		ctx.server_stopped = true;
		return { true, players_changed || players_changed2 };
	}
		
	ss >> str4;
		
	// UUID of player xxx is xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
	// xxx joined the game
	// xxx (formerly known as yyy) joined the game
	// xxx left the game
		
	const auto player_joined = [&](const std::string& player_name)
	{
		auto& [uuid, join_time] = ctx.player_info[player_name];
		if (!uuid)
		{
			std::cout << "WARNING: UUID not found for player " << player_name << " in file " << ctx.cur_filename << ", line " << ctx.line
				<< " (expected UUID message before join message)" << std::endl;
		}
		if (join_time)
		{
			std::cout << "WARNING: Player " << player_name << " appears to have joined multiple times without leaving in file " << ctx.cur_filename << ", line " << ctx.line
				<< " (ignore if server crashed while players were online)" << std::endl;
		}
		join_time = cur_time;
	};
	// @return true on success
	const auto player_left = [&](const std::string& player_name)
	{
		auto& [uuid, join_time] = ctx.player_info[player_name];
		if (!uuid)
		{
			std::cerr << "ERROR: UUID not found for player " << player_name << " in file " << ctx.cur_filename << ", line " << ctx.line << std::endl;
			return false;
		}
		if (!join_time)
		{
			std::cerr << "ERROR: Join time not found for player " << player_name << " in file " << ctx.cur_filename << ", line " << ctx.line << std::endl;
			return false;
		}

		auto& [names, play_info] = data[uuid.value()];
		if (names.empty() || names.back() != player_name)
			{ names.emplace_back(player_name); }

		const auto leave_time = cur_time;
		const auto playtime = leave_time - join_time.value();
		auto& [play_sessions, total_playtime] = play_info;
		play_sessions.emplace_back(join_time.value(), playtime);
		total_playtime += playtime;
		join_time = {};
		return true;
	};
		
	if (!ss)
		{ return { true, players_changed }; }
	if (str1 == "UUID"sv && str2 == "of"sv && str3 == "player"sv)
	{
		// str4 is player name
		ss >> str1 >> str2;
		if (ss.eof() && str1 == "is"sv && str2.size() == 36)
		{
			auto uuid = detail::parse_uuid(std::span<char, 36>(str2));
			if (!uuid)
			{
				std::cerr << "ERROR: UUID parsing failed for " << str2 << "(player " << str4 << ") in file " << ctx.cur_filename << ", line " << ctx.line << std::endl;
				return { true, players_changed };
			}
			ctx.player_info[str4].uuid = uuid.value();
		}
		return { true, players_changed };
	}
	else if (ss.eof() && str3 == "the"sv && str4 == "game"sv)
	{
		// str1 is player name
		if (str2 == "joined"sv)
		{
			player_joined(str1);
			return { true, true };
		}
		else if (str2 == "left"sv)
		{
			bool players_changed2 = player_left(str1);
			return { true, players_changed || players_changed2 };
		}
		return { true, players_changed };
	}
	else if (str2 == "(formerly"sv && str3 == "known"sv && str4 == "as"sv)
	{
		// don't care about the former name
		ss >> str2 >> str2 >> str3 >> str4;
		if (ss.eof() && str2 == "joined"sv && str3 == "the"sv && str4 == "game"sv)
		{
			player_joined(str1);
			return { true, true };
		}
		return { true, players_changed };
	}
	return { true, players_changed };
}

// @return whether players have join/left
inline bool parse_lines(std::string lines, parse_ctx_t& ctx, log_data_t& data)
{
	std::string_view line;
	std::size_t pos = 0;
	bool players_changed = false;
	while (true)
	{
		std::size_t new_pos = lines.find('\n', pos);
		line = std::string_view(lines).substr(pos, ((new_pos == std::string::npos) ? lines.size() : new_pos) - pos);
		if (parse_line(std::string(line), ctx, data).player_join_left)
			{ players_changed = true; }
		if (new_pos == std::string::npos)
			{ break; }
		pos = new_pos + 1;
	}
	return players_changed;
}

// @tparam save_ctx  whether to save parse context. if false, players still online when the log was last written will "leave" at current system time
// @param read_file_cb  callback after a file has been completely read and parsed. should accept const std::pair<std::filesystem::path, bool>& as singlular parameter
// @return pair of log data and parse context if save_ctx is true; log data otherwise
template<bool skip_latest_log = false, bool save_ctx = false>
[[nodiscard]] inline std::conditional_t<save_ctx, std::pair<log_data_t, parse_ctx_t>, log_data_t>
	parse_logs(const std::filesystem::path& logs_dir, const std::chrono::time_zone* target_tz, auto&& read_file_cb)
{
	using namespace std::string_view_literals;
	detail::file_read_ctx_t read_ctx;
	read_ctx.decompressor.reset(libdeflate_alloc_decompressor());
	for (const auto& entry : std::filesystem::directory_iterator(logs_dir))
	{
		if (!entry.is_regular_file())
			{ continue; }
		const auto& path = entry.path();
		const auto ext = path.extension();
		if (ext != ".gz"sv && ext != ".log"sv)
			{ continue; }
		read_ctx.paths.emplace_back(path, (ext == ".gz"sv));
	}
	if (read_ctx.paths.empty())
		{ return {}; }
	// yyyy-mm-dd-##.log(.gz) OR latest.log
	std::erase_if(read_ctx.paths, [](const std::pair<std::filesystem::path, bool>& p)
	{
		static constexpr auto is_digit = [](char c) { return ('0' <= c) && (c <= '9'); };

		const auto& [path, is_gz] = p;
		const auto p_str = path.filename().string();
		if constexpr (!skip_latest_log)
		{
			if (p_str == "latest.log"sv)
				{ return false; }
		}

		// yyyy, mm, dd, and other digits (must be at least one)
		for (std::size_t i : { 0, 1, 2, 3, 5, 6, 8, 9, 11 })
		{
			if (!is_digit(p_str[i]))
				{ return true; }
		}
		for (std::size_t i : { 4, 7, 10 })
		{
			if (p_str[i] != '-')
				{ return true; }
		}
		std::size_t ind = 12;
		while (is_digit(p_str[ind]))
			{ ind++; }
		return (std::string_view(p_str.begin() + ind, p_str.end()) != (is_gz ? ".log.gz"sv : ".log"sv));
	});
	// sort by date, and latest.log last (ascending)
	std::ranges::sort(read_ctx.paths, [](const auto& lhs, const auto& rhs)
	{
		static constexpr auto is_digit = [](char c) { return ('0' <= c) && (c <= '9'); };

		const auto lhs_str = lhs.first.filename().string();
		const auto rhs_str = rhs.first.filename().string();
		
		if constexpr (!skip_latest_log)
		{
			if (lhs_str == "latest.log"sv)
				{ return false; }
			if (rhs_str == "latest.log"sv)
				{ return true; }
		}

		// yyyy-mm-dd (first 10 chars)
		const std::strong_ordering cmp_res = std::lexicographical_compare_three_way(lhs_str.cbegin(), lhs_str.cbegin() + 10, rhs_str.cbegin(), rhs_str.cbegin() + 10);
		if (cmp_res != std::strong_ordering::equal)
			{ return std::is_lt(cmp_res); }
		
		auto lhs_ind = 12, rhs_ind = 12;
		while (is_digit(lhs_str[lhs_ind]))
			{ lhs_ind++; }
		while (is_digit(rhs_str[rhs_ind]))
			{ rhs_ind++; }
		unsigned int lhs_num, rhs_num;
		std::from_chars(lhs_str.data() + 11, lhs_str.data() + lhs_ind, lhs_num);
		std::from_chars(rhs_str.data() + 11, rhs_str.data() + rhs_ind, rhs_num);
		return lhs_num < rhs_num;
	});
	const auto removed_subrange = std::ranges::unique(read_ctx.paths, [](const auto& lhs, const auto& rhs)
	{
		if (lhs == rhs)
		{
			std::cout << "WARNING: duplicate log file found: " << lhs << ", removing" << std::endl;
			return true;
		}
		return false;
	}, log_filename_no_ext);
	read_ctx.paths.erase(removed_subrange.begin(), removed_subrange.end());
	// TODO: verify no gaps, e.g. 2000-01-01-2, 2000-01-01-4 (missing 2000-01-01-1, 2000-01-01,3)
	
	log_data_t info;
	
	parse_ctx_t ctx;
	
	const auto get_cur_file = [&read_ctx]() -> const auto& { return read_ctx.paths[read_ctx.path_ind - 1]; };
	const auto get_prev_file = [&read_ctx]() -> const auto& { return read_ctx.paths[read_ctx.path_ind - 2]; };
	const auto get_cur_filename = [&get_cur_file]() { return get_cur_file().first.filename().string(); };

	bool server_stopped = false, clear_before = false;
	std::chrono::system_clock::time_point last_tp;
	while (true)
	{
		const auto [s, file_is_new] = detail::get_next_line<skip_latest_log>(read_ctx, target_tz);
		if (s.empty())
		{
			if (read_ctx.paths.size() > 0)
				{ read_file_cb(get_cur_file()); }
			break;
		}
		
		if (file_is_new)
		{
			ctx.cur_filename = get_cur_filename();
			ctx.date_tp = read_ctx.date_tp;
			// the server has only necessarily restarted if the date is the same (e.g. 2000-01-01-1 and 2000-01-01-2),
			// otherwise the logs may have just been a continuation of the previous day
			if (ctx.date_tp == last_tp)
				{ clear_before = true; }
			last_tp = ctx.date_tp;
		}
		ctx.line = read_ctx.line;

		if (parse_line(std::move(s), ctx, info, clear_before).read_valid_line)
			{ clear_before = false; }
		
		if (file_is_new && read_ctx.path_ind > 1)
			{ read_file_cb(get_prev_file()); }
	}
	
	if constexpr (save_ctx)
		{ return { info, ctx }; }
	else
	{
		// add players that are still online
		// might be a little sus to use now() for cases where the log files
		// are copied out, but we don't really have any better options
		detail::clear_all_players(ctx, info, std::chrono::system_clock::now());
		return info;
	}
}
template<bool skip_latest_log = false, bool save_ctx = false>
[[nodiscard]] inline decltype(auto) parse_logs(const std::filesystem::path& logs_dir, const std::chrono::time_zone* target_tz)
	{ return parse_logs<skip_latest_log, save_ctx>(logs_dir, target_tz, [](auto&&) {}); }

#endif
