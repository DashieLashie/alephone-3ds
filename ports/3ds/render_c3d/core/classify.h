/*
	Aleph One for the Nintendo 3DS, citro3d back end: the class of a primitive
	(docs/3ds/CITRO3D_DESIGN.md, section 7, M0 specification, "Classes"). The diff harness
	and the class filter select primitives by this mapping. Pure: the front end maps the
	engine's entry point and transfer mode onto Entry and Transfer. Plain C++, host-testable.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_CORE_CLASSIFY_H
#define A1_C3D_CORE_CLASSIFY_H

#include <cstdint>
#include <string>

namespace a1_c3d {

// Which RasterizerClass entry point delivered the primitive.
enum class Entry { Vertical, Horizontal, Rectangle };

// The transfer modes the classes distinguish; everything else is Other.
enum class Transfer { Textured, BigLandscaped, Static, Tinted, Other };

enum Class : int
{
	kWalls,
	kFloors,
	kLandscape,
	kStatic,
	kSprites,
	kWeapons,
	kTinted,
	kOther,
	kClassCount,
	kFrame = kClassCount   // pseudo-class: the unfiltered full frame
};

constexpr uint32_t kAllClassesMask = (1u << (kFrame + 1)) - 1u;

inline Class classify(Entry e, Transfer t, bool foreground)
{
	switch (e)
	{
	case Entry::Vertical:
	case Entry::Horizontal:
		switch (t)
		{
		case Transfer::Textured:
			return e == Entry::Vertical ? kWalls : kFloors;
		case Transfer::BigLandscaped:
			return kLandscape;
		case Transfer::Static:
			return kStatic;
		default:
			return kOther;   // includes Tinted on a polygon
		}
	case Entry::Rectangle:
		if (t == Transfer::Tinted)
			return kTinted;
		if (t == Transfer::Static)
			return kStatic;
		return foreground ? kWeapons : kSprites;
	}
	return kOther;
}

inline const char* class_name(int c)
{
	static const char* const names[] = {"walls", "floors", "landscape", "static", "sprites",
	                                    "weapons", "tinted", "other", "frame"};
	return (c >= 0 && c <= kFrame) ? names[c] : "?";
}

// "all", or a comma list (no spaces) of class names and "frame". Unknown name or empty item:
// false, `mask` untouched.
inline bool parse_classes(const std::string& value, uint32_t& mask)
{
	if (value == "all")
	{
		mask = kAllClassesMask;
		return true;
	}
	uint32_t m = 0;
	std::string::size_type start = 0;
	for (;;)
	{
		const std::string::size_type comma = value.find(',', start);
		const std::string item = value.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
		int found = -1;
		for (int c = 0; c <= kFrame; ++c)
			if (item == class_name(c))
				found = c;
		if (found < 0)   // also an empty item or an empty value
			return false;
		m |= 1u << found;
		if (comma == std::string::npos)
			break;
		start = comma + 1;
	}
	mask = m;
	return true;
}

} // namespace a1_c3d

#endif
