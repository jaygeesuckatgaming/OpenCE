/*
CHASM_BRIDGE.C

The OpenCE chasm bridge's game half. See chasm_bridge.h.

Each frame, while bridge.enabled is true and a single-player campaign is in
progress:
  - the talk key (the configured chat key, T by default) picks the nearest
    living NPC in front of the player within bridge.talk_radius and sends its
    identity plus a line to the worker (chasm_bridge_net.c);
  - the worker's answer is shown on the HUD, and its actions are applied with
    the engine's own AI scripting calls.

The line is bridge.debug_line until the text overlay lands; every other piece
of the turn (identity, profile, presence, generate, HUD) is the real one.
*/

#include "cseries.h"
#include "game/game.h"
#include "game/players.h"
#include "ai/actors.h"
#include "ai/actor_types.h"
#include "ai/ai_script.h"
#include "objects/objects.h"
#include "units/units.h"
#include "text/unicode.h"
#include "interface/hud_messaging.h"
#include "main/console.h"
#include "memory/data.h"
#include "halo_keyboard.h"
#include "network_chat.h"
#include "chasm_bridge.h"
#include "chasm_bridge_net.h"
#include "../src/voice_audio.h"
#include "sound/game_sound.h"
#include "sound/sound_dsound.h"

#include <xtl.h>
#include <stdio.h>
#include <string.h>

/* the platform layer's */
unsigned long system_milliseconds(void);

/* port_config.c's */
int config_boolean(const char *name);
const char *config_string(const char *name);
double config_real(const char *name);
unsigned long config_changes(void);

/* ---------- constants */

#define CHASM_BRIDGE_DEFAULT_RADIUS 6.0f
#define CHASM_BRIDGE_LOOK_HALF_ANGLE 0.55f
#define CHASM_BRIDGE_VOICE_RATE 48000
#define CHASM_BRIDGE_RECORD_FRAMES 750 /* 15 s of 20 ms frames */

/* ---------- globals */

static int chasm_bridge_configured;
static int chasm_bridge_enabled;
static unsigned long chasm_bridge_changes;
static real chasm_bridge_radius = CHASM_BRIDGE_DEFAULT_RADIUS;
static char chasm_bridge_line[512] = "Hello. Who are you?";

static long chasm_bridge_encounter_index = NONE;
static long chasm_bridge_unit_index = NONE;
static char chasm_bridge_target_key[CHASM_BRIDGE_ID_SIZE];
static char chasm_bridge_target_name[CHASM_BRIDGE_NAME_SIZE];
static char chasm_bridge_target_character[CHASM_BRIDGE_ID_SIZE];
static real chasm_bridge_target_distance;

static unsigned long chasm_bridge_key_held;
static int chasm_bridge_waiting;
static int chasm_bridge_handler_set;

/* the NPC's voice: one positional stream at a time */
static LPDIRECTSOUNDSTREAM chasm_bridge_voice_stream;
static unsigned long chasm_bridge_voice_end;
static long chasm_bridge_voice_unit = NONE;

/* push-to-talk: the microphone's 48 kHz frames, kept until the key is let go */
static int chasm_bridge_recording;
static float chasm_bridge_recorded[VOICE_FRAME_SAMPLES * CHASM_BRIDGE_RECORD_FRAMES];
static unsigned long chasm_bridge_recorded_samples;

/* ---------- helpers */

static void chasm_bridge_configure(void)
{
	const char *line;

	chasm_bridge_changes = config_changes();
	chasm_bridge_enabled = config_boolean("bridge.enabled");
	chasm_bridge_radius = (real)config_real("bridge.talk_radius");
	if (!(chasm_bridge_radius > 0.5f))
		chasm_bridge_radius = CHASM_BRIDGE_DEFAULT_RADIUS;

	line = config_string("bridge.debug_line");
	if (line && line[0])
	{
		strncpy(chasm_bridge_line, line, sizeof(chasm_bridge_line) - 1);
		chasm_bridge_line[sizeof(chasm_bridge_line) - 1] = 0;
	}

	chasm_bridge_configured = TRUE;

	return;
}

static long chasm_bridge_player_unit(void)
{
	long player_index;

	if (!players_globals || players_globals->local_player_count <= 0)
		return NONE;

	player_index = players_globals->local_players[0];
	if (player_index == NONE)
		return NONE;

	return player_get(player_index)->unit_index;
}

