#include "playtime_graph.h"

#include "parse_logs.h"

int main()
{
	const auto res = parse_logs("logs", std::chrono::locate_zone("UTC"));

	if (res.empty())
	{
		std::cerr << "FATAL ERROR: log parsing returned empty" << std::endl;
		return -1;
	}

	try
	{
		const auto [svg_data, png_data] = create_graph<true, true>(res);
		
		std::ofstream fout("graph.svg");
		fout << svg_data;
		fout.close();
		
		fout.open("graph.png", std::ios::binary);
		fout << png_data;
		fout.close();
	}
	catch (const std::runtime_error& e)
	{
		std::cerr << "ERROR: " << e.what() << std::endl;
		return -1;
	}
}
