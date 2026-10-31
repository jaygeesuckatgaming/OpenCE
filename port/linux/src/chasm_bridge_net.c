/*
CHASM_BRIDGE_NET.C

The OpenCE chasm bridge's platform half. See chasm_bridge_net.h.

One worker thread does chasm's granular HTTP turn:
live-chats (once) -> presence -> generate. The game thread never blocks on
the network; it submits a turn and polls for the answer. The heartbeat file
in %LOCALAPPDATA%\chasm\bridge tells chasm the game is alive.

Everything here is the shared platform layer, so the Windows build gets it
from port/linux/src too (port/windows/port.json replaces only posix_update.c,
whose update_https_request this uses).
*/

#include "chasm_bridge_net.h"

#include "port_config.h"
#include "server_json.h"
#include "update.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the platform layer's (network_chat.c declares these too) */
void platform_log(const char *format, ...);
unsigned long system_milliseconds(void);

/* ---------- constants */

#define CHASM_BRIDGE_LIVE_CHAT "opence"
#define CHASM_BRIDGE_LOCATION "Installation 04"
#define CHASM_BRIDGE_DEFAULT_URL "http://127.0.0.1:7341"
#define CHASM_BRIDGE_DEFAULT_TTS_URL "http://127.0.0.1:13300/tts"
#define CHASM_BRIDGE_DEFAULT_STT_URL "http://127.0.0.1:13310/transcribe"
#define CHASM_BRIDGE_HTTP_MAXIMUM (8ul * 1024ul * 1024ul)
#define CHASM_BRIDGE_HEARTBEAT_MILLISECONDS 250ul

/* the action vocabulary the model needs to emit usable steps (regular NPC turns
do not inject the action book's list). Kept in step with the OpenCE action book. */
#define CHASM_BRIDGE_ACTION_CONTEXT \
	"You can take these in-world actions: follow (follow the player), stop_follow " \
	"(stop following/hold position), look_at_player, stop_looking, attack (open fire " \
	"on a target), stop_combat (cease fire/stand down). When the player asks you to " \
	"do one, emit exactly ONE action in the actions array as " \
	"{\"action\":\"<alias>\",\"target\":\"<who>\",\"items\":\"\"}. Otherwise actions is []."

/* ---------- types */

struct chasm_bridge_request
{
	int used;
	char npc_key[CHASM_BRIDGE_ID_SIZE];
	char npc_name[CHASM_BRIDGE_NAME_SIZE];
	char character_id[CHASM_BRIDGE_ID_SIZE];
	char player_text[CHASM_BRIDGE_TEXT_SIZE];
	/* a push-to-talk WAV: the worker transcribes it before generating */
	unsigned char *audio;
	unsigned long audio_size;
	double distance_meters;
};

/* ---------- globals */

static pthread_mutex_t bridge_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t bridge_condition = PTHREAD_COND_INITIALIZER;
static int bridge_started;
static int bridge_stop;
static int bridge_busy;
static struct chasm_bridge_request bridge_pending;
static struct chasm_bridge_result bridge_completed;
static int bridge_have_completed;
static unsigned long bridge_last_heartbeat;
static int bridge_heartbeat_failed;

static char bridge_api_url[256];
static char bridge_create_url[384];
static char bridge_generate_url[512];
static char bridge_tts_url[384];
static char bridge_stt_url[384];
static int bridge_tts_reported;

/* ---------- helpers */

static void copy_text(
	char *destination,
	int destination_size,
	const char *source)
{
	if (!destination || destination_size <= 0)
		return;

	if (!source)
		source = "";
	strncpy(destination, source, (size_t)(destination_size - 1));
	destination[destination_size - 1] = 0;

	return;
}

static int http_post_json(
	const char *url,
	const char *body,
	struct update_response *response,
	char *error,
	int error_size)
{
	response->status = 0;
	response->headers[0] = 0;
	response->body = NULL;
	response->body_size = 0;

	return update_https_request(
		"POST",
		url,
		"Content-Type: application/json\r\n",
		body,
		(unsigned long)strlen(body),
		CHASM_BRIDGE_HTTP_MAXIMUM,
		response,
		error,
		error_size);
}

