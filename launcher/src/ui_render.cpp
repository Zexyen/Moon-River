#include "ui_render.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include <SDL3/SDL.h>
#include <RmlUi/Core/Log.h>
#include "../../runtime/prompt_art.h"

// Procedural textures by name, with parameters after '?' (colors as rrggbb, alpha 0-255):
//   gen:glow?x=&y=&rx=&ry=&c1=&a1=&c0=&a0=&stop=&edge=  soft elliptic glow
//   gen:vignette                                         edge-darkening radial vignette over the stage
//   gen:moonphase?p=&dim=                                live astronomical moon phase disc + halo + maria craters
//   gen:shimmer?lit=&dim=                                broken horizontal moonlight reflection on the river
//   gen:builddisc                                        build screen's 300dp moon disc base + faint 170dp track ring
//   gen:moonfill                                         build screen's 300dp lit moon disc gradient (clipped from bottom)
//   gen:glowring?c=&w=                                   build screen's 170dp glowing cyan progress ring
//   gen:ring?c=&a=&w=                                    a circle line w texels thick in a 512 texel square
//   gen:fade?c=&a=&d=                                    fading hairline: d=l (left->right), d=r (right->left), d=c (both ends)
//   gen:pill?w=&h=&f=&fa=&c=&a=&l=&glow=                 smooth antialiased rounded bar / disc (switches, knobs)
//   gen:glyph?n=&c=&t=                                   outlined glowing button prompt (Xbox, PlayStation, Switch, Keyboard)
//   gen:grad?d=&c0=&a0=&c1=&a1=&c2=&a2=&c3=&a3=&p1=&p2=  up to 4-stop horizontal (d=h) or vertical (d=v) gradient
namespace {

struct Params {
	std::map<Rml::String, Rml::String> v;
	float num(const char* k, float def) const
	{
		auto it = v.find(k);
		return it == v.end() ? def : std::strtof(it->second.c_str(), nullptr);
	}
	Rml::Colourb color(const char* k, Rml::Colourb def) const
	{
		auto it = v.find(k);
		if (it == v.end() || it->second.size() != 6)
			return def;
		unsigned long x = std::strtoul(it->second.c_str(), nullptr, 16);
		return Rml::Colourb(Rml::byte(x >> 16), Rml::byte(x >> 8), Rml::byte(x), 255);
	}
};

Params parse(const Rml::String& query)
{
	Params p;
	size_t i = 0;
	while (i < query.size())
	{
		size_t amp = query.find('&', i);
		if (amp == Rml::String::npos)
			amp = query.size();
		Rml::String kv = query.substr(i, amp - i);
		size_t eq = kv.find('=');
		if (eq != Rml::String::npos)
			p.v[kv.substr(0, eq)] = kv.substr(eq + 1);
		i = amp + 1;
	}
	return p;
}

struct Image {
	int w, h;
	std::vector<Rml::byte> px;
	Image(int w, int h) : w(w), h(h), px(size_t(w) * h * 4, 0) {}
	// premultiplied RGBA, as RmlUi's SDL GPU renderer expects
	void put(int x, int y, Rml::Colourb c, float a)
	{
		a = std::clamp(a, 0.f, 1.f) * (c.alpha / 255.f);
		Rml::byte* p = &px[(size_t(y) * w + x) * 4];
		p[0] = Rml::byte(c.red * a + .5f);
		p[1] = Rml::byte(c.green * a + .5f);
		p[2] = Rml::byte(c.blue * a + .5f);
		p[3] = Rml::byte(255 * a + .5f);
	}
	void over(int x, int y, float r, float g, float b, float a)
	{
		a = std::clamp(a, 0.f, 1.f);
		if (a <= 0.f)
			return;
		Rml::byte* p = &px[(size_t(y) * w + x) * 4];
		const float k = 1.f - a;
		p[0] = Rml::byte(std::clamp(p[0] * k + r * a + .5f, 0.f, 255.f));
		p[1] = Rml::byte(std::clamp(p[1] * k + g * a + .5f, 0.f, 255.f));
		p[2] = Rml::byte(std::clamp(p[2] * k + b * a + .5f, 0.f, 255.f));
		p[3] = Rml::byte(std::clamp(p[3] * k + 255.f * a + .5f, 0.f, 255.f));
	}
};

float coverage(float d) { return std::clamp(.5f - d, 0.f, 1.f); }
float smoothstep(float a, float b, float x)
{
	float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
	return t * t * (3.f - 2.f * t);
}

void box_blur(Image& im, int radius)
{
	if (radius <= 0)
		return;
	std::vector<Rml::byte> tmp(im.px.size());
	const int diam = radius * 2 + 1;
	// horizontal
	for (int y = 0; y < im.h; y++)
		for (int x = 0; x < im.w; x++)
		{
			int s[4] = {};
			for (int k = -radius; k <= radius; k++)
			{
				const int xx = std::clamp(x + k, 0, im.w - 1);
				const Rml::byte* q = &im.px[(size_t(y) * im.w + xx) * 4];
				for (int c = 0; c < 4; c++)
					s[c] += q[c];
			}
			Rml::byte* d = &tmp[(size_t(y) * im.w + x) * 4];
			for (int c = 0; c < 4; c++)
				d[c] = Rml::byte((s[c] + diam / 2) / diam);
		}
	// vertical
	for (int y = 0; y < im.h; y++)
		for (int x = 0; x < im.w; x++)
		{
			int s[4] = {};
			for (int k = -radius; k <= radius; k++)
			{
				const int yy = std::clamp(y + k, 0, im.h - 1);
				const Rml::byte* q = &tmp[(size_t(yy) * im.w + x) * 4];
				for (int c = 0; c < 4; c++)
					s[c] += q[c];
			}
			Rml::byte* d = &im.px[(size_t(y) * im.w + x) * 4];
			for (int c = 0; c < 4; c++)
				d[c] = Rml::byte((s[c] + diam / 2) / diam);
		}
}

Image glow(const Params& p)
{
	Image im(320, 200);
	const float cx = p.num("x", .5f), cy = p.num("y", .5f), rx = p.num("rx", .7f), ry = p.num("ry", .9f), stop = p.num("stop", .62f);
	const Rml::Colourb c1 = p.color("c1", {111, 230, 255, 255}), c0 = p.color("c0", {111, 230, 255, 255});
	const float a1 = p.num("a1", 255), a0 = p.num("a0", 0), edge = p.num("edge", 0);
	for (int y = 0; y < im.h; y++)
		for (int x = 0; x < im.w; x++)
		{
			float dx = ((x + .5f) / im.w - cx) / rx, dy = ((y + .5f) / im.h - cy) / ry;
			float t = std::clamp(std::sqrt(dx * dx + dy * dy) / stop, 0.f, 1.f);
			t = t * t * (3 - 2 * t);
			Rml::Colourb c(Rml::byte(c1.red + (c0.red - c1.red) * t), Rml::byte(c1.green + (c0.green - c1.green) * t),
				Rml::byte(c1.blue + (c0.blue - c1.blue) * t), 255);
			float a = (a1 + (a0 - a1) * t) / 255.f;
			if (edge > 0)
			{
				const float u = (x + .5f) / im.w, v = (y + .5f) / im.h;
				const float e = std::clamp(std::min({u, 1 - u, v, 1 - v}) / edge, 0.f, 1.f);
				a *= e * e * (3 - 2 * e);
			}
			im.put(x, y, c, a);
		}
	return im;
}

Image vignette()
{
	// radial-gradient(ellipse 75% 70% at 50% 45%, transparent 55%, rgba(0,3,12,.75))
	Image im(320, 200);
	for (int y = 0; y < im.h; y++)
		for (int x = 0; x < im.w; x++)
		{
			const float dx = ((x + .5f) / im.w - 0.50f) / 0.75f;
			const float dy = ((y + .5f) / im.h - 0.45f) / 0.70f;
			const float d = std::hypot(dx, dy);
			const float a = std::clamp((d - 0.55f) / 0.45f, 0.f, 1.f) * 0.75f;
			im.put(x, y, {0, 3, 12, 255}, a);
		}
	return im;
}

Image moonphase(const Params& p)
{
	// 384x384 texture representing a 480dp x 480dp box centered on the moon (disc r = 95dp, halo r = 230dp).
	const int N = 384;
	const float half = N * 0.5f;
	const float R = half * (95.f / 240.f);
	const float R_halo = half * (230.f / 240.f);
	float phase = std::fmod(p.num("p", 0.5f), 1.f);
	if (phase < 0.f)
		phase += 1.f;
	const bool dim = p.num("dim", 0.f) >= 0.5f;
	const float c = std::cos(2.f * float(M_PI) * phase);
	const float lit = (1.f - c) * 0.5f;
	const bool waxing = phase < 0.5f;
	const float halo_mul = 0.25f + 0.75f * lit;
	const float overall = dim ? 0.12f : 0.90f;

	// Maria craters in normalized disc coordinates (from the mockup SVG)
	struct Crater { float cx, cy, r, a; };
	static const Crater craters[] = {
		{ 23.f / 95.f,  15.f / 95.f, 17.f / 95.f, 0.35f},
		{-30.f / 95.f,  30.f / 95.f, 11.f / 95.f, 0.25f},
		{ 10.f / 95.f, -40.f / 95.f,  9.f / 95.f, 0.22f},
		{-45.f / 95.f, -15.f / 95.f,  7.f / 95.f, 0.18f},
	};

	Image im(N, N);
	for (int y = 0; y < N; y++)
	{
		const float dy = y + 0.5f - half;
		for (int x = 0; x < N; x++)
		{
			const float dx = x + 0.5f - half;
			const float r = std::hypot(dx, dy);

			// 1. Halo
			if (r < R_halo)
			{
				const float rn = r / R_halo;
				const float t = rn <= 0.45f ? 0.f : (rn - 0.45f) / 0.55f;
				const float hr = 170.f + (90.f - 170.f) * t;
				const float hg = 215.f + (160.f - 215.f) * t;
				const float hb = 255.f;
				const float ha = (1.f - t) * 0.28f * halo_mul;
				im.over(x, y, hr, hg, hb, ha);
			}

			// 2. Unlit disc + faint rim stroke
			if (r < R + 1.5f)
			{
				const float disc_cov = coverage(r - R);
				if (disc_cov > 0.f)
					im.over(x, y, 19.f, 37.f, 74.f, 0.55f * disc_cov);
				const float rim_cov = coverage(std::fabs(r - R) - 0.5f);
				if (rim_cov > 0.f)
					im.over(x, y, 170.f, 215.f, 255.f, 0.12f * rim_cov);

				// 3. Lit crescent/gibbous shape clipped to the disc
				if (disc_cov > 0.f)
				{
					const float w = std::sqrt(std::max(0.f, R * R - dy * dy));
					const float x_term = (waxing ? c : -c) * w;
					const float signed_x = waxing ? (dx - x_term) : (x_term - dx);
					// Normalize horizontal distance to approximate Euclidean distance to the terminator ellipse
					const float slope = w > 1.f ? (std::fabs(c) * std::fabs(dy) / w) : 4.f;
					const float dist_term = signed_x / std::sqrt(1.f + slope * slope);
					const float lit_cov = disc_cov * std::clamp(0.5f + dist_term, 0.f, 1.f);
					if (lit_cov > 0.f)
					{
						// Radial gradient centered at (-0.24*R, -0.28*R), radius 1.4*R: #fbfdff -> #e0ecfb (55%) -> #a9c0e0
						const float gx = dx + 0.24f * R;
						const float gy = dy + 0.28f * R;
						const float gt = std::clamp(std::hypot(gx, gy) / (1.4f * R), 0.f, 1.f);
						float mr, mg, mb;
						if (gt < 0.55f)
						{
							const float u = gt / 0.55f;
							mr = 251.f + (224.f - 251.f) * u;
							mg = 253.f + (236.f - 253.f) * u;
							mb = 255.f + (251.f - 255.f) * u;
						}
						else
						{
							const float u = (gt - 0.55f) / 0.45f;
							mr = 224.f + (169.f - 224.f) * u;
							mg = 236.f + (192.f - 236.f) * u;
							mb = 251.f + (224.f - 251.f) * u;
						}
						// Lunar maria craters
						const float nx = dx / R, ny = dy / R;
						for (const Crater& cr : craters)
						{
							const float cd = (std::hypot(nx - cr.cx, ny - cr.cy) - cr.r) * R;
							const float ca = coverage(cd) * cr.a;
							if (ca > 0.f)
							{
								mr = mr * (1.f - ca) + 120.f * ca;
								mg = mg * (1.f - ca) + 150.f * ca;
								mb = mb * (1.f - ca) + 190.f * ca;
							}
						}
						im.over(x, y, mr, mg, mb, lit_cov);
					}
				}
			}
		}
	}

	if (dim)
		box_blur(im, 2);

	// Scale final premultiplied image by overall opacity (0.90 on title, 0.12 on other screens)
	for (size_t i = 0; i < im.px.size(); i++)
		im.px[i] = Rml::byte(im.px[i] * overall + 0.5f);
	return im;
}

Image shimmer(const Params& p)
{
	// 250x210 broken horizontal ripples on the river under the moon
	const int W = 250, H = 210;
	const float lit = std::clamp(p.num("lit", 0.5f), 0.f, 1.f);
	const bool dim = p.num("dim", 0.f) >= 0.5f;
	const float overall = dim ? 0.18f : (0.12f + 0.50f * lit);
	Image im(W, H);
	for (int y = 0; y < H; y++)
	{
		// repeating-linear-gradient(transparent 0 7px, rgba(220,240,255,.55) 7px 9px)
		const float mod = std::fmod(y + 0.5f, 9.f);
		float stripe = 0.f;
		if (mod >= 6.5f)
			stripe = mod < 7.5f ? (mod - 6.5f) : mod <= 8.5f ? 1.f : (9.5f - mod);
		stripe = std::clamp(stripe, 0.f, 1.f);
		if (stripe <= 0.f)
			continue;
		const float v = (y + 0.5f) / H;
		for (int x = 0; x < W; x++)
		{
			const float u = (x + 0.5f) / W - 0.5f;
			// mask: radial-gradient(ellipse 50% 50% at 50% 0%, #000 20%, transparent 75%)
			const float d = std::hypot(u / 0.5f, v / 0.5f);
			const float mask = 1.f - smoothstep(0.20f, 0.75f, d);
			const float a = stripe * 0.55f * mask * overall;
			if (a > 0.f)
				im.put(x, y, {220, 240, 255, 255}, a);
		}
	}
	return im;
}

Image builddisc()
{
	// 400x400: inner moon disc (#0b1c42, stroke rgba(160,220,255,.2) at r=150) + outer track ring (r=170, w=3, rgba(111,230,255,.12))
	const int N = 400;
	const float c = N * 0.5f;
	Image im(N, N);
	for (int y = 0; y < N; y++)
		for (int x = 0; x < N; x++)
		{
			const float r = std::hypot(x + 0.5f - c, y + 0.5f - c);
			const float fill_cov = coverage(r - 150.f);
			if (fill_cov > 0.f)
				im.over(x, y, 11.f, 28.f, 66.f, fill_cov);
			const float edge_cov = coverage(std::fabs(r - 150.f) - 0.6f);
			if (edge_cov > 0.f)
				im.over(x, y, 160.f, 220.f, 255.f, 0.20f * edge_cov);
			const float track_cov = coverage(std::fabs(r - 170.f) - 1.5f);
			if (track_cov > 0.f)
				im.over(x, y, 111.f, 230.f, 255.f, 0.12f * track_cov);
		}
	return im;
}

Image moonfill(const Params& p)
{
	// 300x300 circular lit moon gradient for the build screen's rising fill (p = 0..100)
	const int N = 300;
	const float c = N * 0.5f;
	const float frac = std::clamp(p.num("p", 0.f) / 100.f, 0.f, 1.f);
	const float cut_y = N * (1.f - frac);
	Image im(N, N);
	if (frac <= 0.f)
		return im;
	for (int y = 0; y < N; y++)
	{
		const float row_cov = std::clamp((y + 0.5f) - cut_y + 0.5f, 0.f, 1.f);
		if (row_cov <= 0.f)
			continue;
		for (int x = 0; x < N; x++)
		{
			const float dx = x + 0.5f - c, dy = y + 0.5f - c;
			const float cov = coverage(std::hypot(dx, dy) - 149.f) * row_cov;
			if (cov <= 0.f)
				continue;
			const float gx = (x + 0.5f) / N - 0.40f;
			const float gy = (y + 0.5f) / N - 0.38f;
			const float t = std::clamp(std::hypot(gx, gy) / 0.72f, 0.f, 1.f);
			float r, g, b;
			if (t < 0.60f)
			{
				const float u = t / 0.60f;
				r = 251.f + (207.f - 251.f) * u;
				g = 253.f + (224.f - 253.f) * u;
				b = 255.f + (245.f - 255.f) * u;
			}
			else
			{
				const float u = (t - 0.60f) / 0.40f;
				r = 207.f + (143.f - 207.f) * u;
				g = 224.f + (169.f - 224.f) * u;
				b = 245.f + (208.f - 245.f) * u;
			}
			im.put(x, y, {Rml::byte(r), Rml::byte(g), Rml::byte(b), 255}, 0.28f * cov);
		}
	}
	return im;
}

Image glowring(const Params& p)
{
	// 400x400 glowing progress ring at r=170 (matches builddisc's track ring)
	const int N = 400;
	const float c = N * 0.5f;
	const Rml::Colourb col = p.color("c", {111, 230, 255, 255});
	const float w = p.num("w", 3.f);
	Image im(N, N);
	for (int y = 0; y < N; y++)
		for (int x = 0; x < N; x++)
		{
			const float dist = std::fabs(std::hypot(x + 0.5f - c, y + 0.5f - c) - 170.f);
			if (dist > 20.f)
				continue;
			const float halo = std::exp(-(dist * dist) / 36.f) * 0.55f;
			const float core = coverage(dist - w * 0.5f);
			const float a = std::clamp(halo + core * (1.f - halo), 0.f, 1.f);
			// Core brightens slightly toward white-cyan
			const float r = col.red + (235.f - col.red) * core * 0.4f;
			const float g = col.green + (252.f - col.green) * core * 0.4f;
			const float b = col.blue + (255.f - col.blue) * core * 0.4f;
			im.put(x, y, {Rml::byte(r), Rml::byte(g), Rml::byte(b), 255}, a);
		}
	return im;
}

Image ring(const Params& p)
{
	Image im(512, 512);
	Rml::Colourb c = p.color("c", {111, 230, 255, 255});
	c.alpha = Rml::byte(p.num("a", 255));
	const float w = p.num("w", 2), r = 256 - 1 - w / 2;
	for (int y = 0; y < im.h; y++)
		for (int x = 0; x < im.w; x++)
		{
			float d = std::fabs(std::hypot(x + .5f - 256, y + .5f - 256) - r) - w / 2;
			if (d < 1)
				im.put(x, y, c, coverage(d));
		}
	return im;
}

Image fade(const Params& p)
{
	Image im(256, 2);
	Rml::Colourb c = p.color("c", {111, 230, 255, 255});
	c.alpha = Rml::byte(p.num("a", 255));
	const Rml::String d = p.v.count("d") ? p.v.at("d") : Rml::String("l");
	for (int y = 0; y < im.h; y++)
		for (int x = 0; x < im.w; x++)
		{
			const float u = (x + .5f) / im.w;
			const float a = d == "r" ? u : d == "c" ? (1.f - std::fabs(u * 2.f - 1.f)) : (1.f - u);
			im.put(x, y, c, a);
		}
	return im;
}

Image pill(const Params& p)
{
	Image im(std::max(1, int(p.num("w", 116))), std::max(1, int(p.num("h", 60))));
	const bool filled = p.v.count("f"), edged = p.v.count("c");
	const Rml::Colourb fc = p.color("f", {}), ec = p.color("c", {});
	const float fa = p.num("fa", 255) / 255.f;
	const float ea = p.num("a", 255) / 255.f;
	const float pad = p.num("glow", 0);
	const float line = p.num("l", 2);
	const float r = (im.h - pad * 2.f) / 2.f;
	const float cy = im.h / 2.f;
	for (int y = 0; y < im.h; y++)
		for (int x = 0; x < im.w; x++)
		{
			float px = std::clamp(x + .5f, pad + r, im.w - pad - r);
			float d = std::hypot(x + .5f - px, y + .5f - cy) - r;
			if (pad > 0.f && d > 0.f && d < pad)
			{
				const float ga = std::exp(-(d * d) / (pad * pad * 0.22f)) * 0.65f;
				const Rml::Colourb gc = filled ? fc : ec;
				im.over(x, y, gc.red, gc.green, gc.blue, ga);
			}
			if (filled)
			{
				const float cov = coverage(d) * fa;
				if (cov > 0.f)
					im.over(x, y, fc.red, fc.green, fc.blue, cov);
			}
			if (edged)
			{
				float a = coverage(std::max(d, -d - line)) * ea;
				if (a > 0.f)
					im.over(x, y, ec.red, ec.green, ec.blue, a);
			}
		}
	return im;
}

// Positive-stroke letter rendering on an arbitrary cw x ch cell (SS subsamples/unit) for outlined glowing glyphs
static void seg_w(uint8_t* s, int sw, int sh, float x0, float y0, float x1, float y1, float w, int v)
{
	const float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy, r2 = w * w / 4.f;
	for (int j = 0; j < sh; j++)
		for (int i = 0; i < sw; i++)
		{
			float x = (i + .5f) / SS - x0, y = (j + .5f) / SS - y0;
			float t = len2 > 0 ? std::clamp((x * dx + y * dy) / len2, 0.f, 1.f) : 0.f;
			x -= t * dx;
			y -= t * dy;
			if (x * x + y * y <= r2)
				s[j * sw + i] = uint8_t(v);
		}
}

static void box_w(uint8_t* s, int sw, int sh, float x0, float y0, float x1, float y1, float r, int v)
{
	const float cx = (x0 + x1) / 2.f, cy = (y0 + y1) / 2.f, hx = (x1 - x0) / 2.f - r, hy = (y1 - y0) / 2.f - r;
	for (int j = 0; j < sh; j++)
		for (int i = 0; i < sw; i++)
		{
			float x = std::max(0.f, std::fabs((i + .5f) / SS - cx) - hx);
			float y = std::max(0.f, std::fabs((j + .5f) / SS - cy) - hy);
			if (x * x + y * y <= r * r)
				s[j * sw + i] = uint8_t(v);
		}
}

static void disc_w(uint8_t* s, int sw, int sh, float cx, float cy, float r, int v)
{
	for (int j = 0; j < sh; j++)
		for (int i = 0; i < sw; i++)
		{
			float x = (i + .5f) / SS - cx, y = (j + .5f) / SS - cy;
			if (x * x + y * y <= r * r)
				s[j * sw + i] = uint8_t(v);
		}
}

static void text_pos(uint8_t* s, int sw, int sh, float cw, float ch, const char* t, float k, float stroke, float gap)
{
	const int n = int(std::strlen(t));
	if (n <= 0)
		return;
	const float x0 = (cw - n * 4.f * k - (n - 1) * gap) / 2.f;
	const float y0 = (ch - 6.f * k) / 2.f;
	for (int i = 0; i < n; i++)
	{
		const float ox = x0 + i * (4.f * k + gap);
		const char ch_u = char(std::toupper(static_cast<unsigned char>(t[i])));
		for (const char* q = letter(ch_u); q[0] && q[2] && q[3]; q += 3)
			if (q[2] == ' ')
				seg_w(s, sw, sh, ox + hex(q[0]) * k / 2.f, y0 + hex(q[1]) * k / 2.f, ox + hex(q[3]) * k / 2.f, y0 + hex(q[4]) * k / 2.f, stroke, 1);
	}
}

Image glyph(const Params& p)
{
	const auto n = p.v.count("n") ? p.v.at("n") : Rml::String();
	const auto t = p.v.count("t") ? p.v.at("t") : Rml::String();

	// Wide rectangular keycaps for Enter / Esc / bumpers / arrows; square for face buttons and single-key caps
	const int cw = n == "enter" ? 60 : (n == "esc" || n == "bumper" || n == "arrows") ? 48 : 40;
	const int ch = 40;
	const int sw = cw * SS, sh = ch * SS;
	const float cx = cw * 0.5f, cy = ch * 0.5f;
	std::vector<uint8_t> s(size_t(sw) * sh, 0);
	std::vector<uint8_t> shape(size_t(sw) * sh, 0);

	auto ring_outline = [&](float r) {
		disc_w(shape.data(), sw, sh, cx, cy, r, 1);
		disc_w(s.data(), sw, sh, cx, cy, r, 1);
		disc_w(s.data(), sw, sh, cx, cy, r - 1.55f, 0);
	};
	auto box_outline = [&](float pad_x, float pad_y, float r) {
		box_w(shape.data(), sw, sh, pad_x, pad_y, cw - pad_x, ch - pad_y, r, 1);
		box_w(s.data(), sw, sh, pad_x, pad_y, cw - pad_x, ch - pad_y, r, 1);
		box_w(s.data(), sw, sh, pad_x + 1.55f, pad_y + 1.55f, cw - pad_x - 1.55f, ch - pad_y - 1.55f, std::max(0.5f, r - 1.55f), 0);
	};

	if (n == "cross")
	{
		ring_outline(13.2f);
		seg_w(s.data(), sw, sh, cx - 4.8f, cy - 4.8f, cx + 4.8f, cy + 4.8f, 1.9f, 1);
		seg_w(s.data(), sw, sh, cx + 4.8f, cy - 4.8f, cx - 4.8f, cy + 4.8f, 1.9f, 1);
	}
	else if (n == "circle")
	{
		ring_outline(13.2f);
		disc_w(s.data(), sw, sh, cx, cy, 6.2f, 1);
		disc_w(s.data(), sw, sh, cx, cy, 4.4f, 0);
	}
	else if (n == "enter")
	{
		box_outline(5.0f, 8.0f, 5.5f);
		text_pos(s.data(), sw, sh, float(cw), float(ch), "ENTER", 1.55f, 1.55f, 2.2f);
	}
	else if (n == "esc")
	{
		box_outline(5.0f, 8.0f, 5.5f);
		text_pos(s.data(), sw, sh, float(cw), float(ch), "ESC", 1.55f, 1.55f, 2.4f);
	}
	else if (n == "key")
	{
		box_outline(7.0f, 8.0f, 5.5f);
		text_pos(s.data(), sw, sh, float(cw), float(ch), t.substr(0, 4).c_str(), 1.85f, 1.85f, 2.2f);
	}
	else if (n == "bumper")
	{
		box_outline(5.0f, 8.5f, 5.5f);
		text_pos(s.data(), sw, sh, float(cw), float(ch), t.substr(0, 4).c_str(), 1.65f, 1.7f, 2.4f);
	}
	else if (n == "dpad")
	{
		ring_outline(13.2f);
		seg_w(s.data(), sw, sh, cx - 2.5f, cy - 3.5f, cx - 6.5f, cy, 1.8f, 1);
		seg_w(s.data(), sw, sh, cx - 6.5f, cy, cx - 2.5f, cy + 3.5f, 1.8f, 1);
		seg_w(s.data(), sw, sh, cx + 2.5f, cy - 3.5f, cx + 6.5f, cy, 1.8f, 1);
		seg_w(s.data(), sw, sh, cx + 6.5f, cy, cx + 2.5f, cy + 3.5f, 1.8f, 1);
	}
	else if (n == "arrows")
	{
		box_outline(5.0f, 8.5f, 5.5f);
		seg_w(s.data(), sw, sh, cx - 3.8f, cy - 4.2f, cx - 8.5f, cy, 1.8f, 1);
		seg_w(s.data(), sw, sh, cx - 8.5f, cy, cx - 3.8f, cy + 4.2f, 1.8f, 1);
		seg_w(s.data(), sw, sh, cx + 3.8f, cy - 4.2f, cx + 8.5f, cy, 1.8f, 1);
		seg_w(s.data(), sw, sh, cx + 8.5f, cy, cx + 3.8f, cy + 4.2f, 1.8f, 1);
	}
	else
	{
		ring_outline(13.2f);
		text_pos(s.data(), sw, sh, float(cw), float(ch), n == "b" ? "B" : "A", 2.0f, 2.1f, 0.f);
	}

	// Downsample 2x2 with soft cyan glow around the outline
	const int out_w = sw / 2, out_h = sh / 2;
	Image halo(out_w, out_h);
	Image fg(out_w, out_h);
	const Rml::Colourb col = p.color("c", {234, 250, 255, 255});
	for (int y = 0; y < out_h; y++)
		for (int x = 0; x < out_w; x++)
		{
			const uint8_t* q = &s[(y * 2) * sw + x * 2];
			const float a = (q[0] + q[1] + q[sw] + q[sw + 1]) / 4.f;
			if (a > 0.f)
				fg.put(x, y, col, a);
			const uint8_t* qs = &shape[(y * 2) * sw + x * 2];
			const float sa = (qs[0] + qs[1] + qs[sw] + qs[sw + 1]) / 4.f;
			if (sa > 0.f)
				halo.put(x, y, {111, 230, 255, 255}, sa * 0.35f);
		}
	box_blur(halo, 3);
	box_blur(halo, 3);
	for (int y = 0; y < out_h; y++)
		for (int x = 0; x < out_w; x++)
		{
			const Rml::byte* f = &fg.px[(size_t(y) * out_w + x) * 4];
			if (f[3] > 0)
				halo.over(x, y, col.red, col.green, col.blue, f[3] / 255.f);
		}
	return halo;
}

Image grad(const Params& p)
{
	const bool v = p.v.count("d") && p.v.at("d") == "v";
	Image im(v ? 2 : 512, v ? 512 : 2);
	struct Stop { float at; Rml::Colourb c; float a; };
	std::vector<Stop> stops;
	const char* keys[] = {"c0", "c1", "c2", "c3"};
	const char* alphas[] = {"a0", "a1", "a2", "a3"};
	const char* ats[] = {nullptr, "p1", "p2", nullptr};
	for (int i = 0; i < 4; i++)
		if (p.v.count(keys[i]))
			stops.push_back({i == 0 ? 0.f : i == 3 ? 1.f : p.num(ats[i], i / 3.f), p.color(keys[i], {}), p.num(alphas[i], 255)});
	if (stops.size() == 1)
		stops.push_back(stops[0]);
	if (stops.empty())
		return im;
	stops.back().at = 1;
	for (int y = 0; y < im.h; y++)
		for (int x = 0; x < im.w; x++)
		{
			const float t = (v ? y + .5f : x + .5f) / (v ? im.h : im.w);
			size_t k = 0;
			while (k + 2 < stops.size() && t > stops[k + 1].at)
				k++;
			const Stop &s0 = stops[k], &s1 = stops[k + 1];
			const float u = s1.at > s0.at ? std::clamp((t - s0.at) / (s1.at - s0.at), 0.f, 1.f) : 1;
			Rml::Colourb c(Rml::byte(s0.c.red + (s1.c.red - s0.c.red) * u), Rml::byte(s0.c.green + (s1.c.green - s0.c.green) * u),
				Rml::byte(s0.c.blue + (s1.c.blue - s0.c.blue) * u), 255);
			im.put(x, y, c, (s0.a + (s1.a - s0.a) * u) / 255.f);
		}
	return im;
}

} // namespace

Rml::TextureHandle UiRender::LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source)
{
	const size_t at = source.find("gen:");
	if (at == Rml::String::npos)
	{
		const size_t q = source.find('?');
		const Rml::String path = q == Rml::String::npos ? source : source.substr(0, q);
		return RenderInterface_SDL_GPU::LoadTexture(texture_dimensions, path);
	}
	Rml::String spec = source.substr(at + 4), name = spec, query;
	if (size_t q = spec.find('?'); q != Rml::String::npos)
	{
		name = spec.substr(0, q);
		query = spec.substr(q + 1);
	}
	const Params p = parse(query);
	Image im = name == "glow" ? glow(p)
		: name == "vignette" ? vignette()
		: name == "moonphase" ? moonphase(p)
		: name == "shimmer" ? shimmer(p)
		: name == "builddisc" ? builddisc()
		: name == "moonfill" ? moonfill(p)
		: name == "glowring" ? glowring(p)
		: name == "ring" ? ring(p)
		: name == "pill" ? pill(p)
		: name == "glyph" ? glyph(p)
		: name == "grad" ? grad(p)
		: fade(p);
	texture_dimensions = {im.w, im.h};
	return GenerateTexture({im.px.data(), im.px.size()}, texture_dimensions);
}
