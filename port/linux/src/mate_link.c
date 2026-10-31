/*
MATE_LINK.C

The link to "mate", the voice program of the campaign's teammate Kevin
(port/linux/game/companion.c): it listens on this computer, and the game
connects to it, again whenever the connection drops. Frames are
[u8 kind][u32 big-endian length][payload]: kind 1 a JSON object, kind 2
Kevin's voice, mono 16-bit little-endian samples at the rate the "hello"
message gives; kind 3, from the game, dialogue it played for Kevin to hear:
[u16 big-endian length][JSON][mono 16-bit little-endian samples]; kind 4,
from the game, the player's view mate asked for ("view"):
[u16 big-endian length][JSON: its size, the spots marked][RGB pixels].

The JSON messages wait in a queue for the game's tick (mate_link_next_message);
the voice is resampled to the mix's 48 kHz into a buffer of its own, and fed
from there into a DirectSound stream of its own (dsound_sdl.c), a 3D voice
the mixer plays as it plays the game's sounds: companion.c places it each
tick as the sound manager places a unit's dialogue, or sets it over the
radio when he's far off or out of sight. "stop_audio" (the player talked over
him) empties it.
*/

#include "platform.h"
#include "posix.h"
#include "mate_link.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MATE_PORT 47800
#define MATE_RATE 48000
/* the voice buffered at most: speech arrives faster than it plays */
#define MATE_RING_SAMPLES (MATE_RATE * 60)
/* how much is buffered before it plays (0: from the first audio; more holds
the first words back but rides out audio arriving late) */
#define MATE_PREBUFFER_SAMPLES (MATE_RATE / 20)
/* how far ahead of the mixer the stream is kept fed: topped up to the most
once below the least (packets are retired on the game's thread, at
DirectSoundDoWork: 64 of 50 ms carry it over a 3 second stall) */
#define MATE_STREAM_LEAST (MATE_RATE / 20)
#define MATE_STREAM_MOST (MATE_RATE / 10)
#define MATE_INBOX_MESSAGES 32
#define MATE_MESSAGE_SIZE 2048
#define MATE_KIND_JSON 1
#define MATE_KIND_AUDIO 2
#define MATE_KIND_HEAR 3
#define MATE_KIND_VIEW 4

static pthread_mutex_t mate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mate_send_lock = PTHREAD_MUTEX_INITIALIZER;
static int mate_started;
static int mate_socket = -1;
static int mate_source_rate = 24000;

static float *mate_ring;
static int mate_read, mate_count;
static int mate_playing;
/* his voice's stream, and the samples given it the mixer hasn't played (as
the mixer takes them: as many as it outputs) */
static LPDIRECTSOUNDSTREAM mate_stream;
static unsigned long mate_queued, mate_last_frames;
/* the radio: how much (wanted, and as it is, eased towards that); a narrow
band (two high passes at 300 Hz, two low passes at 3 kHz), overdriven, with
a hiss under the voice; and a burst of static as he keys the radio, and as
he lets go of it (his line over, "audio_end", or cut off) */
static float mate_radio, mate_radio_now;
typedef struct
{
	float b0, b1, b2, a1, a2;
	float x1, x2, y1, y2;
} mate_biquad;
static mate_biquad mate_band[4];
static unsigned long mate_noise = 1;
static int mate_keyed, mate_line_over, mate_squelch, mate_squelch_length;
/* resampling's place between the last source sample and the next */
static float mate_resample_phase, mate_last_sample;

static char mate_inbox[MATE_INBOX_MESSAGES][MATE_MESSAGE_SIZE];
static int mate_inbox_read, mate_inbox_count;

static int mate_receive_exactly(int socket, void *buffer, int length)
{
	char *bytes = buffer;

	while (length > 0)
	{
		int received = posix_socket_recv(socket, bytes, length, 0);

		if (received <= 0)
			return 0;
		bytes += received;
		length -= received;
	}
	return 1;
}

/* a Butterworth high or low pass (the Audio EQ Cookbook's) */
static void mate_biquad_set(mate_biquad *filter, float frequency, int high_pass)
{
	float w = 2.0f * 3.14159265f * frequency / MATE_RATE;
	float cosine = cosf(w);
	float alpha = sinf(w) / (2.0f * 0.70710678f);
	float side = (high_pass ? 1.0f + cosine : 1.0f - cosine) * 0.5f / (1.0f + alpha);

	memset(filter, 0, sizeof(*filter));
	filter->b0 = side;
	filter->b1 = high_pass ? -2.0f * side : 2.0f * side;
	filter->b2 = side;
	filter->a1 = -2.0f * cosine / (1.0f + alpha);
	filter->a2 = (1.0f - alpha) / (1.0f + alpha);
}