static void heartbeat(void)
{
	const char *local_app_data;
	char path[512];
	FILE *file;
	unsigned long now = system_milliseconds();

	if (now - bridge_last_heartbeat < CHASM_BRIDGE_HEARTBEAT_MILLISECONDS)
		return;
	bridge_last_heartbeat = now;

	local_app_data = getenv("LOCALAPPDATA");
	if (!local_app_data || !local_app_data[0])
		local_app_data = getenv("HOME");
	if (!local_app_data || !local_app_data[0])
		return;

	snprintf(path, sizeof(path), "%s/chasm/bridge/runtime_heartbeat.json", local_app_data);
	file = fopen(path, "wb");
	if (!file)
	{
		if (!bridge_heartbeat_failed)
		{
			platform_log("chasm bridge: cannot write heartbeat %s", path);
			bridge_heartbeat_failed = 1;
		}
		return;
	}

	fprintf(file, "{\"bridge\":\"opence\",\"updated_ms\":%lu}", now);
	fclose(file);
	bridge_heartbeat_failed = 0;

	return;
}

/* ---------- the turn ---------- */

static void result_fail(
	struct chasm_bridge_result *result,
	const char *message)
{
	result->valid = 1;
	result->failed = 1;
	copy_text(result->error, sizeof(result->error), message);

	return;
}

static int ensure_live_chat(
	struct chasm_bridge_result *result)
{
	struct server_json json;
	struct update_response response;
	char error[CHASM_BRIDGE_ERROR_SIZE];
	int ok;

	server_json_begin(&json);
	server_json_object_begin(&json, NULL);
	server_json_string(&json, "id", CHASM_BRIDGE_LIVE_CHAT);
	server_json_string(&json, "title", "OpenCE");
	server_json_string(&json, "location", CHASM_BRIDGE_LOCATION);
	server_json_object_end(&json);

	ok = http_post_json(bridge_create_url, server_json_text(&json), &response, error, sizeof(error));
	if (response.body)
		free(response.body);
	server_json_free(&json);

	if (!ok)
	{
		result_fail(result, error);
		return 0;
	}

	/* 2xx: created (or already there). A 400/409 also means it is there. */
	if (response.status >= 400 && response.status != 409)
	{
		char message[CHASM_BRIDGE_ERROR_SIZE];

		snprintf(message, sizeof(message), "live-chat create failed (HTTP %d)", response.status);
		result_fail(result, message);
		return 0;
	}

	return 1;
}

static int update_presence(
	const struct chasm_bridge_request *request,
	struct chasm_bridge_result *result)
{
	struct server_json json;
	struct update_response response;
	char error[CHASM_BRIDGE_ERROR_SIZE];
	char url[512];
	char participant[CHASM_BRIDGE_ID_SIZE * 2];
	int ok;

	snprintf(participant, sizeof(participant), "npc:%s", request->npc_key);
	snprintf(url, sizeof(url), "%s/api/headless/v1/live-chats/%s/presence", bridge_api_url, CHASM_BRIDGE_LIVE_CHAT);

	server_json_begin(&json);
	server_json_object_begin(&json, NULL);
	server_json_boolean(&json, "replace", 1);
	server_json_array_begin(&json, "participants");

	server_json_object_begin(&json, NULL);
	server_json_string(&json, "participantId", participant);
	server_json_string(&json, "type", "npc");
	server_json_string(&json, "characterId", request->character_id);
	server_json_string(&json, "name", request->npc_name);
	server_json_boolean(&json, "present", 1);
	server_json_boolean(&json, "audible", 1);
	server_json_real(&json, "distance", request->distance_meters);
	server_json_object_end(&json);

	server_json_object_begin(&json, NULL);
	server_json_string(&json, "participantId", "player");
	server_json_string(&json, "type", "player");
	server_json_string(&json, "name", "Player");
	server_json_boolean(&json, "present", 1);
	server_json_object_end(&json);

