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
#include "game/game_allegiance.h"
#include "game/players.h"
#include "objects/objects.h"
#include "memory/data.h"
#include "math/real_math.h"
#include "physics/collisions.h"
#include "items/weapon_definitions.h"
#include "items/weapons.h"
#include "units/unit_control_data.h"
#include "units/units.h"
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

/* how far (world units) it looks for enemies to fight */
#define AI_PLAYER_ENEMY_RANGE 40.0f
/* ticks before it starts shooting a target it has just seen */
#define AI_PLAYER_REACTION_TICKS 12
/* fire in bursts: on for so many ticks, off for so many */
#define AI_PLAYER_BURST_ON_TICKS 18
#define AI_PLAYER_BURST_OFF_TICKS 9

/* how often it picks a new side to strafe around a target, ticks */
#define AI_PLAYER_STRAFE_TICKS 90
/* how long it keeps fighting a target it has lost sight of, ticks */
#define AI_PLAYER_ENGAGE_TICKS 90
/* how long it keeps a target it cannot see before choosing another, ticks */
#define AI_PLAYER_TARGET_HOLD_TICKS 30
/* below this share of its shields it backs away, and above this it stops */
#define AI_PLAYER_RETREAT_ENTER 0.25f
#define AI_PLAYER_RETREAT_LEAVE 0.45f
/* how far off its aim it is, radians, more so the farther the target (0: the
   aim is exact, which is what stops it shaking on a target) */
#define AI_PLAYER_AIM_ERROR 0.0f
/* how much of the way to the target its aim turns each tick, fighting, and
   looking about otherwise */
#define AI_PLAYER_AIM_TURN 0.35f
#define AI_PLAYER_LOOK_TURN 0.12f
/* how much of the way to the walk it wants it moves each tick (world space) */
#define AI_PLAYER_WALK_SMOOTH 0.20f

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

/* kept between ticks: what it is fighting, how long it has, and its burst and
   strafe clocks */
static struct
{
	long target_unit_index;
	unsigned long target_time;
	unsigned long target_seen_time;
	unsigned long burst_time;
	unsigned long strafe_time;
	unsigned long engaged_until;
	real strafe_sign;
	boolean retreating;
	short reload_wait;
	real_vector3d aim;
	real_vector3d walk;
	real_point3d last_enemy_point;
} ai_player_bot = { NONE, 0, 0, 0, 0, 0, 1.0f, FALSE, 0,
	{ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } };

/* whether a line from the eye to just short of an object is clear (the
   companion's own sight test: a vehicle blocks sight, and so would block the
   line to its own middle) */
static boolean ai_player_line_clear(
	real_point3d const *from,
	real_point3d const *to,
	long ignore_object_index)
{
	struct collision_result collision;

	return !collision_test_line(
		FLAG(_collision_test_structure_bit) | FLAG(_collision_test_front_facing_surfaces_bit) |
			FLAG(_collision_test_objects_bit) | _collision_test_objects_sight_blocking_flags,
		from, to, ignore_object_index, &collision);
}

static boolean ai_player_can_see(
	real_point3d const *eye,
	long object_index,
	long ignore_object_index)
{
	struct object_datum *object = object_get(object_index);
	real_vector3d to;
	real_point3d end;
	real distance;
	real radius = object->object.bounding_sphere_radius;

	vector_from_points3d(eye, &object->object.bounding_sphere_center, &to);
	distance = magnitude3d(&to);
	if (distance <= radius)
		return TRUE;
	scale_vector3d(&to, (distance - radius) / distance, &to);
	end.x = eye->x + to.i;
	end.y = eye->y + to.j;
	end.z = eye->z + to.k;
	return ai_player_line_clear(eye, &end, ignore_object_index);
}

/* (a team of the player's enemies: a unit of no team counts as everyone's
   enemy to game_team_is_enemy, and isn't) */
static boolean ai_player_enemy_team(
	short player_team_index,
	short team_index)
{
	return VALID_INDEX(team_index, NUMBER_OF_SOLO_CAMPAIGN_TEAMS) && team_index != _game_team_default &&
		game_team_is_enemy(player_team_index, team_index);
}

/* whether one unit is an enemy within range, and the point on it to aim at.
   The teammate is never one, even in a free-for-all. Visibility is not part
   of it: the one being fought is kept as it steps behind cover, and only a
   new one has to be seen to be picked. */