static float mate_biquad_run(mate_biquad *filter, float x)
{
	float y = filter->b0 * x + filter->b1 * filter->x1 + filter->b2 * filter->x2 - filter->a1 * filter->y1 - filter->a2 * filter->y2;

	filter->x2 = filter->x1;
	filter->x1 = x;
	filter->y2 = filter->y1;
	filter->y1 = y;
	return y;
}

/* white noise, -1 to 1 */
static float mate_static(void)
{
	mate_noise = (mate_noise * 1664525UL + 1013904223UL) & 0xFFFFFFFFUL;
	return (float)((mate_noise >> 8) & 0xFFFF) / 32768.0f - 1.0f;
}

/* (with the lock held) a burst of static */
static void mate_squelch_start(float seconds)
{
	mate_squelch = mate_squelch_length = (int)(seconds * MATE_RATE);
}

/* (with the lock held) he lets go of the radio */
static void mate_unkey(void)
{
	if (mate_keyed)
	{
		mate_keyed = 0;
		mate_squelch_start(0.18f);
	}
}

static void mate_clear_voice(void)
{
	pthread_mutex_lock(&mate_lock);
	mate_count = 0;
	mate_playing = 0;
	mate_queued = 0;
	mate_unkey();
	pthread_mutex_unlock(&mate_lock);
	if (mate_stream)
		IDirectSoundStream_Flush(mate_stream);
}

/* (on the link's thread) samples at the source rate, into the buffer at 48 kHz */
static void mate_add_voice(const unsigned char *pcm, int length)
{
	float step = (float)mate_source_rate / MATE_RATE;
	int index;

	pthread_mutex_lock(&mate_lock);
	for (index = 0; index + 1 < length; index += 2)
	{
		float sample = (float)(short)(pcm[index] | pcm[index + 1] << 8) / 32768.0f;

		/* (linear interpolation between the last sample and this one) */
		while (mate_resample_phase < 1.0f)
		{
			if (mate_count < MATE_RING_SAMPLES)
			{
				mate_ring[(mate_read + mate_count) % MATE_RING_SAMPLES] =
					mate_last_sample + (sample - mate_last_sample) * mate_resample_phase;
				mate_count++;
			}
			mate_resample_phase += step;
		}
		mate_resample_phase -= 1.0f;
		mate_last_sample = sample;
	}
	pthread_mutex_unlock(&mate_lock);
}

static void mate_handle_json(const char *json)
{
	if (strstr(json, "\"type\":\"hello\""))
	{
		const char *rate = strstr(json, "\"sample_rate\":");

		if (rate)
			mate_source_rate = atoi(rate + strlen("\"sample_rate\":"));
		platform_log("mate: connected (voice at %d Hz)", mate_source_rate);
	}
	if (strstr(json, "\"type\":\"stop_audio\""))
	{
		mate_clear_voice();
		return;
	}

	pthread_mutex_lock(&mate_lock);
	if (strstr(json, "\"type\":\"audio_end\""))
	{
		/* (he lets go once what's buffered has played) */
		mate_line_over = 1;
		if (!mate_playing && !mate_count)
			mate_unkey();
	}
	if (mate_inbox_count < MATE_INBOX_MESSAGES)
	{
		snprintf(mate_inbox[(mate_inbox_read + mate_inbox_count) % MATE_INBOX_MESSAGES], MATE_MESSAGE_SIZE, "%s", json);
		mate_inbox_count++;
	}
	pthread_mutex_unlock(&mate_lock);
}

