/*
 * libsndfile smoke test for the Aleph One 3DS port.
 *
 * Exercises exactly the libsndfile API Sound/SndfileDecoder.cpp uses:
 * sf_open_virtual() with SF_VIRTUAL_IO callbacks, sf_read_float() in
 * byte-sized chunks the way SndfileDecoder::Decode() calls it, sf_seek()
 * (SndfileDecoder::Rewind() and arbitrary seeks), sf_close(), SF_INFO.
 *
 * The WAV and AIFF inputs are written byte by byte here (not with
 * libsndfile's writer), held in memory and read back through the virtual
 * I/O callbacks. The test also lists the major formats the library was
 * built with and checks that Ogg / FLAC / MP3 streams are rejected.
 *
 * Built by scripts/3ds/build-libsndfile.sh for the 3DS (compile + link
 * check) and for the host (run). Optional argument (host only): path to a
 * real file, e.g. Marathon 2's Music.ogg, to report whether it opens.
 *
 * Exit status: 0 when every check passes.
 */
#include <sndfile.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __3DS__
#include <3ds.h>
#include <stdarg.h>
/* Also write the output to sdmc:/3ds/sndfile-test.log, like the OpenAL and
 * Boost tests: the console alone cannot be read from an emulator or over
 * 3dslink. */
static FILE *g_log;
static int tee_printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (g_log) {
		va_list aq;
		va_copy(aq, ap);
		vfprintf(g_log, fmt, aq);
		va_end(aq);
		fflush(g_log);
	}
	int n = vprintf(fmt, ap);
	va_end(ap);
	return n;
}
#define printf tee_printf
#endif

static int failures = 0;

#define CHECK(cond, ...) do {                                   \
		if (cond) { printf("PASS "); }                          \
		else { printf("FAIL "); failures++; }                   \
		printf(__VA_ARGS__); printf("\n");                      \
	} while (0)

/* ---- in-memory SF_VIRTUAL_IO -------------------------------------------- */

typedef struct {
	const unsigned char *data;
	sf_count_t len;
	sf_count_t pos;
} MemFile;

static sf_count_t mem_get_filelen(void *user)
{
	return ((MemFile *) user)->len;
}

static sf_count_t mem_seek(sf_count_t offset, int whence, void *user)
{
	MemFile *m = (MemFile *) user;
	sf_count_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? m->pos : m->len;
	sf_count_t np = base + offset;
	if (np < 0) np = 0;
	if (np > m->len) np = m->len;
	m->pos = np;
	return np;
}

static sf_count_t mem_read(void *ptr, sf_count_t count, void *user)
{
	MemFile *m = (MemFile *) user;
	sf_count_t avail = m->len - m->pos;
	if (count > avail) count = avail;
	if (count <= 0) return 0;
	memcpy(ptr, m->data + m->pos, (size_t) count);
	m->pos += count;
	return count;
}

static sf_count_t mem_write(const void *ptr, sf_count_t count, void *user)
{
	(void) ptr; (void) count; (void) user;
	return 0; /* read-only, like the engine's use */
}

static sf_count_t mem_tell(void *user)
{
	return ((MemFile *) user)->pos;
}

/* Same field order as SndfileDecoder.cpp's sf_virtual. */
static SF_VIRTUAL_IO mem_vio = {
	&mem_get_filelen, &mem_seek, &mem_read, &mem_write, &mem_tell
};

/* ---- test signal and file writers ---------------------------------------- */

enum { RATE = 22050, CHANNELS = 2, FRAMES = 3000 };

static int16_t sample_at(int frame, int ch)
{
	/* Deterministic, covers the whole int16 range, differs per channel. */
	uint32_t v = (uint32_t) frame * 2654435761u + (uint32_t) ch * 40503u;
	return (int16_t) (v >> 16);
}

static unsigned char *put_le16(unsigned char *p, uint32_t v) { p[0] = v; p[1] = v >> 8; return p + 2; }
static unsigned char *put_le32(unsigned char *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; return p + 4; }
static unsigned char *put_be16(unsigned char *p, uint32_t v) { p[0] = v >> 8; p[1] = v; return p + 2; }
static unsigned char *put_be32(unsigned char *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; return p + 4; }
static unsigned char *put_tag(unsigned char *p, const char *t) { memcpy(p, t, 4); return p + 4; }

