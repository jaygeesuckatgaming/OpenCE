/*
CHASM_BRIDGE_NET.H

The OpenCE chasm bridge's platform half (chasm_bridge_net.c): a worker thread
that turns one NPC turn into the chasm backend's HTTP calls, and the queue the
game side uses to hand it a turn and take the answer back.

Nothing here touches the game: the game side (port/linux/game/chasm_bridge.c)
snapshots the NPC and the player's line, calls chasm_bridge_net_submit_turn,
and polls chasm_bridge_net_take_result.

The contract is chasm's granular headless API (docs/building-a-bridge-mod.md):
  POST /api/headless/v1/live-chats
  POST /api/headless/v1/live-chats/:id/presence
  POST /api/headless/v1/live-chats/:id/generate
*/

#ifndef __CHASM_BRIDGE_NET_H
#define __CHASM_BRIDGE_NET_H
#pragma once

enum
{
	CHASM_BRIDGE_TEXT_SIZE = 2048,
	CHASM_BRIDGE_NAME_SIZE = 128,
	CHASM_BRIDGE_ID_SIZE = 128,
	CHASM_BRIDGE_ERROR_SIZE = 256,
	CHASM_BRIDGE_MAXIMUM_ACTIONS = 8
};

struct chasm_bridge_action
{
	char id[CHASM_BRIDGE_ID_SIZE];
	char target[CHASM_BRIDGE_NAME_SIZE];
};

/* one turn's answer, as the game side reads it */
struct chasm_bridge_result
{
	int valid;
	int failed;
	char error[CHASM_BRIDGE_ERROR_SIZE];
	char speaker[CHASM_BRIDGE_NAME_SIZE];
	char speech[CHASM_BRIDGE_TEXT_SIZE];
	/* what the player said, when the turn came in as a voice clip */
	char transcript[CHASM_BRIDGE_TEXT_SIZE];
	int action_count;
	struct chasm_bridge_action actions[CHASM_BRIDGE_MAXIMUM_ACTIONS];
	/* the turn's synthesized WAV (StyleTTS2); the game side owns it after
	chasm_bridge_net_take_result and frees it once playback has copied it */
	unsigned char *audio;
	unsigned long audio_size;
};

/* starts the worker thread (once); reads bridge.api_url from port_config.c */
void chasm_bridge_net_initialize(void);
/* asks the worker to stop; the game need not call it (the process ends it) */
void chasm_bridge_net_shutdown(void);

/* whether the worker is alive */
int chasm_bridge_net_started(void);

/* a turn is queued or running: the game side should not submit another */
int chasm_bridge_net_busy(void);

/* hands the worker one turn: the player's text, the NPC's stable key, display
name and character card id, and how far away the NPC is (metres) */
void chasm_bridge_net_submit_turn(
	const char *npc_key,
	const char *npc_name,
	const char *character_id,
	const char *player_text,
	double distance_meters);

/* same, but the player's words are a WAV the worker transcribes first. The
worker takes ownership of wav (and frees it if a turn is already in flight). */
void chasm_bridge_net_submit_voice(
	const char *npc_key,
	const char *npc_name,
	const char *character_id,
	unsigned char *wav,
	unsigned long wav_size,
	double distance_meters);

/* takes the completed turn, if one is waiting: 1 if result was filled */
int chasm_bridge_net_take_result(
	struct chasm_bridge_result *result);

/* rewrites the rendezvous heartbeat at most every 250 ms (main loop) */
void chasm_bridge_net_heartbeat(void);

#endif
