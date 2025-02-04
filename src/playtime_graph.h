#ifndef PLAYTIME_GRAPH_H
#define PLAYTIME_GRAPH_H

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <ranges>
#include <utility>
#include <vector>

#include <lunasvg.h>

#include "parse_logs.h"

/*
SVG layout:
 ____________________________________________________
|              |                           |         |
|              |                           |         |
|              |                           |  total  |
| player names |        data bars          |  play   |
|              |                           |  time   |
|              |                           |         |
|______________|___________________________|_________|
|                      dates                         |
|____________________________________________________|
*/


inline constexpr double svg_width = 2000;
inline constexpr double svg_fontsize = 25;
inline constexpr double svg_date_fontsize = 20;
inline constexpr double svg_bar_height = 25;
inline constexpr double svg_bar_stride = 50;
inline constexpr double svg_pad = svg_bar_height / 2;
inline constexpr double svg_side_pad = 15;

namespace detail
{
	inline constexpr std::uint64_t uint64_phi = 0x9e3779b97f4a7c15; // 2^64 / phi (golden ratio), rounded down to odd

	inline std::uint64_t hash_rrmxmx(std::uint64_t x, std::uint64_t gamma = uint64_phi)
	{
		x += gamma;
		x ^= std::rotr(x, 49) ^ std::rotr(x, 24);
		x *= 0x9FB21C651E98DF25ULL;
		x ^= x >> 28;
		x *= 0x9FB21C651E98DF25ULL;
		return x ^ (x >> 28);
	}

	inline std::uint64_t hash_combine(std::uint64_t lhs, std::uint64_t rhs)
	{
		return hash_rrmxmx(hash_rrmxmx(lhs) + rhs);
	}

	// assumes s != 0
	// @return rgb values in 0-255
	inline std::tuple<int, int, int> hsl2rgb(double h, double s, double l)
	{
		const auto f = [h, s, l](int n)
		{
			const double a = s * std::min(l, 1 - l);
			const double k = std::fmod((n + h * 12), 12);
			return l - a * std::clamp<double>(std::min(k - 3, 9 - k), -1, 1);
		};
		return { std::lround(f(0) * 255), std::lround(f(8) * 255), std::lround(f(4) * 255) };
	}

	// generate random but deterministic color from uuid
	inline std::string get_rgb_hex_from_uuid(uuid_t uuid)
	{
		std::uint64_t n = hash_combine(uuid.first, uuid.second);
		constexpr double max = std::numeric_limits<std::uint64_t>::max();
		const double hue = n / max;                     // [0, 1]
		n = hash_rrmxmx(n);
		const double saturation = 0.4 * n / max + 0.4;  // [0.4, 0.8]
		n = hash_rrmxmx(n);
		const double lightness = 0.5 * n / max + 0.25;  // [0.25, 0.75]
		const auto [red, green, blue] = hsl2rgb(hue, saturation, lightness);
		return std::format("#{:02X}{:02X}{:02X}", red, green, blue);
	}

	// @throws std::runtime_error if svg loading fails
	inline lunasvg::Box get_svg_bbox(std::string_view svg_data)
	{
		const auto svg_doc = lunasvg::Document::loadFromData(svg_data.data(), svg_data.size());
		if (!svg_doc)
			{ throw std::runtime_error(std::format("Temporary SVG Document loading failed. SVG data:\n{}", svg_data)); }
		return svg_doc->boundingBox();
	}

	// @return data height
	template<typename Duration, typename Duration2>
	inline double add_player_names(auto&& add, const auto& log_info, std::chrono::sys_time<Duration>& first_time, std::chrono::sys_time<Duration2>& last_time, std::string_view color)
	{
		std::size_t ind = 0;
		double data_height = 0;
		for (const auto& [_, info] : log_info)
		{
			const auto& [names, play_info] = info;
			add(std::format("<text x=\"{}\" y=\"{}\" font-size=\"{}\" font-family=\"monospace\" fill=\"{}\" text-anchor=\"end\" dominant-baseline=\"middle\">{}</text>\n",
				-svg_pad, svg_bar_height / 2 + svg_bar_stride * ind, svg_fontsize, color, names.back()));
			data_height += svg_bar_stride;

			for (const auto& [time, dur] : play_info.first)
			{
				first_time = std::min(first_time, time);
				last_time = std::max(last_time, time + dur);
			}
			ind++;
		}
		return data_height;
	}