/* 16-bit PCM little-endian RIFF/WAVE. */
static unsigned char *make_wav(sf_count_t *len)
{
	uint32_t data_bytes = FRAMES * CHANNELS * 2;
	*len = 44 + data_bytes;
	unsigned char *buf = malloc((size_t) *len), *p = buf;
	p = put_tag(p, "RIFF"); p = put_le32(p, 36 + data_bytes); p = put_tag(p, "WAVE");
	p = put_tag(p, "fmt "); p = put_le32(p, 16);
	p = put_le16(p, 1);                         /* WAVE_FORMAT_PCM */
	p = put_le16(p, CHANNELS);
	p = put_le32(p, RATE);
	p = put_le32(p, RATE * CHANNELS * 2);
	p = put_le16(p, CHANNELS * 2);
	p = put_le16(p, 16);
	p = put_tag(p, "data"); p = put_le32(p, data_bytes);
	for (int f = 0; f < FRAMES; f++)
		for (int c = 0; c < CHANNELS; c++)
			p = put_le16(p, (uint16_t) sample_at(f, c));
	return buf;
}

/* 80-bit IEEE extended, as AIFF's COMM sampleRate. Positive integers only. */
static unsigned char *put_ext80(unsigned char *p, uint32_t value)
{
	int e = 31;
	while (!(value & 0x80000000u)) { value <<= 1; e--; }
	p = put_be16(p, 16383 + e);
	p = put_be32(p, value);
	p = put_be32(p, 0);
	return p;
}

/* 16-bit PCM big-endian AIFF. */
static unsigned char *make_aiff(sf_count_t *len)
{
	uint32_t data_bytes = FRAMES * CHANNELS * 2;
	uint32_t comm = 8 + 18, ssnd = 8 + 8 + data_bytes;
	*len = 12 + comm + ssnd;
	unsigned char *buf = malloc((size_t) *len), *p = buf;
	p = put_tag(p, "FORM"); p = put_be32(p, 4 + comm + ssnd); p = put_tag(p, "AIFF");
	p = put_tag(p, "COMM"); p = put_be32(p, 18);
	p = put_be16(p, CHANNELS);
	p = put_be32(p, FRAMES);
	p = put_be16(p, 16);
	p = put_ext80(p, RATE);
	p = put_tag(p, "SSND"); p = put_be32(p, 8 + data_bytes);
	p = put_be32(p, 0); p = put_be32(p, 0);     /* offset, blockSize */
	for (int f = 0; f < FRAMES; f++)
		for (int c = 0; c < CHANNELS; c++)
			p = put_be16(p, (uint16_t) sample_at(f, c));
	return buf;
}

/* ---- decode checks -------------------------------------------------------- */

static int compare_frames(const float *got, int first_frame, int nframes)
{
	int bad = 0;
	for (int f = 0; f < nframes; f++)
		for (int c = 0; c < CHANNELS; c++) {
			float want = sample_at(first_frame + f, c) / 32768.0f;
			if (got[f * CHANNELS + c] != want) {
				if (bad < 3)
					printf("     frame %d ch %d: got %.9g want %.9g\n",
					       first_frame + f, c, got[f * CHANNELS + c], want);
				bad++;
			}
		}
	return bad;
}

