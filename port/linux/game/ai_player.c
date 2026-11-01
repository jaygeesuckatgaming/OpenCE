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
#include "ai/ai_scenario_definitions.h"
#include "ai/encounters.h"
#include "interface/terminal.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "tag_files/tag_files.h"
#include "ai_player.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
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
/* how near (world units) it closes to before strafing instead of advancing */
#define AI_PLAYER_ENGAGE_DISTANCE 10.0f
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

/* the order the Gem brain gives: what to do, not how to do it. The bot does
   the aiming and firing; the order picks the intent, as the chasm bridge only
   applied whitelisted orders to an NPC. */
enum
{
	_ai_goal_none = 0,
	_ai_goal_follow,
	_ai_goal_engage,
	_ai_goal_hold,
	_ai_goal_retreat,
	_ai_goal_regroup,
	_ai_goal_idle
};

static struct
{
	boolean valid;
	int mode;
} ai_player_goal = { FALSE, _ai_goal_none };

static const char *ai_player_goal_name(
	int mode)
{
	switch (mode)
	{
	case _ai_goal_follow: return "follow";
	case _ai_goal_engage: return "engage";
	case _ai_goal_hold: return "hold";
	case _ai_goal_retreat: return "retreat";
	case _ai_goal_regroup: return "regroup";
	case _ai_goal_idle: return "idle";
	default: return "none";
	}
}

static int ai_player_goal_from_name(
	const char *name)
{
	if (!strcmp(name, "follow")) return _ai_goal_follow;
	if (!strcmp(name, "engage")) return _ai_goal_engage;
	if (!strcmp(name, "hold")) return _ai_goal_hold;
	if (!strcmp(name, "retreat")) return _ai_goal_retreat;
	if (!strcmp(name, "regroup")) return _ai_goal_regroup;
	if (!strcmp(name, "idle")) return _ai_goal_idle;
	return _ai_goal_none;
}

/* the order in what someone types ("stay here", "follow me"): the same words
   the Gem brain hears in chat */
static int ai_player_order_from_words(
	const char *text)
{
	char lowered[128];
	size_t index;

	for (index = 0; text[index] && index < sizeof(lowered) - 1; index++)
		lowered[index] = (char)tolower((unsigned char)text[index]);
	lowered[index] = '\0';

	if (strstr(lowered, "follow") || strstr(lowered, "come with") ||
		strstr(lowered, "with me") || strstr(lowered, "stick") ||
		strstr(lowered, "come along"))
		return _ai_goal_follow;
	if (strstr(lowered, "regroup") || strstr(lowered, "come back") ||
		strstr(lowered, "return"))
		return _ai_goal_regroup;
	if (strstr(lowered, "retreat") || strstr(lowered, "fall back") ||
		strstr(lowered, "pull back") || strstr(lowered, "run away"))
		return _ai_goal_retreat;
	if (strstr(lowered, "attack") || strstr(lowered, "engage") ||
		strstr(lowered, "kill") || strstr(lowered, "fight") ||
		strstr(lowered, "open fire") || strstr(lowered, "push up"))
		return _ai_goal_engage;
	if (strstr(lowered, "hold") || strstr(lowered, "stay") ||
		strstr(lowered, "stop") || strstr(lowered, "wait") ||
		strstr(lowered, "guard") || strstr(lowered, "cover me"))
		return _ai_goal_hold;
	if (strstr(lowered, "idle") || strstr(lowered, "stand still") ||
		strstr(lowered, "do nothing"))
		return _ai_goal_idle;
	return _ai_goal_none;
}

/* a string value of a key in a small flat JSON object */
static int ai_player_json_string(
	const char *json,
	const char *key,
	char *out,
	int out_size)
{
	char pattern[64];
	const char *p;
	const char *start;
	const char *end;

	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	p = strstr(json, pattern);
	if (!p)
		return 0;
	p = strchr(p + strlen(pattern), ':');
	if (!p)
		return 0;
	p = strchr(p, '"');
	if (!p)
		return 0;
	start = p + 1;
	end = strchr(start, '"');
	if (!end)
		return 0;
	if (end - start >= out_size)
		end = start + out_size - 1;
	memcpy(out, start, (size_t)(end - start));
	out[end - start] = '\0';
	return 1;
}

