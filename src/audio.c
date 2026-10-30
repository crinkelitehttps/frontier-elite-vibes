/*
 * audio.c - the game's sound effects (embedded 22kHz WAVs) and music
 * (embedded Ogg Vorbis, when built with OGG_MUSIC), mixed in the SDL audio
 * callback.
 */
#include <SDL.h>

#include "audio.h"
#include "frame_prof.h"
#include "m68000.h"
#include "main.h"

#include "music_data.h"
#include "sfx_data.h"

#ifdef OGG_MUSIC
#define OGG_IMPL
#define VORBIS_IMPL
#include "minivorbis.h"
#endif

BOOL bDisableSound = FALSE;
int audio_volume = 100;


#define SND_FREQ     22050
#define NUM_CHANNELS 4 /* sound effects playing at once */
#define NUM_SFX      33
#define NUM_MUSIC    8

typedef struct
{
	Uint8 *buf;
	int pos, len;
	int loop; /* position to loop back to, -1 = play once */
} Sample;

static Sample sfx[NUM_SFX];
static Sample channels[NUM_CHANNELS];
static bool audio_running;

/* =========================================================================
 * Music
 * ========================================================================= */
#ifdef OGG_MUSIC
static const MusicData music_data[NUM_MUSIC] = {
	{music_00_ogg, sizeof(music_00_ogg)}, {music_01_ogg, sizeof(music_01_ogg)},
	{music_02_ogg, sizeof(music_02_ogg)}, {music_03_ogg, sizeof(music_03_ogg)},
	{music_04_ogg, sizeof(music_04_ogg)}, {music_05_ogg, sizeof(music_05_ogg)},
	{music_06_ogg, sizeof(music_06_ogg)}, {music_07_ogg, sizeof(music_07_ogg)}};

static OggVorbis_File music_file;
static int music_mode;     /* see Call_PlayMusic */
static int enabled_tracks; /* bit per track */
static bool music_playing;

/* libvorbis reads the Ogg data through SDL_RWops */
static size_t rw_read(void *ptr, size_t size, size_t n, void *src)
{
	return SDL_RWread(src, ptr, size, n);
}
static int rw_seek(void *src, ogg_int64_t off, int whence)
{
	return SDL_RWseek(src, off, whence) >= 0 ? 0 : -1;
}
static int rw_close(void *src)
{
	return SDL_RWclose(src), 0;
}
static long rw_tell(void *src)
{
	return (long)SDL_RWtell(src);
}
static const ov_callbacks rw_callbacks = {rw_read, rw_seek, rw_close, rw_tell};

/* Closes the stream without minivorbis' own teardown */
static void close_music(void)
{
	if (music_file.datasource)
		SDL_RWclose((SDL_RWops *)music_file.datasource);
	memset(&music_file, 0, sizeof(music_file));
	music_playing = false;
}

static void play_music(int track)
{
	if (music_playing)
		close_music();
	if (track < 0 || track >= NUM_MUSIC)
		return;

	SDL_RWops *rw = SDL_RWFromConstMem(music_data[track].data, (int)music_data[track].len);
	if (!rw)
	{
		log_printf("Music track %d: %s\n", track, SDL_GetError());
		return;
	}
	if (ov_open_callbacks(rw, &music_file, NULL, 0, rw_callbacks) < 0)
	{
		log_printf("Could not decode music track %d\n", track);
		SDL_RWclose(rw);
		return;
	}
	music_playing = true;
}

static int random_track(void)
{
	if (!enabled_tracks)
		return -1;
	int track;
	do
		track = rand() % NUM_MUSIC;
	while (!(enabled_tracks & (1 << track)));
	return track;
}

/* Mixes music into the buffer; moves on to the next track at the end */
static void mix_music(Uint8 *buf, int len)
{
	for (int done = 0; music_playing && done < len;)
	{
		int section;
		long n = ov_read(&music_file, (char *)buf + done, len - done, 0, 2, 1, &section);
		if (n <= 0)
		{
			if (music_mode == -1)
				play_music(random_track());
			else
				close_music();
			break;
		}
		done += (int)n;
	}
}
#endif /* OGG_MUSIC */