	// @return x coordinate of center of rightmost date, or -1 if no dates were present
	template<typename Duration, typename Duration2>
	inline double add_dates(auto&& add, std::chrono::sys_time<Duration> first_time, std::chrono::sys_time<Duration2> last_time,
		double data_height, double data_area_width, std::string_view color)
	{
		const auto* target_tz = std::chrono::locate_zone("US/Pacific");
		const auto first_time_local = target_tz->to_local(first_time);
		const auto last_time_local = target_tz->to_local(last_time);
		const auto first_day = std::chrono::ceil<std::chrono::days>(first_time_local);
		const auto last_day = std::chrono::floor<std::chrono::days>(last_time_local);
		const auto num_day_intervals = (last_day - first_day).count();
		const auto date_interval = (num_day_intervals - 1) / 10 + 1;  // divide by 10, rounding up
		const std::chrono::duration<double> total_dur = last_time - first_time;
		// TODO: instead of doing this, maybe we should just add as many as we can that are still within bounds
		double last_date_x = -1;
		for (int i = 0; i < 10; i++)
		{
			const auto cur_date = first_day + std::chrono::days(i * date_interval);
			if (cur_date > last_day)
				{ break; }
			const double cur_x = (cur_date - first_time_local) / total_dur * data_area_width;
			add(std::format("<text x=\"{}\" y=\"{}\" font-size=\"{}\" fill=\"{}\" text-anchor=\"middle\" dominant-baseline=\"hanging\">{:%m/%d/%Y}</text>\n",
				cur_x, data_height + svg_pad, svg_date_fontsize, color, cur_date));
			last_date_x = cur_x;
		}
		return last_date_x;
	}

	inline void add_data_labels(auto&& add, const auto& log_info, double data_area_width, std::string_view color)
	{
		std::size_t ind = 0;
		for (const auto& [_, info] : log_info)
		{
			add(std::format("<text x=\"{}\" y=\"{}\" font-size=\"{}\" font-family=\"monospace\" fill=\"{}\" text-anchor=\"end\" dominant-baseline=\"middle\">{:%H:%M:%S}</text>",
				data_area_width, svg_bar_height / 2 + svg_bar_stride * ind, svg_fontsize, color, std::chrono::round<std::chrono::seconds>(info.second.second)));
			ind++;
		}
	}

	template<typename Duration, typename Duration2>
	inline void add_data_bars(auto&& add, const auto& log_info, std::chrono::sys_time<Duration> first_time, std::chrono::sys_time<Duration2> last_time, double data_area_width)
	{
		std::size_t ind = 0;
		const std::chrono::duration<double> total_dur = last_time - first_time;
		for (const auto& [uuid, info] : log_info)
		{
			const auto& [_, play_info] = info;
			const std::string color = get_rgb_hex_from_uuid(uuid);

			for (const auto& [time, dur] : play_info.first)
			{
				add(std::format("<rect x=\"{}\" y=\"{}\" width=\"{}\" height=\"{}\" fill=\"{}\"/>\n",
					(time - first_time) / total_dur * data_area_width, svg_bar_stride * ind, dur / total_dur * data_area_width, svg_bar_height, color));
			}
			ind++;
		}
	}
	
	// pointer to temporary buffer for png data when rendering svg to png
	// (lunasvg png callback is c function pointer so capturing lambda wouldn't work)
	inline std::string* png_data_ptr = nullptr;