static const char *chasm_bridge_character_for_type(
	short actor_type)
{
	const char *name = actor_type_get_name(actor_type);

	if (!name)
		return NULL;

	if (!strcmp(name, "marine") || !strcmp(name, "crew"))
		return "UNSC Marine";
	if (!strcmp(name, "grunt"))
		return "Grunt";
	if (!strcmp(name, "elite"))
		return "Elite";
	if (!strcmp(name, "jackal"))
		return "Jackal";
	if (!strcmp(name, "hunter"))
		return "Hunter";

	return NULL;
}

static void chasm_bridge_print(
	const char *text)
{
	wchar_t wide[128];
	char line[64];
	const char *cursor = text;

	if (!text)
		return;

	while (*cursor)
	{
		int length = 0;
		int last_space = -1;

		while (cursor[length] && length < 60)
		{
			if (cursor[length] == ' ')
				last_space = length;
			length++;
		}
		if (cursor[length] && last_space > 20)
			length = last_space;

		memcpy(line, cursor, (size_t)length);
		line[length] = 0;
		ascii_to_wide(line, wide, NUMBEROF(wide));
		hud_print_message(0, wide);

		cursor += length;
		while (*cursor == ' ')
			cursor++;
	}

	return;
}

static int chasm_bridge_find_target(
	long player_unit)
{
	struct data_iterator iterator;
	struct actor_datum *actor;
	real_point3d player_head;
	real best_distance = chasm_bridge_radius;
	int found = FALSE;

	chasm_bridge_encounter_index = NONE;
	chasm_bridge_unit_index = NONE;

	if (!actor_data)
		return FALSE;

	unit_get_head_position(player_unit, &player_head);
	data_iterator_new(&iterator, actor_data);
	while ((actor = (struct actor_datum *)data_iterator_next(&iterator)) != NULL)
	{
		struct unit_datum *unit;
		real_point3d head;
		real distance;
		const char *character;
		const char *type_name;

		if (!actor->meta.active)
			continue;
		if (actor->meta.unit_index == NONE)
			continue;
		if (player_index_from_unit_index(actor->meta.unit_index) != NONE)
			continue;

		unit = (struct unit_datum *)object_try_and_get_and_verify_type(actor->meta.unit_index, _object_mask_unit);
		if (!unit)
			continue;
		if (TEST_FLAG(unit->object.damage_flags, _object_dead_bit))
			continue;

		character = chasm_bridge_character_for_type(actor->meta.type);
		if (!character)
			continue;

		unit_get_head_position(actor->meta.unit_index, &head);
		distance = distance3d(&player_head, &head);
		if (distance > chasm_bridge_radius || distance >= best_distance)
			continue;
		if (!unit_can_see_point(player_unit, &head, CHASM_BRIDGE_LOOK_HALF_ANGLE))
			continue;

		best_distance = distance;
		found = TRUE;
		chasm_bridge_encounter_index = actor->meta.encounter_index;
		chasm_bridge_unit_index = actor->meta.unit_index;
		chasm_bridge_target_distance = distance;
		strncpy(chasm_bridge_target_name, character, sizeof(chasm_bridge_target_name) - 1);
		chasm_bridge_target_name[sizeof(chasm_bridge_target_name) - 1] = 0;
		strncpy(chasm_bridge_target_character, character, sizeof(chasm_bridge_target_character) - 1);
		chasm_bridge_target_character[sizeof(chasm_bridge_target_character) - 1] = 0;

		type_name = actor_type_get_name(actor->meta.type);
		sprintf(chasm_bridge_target_key, "opence:%s:%ld", type_name ? type_name : "actor",
			actor->meta.unit_index);
	}

	return found;
}