static long ai_player_json_int(
	const char *json,
	const char *key,
	long fallback)
{
	char pattern[64];
	const char *p;

	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	p = strstr(json, pattern);
	if (!p)
		return fallback;
	p = strchr(p + strlen(pattern), ':');
	if (!p)
		return fallback;
	return strtol(p + 1, NULL, 10);
}

/* the route the player walked, kept as crumbs: following it is how a co-op
   partner crosses a level without a pathfinder of its own */
#define AI_PLAYER_TRAIL_MAXIMUM 48
#define AI_PLAYER_TRAIL_SPACING 0.75f
#define AI_PLAYER_TRAIL_TICKS 15
#define AI_PLAYER_TRAIL_REACHED 1.2f
#define AI_PLAYER_TRAIL_STALE 120

static struct
{
	real_point3d points[AI_PLAYER_TRAIL_MAXIMUM];
	short head;
	short count;
	real_point3d last;
	unsigned long time;
} ai_player_trail;

static void ai_player_trail_reset(
	real_point3d const *start)
{
	ai_player_trail.head = 0;
	ai_player_trail.count = 1;
	ai_player_trail.points[0] = *start;
	ai_player_trail.last = *start;
	ai_player_trail.time = game_time_get();
}

static void ai_player_trail_update(
	real_point3d const *teammate_position,
	real distance_to_teammate)
{
	unsigned long now = game_time_get();

	/* close to the player, or an old trail: begin again where they are */
	if (distance_to_teammate < AI_PLAYER_FOLLOW_DISTANCE ||
		ai_player_trail.count == 0 ||
		now - ai_player_trail.time > AI_PLAYER_TRAIL_STALE)
	{
		ai_player_trail_reset(teammate_position);
		return;
	}

	if (distance3d(&ai_player_trail.last, teammate_position) >= AI_PLAYER_TRAIL_SPACING ||
		now - ai_player_trail.time > AI_PLAYER_TRAIL_TICKS)
	{
		short index = (ai_player_trail.head + ai_player_trail.count) % AI_PLAYER_TRAIL_MAXIMUM;

		ai_player_trail.points[index] = *teammate_position;
		if (ai_player_trail.count < AI_PLAYER_TRAIL_MAXIMUM)
			ai_player_trail.count++;
		else
			ai_player_trail.head = (ai_player_trail.head + 1) % AI_PLAYER_TRAIL_MAXIMUM;
		ai_player_trail.last = *teammate_position;
		ai_player_trail.time = now;
	}
}

/* the oldest crumb not reached yet: the way back to the player through
   whatever door or around whatever wall they took */
static real_point3d const *ai_player_trail_target(
	real_point3d const *position)
{
	while (ai_player_trail.count > 0)
	{
		real_point3d const *head = &ai_player_trail.points[ai_player_trail.head];

		if (distance3d(position, head) > AI_PLAYER_TRAIL_REACHED)
			return head;

		ai_player_trail.head = (ai_player_trail.head + 1) % AI_PLAYER_TRAIL_MAXIMUM;
		ai_player_trail.count--;
	}
	return NULL;
}

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

/* the nearest enemy it can see (require_sight), or simply the nearest */
static long ai_player_scan_enemy(
	long unit_index,
	long teammate_unit_index,
	real_point3d const *eye,
	real_point3d const *origin,
	boolean require_sight,
	real_point3d *aim_point,
	real *out_distance)
{
	struct object_iterator iterator;
	struct unit_datum *unit;
	long best = NONE;
	real best_distance = AI_PLAYER_ENEMY_RANGE;

	object_iterator_new(&iterator, _object_mask_unit, 0);
	while ((unit = (struct unit_datum *)object_iterator_next(&iterator)) != NULL)
	{
		real_point3d candidate_aim;
		real candidate_distance;

		if (!ai_player_enemy_valid(unit_index, iterator.index, teammate_unit_index,
			origin, &candidate_aim, &candidate_distance))
			continue;
		if (require_sight && !ai_player_can_see(eye, iterator.index, unit_index))
			continue;
		if (candidate_distance > best_distance)
			continue;

		best = iterator.index;
		best_distance = candidate_distance;
		*aim_point = candidate_aim;
	}

	if (best != NONE)
		*out_distance = best_distance;
	return best;
}

