/*
 
	Copyright (C) 2017 and beyond by Jeremiah Morris
	and the "Aleph One" developers.
 
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

#include "cstypes.h"
#include "cspaths.h"
#include "csstrings.h"
#include "alephversion.h"
#ifdef HAVE_CONFIG_H
#include "confpaths.h"
#endif

#if defined(__APPLE__) && defined(__MACH__)

char get_path_list_separator()
{
	return ':';
}

// other functions handled in cspaths.mm

#elif defined(__WIN32__)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <wchar.h>

char get_path_list_separator()
{
	return ';';
}

static std::string _get_local_data_path()
{
	static std::string local_dir = "";
	if (local_dir.empty())
	{
		wchar_t file_name[MAX_PATH];
		SHGetFolderPathW(NULL,
						CSIDL_PERSONAL | CSIDL_FLAG_CREATE,
						NULL,
						0,
						file_name);
		local_dir = wide_to_utf8(file_name) + "\\AlephOne";
	}
	return local_dir;
}

static std::string _get_default_data_path()
{
	static std::string default_dir = "";
	if (default_dir.empty())
	{
		wchar_t file_name[MAX_PATH];
		const DWORD r = GetModuleFileNameW(NULL, file_name, MAX_PATH); // can truncate
		if (r == 0 || r == MAX_PATH)
			return "";
		*wcsrchr(file_name, L'\\') = L'\0'; // cut off basename, leaving parent dir
		default_dir = wide_to_utf8(file_name);
	}
	return default_dir;
}

static std::string _get_prefs_path()
{
	static std::string prefs_dir = "";
	if (prefs_dir.empty())
	{
		wchar_t file_name[MAX_PATH];
		SHGetFolderPathW(NULL,
						CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE,
						NULL,
						0,
						file_name);
		prefs_dir = wide_to_utf8(file_name) + "\\AlephOne";
	}
	return prefs_dir;
}

static std::string _get_legacy_login_name()
{
	static std::string login_name = "";
	if (login_name.empty())
	{
		char login[17];
		DWORD len = 17;
		
		bool hasName = (GetUserNameA(login, &len) == TRUE);
		if (!hasName || strpbrk(login, "\\/:*?\"<>|") != NULL)
			strcpy(login, "Bob User");
		login_name = login;
	}
	return login_name;
}

std::string get_data_path(CSPathType type)
{
	std::string path = "";
	
	switch (type) {
		case kPathLocalData:
		case kPathLogs:
			path = _get_local_data_path();
			break;
		case kPathDefaultData:
			path = _get_default_data_path();
			break;
		case kPathBundleData:
			// not applicable
			break;
		case kPathPreferences:
			path = _get_prefs_path();
			break;
		case kPathLegacyData:
		case kPathLegacyPreferences:
			path = _get_default_data_path() + "\\Prefs\\" + _get_legacy_login_name();
			break;
		case kPathScreenshots:
			path = _get_local_data_path() + "\\Screenshots";
			break;
		case kPathSavedGames:
			path = _get_local_data_path() + "\\Saved Games";
			break;
		case kPathQuickSaves:
			path = _get_local_data_path() + "\\Quick Saves";
			break;
		case kPathImageCache:
			path = _get_local_data_path() + "\\Image Cache";
			break;
		case kPathRecordings:
			path = _get_local_data_path() + "\\Recordings";
			break;
	}
	return path;
}

std::string get_application_name()
{
	return std::string(A1_DISPLAY_NAME);
}

std::string get_application_identifier()
{
	return std::string("org.bungie.source.AlephOne");
}

#elif defined(A1_PLATFORM_3DS)

// Nintendo 3DS: user data on the SD card, the scenario in a folder chosen by
// sdmc:/3ds/alephone/scenario.txt, engine resources (MML, default theme) in
// the read-only romfs. See ports/3ds/source/a1_3ds_platform.cpp.
#include "a1_3ds_platform.h"

char get_path_list_separator()
{
	// ':' would split "sdmc:/..." (only used for $ALEPHONE_DATA, which the
	// 3DS cannot set anyway)
	return ';';
}

std::string get_data_path(CSPathType type)
{
	const std::string& user = a1_3ds::user_dir();
	switch (type) {
		case kPathLocalData:
		case kPathLogs:
		case kPathPreferences:
			return user;
		case kPathDefaultData:
			return a1_3ds::scenario_dir();
		case kPathBundleData:
			// searched first, like the macOS app bundle
			return a1_3ds::resource_dir();
		case kPathLegacyData:
		case kPathLegacyPreferences:
			return "";
		case kPathScreenshots:
			return user + "/Screenshots";
		case kPathSavedGames:
			return user + "/Saved Games";
		case kPathQuickSaves:
			return user + "/Quick Saves";
		case kPathImageCache:
			return user + "/Image Cache";
		case kPathRecordings:
			return user + "/Recordings";
	}
	return "";
}

std::string get_application_name()
{
	return std::string(A1_DISPLAY_NAME);
}

std::string get_application_identifier()
{
	return std::string("org.bungie.source.AlephOne");
}

#else

// Linux and compatible

char get_path_list_separator()
{
	return ':';
}

static std::string _get_local_data_path()
{
	static std::string local_dir = "";
	if (local_dir.empty())
	{
		const char *home = getenv("HOME");
		if (home)
			local_dir = std::string(home) + "/.alephone";
	}
	return local_dir;
}

std::string get_data_path(CSPathType type)
{
	std::string path = "";
	
	switch (type) {
		case kPathLocalData:
		case kPathLogs:
		case kPathPreferences:
			path = _get_local_data_path();
			break;
		case kPathDefaultData:
#ifdef PKGDATADIR
			path = PKGDATADIR;
#endif
			break;
		case kPathLegacyData:
		case kPathBundleData:
		case kPathLegacyPreferences:
			// not applicable
			break;
		case kPathScreenshots:
			path = _get_local_data_path() + "/Screenshots";
			break;
		case kPathSavedGames:
			path = _get_local_data_path() + "/Saved Games";
			break;
		case kPathQuickSaves:
			path = _get_local_data_path() + "/Quick Saves";
			break;
		case kPathImageCache:
			path = _get_local_data_path() + "/Image Cache";
			break;
		case kPathRecordings:
			path = _get_local_data_path() + "/Recordings";
			break;
	}
	return path;
}

std::string get_application_name()
{
	return std::string(A1_DISPLAY_NAME);
}

std::string get_application_identifier()
{
	return std::string("org.bungie.source.AlephOne");
}


#endif