static void chasm_bridge_run_actions(
	const struct chasm_bridge_result *result,
	long player_unit)
{
	int index;
	struct unit_datum *unit;

	/* single-player campaign only: a client has no actor datums */
	if (player_unit == NONE || game_is_cooperative())
		return;
	if (chasm_bridge_unit_index == NONE || chasm_bridge_encounter_index == NONE)
		return;

	unit = (struct unit_datum *)object_try_and_get_and_verify_type(chasm_bridge_unit_index, _object_mask_unit);
	if (!unit || TEST_FLAG(unit->object.damage_flags, _object_dead_bit))
		return;

	for (index = 0; index < result->action_count; index++)
	{
		const char *id = result->actions[index].id;

		if (!strcmp(id, "movement.follow_target"))
			ai_scripting_follow_target_unit(chasm_bridge_encounter_index, player_unit);
		else if (!strcmp(id, "movement.stop_following"))
			ai_scripting_follow_target_disable(chasm_bridge_encounter_index);
		else if (!strcmp(id, "npc.look_at_player"))
			ai_scripting_look_at_object(chasm_bridge_unit_index, player_unit);
		else if (!strcmp(id, "npc.stop_looking"))
			ai_scripting_stop_looking(chasm_bridge_unit_index);
		else if (!strcmp(id, "combat.start"))
			ai_scripting_berserk(chasm_bridge_encounter_index, TRUE);
		else if (!strcmp(id, "combat.stop"))
			ai_scripting_berserk(chasm_bridge_encounter_index, FALSE);
	}

	return;
}

static unsigned short chasm_bridge_read_u16(
	const unsigned char *bytes)
{
	return (unsigned short)(bytes[0] | (bytes[1] << 8));
}

static unsigned long chasm_bridge_read_u32(
	const unsigned char *bytes)
{
	return (unsigned long)bytes[0] | ((unsigned long)bytes[1] << 8) |
		((unsigned long)bytes[2] << 16) | ((unsigned long)bytes[3] << 24);
}

static void chasm_bridge_write_u16(
	unsigned char *bytes,
	unsigned short value)
{
	bytes[0] = (unsigned char)(value & 0xFF);
	bytes[1] = (unsigned char)((value >> 8) & 0xFF);
}

static void chasm_bridge_write_u32(
	unsigned char *bytes,
	unsigned long value)
{
	bytes[0] = (unsigned char)(value & 0xFF);
	bytes[1] = (unsigned char)((value >> 8) & 0xFF);
	bytes[2] = (unsigned char)((value >> 16) & 0xFF);
	bytes[3] = (unsigned char)((value >> 24) & 0xFF);
}

static void chasm_bridge_stop_voice(
	void)
{
	if (chasm_bridge_voice_stream)
	{
		IDirectSoundStream_Release(chasm_bridge_voice_stream);
		chasm_bridge_voice_stream = NULL;
	}
	if (chasm_bridge_voice_unit != NONE)
	{
		game_sound_set_mouth_aperture(chasm_bridge_voice_unit, 0.0f);
		chasm_bridge_voice_unit = NONE;
	}
	chasm_bridge_voice_end = 0;

	return;
}

