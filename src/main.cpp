#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <dpp/dpp.h>
#include <jsoncons/json.hpp>
#include "parse_logs.h"
#include "file_watcher.h"
#include "playtime_graph.h"

#undef poll  // from dpp socket.h for windows

// RAII wrapper for file_watcher stuff
class file_watcher
{
private:
	file_watcher_ctx ctx;

	struct free_deleter
	{
		void operator()(char* ptr) const noexcept { std::free(ptr); }
	};

public:
	// IMPORTANT: dir and file must be null-terminated! (not all string_views are)
	// @throws std::runtime_error if construction failed
	file_watcher(const char* dir, std::string_view file, void* other_data) : ctx(file_watcher_init(dir, file.data(), file.size(), other_data))
	{
		if (!ctx.has_value)
			{ throw std::runtime_error("Error creating file_watcher"); }
	}
	~file_watcher()
	{
		if (!file_watcher_cleanup(&ctx))
			{ std::cerr << "Error cleaning up file_watcher" << std::endl; }
	}
	
	// TODO: copy and move
	
	struct result_t
	{
		enum class state_t { no_data, data_read, read_more } state;
		bool event_create, event_create_moved, event_modify;
		std::optional<std::pair<std::unique_ptr<char[], free_deleter>, std::size_t>> moved_to;
	};

	// @return nullopt on error, or struct containing bools event_create, event_create_moved, event_modify; optional<pair<unique_ptr<char[]>, size_t>> moved_to
	[[nodiscard]] std::optional<result_t> poll()
	{
		auto res = file_watcher_poll(&ctx);
		if (res.state == -1)
			{ return std::nullopt; }
		decltype(result_t::moved_to) moved_to;
		if (res.moved_to != nullptr)
			{ moved_to = { std::unique_ptr<char[], free_deleter>(res.moved_to), res.moved_to_size }; }
		result_t::state_t s;
		switch (res.state)
		{
		case 0:
			s = result_t::state_t::no_data;
			break;
		case 1:
			s = result_t::state_t::data_read;
			break;
		case 2:
			s = result_t::state_t::read_more;
			break;
		default:
			std::cerr << "Unexpected file_watcher_poll state: " << res.state << std::endl;
			return std::nullopt;
		}
		return std::make_optional<result_t>(s, res.event_create, res.event_create_moved, res.event_modify, std::move(moved_to));
	}
};

using file_watcher_state_t = file_watcher::result_t::state_t;

struct config_t
{
	std::string log_path;
	const std::chrono::time_zone* logs_timezone;
	std::uint64_t guild_id;
	std::string status_0, status_1, status_multi;
	bool windows_notify_on_last_write;
};

template<std::size_t size>
struct string_literal_wrapper
{
	const std::array<char, size> arr;
	constexpr string_literal_wrapper(const char(&str)[size]) : arr(std::to_array(str)) {}
	constexpr const char* c_str() const { return arr.data(); }
	constexpr std::string_view sv() const { return std::string_view(arr.begin(), arr.end() - 1); }
};

// @throws runtime_error on error
template<typename T, string_literal_wrapper T_name>
T get_config_key(const jsoncons::json& config, std::string_view key)
{
	const auto value = config.at(key);
	if (!value.is<T>())
	{
		std::ostringstream ss;
		ss << "Expected " << key << " to be type " << T_name.sv() << ", got " << value.type();  // json_type only has operator<< output, no formatter or string conversion
		throw std::runtime_error(ss.str());
	}
	return value.as<T>();
}

// @throws runtime_error on error
template<typename T, string_literal_wrapper T_name>
T get_optional_config_key(const jsoncons::json& config, std::string_view key, T default_val = {})
{
	const auto& value_it = config.find(key);
	if (value_it != config.object_range().end())
	{
		const auto& value = value_it->value();
		if (!value.is<T>())
		{
			std::ostringstream ss;
			ss << "Expected " << key << " to be type " << T_name.sv() << ", got " << value.type();
			throw std::runtime_error(ss.str());
		}
		return value.as<T>();
	}
	else
	{
		std::cout << "WARNING: " << key << " not found in config (using default)" << std::endl;
		return default_val;
	}
}