static boolean ai_player_enemy_valid(
	long unit_index,
	long candidate_unit_index,
	long teammate_unit_index,
	real_point3d const *origin,
	real_point3d *aim_point,
	real *out_distance)
{
	struct unit_datum *self;
	struct unit_datum *unit;
	real_point3d enemy_origin;
	real distance;

	if (candidate_unit_index == NONE || candidate_unit_index == unit_index ||
		candidate_unit_index == teammate_unit_index)
		return FALSE;
	unit = unit_try_and_get(candidate_unit_index);
	if (!unit || TEST_FLAG(unit->object.damage_flags, _object_dead_bit))
		return FALSE;
	self = unit_get(unit_index);
	if (!ai_player_enemy_team(self->object.owner_team_index, unit->object.owner_team_index))
		return FALSE;
	/* (a crew is its enemy vehicle: a banshee, not its pilot as well) */
	if (unit->object.parent_object_index != NONE &&
		ai_player_enemy_team(self->object.owner_team_index,
			object_get(unit->object.parent_object_index)->object.owner_team_index))
		return FALSE;

	object_get_origin(candidate_unit_index, &enemy_origin);
	distance = distance3d(origin, &enemy_origin);
	if (distance > AI_PLAYER_ENEMY_RANGE)
		return FALSE;

	*aim_point = unit->object.bounding_sphere_center;
	*out_distance = distance;
	return TRUE;
}

/* the enemy it is fighting while that one is still alive and near, else the
   nearest it can see; NONE if none. Keeping the one already chosen is what
   stops it swinging between enemies as their cover comes and goes. */
static long ai_player_nearest_enemy(
	long unit_index,
	long teammate_unit_index,
	long preferred_unit_index,
	real_point3d *eye,
	real_point3d *aim_point,
	real *out_distance)
{
	struct object_iterator iterator;
	struct unit_datum *unit;
	real_point3d origin;
	long best = NONE;
	real best_distance = AI_PLAYER_ENEMY_RANGE;

	object_get_origin(unit_index, &origin);
	unit_get_camera_position(unit_index, eye);

	if (preferred_unit_index != NONE &&
		ai_player_enemy_valid(unit_index, preferred_unit_index, teammate_unit_index,
			&origin, aim_point, out_distance))
	{
		if (ai_player_can_see(eye, preferred_unit_index, unit_index))
		{
			ai_player_bot.target_seen_time = game_time_get();
			return preferred_unit_index;
		}
		/* a step behind cover is kept; gone longer, another is looked for */
		if (game_time_get() - ai_player_bot.target_seen_time < AI_PLAYER_TARGET_HOLD_TICKS)
			return preferred_unit_index;
	}

	object_iterator_new(&iterator, _object_mask_unit, 0);
	while ((unit = (struct unit_datum *)object_iterator_next(&iterator)) != NULL)
	{
		real_point3d candidate_aim;
		real candidate_distance;

		if (!ai_player_enemy_valid(unit_index, iterator.index, teammate_unit_index,
			&origin, &candidate_aim, &candidate_distance))
			continue;
		if (!ai_player_can_see(eye, iterator.index, unit_index))
			continue;
		if (candidate_distance > best_distance)
			continue;

		best = iterator.index;
		best_distance = candidate_distance;
		*aim_point = candidate_aim;
	}

	if (best != NONE)
	{
		ai_player_bot.target_seen_time = game_time_get();
		*out_distance = best_distance;
	}
	return best;
}

/* the holding weapon's magazine is empty but has rounds in reserve */
static boolean ai_player_needs_reload(
	long unit_index)
{
	struct unit_datum *unit = unit_get(unit_index);
	long weapon_index = unit_inventory_get_weapon(unit_index, unit->unit.current_weapon_index);

	if (weapon_index != NONE)
	{
		struct weapon_datum *weapon = weapon_get(weapon_index);

		if (weapon && weapon_definition_get(weapon->definition_index)->weapon.magazines.count > 0 &&
			weapon->weapon.magazines[0].rounds_loaded == 0 &&
			weapon->weapon.magazines[0].rounds_total > 0)
			return TRUE;
	}
	return FALSE;
}

