/*
	Aleph One for the Nintendo 3DS, citro3d back end: fades, flashes and tints in direct mode
	(docs/3ds/CITRO3D_DESIGN.md, section 7, M6b specification). Plain C++, host-tested.

	In a 32-bit build every fade is three per-channel 256-entry curves (the engine's current_gamma_r/g/b, screen.cpp),
	applied by SW as out = curve[in] >> 8 (apply_gamma). The curves are classified here, with no knowledge of the fade
	types, in this order:
	  part 1, affine: out = B + A * in per channel (tint, soft_tint, the liquids, and compositions of them): one blended
	          quad, dst' = B + A * dst.
	  part 2, fold:   out = max(min(g, K1 - s1 * g), K2 - s2 * g) per channel, g = B + A * in, s in {0, 1} (negate,
	          dodge, burn, alone or over a liquid; dodge's uint16 wrap clamped, user decision): part 1's quad for g, then
	          a MIN and a MAX pass whose TEV rebuilds the composited pixel from the view and overlay textures.
	  part 3, static: SW's randomize (each entry + a random offset in [0, mask], the mask shared by the three channels):
	          part 1's quad for the curves' base line, then spatial noise from M5's noise texture over about mask/65536
	          of the pixels (user decision: it may look different from SW).

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
constexpr unsigned kFadeNoWrap = 256;   // FadeFit::wrap_from: no entry wrapped
// Part 2: dodge and burn compute int32(1LL * k * c) >> 16 (fades.cpp), which wraps negative for k = 65535 and an input
// c above 0x8000, so SW's curve leaves the intended form there. A channel SW's curve does not fit is fitted on the
// entries whose fader input g(i) is at most this (the intended form, as the engine's OpenGL faders draw it: user
// decision 2026-11-08), the model standing for the rest.
constexpr unsigned kFadeOverflowTrust = 126;

// fade_parts bits (debug-c3d, test only): which parts may classify a frame's curves.
enum FadePart : unsigned { kFadeAffine = 1, kFadeFold = 2, kFadeStatic = 4, kFadeAllParts = 7 };
// Internal (FadeCache): part 2 runs only its quick phase (no slope search). Set after a fit that found nothing, until
// the curves return to the identity: a run of unclassifiable curves costs no full search per frame (critic, M6b code
// review minor 3: up to ~51 ms each in Azahar).
constexpr unsigned kFadeQuickOnly = 8;

// Part 1's GPU model: the blend ONE, CONSTANT_COLOR with B as the source colour and A as the blend colour,
// dst' = B + A * dst / 255, rounded to nearest and clamped (the overlay blend's model, M6; UNVERIFIED for PICA's
// CONSTANT_COLOR factor: design 7, M6b "Needs console verification"). Part 2's TEV stages 1-2 (MODULATE by A, ADD B)
// use the same model (UNVERIFIED: the TEV's rounding).
// y / 255 for y <= 65535 without a division (the ARM11 has none: a fit evaluates this ~10^5 times); exact, host-tested.
inline unsigned fade_div255(unsigned y)
{
	return (y * 32897u) >> 23;
}

inline unsigned fade_affine_gpu(unsigned in, unsigned a8, unsigned b8)
{
	const unsigned v = b8 + fade_div255(in * a8 + 127);
	return v > 255 ? 255 : v;
}

struct FadeChannel
{
	uint8_t a8 = 255, b8 = 0;   // g = B + A * in (A in 1/255): part 1's quad
	uint8_t s1 = 0, s2 = 0;     // part 2: the MIN and MAX terms' sigma (0 or 1)
	uint16_t k1 = 255, k2 = 0;  // part 2: their K in 8-bit steps, 0..510 (255 and 0: the terms change nothing)
	uint8_t err = 0;            // max |model - curve| over the 256 entries, 8-bit steps (saturated at 255)
};

// Part 2's stage 3 of a pass: K - s * g, as SUBTRACT(K, g) for K <= 255 or ADD(1 - g, K - 255) for K > 255, clamped.
inline unsigned fade_term(unsigned g, unsigned s, unsigned k)
{
	const unsigned sg = s ? g : 0;
	if (k > 255)
	{
		const unsigned v = 255 - sg + (k - 255);
		return v > 255 ? 255 : v;
	}
	return k > sg ? k - sg : 0;
}

// Part 2's GPU model of one channel: part 1's quad (g), then dst = min(dst, term 1), then dst = max(dst, term 2).
inline unsigned fade_fold_gpu(unsigned in, const FadeChannel& c)
{
	const unsigned g = fade_affine_gpu(in, c.a8, c.b8);
	unsigned d = g;
	const unsigned h1 = fade_term(g, c.s1, c.k1);
	if (h1 < d)
		d = h1;
	const unsigned h2 = fade_term(g, c.s2, c.k2);
	if (h2 > d)
		d = h2;
	return d;
}

enum class FadeKind : uint8_t { None, Affine, Fold, Static };

const char* fade_kind_name(FadeKind k);   // "no", "affine", "fold", "static"

struct FadeFit
{
	FadeKind kind = FadeKind::None;
	FadeChannel ch[3];
	unsigned err = 0;           // the worst channel error (of the accepted fit; for None, of the best affine candidate)
	uint16_t wrap_from[3] = {kFadeNoWrap, kFadeNoWrap, kFadeNoWrap};   // part 2: dodge's wrapped entries [from, to)
	uint16_t wrap_to[3] = {kFadeNoWrap, kFadeNoWrap, kFadeNoWrap};
	uint16_t trust[3] = {255, 255, 255};   // part 2: the channel judged only where g(i) <= trust (kFadeOverflowTrust)
	uint16_t mask = 0;          // part 3: SW's random mask (2^k - 1): the static's strength
};

// One channel's curve (uint16, as stored) fitted to part 1's model: candidates A8, B8 within 2 of the end-point
// estimate, the smallest max error wins. True when it is <= tol.
bool fade_fit_affine_channel(const uint16_t* curve, unsigned tol, FadeChannel& out);

// The three curves classified (`parts`: FadePart bits). kind None: no part fits. The hints (optional): an earlier fold
// fit and an earlier affine fit, whose g part 2 tries before its slope search (the liquid under a fade does not change
// between its steps); the result does not depend on them beyond which of several fits within tol is found.
FadeFit fade_fit(const uint16_t* r, const uint16_t* g, const uint16_t* b, unsigned tol, unsigned parts,
                 const FadeFit* hint = nullptr, const FadeFit* hint_g = nullptr);

// The curve (8 bits) a fit is judged against and direct_check compares with: curve >> 8, but for a fold fit dodge's
// wrapped entries (SW's uint16 wrap, clamped on the GPU: user decision) are 0 and, in a channel fitted on the trusted
// entries, the rest (SW's int32 overflow) is the model's intended form.
void fade_reference(const uint16_t* curve, const FadeFit& fit, int ch, uint8_t* out);

// What the composite needs for one frame (design 7, M6b "Lifetime"): the fit and the curves it came from, in 8 bits
// (fade_reference: exactly SW's apply_gamma but for dodge's wrap), direct_check's reference.
struct FadeFrame
{
	FadeFit fit;
	uint8_t ref[3][kFadeEntries];
	uint8_t sw[3][kFadeEntries];   // SW's own curve >> 8: direct_check counts the pixels where it differs from ref
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
	FadeFit m_fit, m_last_fold, m_last_affine;
	unsigned m_fits = 0;
};

} // namespace a1_c3d

#endif