	server_json_array_end(&json);
	server_json_object_end(&json);

	ok = http_post_json(url, server_json_text(&json), &response, error, sizeof(error));
	if (response.body)
		free(response.body);
	server_json_free(&json);

	if (!ok)
	{
		result_fail(result, error);
		return 0;
	}
	if (response.status < 200 || response.status >= 300)
	{
		char message[CHASM_BRIDGE_ERROR_SIZE];

		snprintf(message, sizeof(message), "presence failed (HTTP %d)", response.status);
		result_fail(result, message);
		return 0;
	}

	return 1;
}

static void parse_actions(
	struct server_json_document *document,
	int structured_token,
	struct chasm_bridge_result *result)
{
	int actions_token = server_json_member(document, structured_token, "actions");
	int index;

	if (actions_token < 0)
		return;

	for (index = 0; index < CHASM_BRIDGE_MAXIMUM_ACTIONS; index++)
	{
		int action_token = server_json_element(document, actions_token, index);
		int id_token;
		int target_token;
		struct chasm_bridge_action *action;

		if (action_token < 0)
			break;

		action = &result->actions[result->action_count];
		action->id[0] = 0;
		action->target[0] = 0;

		id_token = server_json_member(document, action_token, "id");
		if (id_token >= 0)
			server_json_get_string(document, id_token, action->id, sizeof(action->id));
		if (!action->id[0])
			continue;

		target_token = server_json_member(document, action_token, "target");
		if (target_token >= 0)
			server_json_get_string(document, target_token, action->target, sizeof(action->target));
		if (!action->target[0])
			copy_text(action->target, sizeof(action->target), "player");

		result->action_count++;
	}

	return;
}

static void parse_turn(
	const char *body,
	unsigned long body_size,
	struct chasm_bridge_result *result)
{
	struct server_json_document document;
	int root = 0;
	int speaker_token;
	int structured_token;

	if (!body || !body_size || !server_json_parse(&document, body, (int)body_size, 16))
	{
		result_fail(result, "the turn's JSON could not be read");
		return;
	}

	speaker_token = server_json_member(&document, root, "speaker");
	if (speaker_token >= 0)
	{
		int name_token = server_json_member(&document, speaker_token, "name");

		if (name_token >= 0)
			server_json_get_string(&document, name_token, result->speaker, sizeof(result->speaker));
	}

	structured_token = server_json_member(&document, root, "structured");
	if (structured_token >= 0)
	{
		int speech_token = server_json_member(&document, structured_token, "speech");

		if (speech_token >= 0)
			server_json_get_string(&document, speech_token, result->speech, sizeof(result->speech));
		parse_actions(&document, structured_token, result);
	}

	/* plain-text turns carry message.content instead */
	if (!result->speech[0])
	{
		int message_token = server_json_member(&document, root, "message");

		if (message_token >= 0)
		{
			int content_token = server_json_member(&document, message_token, "content");

			if (content_token >= 0)
				server_json_get_string(&document, content_token, result->speech, sizeof(result->speech));
		}
	}

	server_json_document_free(&document);
	result->valid = 1;

	return;
}

/* StyleTTS2 chokes on digits and markdown; speak digits as words and drop the
marks the model sometimes leaves in a line. */
static void tts_sanitize(
	const char *text,
	char *out,
	int out_size)
{
	static const char *digits[10] = {
		"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine"
	};
	int o = 0;

	if (!out || out_size <= 0)
		return;
	if (!text)
		text = "";

	while (*text && o < out_size - 12)
	{
		unsigned char c = (unsigned char)*text++;

		if (c >= '0' && c <= '9')
		{
			const char *word = digits[c - '0'];

			if (o && out[o - 1] != ' ')
				out[o++] = ' ';
			while (*word && o < out_size - 1)
				out[o++] = *word++;
			out[o++] = ' ';
		}
		else if (c == '*' || c == '<' || c == '>' || c == '[' || c == ']' ||
			c == '{' || c == '}' || c == '#' || c == '_' || c == '|')
		{
			if (o && out[o - 1] != ' ')
				out[o++] = ' ';
		}
		else
		{
			out[o++] = (char)c;
		}
	}
	out[o] = 0;

	return;
}