static void test_decode(const char *label, unsigned char *data, sf_count_t len, int want_major)
{
	MemFile m = { data, len, 0 };
	SF_INFO info;
	memset(&info, 0, sizeof info);           /* SndfileDecoder sets format = 0 */

	SNDFILE *sf = sf_open_virtual(&mem_vio, SFM_READ, &info, &m);
	CHECK(sf != NULL, "%s: sf_open_virtual (%s)", label, sf ? "ok" : sf_strerror(NULL));
	if (!sf) return;

	CHECK((info.format & SF_FORMAT_TYPEMASK) == want_major
	      && (info.format & SF_FORMAT_SUBMASK) == SF_FORMAT_PCM_16,
	      "%s: SF_INFO.format 0x%06x", label, info.format);
	CHECK(info.channels == CHANNELS && info.samplerate == RATE && info.frames == FRAMES,
	      "%s: SF_INFO channels=%d samplerate=%d frames=%lld seekable=%d",
	      label, info.channels, info.samplerate, (long long) info.frames, info.seekable);

	/* Decode the whole stream the way SndfileDecoder::Decode does: a byte
	   buffer, sf_read_float(max_length / 4) * 4. 4096 bytes = 512 frames. */
	enum { MAX_LENGTH = 4096 };
	float *all = malloc(FRAMES * CHANNELS * sizeof(float));
	unsigned char chunk[MAX_LENGTH];
	int32_t total_bytes = 0, calls = 0;
	for (;;) {
		int32_t got = (int32_t) sf_read_float(sf, (float *) chunk, MAX_LENGTH / 4) * 4;
		if (got <= 0) break;
		if ((size_t) total_bytes + (size_t) got > FRAMES * CHANNELS * sizeof(float)) { total_bytes = -1; break; }
		memcpy((unsigned char *) all + total_bytes, chunk, (size_t) got);
		total_bytes += got;
		calls++;
	}
	CHECK(total_bytes == (int32_t) (FRAMES * CHANNELS * sizeof(float)),
	      "%s: chunked sf_read_float returned %d bytes in %d calls", label, (int) total_bytes, (int) calls);
	if (total_bytes > 0) {
		int bad = compare_frames(all, 0, FRAMES);
		CHECK(bad == 0, "%s: all %d samples equal int16/32768 exactly (%d mismatches)",
		      label, FRAMES * CHANNELS, bad);
	}

	/* SndfileDecoder::Rewind */
	sf_count_t pos = sf_seek(sf, 0, SEEK_SET);
	float buf[64 * CHANNELS];
	sf_count_t n = sf_read_float(sf, buf, 64 * CHANNELS);
	CHECK(pos == 0 && n == 64 * CHANNELS && compare_frames(buf, 0, 64) == 0,
	      "%s: sf_seek(0, SEEK_SET) rewind + re-read", label);

	/* Arbitrary seeks */
	pos = sf_seek(sf, 1234, SEEK_SET);
	n = sf_read_float(sf, buf, 64 * CHANNELS);
	CHECK(pos == 1234 && n == 64 * CHANNELS && compare_frames(buf, 1234, 64) == 0,
	      "%s: sf_seek(1234, SEEK_SET)", label);
	pos = sf_seek(sf, -100, SEEK_CUR);           /* now at 1234 + 64 - 100 */
	n = sf_read_float(sf, buf, 10 * CHANNELS);
	CHECK(pos == 1198 && n == 10 * CHANNELS && compare_frames(buf, 1198, 10) == 0,
	      "%s: sf_seek(-100, SEEK_CUR)", label);
	pos = sf_seek(sf, -5, SEEK_END);
	n = sf_read_float(sf, buf, 64 * CHANNELS);
	CHECK(pos == FRAMES - 5 && n == 5 * CHANNELS && compare_frames(buf, FRAMES - 5, 5) == 0,
	      "%s: sf_seek(-5, SEEK_END) + short read at EOF", label);

	CHECK(sf_close(sf) == 0, "%s: sf_close", label);
	free(all);
}

/* A stream with the given magic must be rejected (no codec compiled in). */
static void test_rejected(const char *label, const char *magic, size_t magic_len)
{
	unsigned char buf[512];
	memset(buf, 0, sizeof buf);
	memcpy(buf, magic, magic_len);
	MemFile m = { buf, sizeof buf, 0 };
	SF_INFO info;
	memset(&info, 0, sizeof info);
	SNDFILE *sf = sf_open_virtual(&mem_vio, SFM_READ, &info, &m);
	CHECK(sf == NULL, "%s stream rejected: \"%s\"", label, sf ? "(opened!)" : sf_strerror(NULL));
	if (sf) sf_close(sf);
}

