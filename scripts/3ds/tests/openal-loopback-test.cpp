// Link/run test for the 3DS OpenAL Soft build (scripts/3ds/build-openal.sh).
//
// Exercises exactly the OpenAL paths Aleph One's OpenALManager uses:
// ALC_SOFT_loopback via alcGetProcAddress (and alGetProcAddress, which is
// what OpenALManager.cpp actually calls), a loopback device rendering
// ALC_SHORT_SOFT stereo at 44100 Hz, an AL_FORMAT_STEREO_FLOAT32 buffer,
// EFX low-pass filters, and the ALC_SOFT_HRTF queries.
//
// On the 3DS it prints to the bottom-screen console and to
// sdmc:/3ds/openal-test.log, then waits for START. Exit status 0 = all passed.
// Declare the EFX/extension prototypes too, so the direct symbols are
// link-checked in addition to the alGetProcAddress path the engine uses.
#define AL_ALEXT_PROTOTYPES 1
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <vector>

#ifdef __3DS__
#include <3ds.h>
#endif

namespace {

FILE *g_log = nullptr;
int g_failures = 0;

void out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    if(g_log)
    {
        va_start(ap, fmt);
        std::vfprintf(g_log, fmt, ap);
        va_end(ap);
        std::fflush(g_log);
    }
}

void check(bool ok, const char *what)
{
    out("%s %s\n", ok ? "PASS" : "FAIL", what);
    if(!ok) ++g_failures;
}