/* the enemy it is fighting while that one is still alive and near, else the
   nearest it can see, or (when seeking) the nearest there is; NONE if none.
   Keeping the one already chosen is what stops it swinging between enemies as
   their cover comes and goes. */
static long ai_player_nearest_enemy(
	long unit_index,
	long teammate_unit_index,
	long preferred_unit_index,
	boolean seek,
	real_point3d *eye,
	real_point3d *aim_point,
	real *out_distance)
{
	real_point3d origin;
	long best;

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

	/* what it can see first; an order to engage may then take what it cannot,
	   so it advances on the enemy instead of standing by the player */
	best = ai_player_scan_enemy(unit_index, teammate_unit_index, eye, &origin, TRUE,
		aim_point, out_distance);
	if (best == NONE && seek)
		best = ai_player_scan_enemy(unit_index, teammate_unit_index, eye, &origin, FALSE,
			aim_point, out_distance);
	if (best != NONE)
		ai_player_bot.target_seen_time = game_time_get();
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

/* share of its body left: the health that does not come back on its own.
   Shields are the layer over it and recharge, so a full body with the
   shields down is not a soldier about to die. Both vitalities are already
   0..1 (the HUD uses them as they are); maximum_* is the tag's scale. */
static real ai_player_health_share(
	long unit_index)
{
	struct unit_datum *unit = unit_get(unit_index);

	return PIN(unit->object.body_vitality, 0.0f, 1.0f);
}

static real ai_player_shield_share(
	long unit_index)
{
	struct unit_datum *unit = unit_get(unit_index);

	return PIN(unit->object.shield_vitality, 0.0f, 1.0f);
}

/* how far ahead it tests the ground before walking there */
#define AI_PLAYER_PROBE_AHEAD 2.0f
/* how far below the floor it stands on the one ahead may be: a step or a
   slope, but not a cliff */
#define AI_PLAYER_MAX_DROP 2.5f
/* how far down its floor is looked for at all */
#define AI_PLAYER_PROBE_DEPTH 8.0f
/* a surface a player stands on has a normal this close to straight up: a
   bridge's side, or too steep a slope, is not one (a ramp is about 0.87) */
#define AI_PLAYER_FLOOR_NORMAL 0.5f
/* what a probe is tested against: the level and the objects (crates, machines,
   vehicles), both faces of each, as coop_enemies.c tests its floor */
#define AI_PLAYER_COLLISION_FLAGS (FLAG(_collision_test_structure_bit) | FLAG(_collision_test_objects_bit) | \
	_collision_test_objects_sight_blocking_flags | FLAG(_collision_test_front_facing_surfaces_bit) | \
	FLAG(_collision_test_back_facing_surfaces_bit))

/* the floor under a point, if there is one within PROBE_DEPTH of it */
static boolean ai_player_ground(
	real_point3d const *point,
	real_point3d *floor)
{
	struct collision_result collision;
	real_point3d from = *point;
	real_vector3d down = { 0.0f, 0.0f, -AI_PLAYER_PROBE_DEPTH };

	from.z += AI_PLAYER_MAX_DROP * 0.5f;
	if (!collision_test_vector(AI_PLAYER_COLLISION_FLAGS, &from, &down, NONE, &collision))
		return FALSE;
	if (collision.plane.n.k < AI_PLAYER_FLOOR_NORMAL)
		return FALSE;
	*floor = collision.point;
	return TRUE;
}

/* whether a step in this direction stays on ground a player would walk on:
   a slope or a step is fine, a void or a real drop is not */