/* one WAV (StyleTTS2: 24 kHz mono PCM) as a positional stream at the NPC's
head. The platform decodes/copies the packet, so the caller may free the
buffer as soon as this returns. */
static void chasm_bridge_play_audio(
	const unsigned char *wav,
	unsigned long size,
	long unit_index,
	const real_point3d *head)
{
	const unsigned char *format_chunk = NULL;
	const unsigned char *data_chunk = NULL;
	unsigned long format_size = 0;
	unsigned long data_size = 0;
	unsigned long offset;
	unsigned long sample_rate;
	unsigned long byte_rate;
	unsigned long duration;
	unsigned short format_tag;
	unsigned short channels;
	unsigned short block_align;
	unsigned short bits;
	DSSTREAMDESC desc;
	WAVEFORMATEX format;
	XMEDIAPACKET packet;
	LPDIRECTSOUND sound;
	LPDIRECTSOUNDSTREAM stream;

	if (!wav || size < 12)
		return;
	if (memcmp(wav, "RIFF", 4) || memcmp(wav + 8, "WAVE", 4))
		return;

	offset = 12;
	while (offset + 8 <= size)
	{
		const unsigned char *id = wav + offset;
		unsigned long chunk_size = chasm_bridge_read_u32(wav + offset + 4);
		const unsigned char *body = wav + offset + 8;

		if (offset + 8 + chunk_size > size)
			break;
		if (!memcmp(id, "fmt ", 4))
		{
			format_chunk = body;
			format_size = chunk_size;
		}
		else if (!memcmp(id, "data", 4))
		{
			data_chunk = body;
			data_size = chunk_size;
		}
		offset += 8 + chunk_size + (chunk_size & 1);
	}

	if (!format_chunk || !data_chunk || format_size < 16 || !data_size)
		return;

	format_tag = chasm_bridge_read_u16(format_chunk);
	channels = chasm_bridge_read_u16(format_chunk + 2);
	sample_rate = chasm_bridge_read_u32(format_chunk + 4);
	byte_rate = chasm_bridge_read_u32(format_chunk + 8);
	block_align = chasm_bridge_read_u16(format_chunk + 12);
	bits = chasm_bridge_read_u16(format_chunk + 14);
	if (format_tag != WAVE_FORMAT_PCM || bits != 16 || !channels || !sample_rate || !byte_rate)
		return;

	sound = (LPDIRECTSOUND)dsound_get();
	if (!sound)
		return;

	csmemset(&format, 0, sizeof(format));
	format.wFormatTag = WAVE_FORMAT_PCM;
	format.nChannels = channels;
	format.nSamplesPerSec = sample_rate;
	format.nAvgBytesPerSec = byte_rate;
	format.nBlockAlign = block_align;
	format.wBitsPerSample = bits;

	csmemset(&desc, 0, sizeof(desc));
	desc.dwMaxAttachedPackets = 4;
	desc.lpwfxFormat = &format;
	desc.dwFlags = DSSTREAMCAPS_CTRL3D;
	desc.lpfnCallback = NULL;
	desc.lpvContext = NULL;

	stream = NULL;
	if (IDirectSound_CreateSoundStream(sound, &desc, &stream, NULL) < 0 || !stream)
		return;

	IDirectSoundStream_SetMode(stream, DS3DMODE_NORMAL, 0);
	if (head)
		IDirectSoundStream_SetPosition(stream, head->x, head->y, head->z, 0);
	IDirectSoundStream_SetMinDistance(stream, 2.0f, 0);
	IDirectSoundStream_SetMaxDistance(stream, 40.0f, 0);

	csmemset(&packet, 0, sizeof(packet));
	packet.pvBuffer = (void *)data_chunk;
	packet.dwMaxSize = data_size;
	if (IDirectSoundStream_Process(stream, &packet, NULL) < 0)
	{
		IDirectSoundStream_Release(stream);
		return;
	}

	chasm_bridge_stop_voice();
	chasm_bridge_voice_stream = stream;
	chasm_bridge_voice_unit = unit_index;
	duration = data_size * 1000ul / byte_rate;
	chasm_bridge_voice_end = system_milliseconds() + duration + 500;
	if (unit_index != NONE)
		game_sound_set_mouth_aperture(unit_index, 0.65f);

	return;
}

static void chasm_bridge_update_voice(
	void)
{
	struct unit_datum *unit;
	real_point3d head;
	unsigned long now;
	real aperture;

	if (!chasm_bridge_voice_stream)
		return;

	now = system_milliseconds();
	unit = (struct unit_datum *)object_try_and_get_and_verify_type(chasm_bridge_voice_unit, _object_mask_unit);
	if (!unit || TEST_FLAG(unit->object.damage_flags, _object_dead_bit) || now >= chasm_bridge_voice_end)
	{
		chasm_bridge_stop_voice();
		return;
	}

	unit_get_head_position(chasm_bridge_voice_unit, &head);
	IDirectSoundStream_SetPosition(chasm_bridge_voice_stream, head.x, head.y, head.z, 0);
	aperture = (now / 90) % 2 ? 0.75f : 0.35f;
	game_sound_set_mouth_aperture(chasm_bridge_voice_unit, aperture);

	return;
}

static void chasm_bridge_capture_frames(
	void)
{
	float frame[VOICE_FRAME_SAMPLES];

	while (chasm_bridge_recorded_samples + VOICE_FRAME_SAMPLES <= NUMBEROF(chasm_bridge_recorded) &&
		voice_audio_read_frame(frame))
	{
		memcpy(&chasm_bridge_recorded[chasm_bridge_recorded_samples], frame, sizeof(frame));
		chasm_bridge_recorded_samples += VOICE_FRAME_SAMPLES;
	}

	return;
}

static void chasm_bridge_start_recording(
	long player_unit)
{
	if (chasm_bridge_net_busy() || chasm_bridge_waiting)
		return;
	if (!chasm_bridge_find_target(player_unit))
	{
		chasm_bridge_print("No one to talk to.");
		return;
	}

	chasm_bridge_recording = TRUE;
	chasm_bridge_recorded_samples = 0;
	voice_audio_microphone(1);
	chasm_bridge_print("Listening...");

	return;
}

