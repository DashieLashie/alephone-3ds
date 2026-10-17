/*
	Aleph One for the Nintendo 3DS, citro3d back end: debug-c3d parser (see config.h).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "config.h"

#include <sstream>

namespace a1_c3d {

namespace {

bool parse_flag(const std::string& value, bool& out)
{
	if (value == "0" || value == "1")
	{
		out = value == "1";
		return true;
	}
	return false;
}

// An integer in [lo, hi], digits only.
bool parse_int(const std::string& value, long lo, long hi, long& out)
{
	if (value.empty() || value.size() > 9 || value.find_first_not_of("0123456789") != std::string::npos)
		return false;
	const long v = std::stol(value);
	if (v < lo || v > hi)
		return false;
	out = v;
	return true;
}

} // namespace

bool config_apply(Config& cfg, const std::string& key, const std::string& value)
{
	if (key == "present")
	{
		if (value != "cpu" && value != "gpu")
			return false;
		cfg.present_gpu = value == "gpu";
		return true;
	}
	if (key == "calibrate")
		return parse_flag(value, cfg.calibrate);
	if (key == "alt_load")
		return parse_flag(value, cfg.alt_load);
	if (key == "odd_surface")
		return parse_flag(value, cfg.odd_surface);
	if (key == "test_vhalt")
	{
		long v = 0;
		if (!parse_int(value, 0, 3, v))
			return false;
		cfg.test_vhalt = static_cast<int>(v);
		return true;
	}
	return false;
}

void config_parse(Config& cfg, const std::string& text, std::vector<std::string>& problems)
{
	std::istringstream in(text);
	std::string line;
	while (std::getline(in, line))
	{
		line = line.substr(0, line.find('#'));
		std::istringstream ls(line);
		std::string key, value;
		if (!(ls >> key))
			continue;
		ls >> value;
		if (!config_apply(cfg, key, value))
			problems.push_back("ignored \"" + key + " " + value + "\"");
	}
}

} // namespace a1_c3d
