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

// An integer in [lo, hi] with an optional leading '-'.
bool parse_signed(const std::string& value, long lo, long hi, int& out)
{
	const bool neg = !value.empty() && value[0] == '-';
	long v = 0;
	if (!parse_int(neg ? value.substr(1) : value, 0, 1000000, v))
		return false;
	if (neg)
		v = -v;
	if (v < lo || v > hi)
		return false;
	out = static_cast<int>(v);
	return true;
}

bool parse_unsigned(const std::string& value, long lo, long hi, unsigned& out)
{
	long v = 0;
	if (!parse_int(value, lo, hi, v))
		return false;
	out = static_cast<unsigned>(v);
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
		if (!parse_int(value, 0, 4, v))
			return false;
		cfg.test_vhalt = static_cast<int>(v);
		return true;
	}
	if (key == "present_diff")
	{
		long v = 0;
		if (!parse_int(value, 0, 100000, v))
			return false;
		cfg.present_diff = static_cast<unsigned>(v);
		return true;
	}
	if (key == "suspend_gate")
		return parse_flag(value, cfg.suspend_gate);
	if (key == "force_abandon")
		return parse_flag(value, cfg.force_abandon);
	if (key == "close_frame")
		return parse_flag(value, cfg.close_frame);
	if (key == "two_frame")
		return parse_flag(value, cfg.two_frame);
	if (key == "renderer")
	{
		if (value != "sw" && value != "c3d")
			return false;
		cfg.renderer_c3d = value == "c3d";
		return true;
	}
	if (key == "mode")
	{
		if (value == "null")
			cfg.mode = Mode::Null;
		else if (value == "readback")
			cfg.mode = Mode::Readback;
		else if (value == "direct")
			cfg.mode = Mode::Direct;
		else
			return false;
		return true;
	}
	if (key == "diff")
	{
		long v = 0;
		if (!parse_int(value, 0, 100000, v))
			return false;
		cfg.diff = static_cast<unsigned>(v);
		return true;
	}
	if (key == "diff_frames")
	{
		long v = 0;
		if (!parse_int(value, 0, 1000000, v))
			return false;
		cfg.diff_frames = static_cast<unsigned>(v);
		return true;
	}
	if (key == "classes")
	{
		uint32_t mask = 0;
		if (!parse_classes(value, mask))
			return false;
		cfg.classes = mask;
		return true;
	}
	if (key == "fullbright")
		return parse_flag(value, cfg.fullbright);
	if (key == "dump")
		return parse_flag(value, cfg.dump);
	if (key == "readback_cal")
		return parse_flag(value, cfg.readback_cal);
	if (key == "readback_cal_resize")
		return parse_flag(value, cfg.readback_cal_resize);
	if (key == "zero_copy")
		return parse_flag(value, cfg.zero_copy);
	if (key == "retire_wait_ms")
	{
		long v = 0;
		if (!parse_int(value, 0, 600000, v))
			return false;
		cfg.retire_wait_ms = static_cast<unsigned>(v);
		return true;
	}
	if (key == "cmdbuf_kib")
		return parse_unsigned(value, 16, 4096, cfg.cmdbuf_kib);
	if (key == "gxq_budget")
		return parse_unsigned(value, 1, 24, cfg.gxq_budget);
	if (key == "ring_vertices")
		return parse_unsigned(value, 64, 32768, cfg.ring_vertices);
	if (key == "ring_indices")
		return parse_unsigned(value, 64, 65536, cfg.ring_indices);
	if (key == "reserve_extra")
		return parse_unsigned(value, 0, 1000000, cfg.reserve_extra);
	if (key == "textures")
		return parse_flag(value, cfg.textures);
	if (key == "uv_half")
		return parse_flag(value, cfg.uv_half);
	if (key == "diff_tol")
		return parse_unsigned(value, 0, 255, cfg.diff_tol);
	if (key == "vram_cap")
	{
		if (!parse_unsigned(value, 0, 4096, cfg.vram_cap))
			return false;
		cfg.vram_cap_set = true;
		return true;
	}
	if (key == "linear_cap")
		return parse_unsigned(value, 64, 16384, cfg.linear_cap);
	if (key == "promote_max")
		return parse_unsigned(value, 0, 8, cfg.promote_max);
	if (key == "lit")
		return parse_flag(value, cfg.lit);
	if (key == "alpha_x2")
		return parse_flag(value, cfg.alpha_x2);
	if (key == "play")
		return parse_flag(value, cfg.play);
	if (key == "play_check")
		return parse_unsigned(value, 0, 100000, cfg.play_check);
	if (key == "rect_bias")
		return parse_unsigned(value, 0, 65536, cfg.rect_bias);
	if (key == "fail_every")
		return parse_unsigned(value, 0, 100000, cfg.fail_every);
	if (key == "landscape_tol")
		return parse_unsigned(value, 0, 255, cfg.landscape_tol);
	if (key == "land_cut")
		return parse_unsigned(value, 64, 512, cfg.land_cut);
	if (key == "quiet")
		return parse_flag(value, cfg.quiet);
	if (key == "overlays")
		return parse_unsigned(value, 0, 2, cfg.overlays);
	if (key == "direct_check")
		return parse_unsigned(value, 0, 100000, cfg.direct_check);
	if (key == "overlay_audit")
		return parse_unsigned(value, 0, 100000, cfg.overlay_audit);
	if (key == "seed_check")
		return parse_flag(value, cfg.seed_check);
	if (key == "band_from_end")
		return parse_flag(value, cfg.band_from_end);
	if (key == "view_v_flip")
		return parse_flag(value, cfg.view_v_flip);
	if (key == "g3a")
		return parse_flag(value, cfg.g3a);
	if (key == "reseed")
		return parse_unsigned(value, 0, 100000, cfg.reseed);
	if (key == "sw_every")
		return parse_unsigned(value, 0, 100000, cfg.sw_every);
	if (key == "fades")
		return parse_flag(value, cfg.fades);
	if (key == "fade_tol")
		return parse_unsigned(value, 0, 32, cfg.fade_tol);
	if (key == "fade_parts")
		return parse_unsigned(value, 0, 7, cfg.fade_parts);
	if (key == "stereo")
		return parse_flag(value, cfg.stereo);
	if (key == "stereo_k")
		return parse_unsigned(value, 0, 512, cfg.stereo_k);
	if (key == "stereo_w0")
		return parse_unsigned(value, 64, 32767, cfg.stereo_w0);
	if (key == "stereo_near")
		return parse_unsigned(value, 0, 400, cfg.stereo_near);
	if (key == "stereo_weapon")
		return parse_signed(value, -100, 100, cfg.stereo_weapon);
	if (key == "stereo_ramp")
		return parse_unsigned(value, 1, 600, cfg.stereo_ramp);
	if (key == "stereo_fill")
		return parse_flag(value, cfg.stereo_fill);
	if (key == "stereo_slider")
		return parse_signed(value, -1, 100, cfg.stereo_slider);
	if (key == "stereo_force")
		return parse_flag(value, cfg.stereo_force);
	if (key == "stereo_cal")
		return parse_flag(value, cfg.stereo_cal);
	if (key == "stereo_check")
		return parse_unsigned(value, 0, 100000, cfg.stereo_check);
	if (key == "stereo_gaps")
		return parse_flag(value, cfg.stereo_gaps);
	if (key == "stereo_edge")
		return parse_flag(value, cfg.stereo_edge);
	if (key == "stereo_fail")
		return parse_flag(value, cfg.stereo_fail);
	if (key == "fallback")
	{
		if (value != "readback" && value != "sw")
			return false;
		cfg.fallback_sw = value == "sw";
		return true;
	}
	if (key == "film")
	{
		if (value.empty())
			return false;
		cfg.film = value;
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
		if (key == "film")   // a path: the rest of the line, trimmed (names have spaces)
		{
			std::getline(ls, value);
			const std::string::size_type b = value.find_first_not_of(" \t\r\n");
			const std::string::size_type e = value.find_last_not_of(" \t\r\n");
			value = b == std::string::npos ? std::string() : value.substr(b, e - b + 1);
		}
		else
			ls >> value;
		if (!config_apply(cfg, key, value))
			problems.push_back("ignored \"" + key + " " + value + "\"");
	}
}

bool config_apply_preference(Config& cfg, bool gpu)
{
	const bool changed = cfg.renderer_c3d != gpu;
	cfg.renderer_c3d = gpu;
	cfg.stereo = gpu;   // M9 (D1): the GPU renderer turns stereo on; the 3D slider is the control
	if (gpu)
	{
		cfg.mode = Mode::Direct;
		cfg.fallback_sw = true;
	}
	return changed;
}

} // namespace a1_c3d