/* submit is FALSE when the turn must be thrown away (left the map, or busy) */
static void chasm_bridge_stop_recording(
	int submit)
{
	unsigned long samples;
	unsigned long data_size;
	unsigned long wav_size;
	unsigned char *wav;
	unsigned long index;

	if (!chasm_bridge_recording)
		return;
	chasm_bridge_recording = FALSE;
	voice_audio_microphone(0);

	samples = chasm_bridge_recorded_samples;
	chasm_bridge_recorded_samples = 0;
	if (!submit)
		return;
	if (samples < VOICE_FRAME_SAMPLES * 10)
	{
		chasm_bridge_print("Too short.");
		return;
	}

	data_size = samples * 2;
	wav_size = 44 + data_size;
	wav = (unsigned char *)malloc(wav_size);
	if (!wav)
	{
		chasm_bridge_print("Out of memory recording.");
		return;
	}

	memcpy(wav, "RIFF", 4);
	chasm_bridge_write_u32(wav + 4, (unsigned long)(wav_size - 8));
	memcpy(wav + 8, "WAVEfmt ", 8);
	chasm_bridge_write_u32(wav + 16, 16);
	chasm_bridge_write_u16(wav + 20, 1);
	chasm_bridge_write_u16(wav + 22, 1);
	chasm_bridge_write_u32(wav + 24, CHASM_BRIDGE_VOICE_RATE);
	chasm_bridge_write_u32(wav + 28, CHASM_BRIDGE_VOICE_RATE * 2);
	chasm_bridge_write_u16(wav + 32, 2);
	chasm_bridge_write_u16(wav + 34, 16);
	memcpy(wav + 36, "data", 4);
	chasm_bridge_write_u32(wav + 40, data_size);

	for (index = 0; index < samples; index++)
	{
		float sample = chasm_bridge_recorded[index];
		int value;

		if (sample > 1.0f)
			sample = 1.0f;
		if (sample < -1.0f)
			sample = -1.0f;
		value = (int)(sample * 32767.0f);
		wav[44 + index * 2] = (unsigned char)(value & 0xFF);
		wav[45 + index * 2] = (unsigned char)((value >> 8) & 0xFF);
	}

	chasm_bridge_waiting = TRUE;
	chasm_bridge_net_submit_voice(
		chasm_bridge_target_key,
		chasm_bridge_target_name,
		chasm_bridge_target_character,
		wav,
		wav_size,
		(double)chasm_bridge_target_distance);

	return;
}

static void chasm_bridge_handle_result(
	const struct chasm_bridge_result *result,
	long player_unit)
{
	char text[CHASM_BRIDGE_TEXT_SIZE + CHASM_BRIDGE_NAME_SIZE + 8];

	if (result->failed)
	{
		chasm_bridge_print(result->error);
		return;
	}

	if (result->transcript[0])
	{
		sprintf(text, "You: %s", result->transcript);
		chasm_bridge_print(text);
	}

	if (result->speaker[0])
		sprintf(text, "%s: %s", result->speaker, result->speech);
	else
		sprintf(text, "%s", result->speech);
	chasm_bridge_print(text);

	if (result->audio && result->audio_size && chasm_bridge_unit_index != NONE)
	{
		struct unit_datum *unit = (struct unit_datum *)object_try_and_get_and_verify_type(chasm_bridge_unit_index, _object_mask_unit);

		if (unit && !TEST_FLAG(unit->object.damage_flags, _object_dead_bit))
		{
			real_point3d head;

			unit_get_head_position(chasm_bridge_unit_index, &head);
			chasm_bridge_play_audio(result->audio, result->audio_size, chasm_bridge_unit_index, &head);
		}
	}

	chasm_bridge_run_actions(result, player_unit);

	return;
}

static void chasm_bridge_submit_line(
	const char *text)
{
	if (!text || !text[0])
		return;
	if (chasm_bridge_net_busy() || chasm_bridge_unit_index == NONE)
		return;

	chasm_bridge_waiting = TRUE;
	chasm_bridge_net_submit_turn(
		chasm_bridge_target_key,
		chasm_bridge_target_name,
		chasm_bridge_target_character,
		text,
		(double)chasm_bridge_target_distance);

	return;
}