static void synthesize_speech(
	const char *text,
	struct chasm_bridge_result *result)
{
	char cleaned[CHASM_BRIDGE_TEXT_SIZE];
	struct server_json json;
	struct update_response response;
	char error[CHASM_BRIDGE_ERROR_SIZE];
	int ok;

	if (!bridge_tts_url[0] || !text || !text[0])
		return;
	tts_sanitize(text, cleaned, sizeof(cleaned));
	if (!cleaned[0])
		return;

	server_json_begin(&json);
	server_json_object_begin(&json, NULL);
	server_json_string(&json, "chatmessage", cleaned);
	server_json_string(&json, "text", cleaned);
	server_json_object_end(&json);

	ok = http_post_json(bridge_tts_url, server_json_text(&json), &response, error, sizeof(error));
	server_json_free(&json);
	if (!ok)
	{
		if (!bridge_tts_reported)
		{
			platform_log("chasm bridge: TTS unavailable: %s", error);
			bridge_tts_reported = 1;
		}
		return;
	}
	if (response.status != 200 || !response.body || response.body_size < 12 ||
		memcmp(response.body, "RIFF", 4) || memcmp(response.body + 8, "WAVE", 4))
	{
		if (!bridge_tts_reported)
		{
			platform_log("chasm bridge: TTS returned no WAV (HTTP %d)", response.status);
			bridge_tts_reported = 1;
		}
		if (response.body)
			free(response.body);
		return;
	}

	/* ownership moves to the game side (chasm_bridge_net_take_result) */
	result->audio = (unsigned char *)response.body;
	result->audio_size = response.body_size;
	bridge_tts_reported = 0;

	return;
}

static int transcribe_audio(
	const unsigned char *wav,
	unsigned long wav_size,
	char *text,
	int text_size,
	struct chasm_bridge_result *result)
{
	struct update_response response;
	struct server_json_document document;
	char error[CHASM_BRIDGE_ERROR_SIZE];
	int ok;
	int token;

	if (!bridge_stt_url[0])
	{
		result_fail(result, "Speech recognition is not configured.");
		return 0;
	}
	if (!wav || !wav_size)
	{
		result_fail(result, "No speech was recorded.");
		return 0;
	}

	ok = update_https_request("POST", bridge_stt_url, "Content-Type: audio/wav\r\n",
		(const char *)wav, wav_size, CHASM_BRIDGE_HTTP_MAXIMUM, &response, error, sizeof(error));
	if (!ok)
	{
		result_fail(result, error);
		return 0;
	}
	if (response.status < 200 || response.status >= 300 || !response.body || !response.body_size)
	{
		char message[CHASM_BRIDGE_ERROR_SIZE];

		snprintf(message, sizeof(message), "speech recognition failed (HTTP %d)", response.status);
		result_fail(result, message);
		if (response.body)
			free(response.body);
		return 0;
	}

	if (!server_json_parse(&document, response.body, (int)response.body_size, 8))
	{
		free(response.body);
		result_fail(result, "speech recognition returned no JSON");
		return 0;
	}

	token = server_json_member(&document, 0, "text");
	if (token < 0 || !server_json_get_string(&document, token, text, text_size) || !text[0])
	{
		server_json_document_free(&document);
		free(response.body);
		result_fail(result, "I didn't catch that.");
		return 0;
	}

	server_json_document_free(&document);
	free(response.body);
	return 1;
}

static void run_turn(
	const struct chasm_bridge_request *request,
	struct chasm_bridge_result *result)
{
	struct server_json json;
	struct update_response response;
	char error[CHASM_BRIDGE_ERROR_SIZE];
	char participant[CHASM_BRIDGE_ID_SIZE * 2];
	const char *player_text = request->player_text;
	int ok;

	memset(result, 0, sizeof(*result));
	copy_text(result->speaker, sizeof(result->speaker), request->npc_name);