/* shares of its shields and body left, 1.0 if it has neither to lose */
static real ai_player_health_share(
	long unit_index)
{
	struct unit_datum *unit = unit_get(unit_index);

	if (unit->object.maximum_shield_vitality > 0.0f)
		return unit->object.shield_vitality / unit->object.maximum_shield_vitality;
	if (unit->object.maximum_body_vitality > 0.0f)
		return unit->object.body_vitality / unit->object.maximum_body_vitality;
	return 1.0f;
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
	real distance;
	real_euler_angles2d desired_angles;
	real delta_yaw;
	real delta_pitch;
	real turn;
	real_point3d eye;
	real_point3d enemy_aim;
	real_vector3d aim_direction;
	real enemy_distance = 0.0f;
	long enemy_unit_index;
	boolean needs_reload;

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

	input->throttle_i = 0.0f;
	input->throttle_j = 0.0f;
	input->primary_trigger = 0.0f;
	input->facing_delta_yaw = 0.0f;
	input->facing_delta_pitch = 0.0f;
	input->unit_control_flags = 0;

	/* one enemy, kept while it is still there to be seen, else the nearest */
	enemy_unit_index = ai_player_nearest_enemy(
		local_unit_index, teammate_unit_index, ai_player_bot.target_unit_index,
		&eye, &enemy_aim, &enemy_distance);

	/* what it would look at: the enemy it sees, the place it last saw one
	   while still engaged, or the teammate */
	if (enemy_unit_index != NONE)
	{
		ai_player_bot.engaged_until = game_time_get() + AI_PLAYER_ENGAGE_TICKS;
		ai_player_bot.last_enemy_point = enemy_aim;

		if (ai_player_bot.target_unit_index != enemy_unit_index)
		{
			ai_player_bot.target_unit_index = enemy_unit_index;
			ai_player_bot.target_time = game_time_get();
			ai_player_bot.burst_time = game_time_get();
		}

		vector_from_points3d(&eye, &enemy_aim, &aim_direction);
		normalize3d(&aim_direction);
		/* a little off, more so the farther the target */
		{
			real error = AI_PLAYER_AIM_ERROR * (1.0f + enemy_distance / 8.0f);

			aim_direction.i += real_random_range(-error, error);
			aim_direction.j += real_random_range(-error, error);
			aim_direction.k += real_random_range(-error, error);
			normalize3d(&aim_direction);
		}
		turn = AI_PLAYER_AIM_TURN;
	}
	else if (game_time_get() < ai_player_bot.engaged_until)
	{
		/* lost sight of it: keep facing where it was, and hold fire */
		vector_from_points3d(&eye, &ai_player_bot.last_enemy_point, &aim_direction);
		if (magnitude3d(&aim_direction) < 0.01f)
			vector_from_points3d(&eye, &teammate_position, &aim_direction);
		normalize3d(&aim_direction);
		turn = AI_PLAYER_LOOK_TURN;
	}
	else
	{
		ai_player_bot.target_unit_index = NONE;
		ai_player_bot.burst_time = game_time_get();
		vector_from_points3d(&eye, &teammate_position, &aim_direction);
		if (magnitude3d(&aim_direction) < 0.01f)
			aim_direction.i = 1.0f;
		normalize3d(&aim_direction);
		turn = AI_PLAYER_LOOK_TURN;
	}

	/* the aim it holds between ticks: a share of the way to the target each
	   tick, so the per-tick error and a moving target do not shake it */
	if (magnitude3d(&ai_player_bot.aim) < 0.01f)
	{
		ai_player_bot.aim = aim_direction;
	}
	else
	{
		ai_player_bot.aim.i += (aim_direction.i - ai_player_bot.aim.i) * turn;
		ai_player_bot.aim.j += (aim_direction.j - ai_player_bot.aim.j) * turn;
		ai_player_bot.aim.k += (aim_direction.k - ai_player_bot.aim.k) * turn;
	}
	normalize3d(&ai_player_bot.aim);

	control = player_control_get(local_player_index);
	if (control)
	{
		euler_angles2d_from_vector3d(&desired_angles, &ai_player_bot.aim);

		delta_yaw = desired_angles.yaw - control->desired_angles.yaw;
		if (delta_yaw > _pi)
			delta_yaw -= _pi * 2.0f;
		if (delta_yaw < -_pi)
			delta_yaw += _pi * 2.0f;
		delta_pitch = desired_angles.pitch - control->desired_angles.pitch;

		if (enemy_unit_index != NONE)
		{
			needs_reload = ai_player_needs_reload(local_unit_index);
			if (needs_reload)
			{
				/* the reload button is latched (player_queues_new.c): it
				   fires on the rising edge and stays until released, so a
				   held flag reloads only once. Pulse it instead. */
				if (ai_player_bot.reload_wait <= 0)
				{
					SET_FLAG(input->unit_control_flags, _unit_control_weapon_reload_bit, TRUE);
					ai_player_bot.reload_wait = 15;
				}
				else
				{
					ai_player_bot.reload_wait--;
				}
			}
			else
			{
				ai_player_bot.reload_wait = 0;
			}

			/* fire in bursts only once the game's own aim assist says the
			   reticle is on an enemy (the red crosshair, hud.c), which is
			   what the eye cannot judge from the angle alone */
			if (!needs_reload &&
				game_time_get() - ai_player_bot.target_time >= AI_PLAYER_REACTION_TICKS &&
				player_control_get_autoaim_level(local_player_index) >= 1.0f)
			{
				unsigned long burst = (game_time_get() - ai_player_bot.burst_time) %
					(AI_PLAYER_BURST_ON_TICKS + AI_PLAYER_BURST_OFF_TICKS);

				if (burst < AI_PLAYER_BURST_ON_TICKS)
				{
					input->primary_trigger = 1.0f;
					SET_FLAG(input->unit_control_flags, _unit_control_weapon_primary_trigger_bit, TRUE);
				}
			}
		}

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

	/* the walk it wants, in the world, smoothed there and only then put into
	   its own frame: smoothing the frame's parts instead made the world
	   direction wobble as the unit turned under it */
	{
		/* the throttle is turned by where the camera looks, not by the body,
		   which lags it (the companion passes its aim as the facing): using
		   the body's forward made the walk wobble as it caught up */
		real_vector3d forward = ai_player_bot.aim;
		real_vector3d left;
		real_vector3d desired_walk = *global_zero_vector3d;

		forward.k = 0.0f;
		if (magnitude3d(&forward) < 0.01f)
		{
			forward.i = 1.0f;
			forward.j = 0.0f;
		}
		normalize3d(&forward);
		cross_product3d(global_up3d, &forward, &left);

		if (enemy_unit_index != NONE || game_time_get() < ai_player_bot.engaged_until)
		{
			real health = ai_player_health_share(local_unit_index);
			real chase = 0.0f;
			real_vector3d follow_walk = *global_zero_vector3d;
			real_vector3d fight_walk = *global_zero_vector3d;

			if (!ai_player_bot.retreating && health < AI_PLAYER_RETREAT_ENTER)
				ai_player_bot.retreating = TRUE;
			else if (ai_player_bot.retreating && health > AI_PLAYER_RETREAT_LEAVE)
				ai_player_bot.retreating = FALSE;

			if (game_time_get() - ai_player_bot.strafe_time > AI_PLAYER_STRAFE_TICKS)
			{
				ai_player_bot.strafe_time = game_time_get();
				ai_player_bot.strafe_sign = real_random_range(-1.0f, 1.0f) > 0.0f ? 1.0f : -1.0f;
			}

			/* how much it goes to the teammate rather than fights where it
			   stands, blended: a teammate near the split point made it flap
			   between the two */
			if (distance > AI_PLAYER_FOLLOW_DISTANCE * 2.0f)
				chase = 1.0f;
			else if (distance > AI_PLAYER_FOLLOW_DISTANCE)
				chase = (distance - AI_PLAYER_FOLLOW_DISTANCE) / AI_PLAYER_FOLLOW_DISTANCE;

			if (chase > 0.0f)
			{
				vector_from_points3d(&player_position, &teammate_position, &follow_walk);
				normalize3d(&follow_walk);
			}
			if (chase < 1.0f && enemy_unit_index != NONE)
			{
				real_vector3d to_enemy;

				vector_from_points3d(&player_position, &enemy_aim, &to_enemy);
				to_enemy.k = 0.0f;
				if (magnitude3d(&to_enemy) > 0.01f)
				{
					normalize3d(&to_enemy);
					if (ai_player_bot.retreating)
					{
						scale_vector3d(&to_enemy, -0.8f, &fight_walk);
					}
					else
					{
						cross_product3d(global_up3d, &to_enemy, &fight_walk);
						scale_vector3d(&fight_walk, ai_player_bot.strafe_sign * 0.6f, &fight_walk);
					}
				}
			}

			desired_walk.i = follow_walk.i * chase + fight_walk.i * (1.0f - chase);
			desired_walk.j = follow_walk.j * chase + fight_walk.j * (1.0f - chase);
			desired_walk.k = follow_walk.k * chase + fight_walk.k * (1.0f - chase);
		}
		else if (distance > AI_PLAYER_FOLLOW_DISTANCE)
		{
			vector_from_points3d(&player_position, &teammate_position, &desired_walk);
			normalize3d(&desired_walk);
		}

		ai_player_bot.walk.i += (desired_walk.i - ai_player_bot.walk.i) * AI_PLAYER_WALK_SMOOTH;
		ai_player_bot.walk.j += (desired_walk.j - ai_player_bot.walk.j) * AI_PLAYER_WALK_SMOOTH;
		ai_player_bot.walk.k += (desired_walk.k - ai_player_bot.walk.k) * AI_PLAYER_WALK_SMOOTH;

		input->throttle_i = dot_product3d(&ai_player_bot.walk, &forward);
		input->throttle_j = dot_product3d(&ai_player_bot.walk, &left);
	}

	ai_player_write_state(&player_position, &teammate_position, distance);

	return 1;
}