	// @param log_info  sorted/ordered vector
	template<bool return_svg, bool render_to_png>
	inline auto create_graph(const auto& log_info, std::string_view color, std::chrono::system_clock::time_point now = std::chrono::system_clock::now())
	{
		// we will delay adding the <svg> to our main svg_data because the viewBox needs to be calculated
		std::string svg_data;
		// these are just placeholders for bbox calculation so we don't actually need to specify a size
		constexpr std::string_view placeholder_svg_header = "<svg xmlns=\"http://www.w3.org/2000/svg\">\n", svg_footer = "</svg>";
		std::string placeholder_svg(placeholder_svg_header);
		std::chrono::system_clock::time_point first_time = now, last_time;

		const auto reset_placeholder_svg = [&]() { placeholder_svg = std::string(placeholder_svg_header); };


		// y-axis
		const double data_height = detail::add_player_names([&](std::string_view cur_line) { svg_data += cur_line; placeholder_svg += cur_line; },
			log_info, first_time, last_time, color);

		double text_width;
		{
			placeholder_svg += svg_footer;
			const auto [x, y, w, h] = detail::get_svg_bbox(placeholder_svg);
			text_width = std::ceil(w / 2.5) * 2.5;  // round up to multiple of 2.5
			reset_placeholder_svg();
		}

		double data_area_width = svg_width - (text_width + svg_pad);

		// calculate size for date labels (x-axis)
		double date_height = 0;
		{
			// TODO: specify target time zone in config or something
			// measure width first so we know how much to shrink our data area by
			const double last_date_x = detail::add_dates([&](std::string_view cur_line) { placeholder_svg += cur_line; },
				first_time, last_time, data_height, data_area_width, color);

			if (last_date_x != -1)
			{
				svg_data += std::format("<line x1=\"0\" y1=\"{0}\" x2=\"{1}\" y2=\"{0}\" stroke=\"{2}\" stroke-width=\"2\"/>\n", data_height, data_area_width, color);

				placeholder_svg += svg_footer;
				const auto [x, y, w, h] = detail::get_svg_bbox(placeholder_svg);
				if (x + w > data_area_width)
				{
					const double last_date_half_width = (x + w) - last_date_x;
					// last_date_x * mult + last_date_half_width = data_area_width
					data_area_width *= (data_area_width - last_date_half_width) / last_date_x;
					data_area_width = std::floor(data_area_width / 2.5) * 2.5;  // round down to multiple of 2.5
				}
				reset_placeholder_svg();

				date_height = svg_pad + h;
				data_area_width = std::ceil(data_area_width / 2.5) * 2.5;  // round up to multiple of 2.5
			}

		}


		svg_data = std::format("<svg width=\"{0}\" height=\"{1}\" viewBox=\"{2} {3} {0} {1}\" xmlns=\"http://www.w3.org/2000/svg\">\n",
			svg_width + 2 * svg_side_pad, data_height + date_height + 2 * svg_side_pad, -(text_width + svg_pad) - svg_side_pad, -svg_side_pad) + svg_data;

		double data_labels_width;
		{
			detail::add_data_labels([&](std::string_view cur_line) { placeholder_svg += cur_line; }, log_info, data_area_width, color);
			placeholder_svg += svg_footer;
			const auto [x, y, w, h] = detail::get_svg_bbox(placeholder_svg);
			data_labels_width = w;
		}

		// actually add x-axis labels
		detail::add_dates([&](std::string_view cur_line) { svg_data += cur_line; }, first_time, last_time,
			data_height, data_area_width - (data_labels_width + svg_pad), color);

		// add data labels (hhh:mm:ss)
		detail::add_data_labels([&](std::string_view cur_line) { svg_data += cur_line; }, log_info, data_area_width, color);

		// data bars
		detail::add_data_bars([&](std::string_view cur_line) { svg_data += cur_line; }, log_info, first_time, last_time, data_area_width - (data_labels_width + svg_pad));

		svg_data += svg_footer;


		if constexpr (render_to_png)
		{
			const auto svg_doc = lunasvg::Document::loadFromData(svg_data.data(), svg_data.size());
			if (!svg_doc)
				{ throw std::runtime_error(std::format("SVG Document loading failed. SVG data:\n{}", svg_data)); }
			const auto svg_bmp = svg_doc->renderToBitmap(svg_doc->width() * 2, svg_doc->height() * 2);
			if (svg_bmp.isNull())
				{ throw std::runtime_error("SVG rendering to bitmap failed."); }
		
			// expects function pointer so capturing lambda won't work
			std::string png_data;
			detail::png_data_ptr = &png_data;
			const auto b = svg_bmp.writeToPng([](void* /*unused*/, void* data, int size) { (*detail::png_data_ptr).append(static_cast<const char*>(data), size); }, nullptr);
			detail::png_data_ptr = nullptr;  // for sanity, I guess
			if (!b)
				{ throw std::runtime_error("Bitmap writing to PNG failed."); }
		
			if constexpr (return_svg)  // svg and png
				{ return std::make_pair(svg_data, png_data); }
			else  // png only
				{ return png_data; }
		}
		else  // svg only
			{ return svg_data; }
	}
}