static void list_formats(void)
{
	char version[128];
	sf_command(NULL, SFC_GET_LIB_VERSION, version, sizeof version);
	printf("INFO %s\n", version);

	int count = 0, have_ogg = 0, have_flac = 0, have_mpeg = 0;
	sf_command(NULL, SFC_GET_FORMAT_MAJOR_COUNT, &count, sizeof count);
	printf("INFO %d major formats:\n", count);
	for (int i = 0; i < count; i++) {
		SF_FORMAT_INFO fi;
		memset(&fi, 0, sizeof fi);
		fi.format = i;
		sf_command(NULL, SFC_GET_FORMAT_MAJOR, &fi, sizeof fi);
		printf("     0x%06x  %-5s %s\n", fi.format, fi.extension ? fi.extension : "", fi.name);
		if (fi.format == SF_FORMAT_OGG)  have_ogg = 1;
		if (fi.format == SF_FORMAT_FLAC) have_flac = 1;
		if (fi.format == SF_FORMAT_MPEG) have_mpeg = 1;
	}

	int scount = 0;
	sf_command(NULL, SFC_GET_FORMAT_SUBTYPE_COUNT, &scount, sizeof scount);
	printf("INFO %d subtypes:", scount);
	for (int i = 0; i < scount; i++) {
		SF_FORMAT_INFO fi;
		memset(&fi, 0, sizeof fi);
		fi.format = i;
		sf_command(NULL, SFC_GET_FORMAT_SUBTYPE, &fi, sizeof fi);
		printf("%s %s", i ? "," : "", fi.name);
	}
	printf("\n");

	CHECK(count > 0, "SFC_GET_FORMAT_MAJOR_COUNT = %d", count);
	CHECK(!have_ogg && !have_flac && !have_mpeg,
	      "no-codec build: OGG listed=%d FLAC listed=%d MPEG listed=%d (expect 0 0 0)",
	      have_ogg, have_flac, have_mpeg);
}

#ifndef __3DS__
static void try_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) { printf("INFO cannot open %s\n", path); return; }
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *data = malloc((size_t) len);
	size_t rd = fread(data, 1, (size_t) len, f);
	fclose(f);
	MemFile m = { data, (sf_count_t) rd, 0 };
	SF_INFO info;
	memset(&info, 0, sizeof info);
	SNDFILE *sf = sf_open_virtual(&mem_vio, SFM_READ, &info, &m);
	if (sf) {
		printf("INFO %s: OPENED format=0x%06x channels=%d rate=%d frames=%lld\n",
		       path, info.format, info.channels, info.samplerate, (long long) info.frames);
		sf_close(sf);
	} else {
		printf("INFO %s: NOT OPENED: %s\n", path, sf_strerror(NULL));
	}
	free(data);
}
#endif

int main(int argc, char **argv)
{
#ifdef __3DS__
	gfxInitDefault();
	consoleInit(GFX_TOP, NULL);
	g_log = fopen("sdmc:/3ds/sndfile-test.log", "w");
#endif

	list_formats();

	sf_count_t len;
	unsigned char *wav = make_wav(&len);
	test_decode("WAV", wav, len, SF_FORMAT_WAV);
	free(wav);

	unsigned char *aiff = make_aiff(&len);
	test_decode("AIFF", aiff, len, SF_FORMAT_AIFF);
	free(aiff);

	test_rejected("Ogg ('OggS')", "OggS\0\2", 6);
	test_rejected("FLAC ('fLaC')", "fLaC\0\0\0\x22", 8);
	test_rejected("MP3 (ID3v2.3)", "ID3\3\0\0\0\0\0\0", 10);

#ifndef __3DS__
	for (int i = 1; i < argc; i++) try_file(argv[i]);
#else
	(void) argc; (void) argv;
#endif

	printf("RESULT: %s (%d failure%s)\n", failures ? "FAILED" : "all passed",
	       failures, failures == 1 ? "" : "s");

#ifdef __3DS__
	printf("Press START to exit.\n");
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START) break;
		gspWaitForVBlank();
	}
	if (g_log) fclose(g_log);
	gfxExit();
#endif
	return failures ? 1 : 0;
}
