/*
AI_PLAYER.C

The Gem AI co-op player's body input: follow the other co-op player (the
host), face where it is, and walk toward it when it is more than a couple of
world units away. It writes the same snapshot the Gem brain reads later.

This is phase 1 of the bot: follow only. Combat, cover, vehicles and pickups
come next, built on the companion's body logic in companion.c.
*/

#include "cseries.h"
#include "game/game.h"
#include "game/players.h"
#include "objects/objects.h"
#include "memory/data.h"
#include "math/real_math.h"
#include "game/game_engine.h"
#include "ai_player.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

/* port_config.c's */
int config_boolean(const char *name);
const char *config_string(const char *name);
/* xbox_kernel.c's */
void platform_log(const char *format, ...);
/* platform.c's */
unsigned long system_milliseconds(void);
/* players.c's */
long find_best_starting_location_index(long player_index);
/* player_control.c's */
struct player_control *player_control_get(short local_player_index);

/* how near (world units) it stops walking; about 3 metres a unit */
#define AI_PLAYER_FOLLOW_DISTANCE 2.0f
/* how fast it may turn toward the teammate, radians a frame */
#define AI_PLAYER_TURN_RATE 0.12f
#define AI_PLAYER_PITCH_RATE 0.08f
/* how often the state file is written, milliseconds */
#define AI_PLAYER_STATE_INTERVAL 100ul

/* the other player (the host) on a network co-op client; remote players have
   no local player index */
static long ai_player_teammate(
	long local_player_index,
	long *unit_index)
{
	struct data_iterator iterator;
	struct player_datum *player;

	*unit_index = NONE;
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		if (iterator.datum_index == local_player_index)
			continue;
		if (player->unit_index == NONE)
			continue;
		if (player->local_player_index != NONE)
			continue;

		*unit_index = player->unit_index;
		return iterator.datum_index;
	}

	return NONE;
}

/* fopen, but a path outside the Xbox tree (an absolute host path the config
   names) goes through platform_translate_path's file:// escape: the port's
   fopen wrapper otherwise re-roots every path under the data root */