static void *mate_thread(void *unused)
{
	(void)unused;
	for (;;)
	{
		struct sockaddr_in address;
		int socket = posix_socket(AF_INET, SOCK_STREAM, 0);

		memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
		/* (network byte order, on this little-endian machine: 127.0.0.1) */
		address.sin_port = (unsigned short)(MATE_PORT >> 8 | (MATE_PORT & 0xFF) << 8);
		address.sin_addr.s_addr = 0x0100007F;
		if (socket >= 0 && posix_socket_connect(socket, &address, sizeof(address)) == 0)
		{
			unsigned char header[5];

			posix_socket_set_nodelay(socket);
			pthread_mutex_lock(&mate_send_lock);
			mate_socket = socket;
			pthread_mutex_unlock(&mate_send_lock);

			while (mate_receive_exactly(socket, header, sizeof(header)))
			{
				unsigned long length = (unsigned long)header[1] << 24 | (unsigned long)header[2] << 16 |
					(unsigned long)header[3] << 8 | header[4];
				unsigned char *payload = malloc(length + 1);

				if (!payload || !mate_receive_exactly(socket, payload, (int)length))
				{
					free(payload);
					break;
				}
				payload[length] = 0;
				if (header[0] == MATE_KIND_JSON)
					mate_handle_json((const char *)payload);
				else if (header[0] == MATE_KIND_AUDIO)
					mate_add_voice(payload, (int)length);
				free(payload);
			}

			platform_log("mate: disconnected");
			pthread_mutex_lock(&mate_send_lock);
			mate_socket = -1;
			pthread_mutex_unlock(&mate_send_lock);
			mate_clear_voice();
		}
		if (socket >= 0)
			posix_socket_close(socket);
		SDL_Delay(2000);
	}
	return NULL;
}

void mate_link_start(void)
{
	pthread_t thread;

	if (mate_started)
		return;
	mate_started = 1;
	mate_ring = calloc(MATE_RING_SAMPLES, sizeof(float));
	{
		WAVEFORMATEX format;
		DSSTREAMDESC description;

		memset(&format, 0, sizeof(format));
		format.wFormatTag = WAVE_FORMAT_PCM;
		format.nChannels = 1;
		format.nSamplesPerSec = MATE_RATE;
		format.nAvgBytesPerSec = MATE_RATE * 2;
		format.nBlockAlign = 2;
		format.wBitsPerSample = 16;
		memset(&description, 0, sizeof(description));
		description.dwFlags = DSSTREAMCAPS_CTRL3D;
		description.lpwfxFormat = &format;
		if (IDirectSound_CreateSoundStream(NULL, &description, &mate_stream, NULL) < 0)
			mate_stream = NULL;
	}
	mate_biquad_set(&mate_band[0], 300.0f, 1);
	mate_biquad_set(&mate_band[1], 300.0f, 1);
	mate_biquad_set(&mate_band[2], 3000.0f, 0);
	mate_biquad_set(&mate_band[3], 3000.0f, 0);
	if (mate_ring)
		pthread_create(&thread, NULL, mate_thread, NULL);
}

int mate_link_connected(void)
{
	return mate_socket >= 0;
}

int mate_link_next_message(char *buffer, int size)
{
	int found = 0;

	pthread_mutex_lock(&mate_lock);
	if (mate_inbox_count > 0)
	{
		snprintf(buffer, size, "%s", mate_inbox[mate_inbox_read]);
		mate_inbox_read = (mate_inbox_read + 1) % MATE_INBOX_MESSAGES;
		mate_inbox_count--;
		found = 1;
	}
	pthread_mutex_unlock(&mate_lock);
	return found;
}

void mate_link_send(const char *json)
{
	unsigned long length = (unsigned long)strlen(json);
	unsigned char header[5];

	header[0] = MATE_KIND_JSON;
	header[1] = (unsigned char)(length >> 24);
	header[2] = (unsigned char)(length >> 16);
	header[3] = (unsigned char)(length >> 8);
	header[4] = (unsigned char)length;
	pthread_mutex_lock(&mate_send_lock);
	if (mate_socket >= 0)
	{
		posix_socket_send(mate_socket, header, sizeof(header), 0);
		posix_socket_send(mate_socket, json, (int)length, 0);
	}
	pthread_mutex_unlock(&mate_send_lock);
}

void mate_link_send_hear(const char *json, const short *samples, unsigned long count)
{
	unsigned long json_length = (unsigned long)strlen(json);
	unsigned long length = 2 + json_length + count * 2;
	unsigned char header[7];

	header[0] = MATE_KIND_HEAR;
	header[1] = (unsigned char)(length >> 24);
	header[2] = (unsigned char)(length >> 16);
	header[3] = (unsigned char)(length >> 8);
	header[4] = (unsigned char)length;
	header[5] = (unsigned char)(json_length >> 8);
	header[6] = (unsigned char)json_length;
	pthread_mutex_lock(&mate_send_lock);
	if (mate_socket >= 0)
	{
		/* (this machine is little-endian: the samples go as they are) */
		posix_socket_send(mate_socket, header, sizeof(header), 0);
		posix_socket_send(mate_socket, json, (int)json_length, 0);
		posix_socket_send(mate_socket, samples, (int)(count * 2), 0);
	}
	pthread_mutex_unlock(&mate_send_lock);
}