/* network_chat.c's bridge line: Enter hands us the text */
static void chasm_bridge_text_submitted(
	const char *text)
{
	chasm_bridge_submit_line(text);

	return;
}

static void chasm_bridge_try_talk(
	long player_unit)
{
	if (chasm_bridge_net_busy())
		return;

	if (!chasm_bridge_find_target(player_unit))
	{
		chasm_bridge_print("No one to talk to.");
		return;
	}

	/* the real text input, drawn by the chat's own line */
	network_chat_bridge_begin();
	if (network_chat_bridge_active())
		return;

	/* no local player to type for: send the configured debug line */
	chasm_bridge_submit_line(chasm_bridge_line);

	return;
}

/* ---------- public code */

void chasm_bridge_update(
	void)
{
	struct chasm_bridge_result result;
	long player_unit;
	unsigned long held;

	if (!chasm_bridge_configured || config_changes() != chasm_bridge_changes)
		chasm_bridge_configure();
	if (!chasm_bridge_enabled)
		return;

	chasm_bridge_net_initialize();
	chasm_bridge_net_heartbeat();
	if (!chasm_bridge_handler_set)
	{
		network_chat_bridge_set_handler(chasm_bridge_text_submitted);
		chasm_bridge_handler_set = TRUE;
	}

	if (chasm_bridge_net_take_result(&result))
	{
		chasm_bridge_handle_result(&result, chasm_bridge_player_unit());
		if (result.audio)
			free(result.audio);
		result.audio = NULL;
		chasm_bridge_waiting = FALSE;
	}

	chasm_bridge_update_voice();

	if (!game_in_progress())
	{
		chasm_bridge_stop_recording(FALSE);
		return;
	}

	held = halo_keyboard_actions(0);

	if (chasm_bridge_recording)
	{
		if (!(held & FLAG(HALO_KEYBOARD_PUSH_TO_TALK)) || network_chat_typing() ||
			chasm_bridge_net_busy() || chasm_bridge_waiting)
		{
			chasm_bridge_stop_recording(!(chasm_bridge_net_busy() || chasm_bridge_waiting));
			chasm_bridge_key_held = held;
			return;
		}
		chasm_bridge_capture_frames();
	}

	if (network_chat_typing())
		return;
	if (chasm_bridge_net_busy() || chasm_bridge_waiting)
		return;
	if (console_is_active())
		return;

	if (!chasm_bridge_recording &&
		(held & FLAG(HALO_KEYBOARD_PUSH_TO_TALK)) &&
		!(chasm_bridge_key_held & FLAG(HALO_KEYBOARD_PUSH_TO_TALK)))
	{
		player_unit = chasm_bridge_player_unit();
		if (player_unit != NONE)
			chasm_bridge_start_recording(player_unit);
	}

	if (!chasm_bridge_recording &&
		(held & FLAG(HALO_KEYBOARD_CHAT)) && !(chasm_bridge_key_held & FLAG(HALO_KEYBOARD_CHAT)))
	{
		player_unit = chasm_bridge_player_unit();
		if (player_unit != NONE)
			chasm_bridge_try_talk(player_unit);
	}
	chasm_bridge_key_held = held;

	return;
}

void chasm_bridge_initialize_for_new_map(
	void)
{
	chasm_bridge_encounter_index = NONE;
	chasm_bridge_unit_index = NONE;
	chasm_bridge_target_key[0] = 0;
	chasm_bridge_target_name[0] = 0;
	chasm_bridge_target_character[0] = 0;
	chasm_bridge_target_distance = 0.0f;
	chasm_bridge_key_held = 0;
	chasm_bridge_waiting = FALSE;
	chasm_bridge_recording = FALSE;
	chasm_bridge_recorded_samples = 0;

	chasm_bridge_configure();

	return;
}

void chasm_bridge_dispose_from_old_map(
	void)
{
	chasm_bridge_stop_recording(FALSE);
	chasm_bridge_stop_voice();
	chasm_bridge_encounter_index = NONE;
	chasm_bridge_unit_index = NONE;
	chasm_bridge_target_key[0] = 0;
	chasm_bridge_target_name[0] = 0;
	chasm_bridge_target_character[0] = 0;
	chasm_bridge_key_held = 0;
	chasm_bridge_waiting = FALSE;

	return;
}