	if (request->audio)
	{
		char transcript[CHASM_BRIDGE_TEXT_SIZE];

		if (!transcribe_audio(request->audio, request->audio_size, transcript, sizeof(transcript), result))
			return;
		player_text = transcript;
		copy_text(result->transcript, sizeof(result->transcript), transcript);
	}

	if (!ensure_live_chat(result))
		return;
	if (!update_presence(request, result))
		return;

	snprintf(participant, sizeof(participant), "npc:%s", request->npc_key);

	server_json_begin(&json);
	server_json_object_begin(&json, NULL);
	server_json_string(&json, "message", player_text);
	server_json_string(&json, "participantId", "player");
	server_json_string(&json, "responseFormat", "structured");
	server_json_string(&json, "extraContext", CHASM_BRIDGE_ACTION_CONTEXT);
	server_json_array_begin(&json, "actionBookScopes");
	server_json_string(&json, NULL, "global");
	server_json_string(&json, NULL, "game:opence");
	server_json_array_end(&json);
	server_json_string(&json, "forceParticipantId", participant);
	server_json_object_end(&json);

	ok = http_post_json(bridge_generate_url, server_json_text(&json), &response, error, sizeof(error));
	server_json_free(&json);

	if (!ok)
	{
		result_fail(result, error);
		return;
	}
	if (response.status < 200 || response.status >= 300)
	{
		char message[CHASM_BRIDGE_ERROR_SIZE];

		snprintf(message, sizeof(message), "generate failed (HTTP %d)", response.status);
		result_fail(result, message);
		if (response.body)
			free(response.body);
		return;
	}

	parse_turn(response.body, response.body_size, result);
	if (response.body)
		free(response.body);
	if (result->failed)
		return;
	if (result->speaker[0] && !strcmp(result->speaker, request->npc_key))
		copy_text(result->speaker, sizeof(result->speaker), request->npc_name);

	synthesize_speech(result->speech, result);

	return;
}

/* ---------- the worker ---------- */

static void *bridge_worker(
	void *context)
{
	(void)context;

	for (;;)
	{
		struct chasm_bridge_request request;
		struct chasm_bridge_result result;

		pthread_mutex_lock(&bridge_mutex);
		while (!bridge_pending.used && !bridge_stop)
			pthread_cond_wait(&bridge_condition, &bridge_mutex);
		if (bridge_stop)
		{
			pthread_mutex_unlock(&bridge_mutex);
			break;
		}
		request = bridge_pending;
		bridge_pending.used = 0;
		bridge_busy = 1;
		pthread_mutex_unlock(&bridge_mutex);

		run_turn(&request, &result);
		if (request.audio)
			free(request.audio);

		pthread_mutex_lock(&bridge_mutex);
		/* an answer the game never took: its WAV is ours to free */
		if (bridge_have_completed && bridge_completed.audio)
			free(bridge_completed.audio);
		bridge_completed = result;
		bridge_have_completed = 1;
		bridge_busy = 0;
		pthread_mutex_unlock(&bridge_mutex);

		if (result.failed)
			platform_log("chasm bridge: %s", result.error);
	}

	return NULL;
}

/* ---------- public code ---------- */

void chasm_bridge_net_initialize(
	void)
{
	pthread_attr_t attributes;
	pthread_t thread;
	const char *configured;

	if (bridge_started)
		return;

	configured = config_string("bridge.api_url");
	if (!configured || !configured[0])
		configured = CHASM_BRIDGE_DEFAULT_URL;
	copy_text(bridge_api_url, sizeof(bridge_api_url), configured);

	configured = config_string("bridge.tts_url");
	if (!configured || !configured[0])
		configured = CHASM_BRIDGE_DEFAULT_TTS_URL;
	copy_text(bridge_tts_url, sizeof(bridge_tts_url), configured);

	configured = config_string("bridge.stt_url");
	if (!configured || !configured[0])
		configured = CHASM_BRIDGE_DEFAULT_STT_URL;
	copy_text(bridge_stt_url, sizeof(bridge_stt_url), configured);

	snprintf(bridge_create_url, sizeof(bridge_create_url), "%s/api/headless/v1/live-chats", bridge_api_url);
	snprintf(bridge_generate_url, sizeof(bridge_generate_url), "%s/api/headless/v1/live-chats/%s/generate",
		bridge_api_url, CHASM_BRIDGE_LIVE_CHAT);

	pthread_mutex_init(&bridge_mutex, NULL);
	pthread_cond_init(&bridge_condition, NULL);
	pthread_attr_init(&attributes);
	pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&thread, &attributes, bridge_worker, NULL) == 0)
	{
		bridge_started = 1;
		platform_log("chasm bridge: worker started, API %s", bridge_api_url);
	}
	else
	{
		platform_log("chasm bridge: could not start the worker");
	}
	pthread_attr_destroy(&attributes);

	return;
}