// @tparam return_svg  whether to return svg data
// @tparam render_to_png  whether to return rendered png data
// @throws std::runtime_error if bounding box calculation or png rendering fails
// @return pair of svg and png data if return_svg and render_to_png are both true, otherwise return single string containing data
template<bool return_svg = true, bool render_to_png = false>
inline auto create_graph(const log_data_t& parse_data, std::string_view color = "black")
{
	static_assert(return_svg || render_to_png, "Graph must be either saved to svg or png");
	
	std::vector<std::pair<log_data_t::key_type, log_data_t::mapped_type>> log_info(parse_data.begin(), parse_data.end());
	std::ranges::sort(log_info, std::ranges::greater(), [](const auto& elem) { return elem.second.second.second; });
	return detail::create_graph<return_svg, render_to_png>(log_info, color);
}

// this overload will ensure currently online players are accounted for; the graph will extend to the current time (when the function is called)
// @tparam return_svg  whether to return svg data
// @tparam render_to_png  whether to return rendered png data
// @throws std::runtime_error if trying to remove unknown player, bounding box calculation fails, or png rendering fails
// @return pair of svg and png data if return_svg and render_to_png are both true, otherwise return single string containing data
template<bool return_svg = true, bool render_to_png = false>
inline auto create_graph(const log_data_t& parse_data, const parse_ctx_t& parse_ctx, std::string_view color = "black")
{
	static_assert(return_svg || render_to_png, "Graph must be either saved to svg or png");

	std::vector<std::pair<log_data_t::key_type, log_data_t::mapped_type>> log_info(parse_data.begin(), parse_data.end());
	// make currently online players leave
	const auto now = std::chrono::system_clock::now();
	for (const auto& [cur_name, cur_info] : parse_ctx.player_info)
	{
		const auto& [uuid, join_time] = cur_info;
		if (uuid && join_time)
		{
			// log_info should already be sorted by uuid since parse_data is a sorted map
			const auto it = std::ranges::lower_bound(log_info, uuid.value(), {}, [](const auto& elem) { return elem.first; });
			if (it == log_info.end())
				{ throw std::runtime_error(std::format("Could not find UUID {} in parse_data while creating graph", uuid.value())); }
			auto& [names, play_info] = (*it).second;
			if (names.empty() || names.back() != cur_name)
				{ names.emplace_back(cur_name); }
			const auto playtime = now - join_time.value();
			auto& [play_sessions, total_playtime] = play_info;
			play_sessions.emplace_back(join_time.value(), playtime);
			total_playtime += playtime;
		}
	}
	std::ranges::sort(log_info, std::ranges::greater(), [](const auto& elem) { return elem.second.second.second; });
	return detail::create_graph<return_svg, render_to_png>(log_info, color, now);
}

#endif
