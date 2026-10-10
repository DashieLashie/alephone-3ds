// Aleph One 3DS port: toolchain probe.
//
// Checks that the pieces of the C++ runtime Aleph One relies on actually work
// on the target (std::filesystem, exceptions, RTTI), then opens a 400x240 SDL2
// window on the top screen and fills it red for 3 seconds.
//
// Results go to the bottom-screen console and to sdmc:/3ds/probe.log.

#include <3ds.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef PROBE_WITH_SDL
#define SDL_MAIN_HANDLED
#include <SDL.h>
#endif

namespace fs = std::filesystem;

namespace {

std::ostringstream g_log;
int g_failures = 0;

void report(const char* test, bool ok, const std::string& detail)
{
	std::string line = std::string(ok ? "PASS " : "FAIL ") + test + ": " + detail;
	std::printf("%s\n", line.c_str());
	g_log << line << '\n';
	if (!ok)
		++g_failures;
}

void note(const std::string& line)
{
	std::printf("  %s\n", line.c_str());
	g_log << "  " << line << '\n';
}

// --- std::filesystem --------------------------------------------------------

void probe_filesystem()
{
	const fs::path dir("sdmc:/3ds");

	// Non-throwing overload first, so a broken filesystem doesn't also depend
	// on exceptions working.
	std::error_code ec;
	fs::directory_iterator it(dir, ec);
	if (ec) {
		report("filesystem(error_code)", false, dir.string() + ": " + ec.message());
	} else {
		int count = 0;
		for (; it != fs::directory_iterator(); it.increment(ec)) {
			if (ec)
				break;
			if (count < 8)
				note(it->path().filename().string() + (it->is_directory() ? "/" : ""));
			++count;
		}
		if (ec)
			report("filesystem(error_code)", false, "increment: " + ec.message());
		else
			report("filesystem(error_code)", true, std::to_string(count) + " entries in " + dir.string());
	}

	// Throwing overload.
	try {
		int count = 0;
		for (const auto& entry : fs::directory_iterator(dir)) {
			(void)entry;
			++count;
		}
		report("filesystem(throwing)", true, std::to_string(count) + " entries");
	} catch (const fs::filesystem_error& e) {
		report("filesystem(throwing)", false, std::string("filesystem_error: ") + e.what());
	} catch (const std::exception& e) {
		report("filesystem(throwing)", false, std::string("exception: ") + e.what());
	}

	// Error path: a missing directory must produce filesystem_error.
	try {
		fs::directory_iterator missing("sdmc:/3ds/__aleph_probe_does_not_exist__");
		report("filesystem(missing dir)", false, "no exception thrown");
	} catch (const fs::filesystem_error& e) {
		report("filesystem(missing dir)", true, std::string("caught: ") + e.code().message());
	}
}

// --- exceptions -------------------------------------------------------------

[[gnu::noinline]] void throw_runtime_error(int value)
{
	throw std::runtime_error("probe exception " + std::to_string(value));
}

void probe_exceptions()
{
	try {
		throw_runtime_error(42);
		report("exceptions", false, "throw did not unwind");
	} catch (const std::runtime_error& e) {
		report("exceptions", std::string(e.what()) == "probe exception 42", e.what());
	} catch (...) {
		report("exceptions", false, "caught by catch(...) instead of runtime_error");
	}
}

// --- RTTI -------------------------------------------------------------------

struct Shape {
	virtual ~Shape() = default;
};
struct Circle : Shape {
	int radius = 7;
};
struct Square : Shape {};

// noinline + runtime argument keeps the compiler from folding the cast.
[[gnu::noinline]] std::unique_ptr<Shape> make_shape(int kind)
{
	if (kind == 0)
		return std::make_unique<Circle>();
	return std::make_unique<Square>();
}

void probe_rtti(int seed)
{
	auto shape = make_shape(seed);
	auto other = make_shape(seed + 1);
	const Circle* hit = dynamic_cast<const Circle*>(shape.get());
	const Circle* miss = dynamic_cast<const Circle*>(other.get());
	bool ok = hit && hit->radius == 7 && miss == nullptr;
	report("dynamic_cast", ok, std::string("hit=") + (hit ? "Circle" : "null") + " miss=" + (miss ? "Circle" : "null") + " typeid=" + typeid(*shape).name());
}

void write_log()
{
	std::ofstream out("sdmc:/3ds/probe.log", std::ios::trunc);
	if (!out) {
		std::printf("FAIL could not open sdmc:/3ds/probe.log\n");
		return;
	}
	out << g_log.str();
	out.close();
	std::printf("%s sdmc:/3ds/probe.log\n", out ? "wrote" : "FAIL writing");
}

#ifdef PROBE_WITH_SDL
bool probe_sdl()
{
	SDL_Window* window = SDL_CreateWindow("aleph-probe", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 400, 240, 0);
	if (!window) {
		report("SDL_CreateWindow", false, SDL_GetError());
		return false;
	}
	SDL_Surface* surface = SDL_GetWindowSurface(window);
	if (!surface) {
		report("SDL_GetWindowSurface", false, SDL_GetError());
		SDL_DestroyWindow(window);
		return false;
	}
	report("SDL_CreateWindow", true, std::to_string(surface->w) + "x" + std::to_string(surface->h) + " " + SDL_GetPixelFormatName(surface->format->format));

	SDL_FillRect(surface, nullptr, SDL_MapRGB(surface->format, 255, 0, 0));
	bool ok = SDL_UpdateWindowSurface(window) == 0;
	report("SDL red fill", ok, ok ? "top screen should be red for 3 s" : SDL_GetError());

	Uint32 start = SDL_GetTicks();
	while (SDL_GetTicks() - start < 3000) {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
		}
		SDL_Delay(16);
	}
	SDL_DestroyWindow(window);
	return ok;
}
#endif

} // namespace

int main(int argc, char** argv)
{
	(void)argv;
	bool sdl_video = false;

#ifdef PROBE_WITH_SDL
	SDL_SetMainReady();
	// SDL's N3DS video driver owns gfxInit; put the console on the bottom
	// screen afterwards, which SDL leaves alone.
	sdl_video = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) == 0;
#endif
	if (!sdl_video)
		gfxInitDefault();
	consoleInit(GFX_BOTTOM, nullptr);

	std::printf("Aleph One 3DS probe\n\n");
#ifdef PROBE_WITH_SDL
	{
		SDL_version v;
		SDL_GetVersion(&v);
		report("SDL_Init", sdl_video, sdl_video ? "SDL " + std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch) : std::string(SDL_GetError()));
	}
#else
	note("built without SDL (PROBE_WITH_SDL=OFF)");
#endif

	probe_filesystem();
	probe_exceptions();
	probe_rtti(argc > 1 ? 1 : 0);

#ifdef PROBE_WITH_SDL
	if (sdl_video)
		probe_sdl();
#endif

	g_log << (g_failures ? "RESULT: " + std::to_string(g_failures) + " failure(s)" : std::string("RESULT: all passed")) << '\n';
	std::printf("\n%s\n", g_failures ? ("RESULT: " + std::to_string(g_failures) + " failure(s)").c_str() : "RESULT: all passed");
	write_log();
	std::printf("\nPress START to exit.\n");

	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;
		gspWaitForVBlank();
	}

#ifdef PROBE_WITH_SDL
	if (sdl_video) {
		SDL_Quit();
		return 0;
	}
#endif
	gfxExit();
	return 0;
}