void chasm_bridge_net_shutdown(
	void)
{
	pthread_mutex_lock(&bridge_mutex);
	bridge_stop = 1;
	pthread_cond_signal(&bridge_condition);
	pthread_mutex_unlock(&bridge_mutex);

	return;
}

int chasm_bridge_net_started(
	void)
{
	return bridge_started;
}

int chasm_bridge_net_busy(
	void)
{
	int busy;

	pthread_mutex_lock(&bridge_mutex);
	busy = bridge_busy || bridge_pending.used;
	pthread_mutex_unlock(&bridge_mutex);

	return busy;
}

void chasm_bridge_net_submit_turn(
	const char *npc_key,
	const char *npc_name,
	const char *character_id,
	const char *player_text,
	double distance_meters)
{
	if (!bridge_started)
		return;

	pthread_mutex_lock(&bridge_mutex);
	if (bridge_busy || bridge_pending.used)
	{
		pthread_mutex_unlock(&bridge_mutex);
		return;
	}

	memset(&bridge_pending, 0, sizeof(bridge_pending));
	bridge_pending.used = 1;
	copy_text(bridge_pending.npc_key, sizeof(bridge_pending.npc_key), npc_key);
	copy_text(bridge_pending.npc_name, sizeof(bridge_pending.npc_name), npc_name);
	copy_text(bridge_pending.character_id, sizeof(bridge_pending.character_id), character_id);
	copy_text(bridge_pending.player_text, sizeof(bridge_pending.player_text), player_text);
	bridge_pending.distance_meters = distance_meters;

	pthread_cond_signal(&bridge_condition);
	pthread_mutex_unlock(&bridge_mutex);

	return;
}

void chasm_bridge_net_submit_voice(
	const char *npc_key,
	const char *npc_name,
	const char *character_id,
	unsigned char *wav,
	unsigned long wav_size,
	double distance_meters)
{
	if (!bridge_started)
	{
		free(wav);
		return;
	}

	pthread_mutex_lock(&bridge_mutex);
	if (bridge_busy || bridge_pending.used)
	{
		pthread_mutex_unlock(&bridge_mutex);
		free(wav);
		return;
	}

	memset(&bridge_pending, 0, sizeof(bridge_pending));
	bridge_pending.used = 1;
	copy_text(bridge_pending.npc_key, sizeof(bridge_pending.npc_key), npc_key);
	copy_text(bridge_pending.npc_name, sizeof(bridge_pending.npc_name), npc_name);
	copy_text(bridge_pending.character_id, sizeof(bridge_pending.character_id), character_id);
	bridge_pending.audio = wav;
	bridge_pending.audio_size = wav_size;
	bridge_pending.distance_meters = distance_meters;

	pthread_cond_signal(&bridge_condition);
	pthread_mutex_unlock(&bridge_mutex);

	return;
}

int chasm_bridge_net_take_result(
	struct chasm_bridge_result *result)
{
	int taken = 0;

	pthread_mutex_lock(&bridge_mutex);
	if (bridge_have_completed)
	{
		*result = bridge_completed;
		bridge_have_completed = 0;
		taken = 1;
	}
	pthread_mutex_unlock(&bridge_mutex);

	return taken;
}

void chasm_bridge_net_heartbeat(
	void)
{
	heartbeat();

	return;
}