void mate_link_send_view(const char *json, const unsigned char *rgb, unsigned long size)
{
	unsigned long json_length = (unsigned long)strlen(json);
	unsigned long length = 2 + json_length + size;
	unsigned char header[7];

	header[0] = MATE_KIND_VIEW;
	header[1] = (unsigned char)(length >> 24);
	header[2] = (unsigned char)(length >> 16);
	header[3] = (unsigned char)(length >> 8);
	header[4] = (unsigned char)length;
	header[5] = (unsigned char)(json_length >> 8);
	header[6] = (unsigned char)json_length;
	pthread_mutex_lock(&mate_send_lock);
	if (mate_socket >= 0)
	{
		posix_socket_send(mate_socket, header, sizeof(header), 0);
		posix_socket_send(mate_socket, json, (int)json_length, 0);
		posix_socket_send(mate_socket, rgb, (int)size, 0);
	}
	pthread_mutex_unlock(&mate_send_lock);
}

void mate_link_set_radio(float radio)
{
	pthread_mutex_lock(&mate_lock);
	mate_radio = radio;
	pthread_mutex_unlock(&mate_lock);
}

struct IDirectSoundStream *mate_link_voice_stream(void)
{
	return mate_stream;
}

int mate_link_voice_playing(void)
{
	return mate_playing;
}

void mate_link_feed(unsigned long frames)
{
	/* (on the audio thread only: Process copies it) */
	static short samples[MATE_STREAM_MOST * 2 + MATE_RATE / 2];
	XMEDIAPACKET packet;
	DWORD status = 0;
	unsigned long want = 0, voice = 0, length = 0, frame;

	if (!mate_ring || !mate_stream)
		return;
	if (frames > MATE_RATE / 2)
		frames = MATE_RATE / 2;
	if (IDirectSoundStream_GetStatus(mate_stream, &status) < 0)
		status = 0;

	pthread_mutex_lock(&mate_lock);
	/* (what the mixer took last time) */
	mate_queued = mate_queued > mate_last_frames ? mate_queued - mate_last_frames : 0;
	mate_last_frames = frames;
	if (!mate_playing && mate_count > 0 && mate_count >= MATE_PREBUFFER_SAMPLES)
	{
		mate_playing = 1;
		if (!mate_keyed)
		{
			/* (a new line: he keys the radio) */
			mate_keyed = 1;
			mate_line_over = 0;
			mate_squelch_start(0.06f);
		}
	}
	if ((status & XMO_STATUSF_ACCEPT_INPUT_DATA) && mate_queued < MATE_STREAM_LEAST + frames)
		want = MATE_STREAM_MOST + frames - mate_queued;
	if (mate_playing)
		voice = (unsigned long)mate_count < want ? (unsigned long)mate_count : want;
	length = voice;
	if (mate_squelch > 0 && length < want)
		length = (unsigned long)mate_squelch < want ? (unsigned long)mate_squelch : want;
	mate_radio_now += (mate_radio - mate_radio_now) * 0.2f;
	for (frame = 0; frame < length; frame++)
	{
		float sample = frame < voice ? mate_ring[mate_read] : 0.0f;

		if (mate_radio_now > 0.01f)
		{
			/* (the band at 48 kHz, then driven into a soft clip for the grit) */
			float radio = sample + mate_static() * ((frame < voice ? 0.02f : 0.0f) +
				(mate_squelch > 0 ? 0.6f * (float)mate_squelch / (float)mate_squelch_length : 0.0f));
			int band;

			for (band = 0; band < 4; band++)
				radio = mate_biquad_run(&mate_band[band], radio);
			radio *= 5.0f;
			radio = radio / (1.0f + fabsf(radio)) * 0.7f;
			sample += (radio - sample) * mate_radio_now;
		}
		if (mate_squelch > 0)
			mate_squelch--;
		if (frame < voice)
			mate_read = (mate_read + 1) % MATE_RING_SAMPLES;
		sample = sample > 1.0f ? 1.0f : sample < -1.0f ? -1.0f : sample;
		samples[frame] = (short)(sample * 32767.0f);
	}
	mate_count -= (int)voice;
	mate_queued += length;
	if (mate_playing && !mate_count)
	{
		mate_playing = 0;
		if (mate_line_over)
			mate_unkey();
	}
	pthread_mutex_unlock(&mate_lock);

	if (length)
	{
		memset(&packet, 0, sizeof(packet));
		packet.pvBuffer = samples;
		packet.dwMaxSize = length * 2;
		IDirectSoundStream_Process(mate_stream, &packet, NULL);
	}
}
