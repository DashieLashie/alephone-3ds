/*
	Copyright (C) 2026 and beyond by the "Aleph One" developers.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	This license is contained in the file "COPYING",
	which is included with this source code; it is available online at
	http://www.gnu.org/licenses/gpl.html
*/

#ifndef A1_FILESYSTEM_H
#define A1_FILESYSTEM_H

// The subset of boost::filesystem the engine uses, as namespace a1fs.
// Defining A1_FS_STD (configure --enable-std-filesystem) maps it onto
// std::filesystem instead, so the compiled boost_filesystem library is not
// needed. Keep calls to the error_code overloads; nothing here throws.

#ifdef HAVE_CONFIG_H // every user must agree on A1_FS_STD (Plugins.h API)
#include "config.h"
#endif

#ifndef A1_FS_STD

#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

namespace a1fs {
using boost::filesystem::path;
using boost::filesystem::directory_iterator;
using boost::filesystem::create_directory;
using boost::filesystem::is_directory;
using boost::filesystem::last_write_time;
using boost::filesystem::remove;
using boost::filesystem::rename;
using boost::filesystem::unique_path;
using boost::filesystem::file_type;
constexpr file_type directory_file = boost::filesystem::directory_file;
constexpr file_type regular_file = boost::filesystem::regular_file;
using boost::system::error_code;
using boost::system::generic_category;
}

#else

#include <cerrno>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <random>
#include <string>
#ifdef A1_PLATFORM_3DS
#include "a1_3ds_platform.h"
#endif
#include <system_error>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace a1fs {
using std::filesystem::path;
using std::filesystem::directory_iterator;
using std::filesystem::create_directory;
using std::filesystem::is_directory;
using std::filesystem::remove;
using std::filesystem::rename;
using std::filesystem::file_type;
constexpr file_type directory_file = file_type::directory;
constexpr file_type regular_file = file_type::regular;
using std::error_code;
using std::generic_category;

// boost returns time_t, and (time_t)-1 on error
inline std::time_t last_write_time(const path& p, error_code& ec)
{
#ifndef _WIN32
	// st_mtime, exactly as boost does on POSIX; C++17 has no exact
	// file_time_type -> time_t conversion
	struct stat st;
	if (::stat(p.c_str(), &st) != 0)
	{
		ec.assign(errno, std::generic_category());
		return static_cast<std::time_t>(-1);
	}
	ec.clear();
	return st.st_mtime;
#else
	const auto ftime = std::filesystem::last_write_time(p, ec);
	if (ec) return static_cast<std::time_t>(-1);
	const auto stime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
		ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
	return std::chrono::system_clock::to_time_t(stime);
#endif
}

// boost::filesystem::unique_path: each '%' becomes a random hex digit
inline path unique_path(const path& model)
{
#ifdef A1_PLATFORM_3DS // std::random_device gives the same seed in every run
	static thread_local std::mt19937 rng{a1_3ds::Urng{}()};
#else
	static thread_local std::mt19937 rng{std::random_device{}()};
#endif
	std::string s = model.string();
	for (auto& c : s)
		if (c == '%')
			c = "0123456789abcdef"[rng() & 15];
	return s;
}
}

#endif

#endif