// will call std::exit(-1) if parsing fails
// @param bot  optional of bot to initialize (needs to be optional ref param because it has no default constructor and is immovable)
[[nodiscard]] static inline config_t parse_config(std::optional<dpp::cluster>& bot)
{
	std::string log_path, status_0, status_1, status_multi;
	const std::chrono::time_zone* logs_timezone;
	std::uint64_t guild_id;
	bool windows_notify_on_last_write;
	try
	{
		std::ifstream fin("qc-v2-config.txt");
		const jsoncons::json config = jsoncons::json::parse(fin);
		fin.close();

		log_path = get_config_key<std::string, "string">(config, "log_path");
		guild_id = get_config_key<std::uint64_t, "uint64">(config, "guild_id");
		status_0 = get_optional_config_key<std::string, "string">(config, "status_empty");
		status_1 = get_optional_config_key<std::string, "string">(config, "status_one");
		status_multi = get_optional_config_key<std::string, "string">(config, "status_multi");
		std::string timezone = get_config_key<std::string, "string">(config, "logs_timezone");
		std::string token = get_config_key<std::string, "string">(config, "bot_token");
		windows_notify_on_last_write = get_optional_config_key<bool, "bool">(config, "windows_notify_on_last_write", false);

		if (!status_multi.empty())  // validate format string
		{
			try
			{
				std::size_t temp = 0;
				std::ignore = std::vformat(status_multi, std::make_format_args(temp));
			}
			catch (const std::format_error& e)
			{
				throw std::runtime_error(
					std::format("Formatting error for status_multi (use exactly one {{}} for number of players and {{{{, }}}} to escape braces): {}", e.what()));
			}
		}

		try
		{
			logs_timezone = std::chrono::locate_zone(timezone);
		}
		catch (const std::runtime_error& e)
		{
			throw std::runtime_error(std::format("Could not locate timezone \"{}\" (is it an IANA time zone ID?): {}", timezone, e.what()));
		}

		bot.emplace(std::move(token));

		return { log_path, logs_timezone, guild_id, status_0, status_1, status_multi, windows_notify_on_last_write };
	}
	catch (const std::exception& e)
	{
		std::cerr << "JSON parsing from qc-v2-config.txt failed: " << e.what() << std::endl;
		std::exit(-1);
	}
}

[[nodiscard]] static inline std::size_t get_num_players(const parse_ctx_t& parse_ctx)
{
	std::size_t num_players = 0;
	for (const auto& [cur_name, cur_info] : parse_ctx.player_info)
	{
		const auto& [uuid, join_time] = cur_info;
		if (join_time)
			{ num_players++; }
	}
	return num_players;
}

static inline void update_player_count(dpp::cluster& bot, const config_t& config, const parse_ctx_t& parse_ctx, std::size_t& last_player_count)
{
	auto new_player_count = get_num_players(parse_ctx);
	if (new_player_count != last_player_count)
	{
		last_player_count = new_player_count;
		std::string str;
		switch (new_player_count)
		{
		case 0:
			str = config.status_0;
			break;
		case 1:
			str = config.status_1;
			break;
		default:
			if (!config.status_multi.empty())
				{ str = std::vformat(config.status_multi, std::make_format_args(new_player_count)); }
			break;
		}
		dpp::presence ps;
		if (str.empty())
			{ ps = dpp::presence(dpp::ps_online, dpp::activity()); }
		else
			{ ps = dpp::presence(dpp::ps_online, dpp::at_game, str); }
		bot.set_presence(ps);
		bot.log(dpp::loglevel::ll_info, "changing presence");
	}
}