static FILE *ai_player_open(const char *path, const char *mode)
{
	char literal[1100];
	int absolute =
		path[0] == '/' || path[0] == '\\' ||
		(((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
			path[1] == ':');

	if (absolute)
	{
		snprintf(literal, sizeof(literal), "file://%s", path);
		return fopen(literal, mode);
	}
	return fopen(path, mode);
}

static void ai_player_write_state(
	real_point3d const *player_position,
	real_point3d const *teammate_position,
	real distance)
{
	static unsigned long last_write;
	static int logged_follow;
	static int logged_open_failure;
	unsigned long now = system_milliseconds();
	const char *path;
	FILE *file;

	if (now - last_write < AI_PLAYER_STATE_INTERVAL)
		return;
	last_write = now;

	path = config_string("ai_player.state_path");
	if (!path || !path[0])
		path = "ai_state.json";

	file = ai_player_open(path, "wb");
	if (!file)
	{
		if (!logged_open_failure)
		{
			platform_log("ai_player: fopen failed (%d) for state %s", errno, path);
			logged_open_failure = 1;
		}
		return;
	}

	fprintf(file,
		"{\"version\":1,\"time_ms\":%lu,"
		"\"player\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},"
		"\"teammate\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},"
		"\"distance\":%.3f}\n",
		now,
		player_position->x, player_position->y, player_position->z,
		teammate_position->x, teammate_position->y, teammate_position->z,
		distance);

	fclose(file);

	if (!logged_follow)
	{
		platform_log("ai_player: following, distance %.3f", distance);
		logged_follow = 1;
	}

	return;
}

/* writes why the bot has no input, so the state file is never simply absent */
static void ai_player_write_reason(
	const char *reason)
{
	static char last_reason[32];
	static int logged_open_failure;
	static unsigned long last_write;
	unsigned long now = system_milliseconds();
	const char *path = config_string("ai_player.state_path");
	FILE *file;

	if (strcmp(last_reason, reason) == 0 && now - last_write < 250)
		return;
	last_write = now;

	if (strcmp(last_reason, reason) != 0)
	{
		platform_log("ai_player: %s", reason);
		strncpy(last_reason, reason, sizeof(last_reason) - 1);
		last_reason[sizeof(last_reason) - 1] = '\0';
	}

	if (!path || !path[0])
		path = "ai_state.json";
	file = ai_player_open(path, "wb");
	if (!file)
	{
		if (!logged_open_failure)
		{
			platform_log("ai_player: fopen failed (%d) for state %s", errno, path);
			logged_open_failure = 1;
		}
		return;
	}

	fprintf(file, "{\"version\":1,\"time_ms\":%lu,\"reason\":\"%s\"}\n",
		system_milliseconds(), reason);
	fclose(file);

	return;
}

/* why a client's player has no unit yet: the spawn gate's own state, so a
   player stuck waiting ("space to clear") names the cause instead of nothing */
static void ai_player_write_spawn_state(
	long player_index,
	struct player_datum *player)
{
	static unsigned long last_write;
	unsigned long now = system_milliseconds();
	const char *path = config_string("ai_player.state_path");
	FILE *file;

	if (now - last_write < 250)
		return;
	last_write = now;

	if (!path || !path[0])
		path = "ai_state.json";
	file = ai_player_open(path, "wb");
	if (!file)
		return;

	fprintf(file,
		"{\"version\":1,\"time_ms\":%lu,\"reason\":\"local_player_no_unit\","
		"\"player_index\":%ld,\"deaths\":%d,\"respawn_timer\":%ld,"
		"\"quit\":%d,\"game_time\":%ld,\"best_location\":%ld}\n",
		system_milliseconds(), player_index, (int)player->statistics.deaths,
		player->respawn_timer, player->quit_out_of_game ? 1 : 0,
		(long)game_time_get(), find_best_starting_location_index(player_index));
	fclose(file);

	return;
}

int ai_player_get_input(
	short local_player_index,
	struct ai_player_input *input)
{
	long player_index;
	long local_unit_index;
	long teammate_unit_index;
	struct player_datum *player;
	struct player_control *control;
	real_point3d player_position;
	real_point3d teammate_position;
	real_vector3d toward_teammate;
	real distance;
	real_euler_angles2d desired_angles;
	real delta_yaw;
	real delta_pitch;

	if (!input)
		return 0;
	if (!config_boolean("ai_player.enabled"))
	{
		ai_player_write_reason("disabled");
		return 0;
	}
	if (!game_in_progress())
	{
		ai_player_write_reason("not_in_game");
		return 0;
	}

	player_index = local_player_get_player_index(local_player_index);
	if (player_index == NONE)
	{
		/* the input loop visits every local slot, valid or not: only the
		   first slot reports, or an empty slot would overwrite the state */
		if (local_player_index == 0)
			ai_player_write_reason("no_local_player");
		return 0;
	}

	player = player_get(player_index);
	local_unit_index = player->unit_index;
	if (local_unit_index == NONE)
	{
		ai_player_write_spawn_state(player_index, player);
		return 0;
	}

	if (ai_player_teammate(player_index, &teammate_unit_index) == NONE)
	{
		ai_player_write_reason("no_teammate");
		return 0;
	}
	if (teammate_unit_index == NONE)
	{
		ai_player_write_reason("teammate_no_unit");
		return 0;
	}

	object_get_origin(local_unit_index, &player_position);
	object_get_origin(teammate_unit_index, &teammate_position);
	distance = distance3d(&player_position, &teammate_position);

	input->throttle_i = distance > AI_PLAYER_FOLLOW_DISTANCE ? 1.0f : 0.0f;
	input->throttle_j = 0.0f;
	input->primary_trigger = 0.0f;
	input->facing_delta_yaw = 0.0f;
	input->facing_delta_pitch = 0.0f;

	if (distance > 0.1f)
	{
		control = player_control_get(local_player_index);
		if (control)
		{
			vector_from_points3d(&player_position, &teammate_position, &toward_teammate);
			euler_angles2d_from_vector3d(&desired_angles, &toward_teammate);

			delta_yaw = desired_angles.yaw - control->desired_angles.yaw;
			if (delta_yaw > _pi)
				delta_yaw -= _pi * 2.0f;
			if (delta_yaw < -_pi)
				delta_yaw += _pi * 2.0f;
			delta_pitch = desired_angles.pitch - control->desired_angles.pitch;

			if (delta_yaw > AI_PLAYER_TURN_RATE)
				delta_yaw = AI_PLAYER_TURN_RATE;
			if (delta_yaw < -AI_PLAYER_TURN_RATE)
				delta_yaw = -AI_PLAYER_TURN_RATE;
			if (delta_pitch > AI_PLAYER_PITCH_RATE)
				delta_pitch = AI_PLAYER_PITCH_RATE;
			if (delta_pitch < -AI_PLAYER_PITCH_RATE)
				delta_pitch = -AI_PLAYER_PITCH_RATE;

			input->facing_delta_yaw = delta_yaw;
			input->facing_delta_pitch = delta_pitch;
		}
	}

	ai_player_write_state(&player_position, &teammate_position, distance);

	return 1;
}