/* D0 = mode: -2 random track once (no Blue Danube / reward music),
 *            -1 random tracks continuously, 0+ that track once.
 * D1:D2 = one byte per track, non-zero if the track is enabled. */
void Call_PlayMusic(void)
{
#ifdef OGG_MUSIC
	u32 mask[2] = {(u32)GetReg(REG_D1), (u32)GetReg(REG_D2)};
	enabled_tracks = 0;
	for (int t = 0; t < NUM_MUSIC; t++)
		if (mask[t / 4] & (0xffu << (24 - 8 * (t % 4))))
			enabled_tracks |= 1 << t;

	music_mode = GetReg(REG_D0);
	SDL_LockAudio();
	if (music_mode == -2)
	{
		enabled_tracks &= ~(0x40 | 0x80);
		play_music(random_track());
	}
	else if (music_mode == -1)
	{
		play_music(random_track());
	}
	else
	{
		play_music(music_mode);
	}
	SDL_UnlockAudio();
#endif
}

void Call_StopMusic(void)
{
#ifdef OGG_MUSIC
	SDL_LockAudio();
	close_music();
	SDL_UnlockAudio();
#endif
}

void Call_IsMusicPlaying(void)
{
#ifdef OGG_MUSIC
	SetReg(REG_D0, music_playing);
#else
	SetReg(REG_D0, 0);
#endif
}

/* =========================================================================
 * Sound effects
 * ========================================================================= */

/* D0 = sample, D1 = channel */
void Call_PlaySFX(void)
{
	int sample = (short)GetReg(REG_D0), chan = (short)GetReg(REG_D1);
	if (sample < 0 || sample >= NUM_SFX || chan < 0 || chan >= NUM_CHANNELS)
		return;
	SDL_LockAudio();
	channels[chan] = sfx[sample];
	channels[chan].pos = 0;
	SDL_UnlockAudio();
}