int run()
{
    out("OpenAL loopback test\n");

    check(alcIsExtensionPresent(nullptr, "ALC_SOFT_loopback") == ALC_TRUE,
        "alcIsExtensionPresent(NULL, ALC_SOFT_loopback)");

    auto loopbackOpen = reinterpret_cast<LPALCLOOPBACKOPENDEVICESOFT>(
        alcGetProcAddress(nullptr, "alcLoopbackOpenDeviceSOFT"));
    auto isRenderFormatSupported = reinterpret_cast<LPALCISRENDERFORMATSUPPORTEDSOFT>(
        alcGetProcAddress(nullptr, "alcIsRenderFormatSupportedSOFT"));
    auto renderSamples = reinterpret_cast<LPALCRENDERSAMPLESSOFT>(
        alcGetProcAddress(nullptr, "alcRenderSamplesSOFT"));
    check(loopbackOpen && isRenderFormatSupported && renderSamples,
        "alcGetProcAddress(loopback procs)");
    if(!loopbackOpen || !isRenderFormatSupported || !renderSamples)
        return 1;

    // OpenALManager loads these through alGetProcAddress.
    check(alGetProcAddress("alcRenderSamplesSOFT") == reinterpret_cast<void*>(renderSamples),
        "alGetProcAddress(alcRenderSamplesSOFT) matches alcGetProcAddress");

    ALCdevice *device = loopbackOpen(nullptr);
    check(device != nullptr, "alcLoopbackOpenDeviceSOFT(NULL)");
    if(!device) return 1;

    check(isRenderFormatSupported(device, 44100, ALC_STEREO_SOFT, ALC_SHORT_SOFT) == ALC_TRUE,
        "alcIsRenderFormatSupportedSOFT(44100, stereo, short)");

    ALCint numHrtf = -1;
    alcGetIntegerv(device, ALC_NUM_HRTF_SPECIFIERS_SOFT, 1, &numHrtf);
    out("     ALC_NUM_HRTF_SPECIFIERS_SOFT = %d\n", numHrtf);
    check(numHrtf >= 0, "alcGetIntegerv(ALC_NUM_HRTF_SPECIFIERS_SOFT)");

    const ALCint attrs[] = {
        ALC_FORMAT_TYPE_SOFT,     ALC_SHORT_SOFT,
        ALC_FORMAT_CHANNELS_SOFT, ALC_STEREO_SOFT,
        ALC_FREQUENCY,            44100,
        ALC_HRTF_SOFT,            ALC_FALSE,
        0
    };
    ALCcontext *context = alcCreateContext(device, attrs);
    check(context != nullptr, "alcCreateContext(loopback, short/stereo/44100)");
    if(!context) { alcCloseDevice(device); return 1; }
    check(alcMakeContextCurrent(context) == ALC_TRUE, "alcMakeContextCurrent");

    ALCint hrtfStatus = -1;
    alcGetIntegerv(device, ALC_HRTF_STATUS_SOFT, 1, &hrtfStatus);
    out("     ALC_HRTF_STATUS_SOFT = 0x%x\n", hrtfStatus);

    check(alIsExtensionPresent("AL_EXT_float32") == AL_TRUE, "AL_EXT_float32");
    check(alIsExtensionPresent("AL_SOFT_direct_channels_remix") == AL_TRUE,
        "AL_SOFT_direct_channels_remix");
    check(alIsExtensionPresent("AL_SOFT_source_spatialize") == AL_TRUE,
        "AL_SOFT_source_spatialize");

    // 0.1 s of a 440 Hz stereo float sine.
    std::vector<float> pcm(4410 * 2);
    for(size_t i = 0; i < pcm.size() / 2; ++i)
        pcm[i*2] = pcm[i*2 + 1] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f
            * static_cast<float>(i) / 44100.0f);

    ALuint buffer = 0, source = 0;
    alGenBuffers(1, &buffer);
    alBufferData(buffer, AL_FORMAT_STEREO_FLOAT32, pcm.data(),
        static_cast<ALsizei>(pcm.size() * sizeof(float)), 44100);
    check(alGetError() == AL_NO_ERROR, "alBufferData(AL_FORMAT_STEREO_FLOAT32)");

    // EFX low-pass filter, the way OpenALManager/SoundPlayer use it.
    auto pGenFilters = reinterpret_cast<LPALGENFILTERS>(alGetProcAddress("alGenFilters"));
    auto pDeleteFilters = reinterpret_cast<LPALDELETEFILTERS>(alGetProcAddress("alDeleteFilters"));
    auto pFilteri = reinterpret_cast<LPALFILTERI>(alGetProcAddress("alFilteri"));
    auto pFilterf = reinterpret_cast<LPALFILTERF>(alGetProcAddress("alFilterf"));
    check(pGenFilters && pDeleteFilters && pFilteri && pFilterf, "alGetProcAddress(EFX filter procs)");
    ALuint filter = 0;
    alGenFilters(1, &filter); // direct symbol (AL_ALEXT_PROTOTYPES)
    alFilteri(filter, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
    alFilterf(filter, AL_LOWPASS_GAIN, 1.0f);
    alFilterf(filter, AL_LOWPASS_GAINHF, 0.5f);
    check(alGetError() == AL_NO_ERROR, "alGenFilters + AL_FILTER_LOWPASS");

    alGenSources(1, &source);
    alSourcei(source, AL_BUFFER, static_cast<ALint>(buffer));
    alSourcei(source, AL_DIRECT_FILTER, static_cast<ALint>(filter));
    alSourcei(source, AL_DIRECT_CHANNELS_SOFT, AL_REMIX_UNMATCHED_SOFT);
    alSourcePlay(source);
    check(alGetError() == AL_NO_ERROR, "alSourcePlay");

    // Render 4 x 1024 frames, as the SDL audio callback would.
    std::vector<ALshort> rendered(1024 * 2);
    long peak = 0;
    for(int block = 0; block < 4; ++block)
    {
        renderSamples(device, rendered.data(), 1024);
        for(ALshort s : rendered)
            peak = std::max(peak, std::labs(static_cast<long>(s)));
    }
    check(alcGetError(device) == ALC_NO_ERROR, "alcRenderSamplesSOFT x4");
    out("     peak sample = %ld\n", peak);
    check(peak > 1000, "rendered audio is not silent");

    ALint state = 0;
    alGetSourcei(source, AL_SOURCE_STATE, &state);
    out("     source state = 0x%x\n", state);

    alSourceStop(source);
    alDeleteSources(1, &source);
    pDeleteFilters(1, &filter);
    alDeleteBuffers(1, &buffer);
    alcMakeContextCurrent(nullptr);
    alcDestroyContext(context);
    check(alcCloseDevice(device) == ALC_TRUE, "alcCloseDevice");

    out("RESULT: %s (%d failures)\n", g_failures ? "FAILED" : "all passed", g_failures);
    return g_failures ? 1 : 0;
}

} // namespace

int main()
{
#ifdef __3DS__
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, nullptr);
    g_log = std::fopen("sdmc:/3ds/openal-test.log", "w");
#endif
    const int rc = run();
#ifdef __3DS__
    if(g_log) std::fclose(g_log);
    std::printf("\nPress START to exit.\n");
    while(aptMainLoop())
    {
        hidScanInput();
        if(hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
    gfxExit();
#endif
    return rc;
}