static boolean ai_player_walk_safe(
	real_point3d const *position,
	real_vector3d const *walk)
{
	real_vector3d step = *walk;
	real_point3d here;
	real_point3d ahead = *position;
	real_point3d floor;
	real reference;

	step.k = 0.0f;
	if (magnitude3d(&step) < 0.01f)
		return TRUE;
	normalize3d(&step);

	/* the floor it stands on, or its own height if that cannot be read */
	reference = ai_player_ground(position, &here) ? here.z : position->z;

	ahead.x += step.i * AI_PLAYER_PROBE_AHEAD;
	ahead.y += step.j * AI_PLAYER_PROBE_AHEAD;

	if (!ai_player_ground(&ahead, &floor))
		return FALSE; /* a hole, or the edge of the level */
	if (floor.z < reference - AI_PLAYER_MAX_DROP)
		return FALSE; /* a drop, not a step */
	return TRUE;
}

/* the walk turned a little, to look for a way along an edge */
static void ai_player_turn_walk(
	real_vector3d const *walk,
	real angle,
	real_vector3d *out)
{
	real cosine = (real)cos(angle);
	real sine = (real)sin(angle);

	out->i = walk->i * cosine - walk->j * sine;
	out->j = walk->i * sine + walk->j * cosine;
	out->k = 0.0f;
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

/* the console's order goes where the brain looks for one: a small file next
   to the goal, which the brain applies and then removes */
static void ai_player_write_order(
	int mode)
{
	char path[1100];
	const char *goal = config_string("ai_player.goal_path");
	const char *slash;
	FILE *file;

	if (!goal || !goal[0])
		goal = "ai_goal.json";
	slash = strrchr(goal, '/');
	if (!slash)
		slash = strrchr(goal, '\\');
	if (slash)
		snprintf(path, sizeof(path), "%.*sai_order.json", (int)(slash - goal) + 1, goal);
	else
		snprintf(path, sizeof(path), "ai_order.json");

	file = ai_player_open(path, "wb");
	if (!file)
		return;
	fprintf(file, "{\"version\":1,\"time_ms\":%lu,\"mode\":\"%s\"}\n",
		system_milliseconds(), ai_player_goal_name(mode));
	fclose(file);

	return;
}

/* the brain's order, re-read a few times a second: a file older than a couple
   of seconds is a brain that stopped, and the bot falls back to its own
   judgement */
static void ai_player_read_goal(void)
{
	static unsigned long last_read;
	unsigned long now = system_milliseconds();
	const char *path;
	FILE *file;
	char buffer[512];
	char name[32];
	long goal_ms;
	size_t size;

	if (now - last_read < 150)
		return;
	last_read = now;

	path = config_string("ai_player.goal_path");
	if (!path || !path[0])
		path = "ai_goal.json";
	file = ai_player_open(path, "rb");
	if (!file)
	{
		ai_player_goal.valid = FALSE;
		return;
	}
	size = fread(buffer, 1, sizeof(buffer) - 1, file);
	fclose(file);
	buffer[size] = '\0';

	goal_ms = ai_player_json_int(buffer, "time_ms", 0);
	if (goal_ms <= 0 || now - (unsigned long)goal_ms > 2000)
	{
		ai_player_goal.valid = FALSE;
		return;
	}
	if (!ai_player_json_string(buffer, "mode", name, sizeof(name)))
	{
		ai_player_goal.valid = FALSE;
		return;
	}
	ai_player_goal.mode = ai_player_goal_from_name(name);
	ai_player_goal.valid = TRUE;
}

static void ai_player_write_state(
	long local_unit_index,
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
	struct unit_datum *unit;
	long weapon_index;
	long ammo_loaded = 0;
	long ammo_total = 0;
	short enemy_count = 0;
	real nearest_enemy = 0.0f;

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

	unit = unit_get(local_unit_index);
	weapon_index = unit_inventory_get_weapon(local_unit_index, unit->unit.current_weapon_index);
	if (weapon_index != NONE)
	{
		struct weapon_datum *weapon = weapon_get(weapon_index);

		if (weapon && weapon_definition_get(weapon->definition_index)->weapon.magazines.count > 0)
		{
			ammo_loaded = weapon->weapon.magazines[0].rounds_loaded;
			ammo_total = weapon->weapon.magazines[0].rounds_total;
		}
	}

	{
		struct object_iterator iterator;
		struct unit_datum *other;

		object_iterator_new(&iterator, _object_mask_unit, 0);
		while ((other = (struct unit_datum *)object_iterator_next(&iterator)) != NULL)
		{
			real_point3d enemy_origin;
			real enemy_distance;

			if (iterator.index == local_unit_index)
				continue;
			if (TEST_FLAG(other->object.damage_flags, _object_dead_bit))
				continue;
			if (!ai_player_enemy_team(unit->object.owner_team_index, other->object.owner_team_index))
				continue;
			object_get_origin(iterator.index, &enemy_origin);
			enemy_distance = distance3d(player_position, &enemy_origin);
			if (enemy_distance > 60.0f)
				continue;
			enemy_count++;
			if (nearest_enemy == 0.0f || enemy_distance < nearest_enemy)
				nearest_enemy = enemy_distance;
		}
	}

	fprintf(file,
		"{\"version\":2,\"time_ms\":%lu,"
		"\"player\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},"
		"\"teammate\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},"
		"\"distance\":%.3f,"
		"\"health\":%.2f,\"shields\":%.2f,"
		"\"ammo_loaded\":%ld,\"ammo_total\":%ld,"
		"\"enemy_count\":%d,\"nearest_enemy\":%.1f,\"goal\":\"%s\"}\n",
		now,
		player_position->x, player_position->y, player_position->z,
		teammate_position->x, teammate_position->y, teammate_position->z,
		distance,
		ai_player_health_share(local_unit_index),
		ai_player_shield_share(local_unit_index), ammo_loaded, ammo_total,
		(int)enemy_count, nearest_enemy,
		ai_player_goal_name(ai_player_goal.valid ? ai_player_goal.mode : _ai_goal_none));

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
	int mode;
	boolean fight;

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
	ai_player_trail_update(&teammate_position, distance);

	input->throttle_i = 0.0f;
	input->throttle_j = 0.0f;
	input->primary_trigger = 0.0f;
	input->facing_delta_yaw = 0.0f;
	input->facing_delta_pitch = 0.0f;
	input->unit_control_flags = 0;

	/* the brain's order (ai_goal.json), if it is still being written */
	ai_player_read_goal();
	mode = ai_player_goal.valid ? ai_player_goal.mode : _ai_goal_none;
	fight = mode == _ai_goal_none || mode == _ai_goal_engage ||
		mode == _ai_goal_hold || mode == _ai_goal_retreat;

	/* one enemy, kept while it is still there to be seen, else the nearest */
	enemy_unit_index = NONE;
	if (fight)
	{
		/* only what it can see: a player does not know where an unseen enemy
		   stands, and a target just behind cover is kept by its memory */
		enemy_unit_index = ai_player_nearest_enemy(
			local_unit_index, teammate_unit_index, ai_player_bot.target_unit_index,
			FALSE, &eye, &enemy_aim, &enemy_distance);
	}
	else
	{
		ai_player_bot.target_unit_index = NONE;
		ai_player_bot.engaged_until = 0;
		unit_get_camera_position(local_unit_index, &eye);
	}

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

		if (mode == _ai_goal_none || mode == _ai_goal_idle || mode == _ai_goal_hold)
		{
			/* holds its ground, and fights from there: it follows only when
			   told to (follow/regroup), not by default */
		}
		else if (mode == _ai_goal_follow || mode == _ai_goal_regroup || mode == _ai_goal_retreat)
		{
			/* goes to the teammate (retreat still fires on what it sees):
			   straight there when it can see them, else back along the way
			   they actually walked, through the door or around the wall */
			if (distance > AI_PLAYER_FOLLOW_DISTANCE)
			{
				real_point3d const *target = NULL;

				if (!ai_player_can_see(&eye, teammate_unit_index, local_unit_index))
					target = ai_player_trail_target(&player_position);
				if (!target)
					target = &teammate_position;

				vector_from_points3d(&player_position, target, &desired_walk);
				normalize3d(&desired_walk);
			}
		}
		else if (enemy_unit_index != NONE)
		{
			real health = ai_player_health_share(local_unit_index);
			real_vector3d to_enemy;

			if (!ai_player_bot.retreating && health < AI_PLAYER_RETREAT_ENTER)
				ai_player_bot.retreating = TRUE;
			else if (ai_player_bot.retreating && health > AI_PLAYER_RETREAT_LEAVE)
				ai_player_bot.retreating = FALSE;

			if (game_time_get() - ai_player_bot.strafe_time > AI_PLAYER_STRAFE_TICKS)
			{
				ai_player_bot.strafe_time = game_time_get();
				ai_player_bot.strafe_sign = real_random_range(-1.0f, 1.0f) > 0.0f ? 1.0f : -1.0f;
			}

			/* close in on it while it is out of range, then strafe across
			   its view (or back away when badly hurt), as a player fights */
			vector_from_points3d(&player_position, &enemy_aim, &to_enemy);
			to_enemy.k = 0.0f;
			if (magnitude3d(&to_enemy) > 0.01f)
			{
				normalize3d(&to_enemy);
				if (ai_player_bot.retreating)
				{
					scale_vector3d(&to_enemy, -0.8f, &desired_walk);
				}
				else if (enemy_distance > AI_PLAYER_ENGAGE_DISTANCE)
				{
					scale_vector3d(&to_enemy, 0.9f, &desired_walk);
				}
				else
				{
					cross_product3d(global_up3d, &to_enemy, &desired_walk);
					scale_vector3d(&desired_walk, ai_player_bot.strafe_sign * 0.6f, &desired_walk);
				}
			}
		}
		else if (game_time_get() < ai_player_bot.engaged_until)
		{
			/* it saw one here a moment ago: go and look, as a player would,
			   instead of pretending to know where it went */
			vector_from_points3d(&player_position, &ai_player_bot.last_enemy_point, &desired_walk);
			desired_walk.k = 0.0f;
			if (magnitude3d(&desired_walk) > 0.01f)
				normalize3d(&desired_walk);
		}
		/* nothing else moves it: it never trails the player unless an order
		   (follow/regroup/retreat) says to */

		/* no walking off an edge: a way along it is looked for, else it stops */
		if (magnitude3d(&desired_walk) > 0.01f && !ai_player_walk_safe(&player_position, &desired_walk))
		{
			static real const turns[] = { 0.6f, -0.6f, 1.2f, -1.2f, 1.8f, -1.8f, 3.14159f };
			real_vector3d turned;
			boolean found = FALSE;
			int turn_index;

			for (turn_index = 0; turn_index < (int)(sizeof(turns) / sizeof(turns[0])); turn_index++)
			{
				ai_player_turn_walk(&desired_walk, turns[turn_index], &turned);
				if (ai_player_walk_safe(&player_position, &turned))
				{
					desired_walk = turned;
					found = TRUE;
					break;
				}
			}
			if (!found)
			{
				/* stop here, at once: letting the smoothed walk coast on
				   would carry it over the edge anyway */
				desired_walk.i = 0.0f;
				desired_walk.j = 0.0f;
				desired_walk.k = 0.0f;
				ai_player_bot.walk.i = 0.0f;
				ai_player_bot.walk.j = 0.0f;
				ai_player_bot.walk.k = 0.0f;
			}
		}

		ai_player_bot.walk.i += (desired_walk.i - ai_player_bot.walk.i) * AI_PLAYER_WALK_SMOOTH;
		ai_player_bot.walk.j += (desired_walk.j - ai_player_bot.walk.j) * AI_PLAYER_WALK_SMOOTH;
		ai_player_bot.walk.k += (desired_walk.k - ai_player_bot.walk.k) * AI_PLAYER_WALK_SMOOTH;

		input->throttle_i = dot_product3d(&ai_player_bot.walk, &forward);
		input->throttle_j = dot_product3d(&ai_player_bot.walk, &left);
	}

	ai_player_write_state(local_unit_index, &player_position, &teammate_position, distance);

	return 1;
}