static Sint16 clamp16(int v)
{
	return (Sint16)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

/* SDL audio thread: 16 bit signed stereo */
static void audio_callback(void *userdata, Uint8 *stream, int len)
{
	(void)userdata;
	uint64_t prof_t0 = PROF_BEGIN();
	memset(stream, 0, (size_t)len);
#ifdef OGG_MUSIC
	mix_music(stream, len);
#endif

	Sint16 *out = (Sint16 *)stream;
	for (int i = 0; i < len / 4; i++)
	{
		int mix = 0;
		for (int c = 0; c < NUM_CHANNELS; c++)
		{
			Sample *s = &channels[c];
			if (!s->buf)
				continue;
			mix += *(const Sint16 *)(s->buf + s->pos);
			s->pos += 2;
			if (s->pos >= s->len)
			{
				if (s->loop >= 0)
					s->pos = s->loop;
				else
					s->buf = NULL;
			}
		}
		if (mix)
		{
			out[2 * i] = clamp16(out[2 * i] + mix);
			out[2 * i + 1] = clamp16(out[2 * i + 1] + mix);
		}
	}

	/* the volume, squared so the slider feels even to the ear */
	int vol = audio_volume < 0 ? 0 : audio_volume > 100 ? 100 : audio_volume;
	if (vol < 100)
	{
		int gain = vol * vol * 256 / 10000;
		for (int i = 0; i < len / 2; i++)
			out[i] = (Sint16)((out[i] * gain) >> 8);
	}
	if (prof_on)
		prof_audio_add(prof_t0);
}

/* Loads an embedded WAV as 22kHz 16 bit signed (8 bit unsigned is
 * converted, other formats are rejected) */
static void load_sfx(Sample *s, const unsigned char *data, unsigned int len, int index)
{
	SDL_AudioSpec spec;
	Uint8 *buf;
	Uint32 bytes;
	s->buf = NULL;

	SDL_RWops *rw = SDL_RWFromConstMem(data, (int)len);
	if (!rw || !SDL_LoadWAV_RW(rw, 1, &spec, &buf, &bytes))
	{
		log_printf("Sound effect %d: %s\n", index, SDL_GetError());
		return;
	}
	if (spec.freq != SND_FREQ || (spec.format != AUDIO_U8 && spec.format != AUDIO_S16))
	{
		log_printf("Sound effect %d has an unsupported format\n", index);
		SDL_FreeWAV(buf);
		return;
	}
	if (spec.format == AUDIO_U8)
	{
		Sint16 *wide = SDL_malloc(bytes * 2);
		if (!wide)
		{
			SDL_FreeWAV(buf);
			return;
		}
		for (Uint32 i = 0; i < bytes; i++)
			wide[i] = (Sint16)((buf[i] ^ 128) << 8);
		SDL_FreeWAV(buf);
		buf = (Uint8 *)wide;
		bytes *= 2;
	}
	s->buf = buf;
	s->len = (int)bytes;
	s->loop = (index == 19) ? SND_FREQ /* hyperspace */ : (index == 23) ? 0 /* noise */ : -1;
}

/* =========================================================================
 * Start up
 * ========================================================================= */
static unsigned char *const sfx_wav[NUM_SFX] = {
	sfx_00_wav, sfx_01_wav, sfx_02_wav, sfx_03_wav, sfx_04_wav, sfx_05_wav, sfx_06_wav,
	sfx_07_wav, sfx_08_wav, sfx_09_wav, sfx_10_wav, sfx_11_wav, sfx_12_wav, sfx_13_wav,
	sfx_14_wav, sfx_15_wav, sfx_16_wav, sfx_17_wav, sfx_18_wav, sfx_19_wav, sfx_20_wav,
	sfx_21_wav, sfx_22_wav, sfx_23_wav, sfx_24_wav, sfx_25_wav, sfx_26_wav, sfx_27_wav,
	sfx_28_wav, sfx_29_wav, sfx_30_wav, sfx_31_wav, sfx_32_wav};
static const unsigned int sfx_wav_len[NUM_SFX] = {
	sizeof(sfx_00_wav), sizeof(sfx_01_wav), sizeof(sfx_02_wav), sizeof(sfx_03_wav), sizeof(sfx_04_wav),
	sizeof(sfx_05_wav), sizeof(sfx_06_wav), sizeof(sfx_07_wav), sizeof(sfx_08_wav), sizeof(sfx_09_wav),
	sizeof(sfx_10_wav), sizeof(sfx_11_wav), sizeof(sfx_12_wav), sizeof(sfx_13_wav), sizeof(sfx_14_wav),
	sizeof(sfx_15_wav), sizeof(sfx_16_wav), sizeof(sfx_17_wav), sizeof(sfx_18_wav), sizeof(sfx_19_wav),
	sizeof(sfx_20_wav), sizeof(sfx_21_wav), sizeof(sfx_22_wav), sizeof(sfx_23_wav), sizeof(sfx_24_wav),
	sizeof(sfx_25_wav), sizeof(sfx_26_wav), sizeof(sfx_27_wav), sizeof(sfx_28_wav), sizeof(sfx_29_wav),
	sizeof(sfx_30_wav), sizeof(sfx_31_wav), sizeof(sfx_32_wav)};

void Audio_Init(void)
{
	if (bDisableSound)
	{
		log_printf("Sound: disabled\n");
		return;
	}
	if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
	{
		log_printf("Could not init audio: %s\n", SDL_GetError());
		return;
	}

	SDL_AudioSpec want = {0};
	want.freq = SND_FREQ;
	want.format = AUDIO_S16;
	want.channels = 2;
	want.samples = 1024;
	want.callback = audio_callback;
	if (SDL_OpenAudio(&want, NULL) != 0)
	{
		log_printf("Can't use audio: %s\n", SDL_GetError());
		return;
	}

	for (int i = 0; i < NUM_SFX; i++)
		load_sfx(&sfx[i], sfx_wav[i], sfx_wav_len[i], i);

	Audio_EnableAudio(TRUE);
}

void Audio_UnInit(void)
{
	Audio_EnableAudio(FALSE);
	SDL_CloseAudio();
	for (int i = 0; i < NUM_SFX; i++)
	{
		if (sfx[i].buf)
			SDL_free(sfx[i].buf); /* SDL_FreeWAV is SDL_free */
		sfx[i].buf = NULL;
	}
#ifdef OGG_MUSIC
	close_music();
#endif
}

void Audio_EnableAudio(BOOL enable)
{
	if ((bool)enable == audio_running)
		return;
	SDL_PauseAudio(enable ? 0 : 1);
	audio_running = enable != 0;
}
