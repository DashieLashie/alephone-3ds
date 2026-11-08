/*
	Aleph One for the Nintendo 3DS, citro3d back end: fades, flashes and tints in direct mode
	(docs/3ds/CITRO3D_DESIGN.md, section 7, M6b specification). Plain C++, host-tested.

	In a 32-bit build every fade is three per-channel 256-entry curves (the engine's current_gamma_r/g/b, screen.cpp),
	applied by SW as out = curve[in] >> 8 (apply_gamma). The curves are classified here, with no knowledge of the fade
	types:
	  part 1, affine: out = B + A * in per channel (tint, soft_tint, the liquids, and compositions of them): one blended
	          quad, dst' = B + A * dst.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_CORE_FADE_FIT_H
#define A1_C3D_CORE_FADE_FIT_H

#include <cstdint>

namespace a1_c3d {

constexpr int kFadeEntries = 256;

// fade_parts bits (debug-c3d, test only): which parts may classify a frame's curves.
enum FadePart : unsigned { kFadeAffine = 1, kFadeFold = 2, kFadeStatic = 4, kFadeAllParts = 7 };

// Part 1's GPU model: the blend ONE, CONSTANT_COLOR with B as the source colour and A as the blend colour,
// dst' = B + A * dst / 255, rounded to nearest and clamped (the overlay blend's model, M6; UNVERIFIED for PICA's
// CONSTANT_COLOR factor: design 7, M6b "Needs console verification").
inline unsigned fade_affine_gpu(unsigned in, unsigned a8, unsigned b8)
{
	const unsigned v = b8 + (in * a8 + 127) / 255;
	return v > 255 ? 255 : v;
}

struct FadeChannel
{
	uint8_t a8 = 255, b8 = 0;   // part 1: out = B + A * in (A in 1/255)
	uint8_t err = 0;            // max |model - curve| over the 256 entries, 8-bit steps (saturated at 255)
};

enum class FadeKind : uint8_t { None, Affine };

struct FadeFit
{
	FadeKind kind = FadeKind::None;
	FadeChannel ch[3];
	unsigned err = 0;   // the worst channel error (of the accepted fit; for None, of the best affine candidate)
};

// One channel's curve (uint16, as stored) fitted to part 1's model: candidates A8, B8 within 2 of the end-point
// estimate, the smallest max error wins. True when it is <= tol.
bool fade_fit_affine_channel(const uint16_t* curve, unsigned tol, FadeChannel& out);

// The three curves classified (`parts`: FadePart bits). kind None: no part fits.
FadeFit fade_fit(const uint16_t* r, const uint16_t* g, const uint16_t* b, unsigned tol, unsigned parts);

// What the composite needs for one frame (design 7, M6b "Lifetime"): the fit and the curves it came from, in 8 bits
// (curve >> 8), direct_check's reference (exactly SW's apply_gamma).
struct FadeFrame
{
	FadeFit fit;
	uint8_t ref[3][kFadeEntries];
};

// The last curves and their fit, kept across frames: a fade step refits once (a 1.5 KiB compare otherwise).
class FadeCache
{
public:
	// The fit of the three curves; `changed` (optional) is set when they, tol or parts differ from the last call.
	const FadeFit& get(const uint16_t* r, const uint16_t* g, const uint16_t* b, unsigned tol, unsigned parts,
	                   bool* changed = nullptr);
	unsigned fits() const { return m_fits; }
	void invalidate() { m_valid = false; }

private:
	uint16_t m_c[3][kFadeEntries] = {};
	bool m_valid = false;
	unsigned m_tol = 0, m_parts = 0;
	FadeFit m_fit;
	unsigned m_fits = 0;
};

} // namespace a1_c3d

#endif