/* ---------- testing: place the map's own encounters, so a fight can be
had without waiting for the script's drop ship */

static real_point3d const *ai_player_encounter_location(
	struct encounter_definition *encounter,
	real *out_distance,
	real_point3d const *player_position)
{
	long squad_index;

	for (squad_index = 0; squad_index < encounter->squads.count; squad_index++)
	{
		struct squad_definition *squad = TAG_BLOCK_GET_ELEMENT(
			&encounter->squads, squad_index, struct squad_definition);
		long location_index;

		for (location_index = 0; location_index < squad->starting_locations.count; location_index++)
		{
			struct actor_starting_location *location = TAG_BLOCK_GET_ELEMENT(
				&squad->starting_locations, location_index, struct actor_starting_location);
			real distance = distance3d(player_position, &location->position);

			if (*out_distance < 0.0f || distance < *out_distance)
			{
				*out_distance = distance;
				return &location->position;
			}
		}
	}
	return NULL;
}

boolean ai_player_console_command(
	char const *command)
{
	static real_argb_color const white = { 1.f, 1.f, 1.f, 1.f };
	struct scenario *scenario;
	long count;
	long index;

	while (*command == ' ')
		command++;
	/* "gem ...", and the older "gem ..." as an alias */
	if (_strnicmp(command, "gem", 3) == 0 && (command[3] == ' ' || command[3] == 0))
		command += 3;
	else if (_strnicmp(command, "gem", 6) == 0 && (command[6] == ' ' || command[6] == 0))
		command += 6;
	else
		return FALSE;
	while (*command == ' ')
		command++;

	scenario = global_scenario_get();
	if (!scenario)
	{
		terminal_printf(&white, "gem: no map loaded");
		return TRUE;
	}
	count = scenario->ai_encounters.count;

	if (!_stricmp(command, "encounters"))
	{
		for (index = 0; index < count; index++)
		{
			struct encounter_definition *encounter = TAG_BLOCK_GET_ELEMENT(
				&scenario->ai_encounters, index, struct encounter_definition);
			terminal_printf(&white, "gem: %ld %s", index, encounter->name);
		}
		terminal_printf(&white, "gem: %ld encounters", count);
		return TRUE;
	}

	if (!_strnicmp(command, "spawn", 5) && (command[5] == ' ' || command[5] == 0))
	{
		command += 5;
		while (*command == ' ')
			command++;

		if (!_stricmp(command, "nearest"))
		{
			long player_index = local_player_get_player_index(0);
			struct player_datum *player = player_index == NONE ? NULL : player_get(player_index);
			real_point3d player_position;
			real best_distance = -1.0f;
			long best = NONE;

			if (!player || player->unit_index == NONE)
			{
				terminal_printf(&white, "gem: no player to place an encounter near");
				return TRUE;
			}
			object_get_origin(player->unit_index, &player_position);
			for (index = 0; index < count; index++)
			{
				struct encounter_definition *encounter = TAG_BLOCK_GET_ELEMENT(
					&scenario->ai_encounters, index, struct encounter_definition);
				real distance = -1.0f;

				if (ai_player_encounter_location(encounter, &distance, &player_position) &&
					(best == NONE || distance < best_distance))
				{
					best = index;
					best_distance = distance;
				}
			}
			if (best == NONE)
			{
				terminal_printf(&white, "gem: no encounter has a starting location");
				return TRUE;
			}
			encounter_create(best, NONE, NONE);
			terminal_printf(&white, "gem: placed %ld (%s), %.1f units away", best,
				TAG_BLOCK_GET_ELEMENT(&scenario->ai_encounters, best, struct encounter_definition)->name,
				best_distance);
			return TRUE;
		}

		if (*command)
		{
			index = strtol(command, NULL, 10);
			if (index >= 0 && index < count)
			{
				encounter_create(index, NONE, NONE);
				terminal_printf(&white, "gem: placed %ld", index);
			}
			else
			{
				terminal_printf(&white, "gem: no encounter %ld (0..%ld)", index, count - 1);
			}
			return TRUE;
		}

		terminal_printf(&white, "gem spawn <index|nearest>");
		return TRUE;
	}

	{
		int order = ai_player_order_from_words(command);

		if (order != _ai_goal_none)
		{
			ai_player_write_order(order);
			terminal_printf(&white, "gem: %s", ai_player_goal_name(order));
			return TRUE;
		}
	}

	terminal_printf(&white,
		"gem encounters | gem spawn <index|nearest> |"
		" gem follow|stay|engage|retreat|regroup|idle");
	return TRUE;
}