int main()
{
	std::optional<dpp::cluster> bot_;
	auto config = parse_config(bot_);
	auto& bot = bot_.value();

	bot.on_log(dpp::utility::cout_logger());
	
	bot.on_ready([&bot, &config](const dpp::ready_t& event)
	{
		if (dpp::run_once<decltype([]() {})>())
		{
			dpp::slashcommand command_graph("graph", "Create a graph of play times", bot.me.id);
			// TODO: make this a subcommand and allow specifying size for png?
			command_graph.add_option(dpp::command_option(dpp::co_string, "format", "File format of graph", false)
				.add_choice(dpp::command_option_choice("png", std::string("png")))
				.add_choice(dpp::command_option_choice("svg", std::string("svg"))));
			// false for light and true for dark
			command_graph.add_option(dpp::command_option(dpp::co_boolean, "dark", "Use dark theme for drawing graph labels and axes", false)
				.add_choice(dpp::command_option_choice("false", false))
				.add_choice(dpp::command_option_choice("true", true)));
			dpp::slashcommand command_players("players", "List online players", bot.me.id);
			bot.guild_bulk_command_create({ command_graph, command_players }, config.guild_id);
		}
	});

	std::vector<std::string> read_files;
	std::pair<log_data_t, parse_ctx_t> parse_data_ctx, persistent_data_ctx;
	auto& [parse_data, parse_ctx] = parse_data_ctx;
	std::mutex parse_data_ctx_mutex;
	std::size_t last_player_count = 0;

	parse_data_ctx_mutex.lock();

	// next available time point when the graph command can be called
	std::chrono::system_clock::time_point graph_command_next_tp;
	std::mutex next_tp_mutex;
	
	// this slash command handler will only ever read from parse_data (not write)
	bot.on_slashcommand([&](const dpp::slashcommand_t& event) -> dpp::task<void>
	{
		using namespace std::string_view_literals;
		const auto cmd_name = event.command.get_command_name();
		if (cmd_name == "graph"sv)
		{
			// rate limit
			{
				std::scoped_lock(next_tp_mutex);
				const auto now = std::chrono::system_clock::now();
				if (now >= graph_command_next_tp)
					{ graph_command_next_tp = now + std::chrono::seconds(60); }  // TODO: configurable rate limit
				else
				{
					event.reply(dpp::message(std::format("Last graph was generated recently, please try again <t:{:%Q}:R>",
						std::chrono::ceil<std::chrono::seconds>(graph_command_next_tp.time_since_epoch()))).set_flags(dpp::m_ephemeral));
					co_return;
				}
			}
			dpp::async thinking = event.co_thinking(false);
			
			// format is png by default
			const auto format_param = event.get_parameter("format");
			const std::string* format_str_ptr = std::get_if<std::string>(&format_param);
			const std::string_view format = (format_str_ptr == nullptr) ? "png"sv : *format_str_ptr;

			const auto dark_param = event.get_parameter("dark");
			const bool* dark_ptr = std::get_if<bool>(&dark_param);
			const std::string_view color = (dark_ptr != nullptr && *dark_ptr) ? "white" : "black";  // white text for darkmode and dark text otherwise

			const bool format_is_svg = (format == "svg"sv);
			const std::string_view file_mime_type = format_is_svg ? "image/svg+xml"sv : "image/png"sv;
			const std::string_view filename = format_is_svg ? "graph.svg"sv : "graph.png"sv;
			std::string file_contents;
			{
				// TODO: allow user to specify date range
				// TODO: set last date to current time instead of last player time
				std::scoped_lock lock(parse_data_ctx_mutex);
				std::cout << "INFO: Creating graph in " << (format_is_svg ? "svg"sv : "png"sv) << " format" << std::endl;
				if (format_is_svg)
					{ file_contents = create_graph<true, false>(parse_data, parse_ctx, color); }
				else
					{ file_contents = create_graph<false, true>(parse_data, parse_ctx, color); }
				std::cout << "INFO: Finished creating graph" << std::endl;
			}
			
			co_await thinking;
			event.edit_original_response(dpp::message().add_file(filename, file_contents, file_mime_type));
		}
		else if (cmd_name == "players"sv)
		{
			dpp::async thinking = event.co_thinking(false);

			std::string msg;
			std::size_t num_players = 0;
			{
				std::scoped_lock lock(parse_data_ctx_mutex);
				for (const auto& [cur_name, cur_info] : parse_ctx.player_info)
				{
					const auto& [uuid, join_time] = cur_info;
					if (join_time)
					{
						msg += cur_name;
						msg += ", ";
						num_players++;
					}
				}
				if (num_players == 0)
					{ msg = "No players online"; }
				else
				{
					msg.resize(msg.size() - 2);  // remove final ", "
					msg = std::format("**{} players online:** {}", num_players, msg);
				}
			}
			co_await thinking;
			event.edit_original_response(dpp::message(msg));
		}
	});
	
	// bot.start will block in 10.0.35, even with dpp::st_return
	std::thread([&]() { bot.start(dpp::st_return); }).detach();
	
	std::cout << "INFO: Performing initial parse" << std::endl;

	parse_data_ctx = parse_logs<true, true>(config.log_path, config.logs_timezone, [&](const auto& p) { read_files.emplace_back(log_filename_no_ext(p)); });
	persistent_data_ctx = parse_data_ctx;
	
	const std::filesystem::path latest_log = std::filesystem::path(config.log_path) / "latest.log";
#ifdef _WIN32
#define FILE_WATCHER_USER_DATA &(config.windows_notify_on_last_write)
#else
#define FILE_WATCHER_USER_DATA nullptr
#endif
	file_watcher watcher(config.log_path.c_str(), latest_log.filename().string(), FILE_WATCHER_USER_DATA);
#undef FILE_WATCHER_USER_DATA

	const auto update_date_tp = [&](bool latest_log_exists)
	{
		if (latest_log_exists)
			{ parse_ctx.date_tp = file_modification_date(latest_log, config.logs_timezone); }
	};

	std::uintmax_t prev_size = 0;
	// parse latest.log initially
	{
		const bool latest_log_exists = std::filesystem::exists(latest_log);
		const auto size = latest_log_exists ? std::filesystem::file_size(latest_log) : 0;
		update_date_tp(latest_log_exists);
		if (size > 0)
		{
			std::ifstream fin(latest_log, std::ios::binary);
			std::string s;
			s.resize_and_overwrite(size, [&fin](char* buf, std::size_t buf_size)
			{
				fin.read(buf, buf_size);
				return buf_size;
			});
			fin.close();
			parse_lines(std::move(s), parse_ctx, parse_data);
		}
	}
	update_player_count(bot, config, parse_ctx, last_player_count);

	std::cout << "INFO: Finished initial parse" << std::endl;
	parse_data_ctx_mutex.unlock();

	while (true)
	{
		auto res = watcher.poll();
		if (!res)
		{
			// TODO: handle error (close and reopen watcher?)
			std::cerr << "FATAL ERROR: Could not poll for changes in directory" << std::endl;
			return -1;
		}
		if (res->state == file_watcher_state_t::data_read)
		{
			if (res->event_create)
			{
				update_date_tp(std::filesystem::exists(latest_log));
				prev_size = 0;
			}

			if (res->event_create_moved)
			{
				std::scoped_lock lock(parse_data_ctx_mutex);
				const bool latest_log_exists = std::filesystem::exists(latest_log);
				const auto size = latest_log_exists ? std::filesystem::file_size(latest_log) : 0;
				update_date_tp(latest_log_exists);
				if (size > 0)
				{
					std::cout << "WARNING: latest.log shouldn't be moved to (from another file), discarding data and reading entirely" << std::endl;
					parse_data_ctx = persistent_data_ctx;
					std::ifstream fin(latest_log, std::ios::binary);
					std::string s;
					s.resize_and_overwrite(size, [&fin](char* buf, std::size_t buf_size)
					{
						fin.read(buf, buf_size);
						return buf_size;
					});
					fin.close();
					parse_lines(std::move(s), parse_ctx, parse_data);
					update_player_count(bot, config, parse_ctx, last_player_count);
				}
				prev_size = size;
			}

			if (res->event_modify)
			{
				std::scoped_lock lock(parse_data_ctx_mutex);
				const auto size = std::filesystem::exists(latest_log) ? std::filesystem::file_size(latest_log) : 0;
				bool do_update = false;
				if (size < prev_size)
				{
					std::cout << "WARNING: latest.log shrunk somehow, discarding data and re-reading from start" << std::endl;
					prev_size = 0;
					parse_data_ctx = persistent_data_ctx;
					do_update = true;
				}
				if (size > 0 && size > prev_size)
				{
					std::ifstream fin(latest_log, std::ios::binary);
					fin.seekg(prev_size, std::ios::beg);
					std::string s;
					s.resize_and_overwrite(size - prev_size, [&fin](char* buf, std::size_t buf_size)
					{
						fin.read(buf, buf_size);
						return buf_size;
					});
					fin.close();
					if (parse_lines(std::move(s), parse_ctx, parse_data) || do_update)
						{ update_player_count(bot, config, parse_ctx, last_player_count); }
				}
				prev_size = size;
			}

			if (res->moved_to)
			{
				std::string_view moved_to(res->moved_to->first.get(), res->moved_to->second);
				if (!moved_to.ends_with(".log"))
				{
					std::cout << "WARNING: latest.log was moved to file with unexpected extension (expected .log), ignoring: " << moved_to << std::endl;
				}
				else
				{
					// remove extension
					read_files.emplace_back(moved_to.substr(0, moved_to.size() - 4));
					// "commit" latest.log data/ctx to persistent
					// no need to lock because we aren't writing to parse_data_ctx
					persistent_data_ctx = parse_data_ctx;
				}
				prev_size = 0;
			}
		}
		else if (res->state == file_watcher_state_t::no_data)  // don't sleep if state is read_more; read more immediately
			{ std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
	}
}
