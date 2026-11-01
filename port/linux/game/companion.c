/*
COMPANION.C

Gem: a teammate in the campaign, a Spartan in red armour. He is the
player's own biped, painted red as a network co-op player is
(players_coop_color_unit), carrying copies of the player's weapons. He has
no actor: nothing of the game's AI decides for him.

His decisions come from "mate", the voice program (port/linux/src/mate_link.c).
Twice a second the game tells it what is going on, in words, and what Gem
can do, for mate's decision model to pick from ("state");
what happens in between goes to it as events ("Gem killed a grunt."). mate
answers with the action Gem takes. The game is only his body, carrying out
the action every tick through unit_control, as a player's input drives a
player's unit:
- he aims at the enemy mate chose, turning quickly but not instantly and not
  exactly (an aim lock, not a mouse), and fires in bursts while he can see it;
- he walks by the level's own pathfinding (path.c) to wherever the action
  needs him: the player (follow), nowhere (stay), close to his target (rush),
  a spot the enemy can't see (cover), or away from it (fall back);
- he reloads, switches weapons, throws a frag or a plasma grenade, melees,
  jumps, crouches, and turns his flashlight on and off when mate says;
- he walks to a weapon lying nearby and picks it up (dropping the one in his
  hands if both are full), walks to a vehicle and gets in, and gets out;
- he can turn his gun on the Chief.
He does nothing of his own accord: no target, no shooting, no reloading.
If he falls far behind the player he is put down beside him again, and if he
dies he comes back a while later.

mate also hears the player and speaks for Gem; his voice is a 3D sound of
the game's mixer, placed as the game places a unit's dialogue (from his head,
muffled by what's between, in the level's reverb), or over the radio when he's
far off or out of sight.
Gem hears what the player hears: the game sends mate the dialogue it plays
(Cortana on the radio, cutscenes) to transcribe, and each new objective. His
memory goes with the game's saves: mate keeps it at each checkpoint and brings
it back when that save is loaded (dying forgets what happened since).

Only a single-player campaign has him: not a multiplayer game, and not a
client of another machine's game.
*/

#include "cseries.h"
#include "cseries/cseries_windows.h"
#include "ai/path.h"
#include "ai/path_structure_bsp.h"
#include "bitmaps/bitmap_group.h"
#include "bitmaps/bitmap_group_lookup.h"
#include "camera/observer.h"
#include "cutscene/cinematics.h"
#include "game/game.h"
#include "game/game_allegiance.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "interface/hud_messaging.h"
#include "interface/terminal.h"
#include "interface/ui_widget.h"
#include "main/console.h"
#include "networking/network_game_globals.h"
#include "items/weapon_definitions.h"
#include "items/weapons.h"
#include "objects/objects.h"
#include "physics/collisions.h"
#include "render/render.h"
#include "main/main.h"
#include "render/render_cameras_internal.h"
#include "scenario/scenario.h"
#include "sound/game_sound.h"
#include "sound/sound_classes.h"
#include "sound/sound_definitions.h"
#include "sound/sound_manager.h"
#include "structures/structures.h"
#include "tag_files/tag_files.h"
#include "units/bipeds.h"
#include "units/unit_control_data.h"
#include "units/unit_definitions.h"
#include "units/units.h"

#include "companion.h"
#include "coop_enemies.h"
#include "network_distributed.h"
#include "halo_keyboard.h"
#include "../src/mate_link.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* platform.c's */
void platform_log(char const *format, ...);
/* port_config.c's */
double config_real(const char *name);
/* dsound_sdl.c's: Xbox ADPCM to 16-bit samples, freed there (its malloc is the
C library's, not the game's) */
short *dsound_decode_xbox_adpcm(const unsigned char *source, unsigned long size, unsigned long channels,
	unsigned long *frame_count);
void dsound_free_decoded(short *samples);
/* sound_definitions.c's */
extern unsigned long const sound_sample_rate_samples_per_second[];

/* ---------- constants */

enum
{
	/* how long after his death he comes back */
	COMPANION_RESPAWN_TICKS = 10 * TICKS_PER_SECOND,
	/* how often mate is told what is going on */
	COMPANION_STATE_TICKS = TICKS_PER_SECOND / 2,
	/* how often his path is found again */
	COMPANION_PATH_TICKS = 15,
	/* his bursts: the trigger held, then let go */
	COMPANION_BURST_ON_TICKS = 18,
	COMPANION_BURST_OFF_TICKS = 9,
	/* how long he strafes one way while fighting */
	COMPANION_STRAFE_TICKS = 45,
	COMPANION_MAXIMUM_ENEMIES = 12,
	/* the weapons and vehicles near him he is told of */
	COMPANION_MAXIMUM_THINGS = 6,
	/* how long he takes to start shooting a new target, and to set off after
	the Chief */
	COMPANION_REACTION_TICKS = 12,
	COMPANION_FOLLOW_REACTION_TICKS = 10,
};

/* one-off actions, and the ticks each holds its control */
enum
{
	_companion_act_none = 0,
	_companion_act_reload,
	_companion_act_grenade,
	_companion_act_melee,
	_companion_act_swap,
	_companion_act_jump,
	_companion_act_light,
	_companion_act_get_out,
	COMPANION_GRENADE_TICKS = 6,
};

enum
{
	_companion_order_follow = 0,
	_companion_order_hold,
	_companion_order_attack,
	_companion_order_advance,
	_companion_order_cover,
	_companion_order_retreat,
	_companion_order_pick_up,
	_companion_order_get_in,
	NUMBER_OF_COMPANION_ORDERS
};

static char const *const companion_order_names[NUMBER_OF_COMPANION_ORDERS] =
	{ "follow", "stay", "attack", "rush", "cover", "fall back", "pick up", "get in" };

/* following (world units, 3 m each): beside and behind the Chief while he
moves; while he's still, a spot of his own between the near and far distances
from him, another after a while */
#define COMPANION_FOLLOW_BEHIND 0.6f
#define COMPANION_FOLLOW_SIDE 0.45f
#define COMPANION_IDLE_NEAR 0.5f
#define COMPANION_IDLE_FAR 1.4f
/* the Chief's speed (units a tick) above which he's on the move, and a
Spartan's running speed */
#define COMPANION_MOVING_SPEED 0.01f
#define COMPANION_RUN_SPEED (2.25f / TICKS_PER_SECOND)
/* how fast his aim turns while fighting, and otherwise (a share a tick) */
#define COMPANION_LOOK_TURN 0.12f
/* how far behind the player he is put down beside him again */
#define COMPANION_TELEPORT_DISTANCE 40.f
/* how far he looks for enemies */
#define COMPANION_ENEMY_DISTANCE 45.f
/* the range he fights at (attack), and how near he gets (advance) */
#define COMPANION_FIGHT_NEAR 6.f
#define COMPANION_FIGHT_FAR 16.f
#define COMPANION_ADVANCE_DISTANCE 4.f
/* how far he goes looking for cover, and back (retreat) */
#define COMPANION_COVER_SEARCH 10.f
#define COMPANION_RETREAT_DISTANCE 10.f
/* how far he looks for weapons and vehicles, and how near he must be to take
one or get in */
#define COMPANION_WEAPON_DISTANCE 4.f
#define COMPANION_VEHICLE_DISTANCE 8.f
#define COMPANION_REACH 0.6f
#define COMPANION_MELEE_DISTANCE 0.7f
/* the aim lock: how much of the way to the target his aim turns each tick,
how far off it is (radians, at random), and how near the target it must be
to shoot */
#define COMPANION_AIM_TURN 0.35f
#define COMPANION_AIM_ERROR 0.025f
#define COMPANION_AIM_FIRE_COSINE 0.985f
#define COMPANION_PATHFINDING_RADIUS 0.3f
/* how far ahead his paths look for objects in the way (boulders, wrecks) */
#define COMPANION_OBSTACLE_REACH 16.f
/* his voice falls off beyond the dialogue class's minimum distance, or this
(world units) if the map's class has none */
#define COMPANION_VOICE_MINIMUM_DISTANCE 1.f
/* a world unit is 10 feet: distances for mate are in metres */
#define COMPANION_METRES_PER_UNIT 3.048f
/* his voice's loudness against the game's sounds */
#define COMPANION_VOICE_GAIN 4.f
/* and over the radio, against that (companion.radio_volume's 1) */
#define COMPANION_RADIO_GAIN 0.375f

/* ---------- structures */

struct companion_enemy
{
	long unit_index;
	real distance;
	boolean visible;
	real_point3d aim_point;
};

struct companion_body
{
	short order;
	long target_unit_index;
	/* hold: where he stands */
	real_point3d hold_point;
	/* where the order takes him, and the way there */
	boolean goal_valid;
	real_point3d goal;
	boolean path_valid;
	struct path_result path;
	long path_time;
	real_vector3d aim;
	long burst_time;
	long strafe_time;
	real strafe_sign;
	boolean crouch;
	/* the Chief told him to stop fighting: no target, no shooting */
	boolean hold_fire;
	/* mate had him fight (shoot or rush): when his target falls he moves on
	to the nearest enemy he sees, until he holds his fire */
	boolean engaging;
	/* a one-off action (reload, grenade, melee, swap...), at whom, and since when */
	short act;
	long act_target_unit_index;
	long act_time;
	short act_grenade_type;
	long target_time;
	/* what he walks to (a weapon to pick up, a vehicle to get in) */
	long goal_object_index;
	boolean crouching;
	/* following: how fast he goes (0 to 1), the side of the Chief he keeps, since
	when the Chief has been on the move, his own spot near the Chief while the
	Chief is still (and until when), and where he looks while idle (and until
	when; or at the Chief) */
	real pace;
	real follow_side;
	long player_moving_since;
	boolean idle_valid;
	real_point3d idle_spot;
	long idle_until;
	/* (getting nearer it: the nearest he's been, and when) */
	real idle_nearest;
	long idle_nearer_time;
	real_vector3d look;
	boolean look_at_player;
	long look_until;
};

/* a weapon lying near him, or a vehicle */
struct companion_thing
{
	long object_index;
	real distance;
};

/* ---------- globals */

static long companion_unit_index = NONE;
static long companion_respawn_time = 0;
static struct companion_body companion_body;
/* (path.c's search: too large for the stack) */
static struct path_state companion_path_state;
static struct companion_enemy companion_enemies[COMPANION_MAXIMUM_ENEMIES];
static short companion_enemy_count = 0;
static struct companion_thing companion_weapons[COMPANION_MAXIMUM_THINGS];
static short companion_weapon_count = 0;
static struct companion_thing companion_vehicles[COMPANION_MAXIMUM_THINGS];

/* the view mate asked for, Gem's eyes on what the player sees: taken as the
next frame is shown (d3d8_gl.c, companion_view_taken), with spots marked on
it, a grid across the lower part of the view, each the ground there, which
he can be sent to ("go_to") */
#define COMPANION_VIEW_COLUMNS 8
#define COMPANION_VIEW_ROWS 5
/* how far the view's spots, and where the player aims, reach (world units) */
#define COMPANION_VIEW_REACH 40.f
static boolean companion_view_requested;
static short companion_view_spot_count;
static real_point3d companion_view_spots[COMPANION_VIEW_COLUMNS * COMPANION_VIEW_ROWS];
static short companion_vehicle_count = 0;
static unsigned long companion_random_seed = 12345;
static boolean companion_player_dead = FALSE;
/* his voice is over the radio (companion_place_voice): mate's radio prompt */
static boolean companion_on_radio = FALSE;

/* ---------- private code */

static real companion_random(
	void)
{
	companion_random_seed = companion_random_seed * 1103515245u + 12345u;
	return (real)((companion_random_seed >> 8) & 0xFFFF) / 65535.f * 2.f - 1.f;
}

static long companion_player_unit(
	void)
{
	long player_index = local_player_get_player_index(0);

	return player_index == NONE ? NONE : player_get(player_index)->unit_index;
}

/* whether any of the colours companion_paint paints is red */
static boolean companion_is_red(
	struct unit_datum *unit)
{
	real_rgb_color const *red = global_real_rgb_red;
	short index;

	for (index = 0; index < NUMBER_OF_OBJECT_CHANGE_COLORS; index++)
	{
		real_rgb_color const *color = &unit->object.base_change_colors[index];

		if (color->red == red->red && color->green == red->green && color->blue == red->blue)
			return TRUE;
	}

	return FALSE;
}

/* whether this unit is Gem, alive: the player's biped with no player and
red armour (a loaded game has him at an index of its own) */
static boolean companion_is_alive(
	long unit_index,
	long definition_index)
{
	struct unit_datum *unit = unit_index == NONE ? NULL : unit_try_and_get(unit_index);

	return unit &&
		unit->definition_index == definition_index &&
		unit->unit.player_index == NONE &&
		unit->unit.actor_index == NONE &&
		!TEST_FLAG(unit->object.damage_flags, _object_dead_bit) &&
		companion_is_red(unit);
}

static long companion_find(
	long definition_index)
{
	struct object_iterator iterator;
	struct biped_datum *biped;

	object_iterator_new(&iterator, _object_mask_biped, 0);
	while ((biped = (struct biped_datum *)object_iterator_next(&iterator)) != NULL)
	{
		if (companion_is_alive(iterator.index, definition_index))
			return iterator.index;
	}

	return NONE;
}

/* the same red as players_coop_color_unit paints (the campaign Spartan's tag
fixes its colour, which overrides the colour a unit is created with) */
static void companion_paint(
	long unit_index)
{
	struct object_datum *object = object_get(unit_index);
	struct object_definition *definition = object_definition_get(object->definition_index);
	short index;

	for (index = 0; index < definition->object.change_colors.count && index < NUMBER_OF_OBJECT_CHANGE_COLORS; index++)
	{
		struct object_change_color_definition *change_color = TAG_BLOCK_GET_ELEMENT(
			&definition->object.change_colors, index, struct object_change_color_definition);

		if (change_color->permutations.count > 0 && !change_color->scaled_by && !change_color->darken_by)
		{
			object->object.base_change_colors[index] = *global_real_rgb_red;
			object->object.outgoing_change_colors[index] = *global_real_rgb_red;
		}
	}
}

static void companion_arm(
	long unit_index,
	long player_unit_index)
{
	short index;

	for (index = 0; index < 4; index++)
	{
		long player_weapon_index = unit_inventory_get_weapon(player_unit_index, index);
		struct object_placement_data placement_data;
		long weapon_index;

		if (player_weapon_index == NONE)
			continue;

		object_placement_data_new(&placement_data, object_get(player_weapon_index)->definition_index, unit_index);
		weapon_index = object_new(&placement_data);
		if (weapon_index != NONE && !unit_add_weapon_to_inventory(unit_index, weapon_index, TRUE))
			object_delete(weapon_index);
	}
}

static long companion_spawn(
	long player_unit_index)
{
	struct unit_datum *player_unit = unit_get(player_unit_index);
	struct object_placement_data placement_data;
	real_point3d player_origin;
	long unit_index;

	object_get_origin(player_unit_index, &player_origin);
	object_placement_data_new(&placement_data, player_unit->definition_index, NONE);
	/* (a spot beside the player on his floor, with room to stand, as co-op's
	extra enemies are placed) */
	if (!coop_enemies_spread_position(&player_origin, 1, NULL, &placement_data.position))
		return NONE;
	placement_data.forward = player_unit->object.forward;
	placement_data.up = *global_up3d;
	unit_index = object_new(&placement_data);
	if (unit_index == NONE)
		return NONE;

	companion_paint(unit_index);
	unit_get(unit_index)->object.owner_team_index = player_unit->object.owner_team_index;
	companion_arm(unit_index, player_unit_index);
	csmemcpy(unit_get(unit_index)->unit.grenade_counts, player_unit->unit.grenade_counts,
		sizeof(player_unit->unit.grenade_counts));
	unit_set_actively_controlled(unit_index, TRUE);

	csmemset(&companion_body, 0, sizeof(companion_body));
	companion_body.order = _companion_order_follow;
	companion_body.target_unit_index = NONE;
	companion_body.aim = player_unit->object.forward;
	companion_body.strafe_sign = 1.f;
	companion_body.pace = 1.f;
	companion_body.player_moving_since = NONE;
	companion_body.act_target_unit_index = NONE;
	companion_body.goal_object_index = NONE;

	platform_log("Gem is here");
	return unit_index;
}

static void companion_catch_up(
	long unit_index,
	long player_unit_index)
{
	real_point3d origin;
	real_point3d player_origin;

	if (companion_body.order == _companion_order_hold || unit_get(unit_index)->object.parent_object_index != NONE)
		return;

	object_get_origin(unit_index, &origin);
	object_get_origin(player_unit_index, &player_origin);
	if (distance3d(&origin, &player_origin) > COMPANION_TELEPORT_DISTANCE &&
		coop_enemies_spread_position(&player_origin, 1, NULL, &origin))
	{
		object_set_position(unit_index, &origin, NULL, NULL);
		companion_body.path_valid = FALSE;
	}
}

/* ---------- seeing */

static boolean companion_line_clear(
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

/* whether he can see an object: a line from his eyes to just short of it (a
vehicle blocks sight, and so would block the line to its own middle) */
static boolean companion_can_see(
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
	return companion_line_clear(eye, &end, ignore_object_index);
}

/* (a team of the player's enemies: a unit of no team, like the lifepod's pilot,
counts as everyone's enemy to game_team_is_enemy, and isn't) */
static boolean companion_enemy_team(
	short player_team_index,
	short team_index)
{
	return VALID_INDEX(team_index, NUMBER_OF_SOLO_CAMPAIGN_TEAMS) && team_index != _game_team_default &&
		game_team_is_enemy(player_team_index, team_index);
}

static boolean companion_unit_alive(
	long unit_index)
{
	struct unit_datum *unit = unit_index == NONE ? NULL : unit_try_and_get(unit_index);

	return unit && !TEST_FLAG(unit->object.damage_flags, _object_dead_bit);
}

/* the player's living enemies near Gem, nearest first, and whether he can
see each */
static void companion_look_around(
	long unit_index,
	long player_unit_index)
{
	struct unit_datum *player_unit = unit_get(player_unit_index);
	struct object_iterator iterator;
	struct unit_datum *unit;
	real_point3d eye;
	real_point3d origin;

	unit_get_camera_position(unit_index, &eye);
	object_get_origin(unit_index, &origin);
	companion_enemy_count = 0;
	object_iterator_new(&iterator, _object_mask_unit, 0);
	while ((unit = (struct unit_datum *)object_iterator_next(&iterator)) != NULL)
	{
		struct companion_enemy enemy;
		real_point3d enemy_origin;
		short index;

		/* (a crew is its enemy vehicle: a banshee, not its pilot as well) */
		if (TEST_FLAG(unit->object.damage_flags, _object_dead_bit) ||
			!companion_enemy_team(player_unit->object.owner_team_index, unit->object.owner_team_index) ||
			(unit->object.parent_object_index != NONE &&
				companion_enemy_team(player_unit->object.owner_team_index,
					object_get(unit->object.parent_object_index)->object.owner_team_index)))
		{
			continue;
		}
		object_get_origin(iterator.index, &enemy_origin);
		enemy.distance = distance3d(&origin, &enemy_origin);
		if (enemy.distance > COMPANION_ENEMY_DISTANCE)
			continue;

		enemy.unit_index = iterator.index;
		enemy.aim_point = unit->object.bounding_sphere_center;
		enemy.visible = companion_can_see(&eye, iterator.index, unit_index);

		/* (kept nearest first; the farthest dropped when full) */
		for (index = companion_enemy_count; index > 0 && companion_enemies[index - 1].distance > enemy.distance; index--)
		{
			if (index < COMPANION_MAXIMUM_ENEMIES)
				companion_enemies[index] = companion_enemies[index - 1];
		}
		if (index < COMPANION_MAXIMUM_ENEMIES)
		{
			companion_enemies[index] = enemy;
			if (companion_enemy_count < COMPANION_MAXIMUM_ENEMIES)
				companion_enemy_count++;
		}
	}
}

/* the things of a kind near him, nearest first: weapons lying on the ground,
or vehicles he isn't in */
static short companion_look_for(
	long unit_index,
	unsigned long mask,
	real range,
	struct companion_thing *things)
{
	struct object_iterator iterator;
	struct object_datum *object;
	real_point3d origin;
	short count = 0;

	object_get_origin(unit_index, &origin);
	object_iterator_new(&iterator, mask, 0);
	while ((object = object_iterator_next(&iterator)) != NULL)
	{
		struct companion_thing thing;
		short index;

		if (object->object.parent_object_index != NONE ||
			iterator.index == unit_get(unit_index)->object.parent_object_index ||
			TEST_FLAG(object->object.damage_flags, _object_dead_bit))
		{
			continue;
		}
		thing.object_index = iterator.index;
		thing.distance = distance3d(&origin, &object->object.bounding_sphere_center);
		if (thing.distance > range)
			continue;
		for (index = count; index > 0 && things[index - 1].distance > thing.distance; index--)
		{
			if (index < COMPANION_MAXIMUM_THINGS)
				things[index] = things[index - 1];
		}
		if (index < COMPANION_MAXIMUM_THINGS)
		{
			things[index] = thing;
			if (count < COMPANION_MAXIMUM_THINGS)
				count++;
		}
	}
	return count;
}

static struct companion_enemy *companion_enemy_get(
	long unit_index)
{
	short index;

	for (index = 0; index < companion_enemy_count; index++)
	{
		if (companion_enemies[index].unit_index == unit_index)
			return &companion_enemies[index];
	}
	return NULL;
}

/* his target: mate's choice, while it lives and he knows of it (an enemy,
or the Chief); once it falls, while he's fighting, the nearest enemy he sees */
static struct companion_enemy *companion_current_target(
	void)
{
	static struct companion_enemy chief;
	long player_unit_index = companion_player_unit();
	real_point3d eye;

	if (companion_body.hold_fire)
		return NULL;
	if (companion_body.target_unit_index == NONE || companion_body.target_unit_index != player_unit_index)
	{
		struct companion_enemy *enemy = companion_enemy_get(companion_body.target_unit_index);
		short index;

		for (index = 0; !enemy && companion_body.engaging && index < companion_enemy_count; index++)
		{
			if (companion_enemies[index].visible)
			{
				enemy = &companion_enemies[index];
				companion_body.target_unit_index = enemy->unit_index;
				companion_body.target_time = game_time_get();
			}
		}
		return enemy;
	}
	if (!companion_unit_alive(player_unit_index))
		return NULL;
	unit_get_camera_position(companion_unit_index, &eye);
	chief.unit_index = player_unit_index;
	chief.aim_point = unit_get(player_unit_index)->object.bounding_sphere_center;
	chief.distance = distance3d(&eye, &chief.aim_point);
	chief.visible = companion_can_see(&eye, player_unit_index, companion_unit_index);
	return &chief;
}

/* ---------- moving */

/* the pathfinding surface under a point, walking the ground there in a straight
line from a biped's own; NONE if the walk stops short of it (a rock, a wall, a
ledge in the way), not the surface where it stopped */
static long companion_surface_from(
	long biped_index,
	real_point3d const *point)
{
	real_point3d start;
	long start_surface_index = biped_find_pathfinding_surface_index(biped_index, &start);
	real_point2d known;
	real_point2d target;
	long surface_index;

	if (start_surface_index == NONE)
		return NONE;
	known.x = start.x;
	known.y = start.y;
	target.x = point->x;
	target.y = point->y;
	surface_index = structure_surface_index_from_point(global_structure_bsp_get(), FALSE, &known,
		start_surface_index, &target);
	if ((target.x - point->x) * (target.x - point->x) + (target.y - point->y) * (target.y - point->y) > 0.3f * 0.3f)
		return NONE;
	return surface_index;
}

/* the pathfinding surface of a spot he goes to: walked to from the Chief's
(his goals are mostly near the Chief: beside him, his own spot near him), else
from his own; the way there is the path search's, around what's between */
static long companion_goal_surface(
	long unit_index,
	real_point3d const *point)
{
	long player_unit_index = companion_player_unit();
	long surface_index = companion_unit_alive(player_unit_index) ? companion_surface_from(player_unit_index, point) : NONE;

	return surface_index != NONE ? surface_index : companion_surface_from(unit_index, point);
}

static void companion_find_path(
	long unit_index,
	long goal_surface_index)
{
	struct path_input input;
	real_point3d start;
	long start_surface_index = biped_find_pathfinding_surface_index(unit_index, &start);

	companion_body.path_valid = FALSE;
	companion_body.path_time = game_time_get();
	if (start_surface_index == NONE || goal_surface_index == NONE)
		return;

	path_input_new(&input, COMPANION_PATHFINDING_RADIUS, FALSE, unit_index);
	path_input_set_start(&input, &start, start_surface_index);
	path_state_new(&input, &companion_path_state, NULL);
	path_state_destination(&companion_path_state, &companion_body.goal, goal_surface_index, 0.5f);
	if (!path_state_find(&companion_path_state))
		return;
	path_obstacle_search_reach = COMPANION_OBSTACLE_REACH;
	companion_body.path_valid = path_state_build_path(&companion_path_state, &companion_body.path);
	path_obstacle_search_reach = 0.f;
}

/* a spot near him the enemy can't see, standing: the nearest of rings
around him, with ground under it and the way there open */
static boolean companion_find_cover(
	long unit_index,
	real_point3d const *threat,
	real_point3d *cover)
{
	real_point3d origin;
	real radius;

	object_get_origin(unit_index, &origin);
	for (radius = 2.5f; radius <= COMPANION_COVER_SEARCH; radius += 2.5f)
	{
		short place;

		for (place = 0; place < 12; place++)
		{
			real angle = (real)place * (_pi * 2.f / 12.f);
			real_point3d spot = origin;
			real_point3d chest;
			real_vector3d down = { 0.f, 0.f, -3.f };
			struct collision_result collision;

			spot.x += (real)cos(angle) * radius;
			spot.y += (real)sin(angle) * radius;
			spot.z += 1.f;
			if (!companion_line_clear(&origin, &spot, unit_index) ||
				!collision_test_vector(FLAG(_collision_test_structure_bit) | FLAG(_collision_test_front_facing_surfaces_bit),
					&spot, &down, NONE, &collision))
			{
				continue;
			}
			chest = collision.point;
			chest.z += 1.2f;
			if (!companion_line_clear(threat, &chest, unit_index))
			{
				*cover = collision.point;
				return TRUE;
			}
		}
	}
	return FALSE;
}

static real companion_random01(
	void)
{
	return (companion_random() + 1.f) * 0.5f;
}

/* how far apart two points are on the ground (heights differ on slopes) */
static real companion_flat_distance(
	real_point3d const *a,
	real_point3d const *b)
{
	real dx = a->x - b->x;
	real dy = a->y - b->y;

	return (real)sqrt(dx * dx + dy * dy);
}

/* the player's view: its camera as main.c makes the player's window's
(set_window_camera_values: smooth motion, the camera's shake, display.fov)
and its projection, on the screen as drawn (render.camera's bounds, the
whole screen's, as the player's window's are with one player); FALSE
without a player */
static boolean companion_view_camera(
	struct render_camera *camera,
	struct render_frustum *frustum)
{
	struct render_window window;
	short local_player_index = local_player_get_next(NONE);
	struct observer_result const *observer;

	if (local_player_index == NONE)
		return FALSE;
	observer = render_interpolation_camera(local_player_index, observer_get_camera(local_player_index));
	if (!observer)
		return FALSE;
	memset(&window, 0, sizeof(window));
	window.local_player_index = local_player_index;
	window.rasterizer_camera.viewport_bounds = render.camera.viewport_bounds;
	window.rasterizer_camera.window_bounds = render.camera.window_bounds;
	set_window_camera_values(&window, observer);
	*camera = window.rasterizer_camera;
	render_camera_build_frustum(camera, NULL, frustum, TRUE);
	return TRUE;
}

/* the way into a vehicle: the entrance of the nearest seat he can take now
(the game's own, unit_get_seat_entrance_point); FALSE if none is free */
static boolean companion_seat_entrance(
	long unit_index,
	long vehicle_index,
	real_point3d *entrance)
{
	struct unit_definition *definition = unit_definition_get(unit_get(vehicle_index)->definition_index);
	real_point3d origin;
	real best = REAL_MAX;
	short seat_index;

	object_get_origin(unit_index, &origin);
	for (seat_index = 0; seat_index < definition->unit.seats.count; seat_index++)
	{
		real_point3d point;
		real_point3d seat;
		long occupant_index = NONE;

		if (unit_get_seat_entrance_point(unit_index, vehicle_index, seat_index, &point, &seat, NULL) &&
			unit_can_enter_seat(unit_index, vehicle_index, seat_index, &occupant_index) &&
			distance3d(&origin, &point) < best)
		{
			best = distance3d(&origin, &point);
			*entrance = point;
		}
	}
	return best < REAL_MAX;
}

/* the ground at a point of the player's view (-1 to 1 across, and down, as
its projection has it): where a line from the camera through it meets the
level or an object, or, a wall or an object, the ground a step short of it;
FALSE if it meets nothing within reach, or there's no ground */
static boolean companion_view_ground(
	struct render_camera const *camera,
	struct render_frustum const *frustum,
	real x,
	real y,
	real_point3d *ground)
{
	real_point2d screen;
	real_point3d from;
	real_vector3d along;
	real_vector3d down = { 0.f, 0.f, -4.f };
	real_point3d above;
	struct collision_result collision;

	screen.x = x;
	screen.y = y;
	render_camera_screen_to_world(camera, frustum, &screen, &from, &along);
	normalize3d(&along);
	scale_vector3d(&along, COMPANION_VIEW_REACH, &along);
	if (!collision_test_vector(FLAG(_collision_test_structure_bit) | FLAG(_collision_test_front_facing_surfaces_bit) |
			FLAG(_collision_test_objects_bit) | _collision_test_objects_sight_blocking_flags,
		&from, &along, companion_player_unit(), &collision))
	{
		return FALSE;
	}
	if (collision.type == _collision_result_structure && collision.plane.n.k >= 0.6f)
	{
		*ground = collision.point;
		return TRUE;
	}
	normalize3d(&along);
	point_from_line3d(&collision.point, &along, -0.6f, &above);
	above.z += 0.5f;
	if (!collision_test_vector(FLAG(_collision_test_structure_bit) | FLAG(_collision_test_front_facing_surfaces_bit),
			&above, &down, NONE, &collision) ||
		collision.plane.n.k < 0.6f)
	{
		return FALSE;
	}
	*ground = collision.point;
	return TRUE;
}

/* whether he can find a way to a point (the search is his path's: his path is
found again after) */
static boolean companion_reachable(
	long unit_index,
	real_point3d const *point)
{
	struct path_input input;
	struct path_result result;
	real_point3d start;
	real_point3d goal = *point;
	long start_surface_index = biped_find_pathfinding_surface_index(unit_index, &start);
	long goal_surface_index = companion_goal_surface(unit_index, &goal);
	boolean reachable;

	companion_body.goal_valid = FALSE;
	if (start_surface_index == NONE || goal_surface_index == NONE)
		return FALSE;
	path_input_new(&input, COMPANION_PATHFINDING_RADIUS, FALSE, unit_index);
	path_input_set_start(&input, &start, start_surface_index);
	path_state_new(&input, &companion_path_state, NULL);
	path_state_destination(&companion_path_state, &goal, goal_surface_index, 0.5f);
	if (!path_state_find(&companion_path_state))
		return FALSE;
	path_obstacle_search_reach = COMPANION_OBSTACLE_REACH;
	reachable = path_state_build_path(&companion_path_state, &result);
	path_obstacle_search_reach = 0.f;
	return reachable;
}

/* a spot near the Chief to be in while he's still: on his side of the Chief,
on the ground, in the Chief's sight, one he can get to with no object (a wreck,
a vehicle) in the way, which the level's pathfinding doesn't know of; his own
place, if none */
static void companion_pick_idle_spot(
	long unit_index,
	real_point3d const *player_origin)
{
	real_point3d origin;
	real side;
	short attempt;

	object_get_origin(unit_index, &origin);
	companion_body.idle_spot = origin;
	side = (real)atan2(origin.y - player_origin->y, origin.x - player_origin->x);
	for (attempt = 0; attempt < 6; attempt++)
	{
		real angle = side + companion_random() * 1.2f;
		real radius = COMPANION_IDLE_NEAR + companion_random01() * (COMPANION_IDLE_FAR - COMPANION_IDLE_NEAR);
		real_point3d spot = *player_origin;
		real_point3d from = *player_origin;
		real_point3d from_him = origin;
		real_point3d to;
		struct collision_result collision;

		spot.x += (real)cos(angle) * radius;
		spot.y += (real)sin(angle) * radius;
		from.z += 0.5f;
		to = spot;
		to.z += 0.5f;
		from_him.z += 0.5f;
		if (companion_line_clear(&from, &to, unit_index) &&
			!collision_test_line(FLAG(_collision_test_objects_bit) | _collision_test_objects_sight_blocking_flags,
				&from_him, &to, unit_index, &collision) &&
			companion_reachable(unit_index, &spot))
		{
			companion_body.idle_spot = spot;
			break;
		}
	}
	companion_body.idle_valid = TRUE;
	companion_body.idle_nearest = 1000.f;
	companion_body.idle_nearer_time = game_time_get();
	companion_body.idle_until = game_time_get() + (long)((6.f + 9.f * companion_random01()) * TICKS_PER_SECOND);
}

/* following the Chief, as a mate does: while he's on the move (a moment after
he sets off), beside and a little behind him on the side he's already on, at
his pace and faster to catch up; while he's still, a spot of his own near him,
walked to, another now and then (sometimes close by, sometimes a few metres
off), never right on top of him */
static boolean companion_follow_goal(
	long unit_index,
	long player_unit_index,
	real_point3d *goal)
{
	real_vector3d velocity = object_get(player_unit_index)->object.translational_velocity;
	real_point3d origin;
	real_point3d player_origin;
	real speed;
	real distance;

	object_get_origin(unit_index, &origin);
	object_get_origin(player_unit_index, &player_origin);
	velocity.k = 0.f;
	speed = magnitude3d(&velocity);
	if (speed < COMPANION_MOVING_SPEED)
		companion_body.player_moving_since = NONE;
	else if (companion_body.player_moving_since == NONE)
		companion_body.player_moving_since = game_time_get();

	if (companion_body.player_moving_since != NONE &&
		game_time_get() - companion_body.player_moving_since >= COMPANION_FOLLOW_REACTION_TICKS)
	{
		real_vector3d forward = velocity;
		real_vector3d left;

		normalize3d(&forward);
		cross_product3d(global_up3d, &forward, &left);
		if (companion_body.idle_valid)
		{
			/* (setting off: he keeps to the side he's on) */
			companion_body.follow_side = (origin.x - player_origin.x) * left.i + (origin.y - player_origin.y) * left.j > 0.f ? 1.f : -1.f;
			companion_body.idle_valid = FALSE;
		}
		goal->x = player_origin.x - forward.i * COMPANION_FOLLOW_BEHIND + left.i * COMPANION_FOLLOW_SIDE * companion_body.follow_side;
		goal->y = player_origin.y - forward.j * COMPANION_FOLLOW_BEHIND + left.j * COMPANION_FOLLOW_SIDE * companion_body.follow_side;
		goal->z = player_origin.z;
		distance = companion_flat_distance(&origin, goal);
		companion_body.pace = PIN(speed / COMPANION_RUN_SPEED + distance * 0.8f, 0.3f, 1.f);
		return distance > 0.15f;
	}

	/* (a way off: over to him first) */
	if (companion_flat_distance(&origin, &player_origin) > COMPANION_IDLE_FAR * 2.f)
	{
		*goal = player_origin;
		companion_body.pace = 1.f;
		companion_body.idle_valid = FALSE;
		return TRUE;
	}
	if (!companion_body.idle_valid || game_time_get() >= companion_body.idle_until ||
		companion_flat_distance(&companion_body.idle_spot, &player_origin) > COMPANION_IDLE_FAR + 0.3f)
	{
		companion_pick_idle_spot(unit_index, &player_origin);
	}
	*goal = companion_body.idle_spot;
	distance = companion_flat_distance(&origin, goal);
	/* (stuck on the way: he stays where he got to) */
	if (distance < companion_body.idle_nearest - 0.1f)
	{
		companion_body.idle_nearest = distance;
		companion_body.idle_nearer_time = game_time_get();
	}
	else if (distance > 0.2f && game_time_get() - companion_body.idle_nearer_time > 2 * TICKS_PER_SECOND)
	{
		companion_body.idle_spot = origin;
		*goal = origin;
		distance = 0.f;
	}
	/* (strolling over, unless it's a way off) */
	companion_body.pace = distance > 1.5f ? 1.f : 0.45f;
	return distance > 0.2f;
}

/* where the order takes him now (FALSE: nowhere, he stands) */
static boolean companion_goal(
	long unit_index,
	long player_unit_index,
	struct companion_enemy const *target,
	real_point3d *goal)
{
	real_point3d origin;
	real_point3d player_origin;
	real_vector3d away;
	real distance;

	object_get_origin(unit_index, &origin);
	object_get_origin(player_unit_index, &player_origin);
	companion_body.pace = 1.f;
	switch (companion_body.order)
	{
	case _companion_order_hold:
		*goal = companion_body.hold_point;
		return distance3d(&origin, goal) > 1.5f;

	case _companion_order_pick_up:
		if (!object_try_and_get_and_verify_type(companion_body.goal_object_index, _object_mask_all))
			break;
		*goal = object_get(companion_body.goal_object_index)->object.bounding_sphere_center;
		return distance3d(&origin, goal) > COMPANION_REACH;

	case _companion_order_get_in:
		/* (to a free seat's entrance: its middle is inside it) */
		if (!object_try_and_get_and_verify_type(companion_body.goal_object_index, _object_mask_all) ||
			!companion_seat_entrance(unit_index, companion_body.goal_object_index, goal))
		{
			break;
		}
		return distance3d(&origin, goal) > COMPANION_REACH;

	case _companion_order_attack:
		if (!target)
			break;
		/* (into range, out of too close, or across the target's view) */
		if (target->distance > COMPANION_FIGHT_FAR || target->distance < COMPANION_FIGHT_NEAR || !target->visible)
		{
			vector_from_points3d(&target->aim_point, &origin, &away);
			away.k = 0.f;
			normalize3d(&away);
			distance = target->visible
				? PIN(target->distance, COMPANION_FIGHT_NEAR + 1.f, COMPANION_FIGHT_FAR - 2.f)
				: COMPANION_FIGHT_NEAR;
			goal->x = target->aim_point.x + away.i * distance;
			goal->y = target->aim_point.y + away.j * distance;
			goal->z = origin.z;
			return TRUE;
		}
		return FALSE;

	case _companion_order_advance:
		if (!target)
			break;
		*goal = target->aim_point;
		return target->distance > COMPANION_ADVANCE_DISTANCE;

	case _companion_order_cover:
		if (target && target->visible)
		{
			if (companion_find_cover(unit_index, &target->aim_point, goal))
				return TRUE;
		}
		else
		{
			return FALSE;
		}
		/* (no cover: back off) */
		/* fall through */
	case _companion_order_retreat:
		if (!target)
			break;
		vector_from_points3d(&target->aim_point, &origin, &away);
		away.k = 0.f;
		normalize3d(&away);
		goal->x = origin.x + away.i * COMPANION_RETREAT_DISTANCE;
		goal->y = origin.y + away.j * COMPANION_RETREAT_DISTANCE;
		goal->z = origin.z;
		return TRUE;
	}

	/* follow, or an order with nobody to fight */
	return companion_follow_goal(unit_index, player_unit_index, goal);
}

/* the direction to walk this tick (zero: stand), along his path */
static void companion_walk_direction(
	long unit_index,
	long player_unit_index,
	struct companion_enemy const *target,
	real_vector3d *direction)
{
	real_point3d origin;
	real_point3d goal;

	*direction = *global_zero_vector3d;
	object_get_origin(unit_index, &origin);
	if (!companion_goal(unit_index, player_unit_index, target, &goal))
	{
		companion_body.goal_valid = FALSE;
		companion_body.path_valid = FALSE;
		return;
	}

	if (!companion_body.goal_valid ||
		distance3d(&goal, &companion_body.goal) > 2.f ||
		game_time_get() - companion_body.path_time >= COMPANION_PATH_TICKS)
	{
		long goal_surface_index;

		companion_body.goal = goal;
		companion_body.goal_valid = TRUE;
		goal_surface_index = companion_goal_surface(unit_index, &companion_body.goal);
		companion_find_path(unit_index, goal_surface_index);
	}

	if (companion_body.path_valid)
	{
		struct path_result *path = &companion_body.path;

		/* (steps reached are passed) */
		while (path->step_index < path->step_count - 1)
		{
			real_vector3d to_step;

			vector_from_points3d(&origin, &path->steps[path->step_index].point, &to_step);
			to_step.k = 0.f;
			if (magnitude3d(&to_step) > 0.6f)
				break;
			path->step_index++;
		}
		vector_from_points3d(&origin, &path->steps[path->step_index].point, direction);
	}
	else
	{
		/* (no path: straight there) */
		vector_from_points3d(&origin, &companion_body.goal, direction);
	}
	direction->k = 0.f;
	if (magnitude3d(direction) < 0.05f)
	{
		*direction = *global_zero_vector3d;
	}
	else
	{
		normalize3d(direction);
		scale_vector3d(direction, companion_body.pace, direction);
	}
}

/* ---------- the body */

static void companion_send_event(char const *text);
static void companion_text(wchar_t const *text, char *out, int size);

/* at the weapon he walked to, he takes it (the one in his hands dropped when
both his hands are full, as a player's swap); at the vehicle, he gets in */
static void companion_reach(
	long unit_index)
{
	struct object_datum *object;
	real_point3d origin;
	char text[96];

	if (companion_body.order != _companion_order_pick_up && companion_body.order != _companion_order_get_in)
		return;
	object = object_try_and_get_and_verify_type(companion_body.goal_object_index, _object_mask_all);
	object_get_origin(unit_index, &origin);
	if (object && companion_body.order == _companion_order_get_in)
	{
		short seat_index = NONE;
		real_point3d entrance;

		/* (at a seat's entrance, as the player gets in; on his way to one
		while one is free) */
		if (unit_find_nearby_seat(unit_index, companion_body.goal_object_index, &seat_index) == _unit_nearby_seat_available &&
			unit_enter_seat(unit_index, companion_body.goal_object_index, seat_index))
		{
			snprintf(text, sizeof(text), "Gem got in the %s.", tag_name_strip_path(tag_get_name(object->definition_index)));
			companion_send_event(text);
		}
		else if (companion_seat_entrance(unit_index, companion_body.goal_object_index, &entrance) &&
			distance3d(&origin, &entrance) > COMPANION_REACH)
		{
			return;
		}
		companion_body.order = _companion_order_follow;
		companion_body.goal_valid = FALSE;
		return;
	}
	if (object && distance3d(&origin, &object->object.bounding_sphere_center) > COMPANION_REACH)
		return;
	if (object && companion_body.order == _companion_order_pick_up)
	{
		if (unit_inventory_get_weapon(unit_index, 1) != NONE)
			unit_drop_current_weapon(unit_index, TRUE);
		if (unit_add_weapon_to_inventory(unit_index, companion_body.goal_object_index, _unit_add_weapon_normal))
		{
			snprintf(text, sizeof(text), "Gem picked up a %s.", tag_name_strip_path(tag_get_name(object->definition_index)));
			companion_send_event(text);
		}
	}
	companion_body.order = _companion_order_follow;
	companion_body.goal_valid = FALSE;
}

static void companion_control(
	long unit_index,
	long player_unit_index)
{
	struct unit_datum *unit = unit_get(unit_index);
	struct companion_enemy *target = companion_current_target();
	struct companion_enemy *act_target = companion_enemy_get(companion_body.act_target_unit_index);
	long act_ticks = game_time_get() - companion_body.act_time;
	long held_weapon_index = unit_inventory_get_weapon(unit_index, unit->unit.current_weapon_index);
	short grenade_index = NONE;
	short weapon_choice = NONE;
	real turn;
	struct unit_control_data control;
	real_vector3d walk;
	real_vector3d desired;
	real_vector3d facing;
	real_vector3d left;
	unsigned long flags = 0;
	long weapon_index;

	/* (his own, as any soldier's: a magazine run dry is reloaded, if there's
	ammo for it) */
	if (companion_body.act == _companion_act_none && held_weapon_index != NONE)
	{
		struct weapon_datum *held = weapon_get(held_weapon_index);

		if (weapon_definition_get(held->definition_index)->weapon.magazines.count > 0 &&
			held->weapon.magazines[0].rounds_loaded == 0 && held->weapon.magazines[0].rounds_total > 0)
		{
			companion_body.act = _companion_act_reload;
			companion_body.act_target_unit_index = NONE;
			companion_body.act_time = game_time_get();
			act_ticks = 0;
		}
	}

	if (unit->object.parent_object_index != NONE)
		walk = *global_zero_vector3d;
	else
		companion_walk_direction(unit_index, player_unit_index, target, &walk);
	companion_reach(unit_index);

	/* fighting: strafe across the target's view at fighting range */
	if (target && target->visible && companion_body.order == _companion_order_attack &&
		magnitude3d(&walk) < 0.5f)
	{
		real_vector3d to_target;

		if (game_time_get() - companion_body.strafe_time > COMPANION_STRAFE_TICKS)
		{
			companion_body.strafe_time = game_time_get();
			companion_body.strafe_sign = companion_random() > 0.f ? 1.f : -1.f;
		}
		vector_from_points3d(&unit->object.position, &target->aim_point, &to_target);
		to_target.k = 0.f;
		normalize3d(&to_target);
		cross_product3d(global_up3d, &to_target, &walk);
		scale_vector3d(&walk, companion_body.strafe_sign * 0.6f, &walk);
	}

	/* (a grenade or a punch goes at its own enemy) */
	if ((companion_body.act == _companion_act_grenade || companion_body.act == _companion_act_melee) && act_target)
		target = act_target;

	/* the aim lock: towards the target, a share of the way each tick, a little off */
	if (target && target->visible)
	{
		real_point3d eye;

		/* (a little off, more so the farther the target) */
		real error = COMPANION_AIM_ERROR * (1.f + target->distance / 8.f);

		unit_get_camera_position(unit_index, &eye);
		vector_from_points3d(&eye, &target->aim_point, &desired);
		normalize3d(&desired);
		desired.i += companion_random() * error;
		desired.j += companion_random() * error;
		desired.k += companion_random() * error;
		normalize3d(&desired);
	}
	else if (mate_link_voice_playing() && companion_unit_alive(player_unit_index))
	{
		/* (talking: he looks at the Chief) */
		real_point3d eye;
		real_point3d head;

		unit_get_camera_position(unit_index, &eye);
		unit_get_camera_position(player_unit_index, &head);
		vector_from_points3d(&eye, &head, &desired);
		if (magnitude3d(&desired) < 0.01f)
			desired = companion_body.aim;
		normalize3d(&desired);
	}
	else if (magnitude3d(&walk) > 0.f)
	{
		desired = walk;
		normalize3d(&desired);
	}
	else
	{
		/* (standing about: he looks around, and at the Chief now and then) */
		if (game_time_get() >= companion_body.look_until)
		{
			real yaw = (real)atan2(companion_body.aim.j, companion_body.aim.i) + companion_random() * 1.2f;
			real pitch = companion_random() * 0.15f;

			companion_body.look_at_player = companion_random01() < 0.35f;
			companion_body.look.i = (real)(cos(yaw) * cos(pitch));
			companion_body.look.j = (real)(sin(yaw) * cos(pitch));
			companion_body.look.k = (real)sin(pitch);
			companion_body.look_until = game_time_get() + (long)((2.f + 4.f * companion_random01()) * TICKS_PER_SECOND);
		}
		desired = companion_body.look;
		if (companion_body.look_at_player && companion_unit_alive(player_unit_index))
		{
			real_point3d eye;
			real_point3d head;

			unit_get_camera_position(unit_index, &eye);
			unit_get_camera_position(player_unit_index, &head);
			vector_from_points3d(&eye, &head, &desired);
			if (magnitude3d(&desired) < 0.01f)
				desired = companion_body.aim;
			normalize3d(&desired);
		}
	}
	turn = target && target->visible ? COMPANION_AIM_TURN : COMPANION_LOOK_TURN;
	companion_body.aim.i += (desired.i - companion_body.aim.i) * turn;
	companion_body.aim.j += (desired.j - companion_body.aim.j) * turn;
	companion_body.aim.k += (desired.k - companion_body.aim.k) * turn;
	if (magnitude3d(&companion_body.aim) < 0.01f)
		companion_body.aim = unit->object.forward;
	normalize3d(&companion_body.aim);

	/* (he faces where he aims, level; he walks in that frame) */
	facing = companion_body.aim;
	facing.k = 0.f;
	if (magnitude3d(&facing) < 0.01f)
		facing = unit->object.forward;
	normalize3d(&facing);
	cross_product3d(global_up3d, &facing, &left);

	/* the one-off action mate chose: a press, or a grenade's throw held */
	switch (companion_body.act)
	{
	case _companion_act_reload: SET_FLAG(flags, _unit_control_weapon_reload_bit, act_ticks == 0); break;
	case _companion_act_melee: SET_FLAG(flags, _unit_control_use_equipment_bit, act_ticks == 0); break;
	case _companion_act_jump: SET_FLAG(flags, _unit_control_jump_bit, act_ticks == 0); break;
	case _companion_act_light: SET_FLAG(flags, _unit_control_integrated_light_bit, act_ticks == 0); break;
	case _companion_act_get_out: SET_FLAG(flags, _unit_control_action_bit, act_ticks == 0); break;
	case _companion_act_swap:
		/* (the other weapon, as the one he wants: unit_control's weapon index) */
		if (act_ticks == 0 && unit_inventory_get_weapon(unit_index, unit->unit.current_weapon_index ? 0 : 1) != NONE)
			weapon_choice = unit->unit.current_weapon_index ? 0 : 1;
		break;
	case _companion_act_grenade:
		if (unit->unit.grenade_counts[companion_body.act_grenade_type])
			grenade_index = companion_body.act_grenade_type;
		SET_FLAG(flags, _unit_control_throw_grenade_bit, grenade_index != NONE && act_ticks < COMPANION_GRENADE_TICKS);
		break;
	}
	if (act_ticks >= COMPANION_GRENADE_TICKS)
		companion_body.act = _companion_act_none;

	/* fire in bursts at mate's target while he sees it and has his aim on it */
	weapon_index = unit_inventory_get_weapon(unit_index, unit->unit.current_weapon_index);
	if (weapon_index != NONE && companion_body.act == _companion_act_none)
	{
		if (target && target->visible &&
			game_time_get() - companion_body.target_time >= COMPANION_REACTION_TICKS &&
			dot_product3d(&companion_body.aim, &desired) > COMPANION_AIM_FIRE_COSINE)
		{
			long burst = (game_time_get() - companion_body.burst_time) % (COMPANION_BURST_ON_TICKS + COMPANION_BURST_OFF_TICKS);

			SET_FLAG(flags, _unit_control_weapon_primary_trigger_bit, burst < COMPANION_BURST_ON_TICKS);
		}
		else
		{
			companion_body.burst_time = game_time_get();
		}
	}

	/* crouching as mate said, and in cover once there */
	companion_body.crouch = companion_body.crouching ||
		(companion_body.order == _companion_order_cover && magnitude3d(&walk) == 0.f && target != NULL);
	SET_FLAG(flags, _unit_control_crouch_modifier_bit, companion_body.crouch);

	csmemset(&control, 0, sizeof(control));
	/* (as a player's unit always is: "alert" walks at walking speed, and can't strafe) */
	control.animation_state = _unit_animation_state_in_combat;
	control.aiming_speed = _unit_aiming_speed_alert;
	control.control_flags = (word)flags;
	control.weapon_index = weapon_choice;
	control.grenade_index = grenade_index;
	control.zoom_level = NONE;
	control.throttle.i = dot_product3d(&walk, &facing);
	control.throttle.j = dot_product3d(&walk, &left);
	control.throttle.k = 0.f;
	control.primary_trigger = TEST_FLAG(flags, _unit_control_weapon_primary_trigger_bit) ? 1.f : 0.f;
	control.facing_vector = facing;
	control.aiming_vector = companion_body.aim;
	control.looking_vector = companion_body.aim;
	unit_control(unit_index, &control);
}

/* ---------- mate */

static void companion_set_order(
	short order,
	long target_unit_index,
	boolean hold_fire)
{
	if (order == _companion_order_hold && companion_body.order != _companion_order_hold && companion_unit_index != NONE)
		object_get_origin(companion_unit_index, &companion_body.hold_point);
	if (order != companion_body.order)
		companion_body.goal_valid = FALSE;
	/* (what he decided, on the console in his red) */
	if (order != companion_body.order || target_unit_index != companion_body.target_unit_index ||
		hold_fire != companion_body.hold_fire)
	{
		static real_argb_color const red = { 1.f, 1.f, 0.45f, 0.4f };
		struct companion_enemy const *target = companion_enemy_get(target_unit_index);

		if (target)
		{
			terminal_printf(&red, "Gem: %s, %s %d m away", companion_order_names[order],
				tag_name_strip_path(tag_get_name(unit_get(target->unit_index)->definition_index)),
				(int)(target->distance * COMPANION_METRES_PER_UNIT));
		}
		else
		{
			terminal_printf(&red, "Gem: %s%s", companion_order_names[order], hold_fire ? ", holding fire" : "");
		}
	}
	if (target_unit_index != companion_body.target_unit_index)
		companion_body.target_time = game_time_get();
	companion_body.order = order;
	companion_body.target_unit_index = target_unit_index;
	companion_body.hold_fire = hold_fire;
	if (hold_fire)
		companion_body.engaging = FALSE;
}

/* the enemy, weapon or vehicle an option of companion_send_state names (NONE
if none) */
static long companion_unit_from_id(
	long id)
{
	short index;

	for (index = 0; index < companion_enemy_count; index++)
	{
		if ((companion_enemies[index].unit_index & 0xFFFF) == id)
			return companion_enemies[index].unit_index;
	}
	for (index = 0; index < companion_weapon_count; index++)
	{
		if ((companion_weapons[index].object_index & 0xFFFF) == id)
			return companion_weapons[index].object_index;
	}
	for (index = 0; index < companion_vehicle_count; index++)
	{
		if ((companion_vehicles[index].object_index & 0xFFFF) == id)
			return companion_vehicles[index].object_index;
	}
	return NONE;
}

/* mate's actions: what Gem does, as its decision model chose from the
actions companion_send_state offered */
static void companion_act(
	short act,
	char const *name,
	long target_unit_index)
{
	static real_argb_color const red = { 1.f, 1.f, 0.45f, 0.4f };

	companion_body.act = act;
	companion_body.act_target_unit_index = target_unit_index;
	companion_body.act_time = game_time_get();
	terminal_printf(&red, "Gem: %s", name);
}

static void companion_handle_messages(
	void)
{
	char message[2048];

	while (mate_link_next_message(message, sizeof(message)))
	{
		char const *option = strstr(message, "\"option\":\"");
		long target_unit_index = option ? companion_unit_from_id(atol(option + strlen("\"option\":\""))) : NONE;

		if (strstr(message, "\"type\":\"view\""))
		{
			companion_view_requested = TRUE;
			continue;
		}
		if (!strstr(message, "\"type\":\"action\""))
			continue;
		if (strstr(message, "\"action\":\"follow\""))
			companion_set_order(_companion_order_follow, companion_body.target_unit_index, companion_body.hold_fire);
		else if (strstr(message, "\"action\":\"stay\""))
			companion_set_order(_companion_order_hold, companion_body.target_unit_index, companion_body.hold_fire);
		else if (strstr(message, "\"action\":\"shoot\""))
		{
			companion_set_order(companion_body.order, target_unit_index, FALSE);
			companion_body.engaging = TRUE;
		}
		else if (strstr(message, "\"action\":\"hold_fire\""))
			companion_set_order(companion_body.order, NONE, TRUE);
		else if (strstr(message, "\"action\":\"rush\""))
		{
			companion_set_order(_companion_order_advance, target_unit_index, FALSE);
			companion_body.engaging = TRUE;
		}
		else if (strstr(message, "\"action\":\"cover\""))
			companion_set_order(_companion_order_cover, companion_body.target_unit_index, companion_body.hold_fire);
		else if (strstr(message, "\"action\":\"fall_back\""))
			companion_set_order(_companion_order_retreat, companion_body.target_unit_index, companion_body.hold_fire);
		else if (strstr(message, "\"action\":\"go_to\"") || strstr(message, "\"action\":\"go_aim\""))
		{
			/* (a spot of the last view, or where the player aims now: he
			stays there) */
			long spot = option ? atol(option + strlen("\"option\":\"")) : 0;
			real_point3d aim;
			struct render_camera camera;
			struct render_frustum frustum;
			boolean found = strstr(message, "\"action\":\"go_to\"") ?
				spot >= 1 && spot <= companion_view_spot_count :
				companion_view_camera(&camera, &frustum) && companion_view_ground(&camera, &frustum, 0.f, 0.f, &aim);

			if (found)
			{
				companion_set_order(_companion_order_hold, companion_body.target_unit_index, companion_body.hold_fire);
				companion_body.hold_point = strstr(message, "\"action\":\"go_to\"") ? companion_view_spots[spot - 1] : aim;
				companion_body.goal_valid = FALSE;
			}
		}
		else if (strstr(message, "\"action\":\"shoot_chief\""))
			companion_set_order(companion_body.order, companion_player_unit(), FALSE);
		else if (strstr(message, "\"action\":\"switch_weapon\""))
			companion_act(_companion_act_swap, "switch weapon", NONE);
		else if (strstr(message, "\"action\":\"frag_grenade\""))
		{
			companion_act(_companion_act_grenade, "frag grenade", target_unit_index);
			companion_body.act_grenade_type = _unit_grenade_human_fragmentation;
		}
		else if (strstr(message, "\"action\":\"plasma_grenade\""))
		{
			companion_act(_companion_act_grenade, "plasma grenade", target_unit_index);
			companion_body.act_grenade_type = _unit_grenade_covenant_plasma;
		}
		else if (strstr(message, "\"action\":\"melee\""))
			companion_act(_companion_act_melee, "melee", target_unit_index);
		else if (strstr(message, "\"action\":\"jump\""))
			companion_act(_companion_act_jump, "jump", NONE);
		else if (strstr(message, "\"action\":\"flashlight\""))
			companion_act(_companion_act_light, "flashlight", NONE);
		else if (strstr(message, "\"action\":\"get_out\""))
			companion_act(_companion_act_get_out, "get out", NONE);
		else if (strstr(message, "\"action\":\"crouch\""))
			companion_body.crouching = TRUE;
		else if (strstr(message, "\"action\":\"stand_up\""))
			companion_body.crouching = FALSE;
		else if ((strstr(message, "\"action\":\"pick_up\"") || strstr(message, "\"action\":\"get_in\"")) &&
			target_unit_index != NONE)
		{
			companion_body.goal_object_index = target_unit_index;
			companion_set_order(strstr(message, "\"action\":\"pick_up\"") ? _companion_order_pick_up : _companion_order_get_in,
				companion_body.target_unit_index, companion_body.hold_fire);
		}
	}
}

/* where something is from him, in words ("ahead", "to the left", ...) */
static char const *companion_direction_name(
	long unit_index,
	real_point3d const *point)
{
	real_vector3d to;
	real_vector3d left;
	real forward_amount;
	real left_amount;

	vector_from_points3d(&unit_get(unit_index)->object.position, point, &to);
	to.k = 0.f;
	normalize3d(&to);
	cross_product3d(global_up3d, &companion_body.aim, &left);
	forward_amount = to.i * companion_body.aim.i + to.j * companion_body.aim.j;
	left_amount = dot_product3d(&to, &left);
	if (forward_amount > 0.7f)
		return "ahead";
	if (forward_amount < -0.7f)
		return "behind";
	return left_amount > 0.f ? "to the left" : "to the right";
}

/* (the decision model reads words far better than numbers) */
static char const *companion_health_words(
	real vitality)
{
	return vitality >= 0.95f ? "unhurt" : vitality >= 0.6f ? "a bit hurt" : vitality >= 0.3f ? "hurt" : "badly hurt";
}

static char const *companion_shield_words(
	real vitality)
{
	return vitality >= 0.95f ? "shields full" : vitality >= 0.5f ? "shields holding" : vitality > 0.f ? "shields low" : "shields down";
}

/* (a unit's, a weapon's or a vehicle's: its tag's name) */
static char const *companion_unit_kind(
	long object_index)
{
	return tag_name_strip_path(tag_get_name(object_get(object_index)->definition_index));
}

/* an enemy in words: "elite close (9 m) ahead, in sight, unhurt, shields full" */
static void companion_describe_enemy(
	struct companion_enemy const *enemy,
	char *buffer,
	int size)
{
	struct unit_datum *unit = unit_get(enemy->unit_index);
	int metres = (int)(enemy->distance * COMPANION_METRES_PER_UNIT);

	char aboard[64] = "";

	if (unit->object.parent_object_index != NONE)
	{
		char const *vehicle = companion_unit_kind(unit->object.parent_object_index);

		snprintf(aboard, sizeof(aboard), " aboard a%s %s", strchr("aeiou", vehicle[0]) ? "n" : "", vehicle);
	}
	snprintf(buffer, size, "%s%s %s (%d m) %s, %s, %s%s%s",
		companion_unit_kind(enemy->unit_index),
		aboard,
		metres <= 10 ? "close" : metres <= 25 ? "at mid range" : "far away",
		metres,
		companion_direction_name(companion_unit_index, &enemy->aim_point),
		enemy->visible ? "in sight" : "out of sight",
		companion_health_words(unit->object.body_vitality),
		unit->object.shield_vitality > 0.f ? ", " : "",
		unit->object.shield_vitality > 0.f ? companion_shield_words(unit->object.shield_vitality) : "");
}

/* enemies of a kind, counted: aboard a vehicle, or on foot in a direction */
struct companion_group
{
	long vehicle_index;
	char const *kind;
	char const *direction;
	short count;
};

/* one more enemy in the groups: of its kind aboard its vehicle (NONE: on foot, in its
direction) */
static void companion_group_add(
	struct companion_group *groups,
	short *count,
	short maximum,
	long vehicle_index,
	char const *kind,
	char const *direction)
{
	short index;

	for (index = 0; index < *count; index++)
	{
		struct companion_group *group = &groups[index];

		if (group->vehicle_index == vehicle_index && !strcmp(group->kind, kind) &&
			(vehicle_index != NONE || !strcmp(group->direction, direction)))
		{
			group->count++;
			return;
		}
	}
	if (*count < maximum)
	{
		groups[*count].vehicle_index = vehicle_index;
		groups[*count].kind = kind;
		groups[*count].direction = direction;
		groups[*count].count = 1;
		(*count)++;
	}
}

/* a vehicle's groups in words: "4 grunts and an elite" */
static void companion_riders_words(
	struct companion_group const *groups,
	short count,
	long vehicle_index,
	char *buffer,
	int size)
{
	short index;

	buffer[0] = 0;
	for (index = 0; index < count; index++)
	{
		struct companion_group const *group = &groups[index];
		char one[64];

		if (group->vehicle_index != vehicle_index)
			continue;
		if (group->count == 1)
			snprintf(one, sizeof(one), "%sa%s %s", buffer[0] ? " and " : "", strchr("aeiou", group->kind[0]) ? "n" : "", group->kind);
		else
			snprintf(one, sizeof(one), "%s%d %ss", buffer[0] ? " and " : "", group->count, group->kind);
		strncat(buffer, one, size - strlen(buffer) - 1);
	}
}

/* the groups in words: "a dropship ahead, carrying 4 grunts and an elite; 2 jackals to the left" */
static void companion_groups_words(
	struct companion_group const *groups,
	short count,
	char *buffer,
	int size)
{
	short index;

	buffer[0] = 0;
	for (index = 0; index < count; index++)
	{
		struct companion_group const *group = &groups[index];
		char one[256];

		if (group->vehicle_index != NONE)
		{
			char const *vehicle = companion_unit_kind(group->vehicle_index);
			char riders[192];
			short earlier;

			/* (a vehicle once, at its first group) */
			for (earlier = 0; earlier < index && groups[earlier].vehicle_index != group->vehicle_index; earlier++)
				;
			if (earlier < index)
				continue;
			companion_riders_words(groups, count, group->vehicle_index, riders, sizeof(riders));
			snprintf(one, sizeof(one), "%sa%s %s %s, carrying %s", buffer[0] ? "; " : "",
				strchr("aeiou", vehicle[0]) ? "n" : "", vehicle,
				companion_direction_name(companion_unit_index, &object_get(group->vehicle_index)->object.bounding_sphere_center),
				riders);
		}
		else if (group->count == 1)
		{
			snprintf(one, sizeof(one), "%sa%s %s %s", buffer[0] ? "; " : "",
				strchr("aeiou", group->kind[0]) ? "n" : "", group->kind, group->direction);
		}
		else
		{
			snprintf(one, sizeof(one), "%s%d %ss %s", buffer[0] ? "; " : "", group->count, group->kind, group->direction);
		}
		strncat(buffer, one, size - strlen(buffer) - 1);
	}
}

/* what Gem is doing, in words, each part after a comma (", following
{player}, shooting at the grunt"); staying where he is, nothing of it */
static void companion_describe_doing(
	char *buffer,
	int size)
{
	struct companion_enemy const *target = companion_current_target();

	switch (companion_body.order)
	{
	case _companion_order_follow: snprintf(buffer, size, ", following {player}"); break;
	case _companion_order_advance: snprintf(buffer, size, ", rushing in"); break;
	case _companion_order_cover: snprintf(buffer, size, ", taking cover"); break;
	case _companion_order_retreat: snprintf(buffer, size, ", falling back"); break;
	case _companion_order_pick_up: snprintf(buffer, size, ", going to pick up a weapon"); break;
	case _companion_order_get_in: snprintf(buffer, size, ", going to get in a vehicle"); break;
	default: buffer[0] = 0; break;
	}
	if (target && target->unit_index == companion_player_unit())
	{
		strncat(buffer, ", shooting at {player}", size - strlen(buffer) - 1);
	}
	else if (target)
	{
		strncat(buffer, ", shooting at the ", size - strlen(buffer) - 1);
		strncat(buffer, companion_unit_kind(target->unit_index), size - strlen(buffer) - 1);
	}
	if (companion_body.crouching)
		strncat(buffer, ", crouching", size - strlen(buffer) - 1);
	if (companion_body.hold_fire)
		strncat(buffer, ", holding his fire", size - strlen(buffer) - 1);
}

/* an action for mate's list: its name, its description, and its targets
(NULL: it has none; empty: there is nothing to do it to, so it isn't offered) */
static void companion_add_action(
	char *actions,
	int size,
	char const *name,
	char const *description,
	char const *targets)
{
	char one[1536];

	if (targets && !targets[0])
		return;
	snprintf(one, sizeof(one), "%s{\"name\":\"%s\",\"description\":\"%s\"%s%s%s}", actions[0] ? "," : "",
		name, description, targets ? ",\"options\":{" : "", targets ? targets : "", targets ? "}" : "");
	if ((int)(strlen(actions) + strlen(one)) < size)
		strcat(actions, one);
}

/* the things in a list, what a search finds ("shotgun (4 m) ahead; ...", or
"none nearby"), and as options for mate */
static void companion_describe_things(
	struct companion_thing const *things,
	short count,
	char *list,
	int list_size,
	char *options,
	int options_size)
{
	char one[160];
	char option[200];
	short index;

	snprintf(list, list_size, "%s", count ? "" : "none nearby");
	for (index = 0; index < count; index++)
	{
		int metres = (int)(things[index].distance * COMPANION_METRES_PER_UNIT);

		snprintf(one, sizeof(one), "%s (%d m) %s",
			tag_name_strip_path(tag_get_name(object_get(things[index].object_index)->definition_index)),
			metres, companion_direction_name(companion_unit_index, &object_get(things[index].object_index)->object.bounding_sphere_center));
		if (index)
			strncat(list, "; ", list_size - strlen(list) - 1);
		strncat(list, one, list_size - strlen(list) - 1);
		snprintf(option, sizeof(option), "%s\"%ld\":\"%s\"", index ? "," : "", things[index].object_index & 0xFFFF, one);
		if ((int)(strlen(options) + strlen(option)) < options_size)
			strcat(options, option);
	}
}

/* an action that reports back, a search: what he finds (its "result", for
mate's feed), then what he can do about it (its "then": an action whose
options are what he found) */
static void companion_add_search(
	char *actions,
	int size,
	char const *name,
	char const *description,
	char const *result,
	char const *then_name,
	char const *then_description,
	char const *then_options)
{
	char one[2048];
	char then[1024] = "";

	if (then_options[0])
	{
		snprintf(then, sizeof(then), ",\"then\":[{\"name\":\"%s\",\"description\":\"%s\",\"options\":{%s}}]",
			then_name, then_description, then_options);
	}
	snprintf(one, sizeof(one), "%s{\"name\":\"%s\",\"description\":\"%s\",\"result\":\"%s\"%s}",
		actions[0] ? "," : "", name, description, result, then);
	if ((int)(strlen(actions) + strlen(one)) < size)
		strcat(actions, one);
}

/* an action of the player's view (mate asks for it, companion_view_taken):
"describe", what he sees goes in mate's feed; "choose", a spot marked on it */
static void companion_add_view(
	char *actions,
	int size,
	char const *name,
	char const *description,
	char const *view)
{
	char one[256];

	snprintf(one, sizeof(one), "%s{\"name\":\"%s\",\"description\":\"%s\",\"view\":\"%s\"}",
		actions[0] ? "," : "", name, description, view);
	if ((int)(strlen(actions) + strlen(one)) < size)
		strcat(actions, one);
}

/* the game as mate needs it: what is going on, in words, and what Gem can
do (its decision model picks one) */
static void companion_send_state(
	long player_unit_index)
{
	struct unit_datum *player_unit = unit_get(player_unit_index);
	char state[1536];
	char options[1024] = "";
	char melee_options[512] = "";
	char weapon_options[512] = "";
	char vehicle_options[512] = "";
	char found_weapons[512];
	char found_vehicles[512];
	char actions[8192] = "";
	char one[160];
	char json[12288];
	real_point3d player_origin;
	short index;

	object_get_origin(player_unit_index, &player_origin);
	if (companion_unit_alive(companion_unit_index))
	{
		struct unit_datum *unit = unit_get(companion_unit_index);
		long weapon_index = unit_inventory_get_weapon(companion_unit_index, unit->unit.current_weapon_index);
		long other_index = unit_inventory_get_weapon(companion_unit_index, unit->unit.current_weapon_index ? 0 : 1);
		char weapon[384] = "no weapon";
		char doing[96];
		real_point3d origin;
		int metres;

		if (weapon_index != NONE)
		{
			struct weapon_datum *datum = weapon_get(weapon_index);
			int loaded = datum->weapon.magazines[0].rounds_loaded;
			char const *name = tag_name_strip_path(tag_get_name(datum->definition_index));

			snprintf(weapon, sizeof(weapon), "holding a%s %s (%s, %s)", strchr("aeiou", name[0]) ? "n" : "", name,
				loaded ? (snprintf(one, sizeof(one), "%d rounds loaded", loaded), one) : "magazine empty",
				datum->weapon.magazines[0].rounds_total > 0 ? "spare ammo" : "no spare ammo");
		}
		if (other_index != NONE)
		{
			struct weapon_datum *other = weapon_get(other_index);
			char const *other_name = tag_name_strip_path(tag_get_name(other->definition_index));

			snprintf(one, sizeof(one), ", a%s %s on his back (%d rounds loaded, %s)",
				strchr("aeiou", other_name[0]) ? "n" : "", other_name,
				other->weapon.magazines[0].rounds_loaded,
				other->weapon.magazines[0].rounds_total > 0 ? "spare ammo" : "no spare ammo");
			strncat(weapon, one, sizeof(weapon) - strlen(weapon) - 1);
		}
		else
		{
			strncat(weapon, ", no second weapon", sizeof(weapon) - strlen(weapon) - 1);
		}
		snprintf(one, sizeof(one), ", %d frag and %d plasma grenades",
			unit->unit.grenade_counts[_unit_grenade_human_fragmentation], unit->unit.grenade_counts[_unit_grenade_covenant_plasma]);
		strncat(weapon, one, sizeof(weapon) - strlen(weapon) - 1);
		if (unit->object.parent_object_index != NONE)
		{
			snprintf(one, sizeof(one), ", riding in a %s",
				tag_name_strip_path(tag_get_name(object_get(unit->object.parent_object_index)->definition_index)));
			strncat(weapon, one, sizeof(weapon) - strlen(weapon) - 1);
		}
		if (unit->unit.integrated_light_power > 0.f)
			strncat(weapon, ", flashlight on", sizeof(weapon) - strlen(weapon) - 1);
		companion_describe_doing(doing, sizeof(doing));
		object_get_origin(companion_unit_index, &origin);
		metres = (int)(distance3d(&origin, &player_origin) * COMPANION_METRES_PER_UNIT);
		snprintf(state, sizeof(state), "Gem: %s, %s, %s%s. {player}: %s %s, %s, %s. Enemies:",
			companion_health_words(unit->object.body_vitality),
			companion_shield_words(unit->object.shield_vitality),
			weapon, doing,
			metres <= 6 ? "close by" : metres <= 25 ? "nearby" : "far away",
			companion_direction_name(companion_unit_index, &player_origin),
			companion_health_words(player_unit->object.body_vitality),
			companion_shield_words(player_unit->object.shield_vitality));
		for (index = 0; index < companion_enemy_count; index++)
		{
			companion_describe_enemy(&companion_enemies[index], one, sizeof(one));
			strncat(state, index ? "; " : " ", sizeof(state) - strlen(state) - 1);
			strncat(state, one, sizeof(state) - strlen(state) - 1);
			snprintf(json, sizeof(json), "%s\"%ld\":\"%s\"", index ? "," : "", companion_enemies[index].unit_index & 0xFFFF, one);
			/* (whole options only, so that the JSON stays whole) */
			if (strlen(options) + strlen(json) < sizeof(options))
				strcat(options, json);
			if (companion_enemies[index].distance <= COMPANION_MELEE_DISTANCE)
			{
				snprintf(json, sizeof(json), "%s\"%ld\":\"%s\"", melee_options[0] ? "," : "",
					companion_enemies[index].unit_index & 0xFFFF, one);
				if (strlen(melee_options) + strlen(json) < sizeof(melee_options))
					strcat(melee_options, json);
			}
		}
		strncat(state, companion_enemy_count ? "." : " none.", sizeof(state) - strlen(state) - 1);
		/* (what's lying about is found by searching: not in the state, where
		it would be all he talked about) */
		companion_describe_things(companion_weapons, companion_weapon_count,
			found_weapons, sizeof(found_weapons), weapon_options, sizeof(weapon_options));
		companion_describe_things(companion_vehicles, companion_vehicle_count,
			found_vehicles, sizeof(found_vehicles), vehicle_options, sizeof(vehicle_options));

		/* what he can do now, and only that */
		{
			struct companion_enemy const *target = companion_current_target();
			boolean in_vehicle = unit->object.parent_object_index != NONE;

			if (companion_body.order != _companion_order_follow)
				companion_add_action(actions, sizeof(actions), "follow", "follows {player}", NULL);
			if (companion_body.order != _companion_order_hold)
				companion_add_action(actions, sizeof(actions), "stay", "stays where he is", NULL);
			companion_add_action(actions, sizeof(actions), "shoot", "shoots at an enemy", options);
			if (target)
			{
				char stop[96];

				if (target->unit_index == player_unit_index)
					snprintf(stop, sizeof(stop), "stops shooting at {player}");
				else
					snprintf(stop, sizeof(stop), "stops shooting at the %s", companion_unit_kind(target->unit_index));
				companion_add_action(actions, sizeof(actions), "hold_fire", stop, NULL);
			}
			if (!target || target->unit_index != player_unit_index)
				companion_add_action(actions, sizeof(actions), "shoot_chief", "turns his gun on {player}", NULL);
			if (companion_enemy_count && !in_vehicle)
			{
				companion_add_action(actions, sizeof(actions), "rush", "rushes an enemy", options);
				companion_add_action(actions, sizeof(actions), "cover", "takes cover", NULL);
				companion_add_action(actions, sizeof(actions), "fall_back", "falls back", NULL);
			}
			if (unit->unit.grenade_counts[_unit_grenade_human_fragmentation] && !in_vehicle)
				companion_add_action(actions, sizeof(actions), "frag_grenade", "throws a frag grenade", options);
			if (unit->unit.grenade_counts[_unit_grenade_covenant_plasma] && !in_vehicle)
				companion_add_action(actions, sizeof(actions), "plasma_grenade", "sticks a plasma grenade on an enemy", options);
			if (!in_vehicle)
				companion_add_action(actions, sizeof(actions), "melee", "punches an enemy", melee_options);
			if (other_index != NONE)
			{
				snprintf(one, sizeof(one), "switches to his %s", companion_unit_kind(other_index));
				companion_add_action(actions, sizeof(actions), "switch_weapon", one, NULL);
			}
			if (in_vehicle)
			{
				companion_add_action(actions, sizeof(actions), "get_out", "gets out of the vehicle", NULL);
			}
			else
			{
				real_point3d aim;
				struct render_camera camera;
				struct render_frustum frustum;

				companion_add_search(actions, sizeof(actions), "search_weapons", "looks around for weapons",
					found_weapons, "pick_up", "picks up a weapon", weapon_options);
				companion_add_search(actions, sizeof(actions), "search_vehicles", "looks around for vehicles",
					found_vehicles, "get_in", "gets in a vehicle", vehicle_options);
				companion_add_view(actions, sizeof(actions), "go_to", "goes to a spot {player} can see", "choose");
				if (companion_view_camera(&camera, &frustum) && companion_view_ground(&camera, &frustum, 0.f, 0.f, &aim))
				{
					snprintf(one, sizeof(one), "goes where {player} is aiming, %d m from {player}",
						(int)(distance3d(&camera.position, &aim) * COMPANION_METRES_PER_UNIT));
					companion_add_action(actions, sizeof(actions), "go_aim", one, NULL);
				}
				companion_add_action(actions, sizeof(actions), "jump", "jumps", NULL);
				companion_add_action(actions, sizeof(actions), companion_body.crouching ? "stand_up" : "crouch",
					companion_body.crouching ? "stands up" : "crouches", NULL);
			}
			companion_add_view(actions, sizeof(actions), "look", "takes a look at something {player} wants him to see", "describe");
			companion_add_action(actions, sizeof(actions), "flashlight",
				unit->unit.integrated_light_power > 0.f ? "switches his flashlight off" : "switches his flashlight on", NULL);
		}
	}
	else
	{
		snprintf(state, sizeof(state), "Gem is dead and will be back soon. {player}: %s, %s.",
			companion_health_words(player_unit->object.body_vitality),
			companion_shield_words(player_unit->object.shield_vitality));
	}

	snprintf(json, sizeof(json),
		"{\"type\":\"state\",\"game\":\"Halo: Combat Evolved\",\"player_name\":\"Chief\",\"level\":\"%s\","
		"\"in_combat\":%s,\"on_radio\":%s,\"state\":\"%s\",\"actions\":[%s]}",
		tag_name_strip_path(tag_get_name(global_scenario_index)), companion_enemy_count ? "true" : "false",
		companion_on_radio ? "true" : "false", state, actions);
	mate_link_send(json);
}

/* what happened since the last state, in words, as one event for mate */
static void companion_send_events(
	long player_unit_index)
{
	static boolean valid = FALSE;
	static real kevin_shield, kevin_health, player_shield;
	/* (heavy fire: his shields half a second ago, and when that was) */
	static real kevin_shield_before = 0.f;
	static long kevin_shield_time = 0;
	static boolean in_contact = FALSE;
	static long known[COMPANION_MAXIMUM_ENEMIES];
	static short known_count = 0;
	/* (the enemies aboard a vehicle last time, and theirs) */
	static long riders[COMPANION_MAXIMUM_ENEMIES];
	static long rider_vehicles[COMPANION_MAXIMUM_ENEMIES];
	static short rider_count = 0;
	/* (the time of the last of the player's hits told) */
	static unsigned long shot_time = (unsigned long)NONE;
	static real player_health;
	static long player_weapons[4];
	static long player_vehicle = NONE;
	static long last_time = 0;
	static char objective[256] = "";
	char now_objective[256];
	struct unit_datum *player_unit = unit_get(player_unit_index);
	struct unit_datum *unit = companion_unit_alive(companion_unit_index) ? unit_get(companion_unit_index) : NULL;
	char events[1024] = "";
	char one[160];
	short index;

	/* (a new level, or a save loaded: the clock went back, and what was seen is stale) */
	if (game_time_get() < last_time)
	{
		valid = FALSE;
		shot_time = (unsigned long)NONE;
		rider_count = 0;
	}
	last_time = game_time_get();

	if (valid && unit)
	{
		/* (contact: enemies in sight, once, until none are left near him) */
		if (!in_contact)
		{
			struct companion_group groups[8];
			short group_count = 0;
			char seen[512];

			for (index = 0; index < companion_enemy_count; index++)
			{
				struct companion_enemy const *enemy = &companion_enemies[index];

				if (!enemy->visible)
					continue;
				companion_group_add(groups, &group_count, (short)NUMBEROF(groups),
					unit_get(enemy->unit_index)->object.parent_object_index, companion_unit_kind(enemy->unit_index),
					companion_direction_name(companion_unit_index, &enemy->aim_point));
			}
			if (group_count)
			{
				companion_groups_words(groups, group_count, seen, sizeof(seen));
				strncat(events, "Enemies came into view: ", sizeof(events) - strlen(events) - 1);
				strncat(events, seen, sizeof(events) - strlen(events) - 1);
				strncat(events, ". ", sizeof(events) - strlen(events) - 1);
				in_contact = TRUE;
			}
		}
		/* (riders off their vehicle: a dropship dropping them off) */
		{
			struct companion_group groups[8];
			short group_count = 0;
			short group;

			for (index = 0; index < rider_count; index++)
			{
				struct unit_datum *rider = (struct unit_datum *)object_try_and_get_and_verify_type(riders[index], _object_mask_unit);

				if (rider && !TEST_FLAG(rider->object.damage_flags, _object_dead_bit) && rider->object.parent_object_index == NONE)
				{
					companion_group_add(groups, &group_count, (short)NUMBEROF(groups), rider_vehicles[index],
						companion_unit_kind(riders[index]), "");
				}
			}
			for (group = 0; group < group_count; group++)
			{
				char off[192];
				short earlier;

				for (earlier = 0; earlier < group && groups[earlier].vehicle_index != groups[group].vehicle_index; earlier++)
					;
				if (earlier < group)
					continue;
				companion_riders_words(groups, group_count, groups[group].vehicle_index, off, sizeof(off));
				if (object_try_and_get_and_verify_type(groups[group].vehicle_index, _object_mask_all))
				{
					snprintf(one, sizeof(one), "%s got off the %s %s. ", off, companion_unit_kind(groups[group].vehicle_index),
						companion_direction_name(companion_unit_index,
							&object_get(groups[group].vehicle_index)->object.bounding_sphere_center));
				}
				else
				{
					snprintf(one, sizeof(one), "%s got off their vehicle. ", off);
				}
				one[0] = (char)toupper((unsigned char)one[0]);
				strncat(events, one, sizeof(events) - strlen(events) - 1);
			}
		}
		/* (an enemy that died: whoever hurt it last killed it) */
		for (index = 0; index < known_count; index++)
		{
			struct unit_datum *dead = unit_try_and_get(known[index]);
			struct unit_attacker const *killer = NULL;
			short attacker;

			if (!dead || !TEST_FLAG(dead->object.damage_flags, _object_dead_bit))
				continue;
			for (attacker = 0; attacker < MAXIMUM_ATTACKERS_PER_UNIT; attacker++)
			{
				struct unit_attacker const *one_attacker = &dead->unit.attackers[attacker];

				if (one_attacker->game_time_stamp != (unsigned long)NONE &&
					(!killer || one_attacker->game_time_stamp > killer->game_time_stamp))
				{
					killer = one_attacker;
				}
			}
			{
				char const *kind = companion_unit_kind(known[index]);
				char const *who = killer && killer->object_index == companion_unit_index ? "Gem killed" :
					killer && killer->player_index != NONE ? "{player} killed" : "Someone killed";

				snprintf(one, sizeof(one), "%s a%s %s. ", who, strchr("aeiou", kind[0]) ? "n" : "", kind);
				strncat(events, one, sizeof(events) - strlen(events) - 1);
			}
		}
		/* (the player's shots: a player among those who hurt Gem, newer than
		the hit last told; a hit later in the tick he last looked has that
		tick's time, so the time he looked won't do) */
		{
			boolean shot = FALSE;

			/* (a game just loaded: its hits are old news) */
			if (shot_time == (unsigned long)NONE)
				shot_time = (unsigned long)game_time_get();
			for (index = 0; index < MAXIMUM_ATTACKERS_PER_UNIT; index++)
			{
				struct unit_attacker const *attacker = &unit->unit.attackers[index];

				if (attacker->game_time_stamp != (unsigned long)NONE && attacker->player_index != NONE &&
					attacker->game_time_stamp > shot_time)
				{
					shot_time = attacker->game_time_stamp;
					shot = TRUE;
				}
			}
			if (shot)
				strncat(events, "{player} shot Gem. ", sizeof(events) - strlen(events) - 1);
		}
		if (kevin_shield > 0.f && unit->object.shield_vitality <= 0.f)
		{
			strncat(events, "Gem's shields broke. ", sizeof(events) - strlen(events) - 1);
		}
		if (kevin_health >= 0.3f && unit->object.body_vitality < 0.3f)
		{
			strncat(events, "Gem is badly hurt. ", sizeof(events) - strlen(events) - 1);
		}
		if (game_time_get() - kevin_shield_time >= COMPANION_STATE_TICKS)
		{
			if (kevin_shield_before - unit->object.shield_vitality >= 0.3f && unit->object.shield_vitality > 0.f)
				strncat(events, "Gem is taking heavy fire. ", sizeof(events) - strlen(events) - 1);
			kevin_shield_before = unit->object.shield_vitality;
			kevin_shield_time = game_time_get();
		}
	}
	if (valid && player_shield > 0.f && player_unit->object.shield_vitality <= 0.f)
	{
		strncat(events, "{player}'s shields went down. ", sizeof(events) - strlen(events) - 1);
	}
	if (valid && player_health >= 0.3f && player_unit->object.body_vitality < 0.3f)
	{
		strncat(events, "{player} is badly hurt. ", sizeof(events) - strlen(events) - 1);
	}
	/* (a weapon in his hands he didn't have: picked up) */
	for (index = 0; valid && index < 4; index++)
	{
		long weapon_index = unit_inventory_get_weapon(player_unit_index, index);

		if (weapon_index != NONE && weapon_index != player_weapons[0] && weapon_index != player_weapons[1] &&
			weapon_index != player_weapons[2] && weapon_index != player_weapons[3])
		{
			snprintf(one, sizeof(one), "{player} picked up a %s. ", companion_unit_kind(weapon_index));
			strncat(events, one, sizeof(events) - strlen(events) - 1);
		}
	}
	if (valid && player_unit->object.parent_object_index != player_vehicle)
	{
		long vehicle = player_unit->object.parent_object_index != NONE ? player_unit->object.parent_object_index : player_vehicle;

		if (object_try_and_get_and_verify_type(vehicle, _object_mask_all))
		{
			snprintf(one, sizeof(one), "{player} got %s the %s. ",
				player_unit->object.parent_object_index != NONE ? "in" : "out of", companion_unit_kind(vehicle));
			strncat(events, one, sizeof(events) - strlen(events) - 1);
		}
	}
	/* (a new objective on the player's screen) */
	companion_text(hud_messaging_get_objective(), now_objective, sizeof(now_objective));
	if (valid && now_objective[0] && strcmp(now_objective, objective))
	{
		strncat(events, "New objective: ", sizeof(events) - strlen(events) - 1);
		strncat(events, now_objective, sizeof(events) - strlen(events) - 1);
		strncat(events, ". ", sizeof(events) - strlen(events) - 1);
	}
	csstrncpy(objective, now_objective, sizeof(objective));

	if (events[0])
	{
		char json[1152];

		events[strlen(events) - 1] = 0;
		/* (the state as it is now, for mate to decide on) */
		companion_send_state(player_unit_index);
		snprintf(json, sizeof(json), "{\"type\":\"event\",\"text\":\"%s\"}", events);
		mate_link_send(json);
	}

	valid = TRUE;
	player_shield = player_unit->object.shield_vitality;
	player_health = player_unit->object.body_vitality;
	for (index = 0; index < 4; index++)
		player_weapons[index] = unit_inventory_get_weapon(player_unit_index, index);
	player_vehicle = player_unit->object.parent_object_index;
	kevin_shield = unit ? unit->object.shield_vitality : 0.f;
	kevin_health = unit ? unit->object.body_vitality : 0.f;
	if (!companion_enemy_count)
		in_contact = FALSE;
	known_count = 0;
	rider_count = 0;
	for (index = 0; index < companion_enemy_count; index++)
	{
		long vehicle = unit_get(companion_enemies[index].unit_index)->object.parent_object_index;

		known[known_count++] = companion_enemies[index].unit_index;
		if (vehicle != NONE)
		{
			riders[rider_count] = companion_enemies[index].unit_index;
			rider_vehicles[rider_count] = vehicle;
			rider_count++;
		}
	}
}

static void companion_send_event(
	char const *text)
{
	char json[256];

	snprintf(json, sizeof(json), "{\"type\":\"event\",\"text\":\"%s\"}", text);
	mate_link_send(json);
}

/* a string of the game's (UTF-16) as plain text for mate: no line breaks
("|n"), quotes as apostrophes, characters past ASCII left out (the objectives'
"»") */
static void companion_text(
	wchar_t const *text,
	char *out,
	int size)
{
	int length = 0;

	for (; text && *text && length < size - 1; text++)
	{
		wchar_t c = *text;

		if (c == '|' && text[1] == 'n')
		{
			c = ' ';
			text++;
		}
		if (c == '"')
			c = '\'';
		if (c == '\\')
			c = '/';
		if (c < ' ' || c > '~' || (c == ' ' && (length == 0 || out[length - 1] == ' ')))
			continue;
		out[length++] = (char)c;
	}
	while (length && out[length - 1] == ' ')
		length--;
	out[length] = 0;
}

/* who speaks a line of dialogue, from its sound's name: "a30_010_cortana",
"a30_insert_020_lifeboatpilot", or a cutscene's "cor01", "sgt05b", "lif_01" */
static void companion_speaker(
	long definition_index,
	char *speaker,
	int size)
{
	static char const *const speakers[][2] =
	{
		{ "cortana", "Cortana" }, { "cor", "Cortana" },
		{ "captkeyes", "Captain Keyes" }, { "keyes", "Captain Keyes" },
		{ "sarge", "the Sergeant" }, { "sgt", "the Sergeant" },
		{ "lifeboatpilot", "the lifeboat pilot" }, { "lif", "the lifeboat pilot" },
		{ "pilot", "the pilot" }, { "flightofficer", "the flight officer" },
		{ "chief", "{player}" }, { "che", "{player}" },
		{ "cryotech", "a crewman" }, { "cryoassist", "a crewman" }, { "tec", "a crewman" }, { "ass", "a crewman" },
		{ "crewman", "a crewman" },
	};
	char const *name = tag_name_strip_path(tag_get_name(definition_index));
	char word[32] = "";
	short index;

	/* (the last part with a letter, up to its first digit or space) */
	while (*name)
	{
		char part[32];
		int length = 0;
		boolean letters = FALSE;

		for (; *name && *name != '_'; name++)
		{
			if (length < (int)sizeof(part) - 1)
				part[length++] = (char)tolower((unsigned char)*name);
			letters |= isalpha((unsigned char)*name) != 0;
		}
		part[length] = 0;
		if (*name)
			name++;
		if (letters)
		{
			for (length = 0; part[length] && !isdigit((unsigned char)part[length]) && part[length] != ' '; length++)
				;
			part[length] = 0;
			csstrncpy(word, part, sizeof(word));
		}
	}
	csstrncpy(speaker, "a marine", size);
	for (index = 0; index < NUMBEROF(speakers); index++)
	{
		if (!strcmp(word, speakers[index][0]))
			csstrncpy(speaker, speakers[index][1], size);
	}
}

/* (a save's id, the same when it's made and when it's loaded: the level, and
the game time, which the save carries) */
static void companion_send_save(
	char const *type)
{
	char json[160];

	if (game_engine_running() || network_game_distributed_client())
		return;
	snprintf(json, sizeof(json), "{\"type\":\"%s\",\"id\":\"%s:%ld\"}", type,
		tag_name_strip_path(tag_get_name(global_scenario_index)), game_time_get());
	mate_link_send(json);
}

/* a gain in DirectSound's millibels (as sound_dsound_xbox.c's
dsound_volume_from_gain, and above 1 too: his voice can be turned up past
the game's) */
static long companion_millibels(
	real gain)
{
	long millibels;

	if (gain <= 0.f)
		return DSBVOLUME_MIN;
	millibels = (long)(2000.0 * log10(gain));
	return millibels > DSBVOLUME_MIN ? millibels : DSBVOLUME_MIN;
}

/* his voice: a 3D voice of the game's mixer (mate_link.c's stream), placed
each tick as the sound manager places a unit's dialogue (sound_manager.c,
update_channels and update_channel_for_impulse_sound; sound_dsound_xbox.c,
dsound_channel_set_location and dsound_channel_set_I3DL2_properties): at his
head, relative to the player's camera, falling off beyond the dialogue
class's minimum distance, muffled by what's between them
(compute_sound_obstruction), sent to the reverb as dialogue is, at the gain
the game gives dialogue (its volume setting, ducked under scripted
dialogue). Far off (companion.radio_distance), or a wall between, it's over
the radio instead: centred, and dry; each at its own volume
(companion.voice_volume, companion.radio_volume) */
static void companion_place_voice(
	void)
{
	struct IDirectSoundStream *stream = mate_link_voice_stream();
	struct sound_class_definition const *dialog = sound_class_get(_sound_class_unit_dialog);
	struct observer_result const *camera;
	short local_player_index = local_player_get_next(NONE);
	real gain = sound_manager_port_class_gain(_sound_class_unit_dialog) * COMPANION_VOICE_GAIN;
	real radio_distance = (real)PIN(config_real("companion.radio_distance"), 1.0, 1000.0) / COMPANION_METRES_PER_UNIT;
	boolean radio = TRUE;
	static struct sound_class_definition const *logged;

	if (!stream)
		return;
	if (logged != dialog)
	{
		logged = dialog;
		platform_log("Gem's voice: dialogue's minimum distance %.2f, wet gain %.2f, gain %.2f",
			dialog->minimum_distance, dialog->wet_gain, gain);
	}
	if (companion_unit_alive(companion_unit_index) &&
		local_player_index != NONE &&
		(camera = observer_get_camera(local_player_index)) != NULL)
	{
		real_point3d head;
		struct collision_result collision;
		/* (where he is in the level: in a vehicle, the vehicle's; his own
		isn't kept while he rides, and without it the game takes a sound for
		walled off, all but silent) */
		long located_index = companion_unit_index;

		while (object_get(located_index)->object.parent_object_index != NONE)
			located_index = object_get(located_index)->object.parent_object_index;
		unit_get_camera_position(companion_unit_index, &head);
		if (distance3d(&camera->position, &head) <= radio_distance &&
			!collision_test_line(FLAG(_collision_test_structure_bit) | FLAG(_collision_test_front_facing_surfaces_bit),
				&camera->position, &head, NONE, &collision))
		{
			real_matrix4x3 listener;
			real_point3d relative;
			real obstruction;
			real occlusion;
			DSI3DL2BUFFER i3dl2;

			radio = FALSE;
			/* (the listener's space, as the sound manager's; DirectSound's
			axes as sound_dsound_xbox.c's DSOUND_POINT3D) */
			matrix4x3_from_point_and_vectors(&listener, &camera->position, &camera->forward, &camera->up);
			matrix4x3_inverse_transform_point(&listener, &head, &relative);
			game_sound_port_obstruction(local_player_index, &head,
				&object_get(located_index)->object.location, &obstruction, &occlusion);
			memset(&i3dl2, 0, sizeof(i3dl2));
			i3dl2.lRoom = companion_millibels((1.f - dialog->wet_gain) * 0.5f);
			i3dl2.lRoomHF = i3dl2.lRoom;
			i3dl2.Obstruction.lHFLevel = companion_millibels(1.f - obstruction);
			i3dl2.Occlusion.lHFLevel = companion_millibels(1.f - occlusion);
			i3dl2.Occlusion.flLFRatio = 0.2f;
			IDirectSoundStream_SetMode(stream, DS3DMODE_NORMAL, DS3D_DEFERRED);
			IDirectSoundStream_SetPosition(stream, relative.x, relative.z, relative.y, DS3D_DEFERRED);
			IDirectSoundStream_SetMinDistance(stream,
				dialog->minimum_distance > 0.f ? dialog->minimum_distance : COMPANION_VOICE_MINIMUM_DISTANCE,
				DS3D_DEFERRED);
			IDirectSoundStream_SetMaxDistance(stream, REAL_MAX, DS3D_DEFERRED);
			IDirectSoundStream_SetI3DL2Source(stream, &i3dl2, DS3D_DEFERRED);
		}
	}
	if (radio)
		IDirectSoundStream_SetMode(stream, DS3DMODE_DISABLE, DS3D_DEFERRED);
	IDirectSoundStream_SetVolume(stream, companion_millibels(gain * (radio ?
		COMPANION_RADIO_GAIN * (real)PIN(config_real("companion.radio_volume"), 0.0, 16.0) :
		(real)PIN(config_real("companion.voice_volume"), 0.0, 16.0))));
	mate_link_set_radio(radio ? 1.f : 0.f);
	/* (over the radio only while he's about: dead, he isn't talking) */
	companion_on_radio = radio && companion_unit_alive(companion_unit_index);
}

/* the microphone mate hears the player by: muted and unmuted with the KEVIN
MIC key (controls.kevin_mic), a press each; mate is told when it changes,
and again whenever it connects */
static boolean companion_mic_muted;

static void companion_update_mic(
	void)
{
	static boolean was_held;
	static boolean was_connected;
	boolean held = halo_kevin_mic_held() && !console_is_active();
	boolean connected = mate_link_connected();

	if (held && !was_held)
		companion_mic_muted = !companion_mic_muted;
	if ((held && !was_held) || (connected && !was_connected))
	{
		char json[64];

		snprintf(json, sizeof(json), "{\"type\":\"mic\",\"muted\":%s}", companion_mic_muted ? "true" : "false");
		mate_link_send(json);
	}
	was_held = held;
	was_connected = connected;
}

/* what mate keeps of the settings: how long after his voice stops before the
game's events can have him talk again (companion.speech_cooldown); sent when
it changes, and again whenever mate connects */
static void companion_update_settings(
	void)
{
	static boolean was_connected;
	static real sent_cooldown;
	boolean connected = mate_link_connected();
	real cooldown = (real)PIN(config_real("companion.speech_cooldown"), 0.0, 600.0);

	if (connected && (!was_connected || cooldown != sent_cooldown))
	{
		char json[80];

		snprintf(json, sizeof(json), "{\"type\":\"settings\",\"speech_cooldown\":%.2f}", cooldown);
		mate_link_send(json);
		sent_cooldown = cooldown;
	}
	was_connected = connected;
}

/* ---------- public code */

boolean companion_view_wanted(
	void)
{
	boolean wanted = companion_view_requested;

	companion_view_requested = FALSE;
	return wanted;
}

void companion_view_taken(
	unsigned char const *rgb,
	unsigned long width,
	unsigned long height)
{
	struct render_camera camera;
	struct render_frustum frustum;
	boolean seen = companion_view_camera(&camera, &frustum);
	char json[4096];
	short column;
	short row;

	companion_view_spot_count = 0;
	snprintf(json, sizeof(json), "{\"width\":%lu,\"height\":%lu,\"spots\":[", width, height);
	for (row = 0; row < COMPANION_VIEW_ROWS; row++)
	{
		for (column = 0; column < COMPANION_VIEW_COLUMNS; column++)
		{
			/* (across most of the view, from just above its middle down; the
			view, one player's, is the whole frame) */
			real x = -0.875f + 1.75f * column / (COMPANION_VIEW_COLUMNS - 1);
			real y = -0.05f + 0.85f * row / (COMPANION_VIEW_ROWS - 1);
			real_point3d ground;
			char one[96];

			if (!seen || !companion_view_ground(&camera, &frustum, x, y, &ground))
				continue;
			companion_view_spots[companion_view_spot_count++] = ground;
			snprintf(one, sizeof(one), "%s{\"id\":%d,\"x\":%.3f,\"y\":%.3f,\"metres\":%d}",
				companion_view_spot_count > 1 ? "," : "", companion_view_spot_count,
				(x + 1.f) * 0.5f, (y + 1.f) * 0.5f,
				(int)(distance3d(&camera.position, &ground) * COMPANION_METRES_PER_UNIT));
			strncat(json, one, sizeof(json) - strlen(json) - 1);
		}
	}
	strncat(json, "]}", sizeof(json) - strlen(json) - 1);
	mate_link_send_view(json, rgb, width * height * 3);
}

/* the microphone's icon, at the left of the view where voice chat lists who
talks: the menus' voice/microphone (Lucide's mic, white: tools/ce_menus.py),
faint while Gem hears you, red and struck through while muted; while mate
is connected, in the campaign */
void companion_draw_mic(
	void)
{
	long bitmap_index;
	struct bitmap_data *bitmap;
	rectangle2d bounds = render.camera.window_bounds;
	rectangle2d icon;
	short size;

	if (!mate_link_connected() || game_engine_running() || network_game_distributed_client())
		return;
	bitmap_index = tag_loaded(BITMAP_GROUP_TAG, "pc\\voice\\microphone");
	bitmap = bitmap_index != NONE ? bitmap_group_try_and_get_bitmap(bitmap_index, companion_mic_muted ? 1 : 0) : NULL;
	if (!bitmap)
		return;
	offset_rectangle2d(&bounds, -render.camera.viewport_bounds.x0, -render.camera.viewport_bounds.y0);
	size = (short)((bounds.y1 - bounds.y0) / 24);
	icon.x0 = (short)(bounds.x0 + 16);
	icon.x1 = (short)(icon.x0 + size);
	icon.y0 = (short)(bounds.y0 + (bounds.y1 - bounds.y0) * 55 / 100);
	icon.y1 = (short)(icon.y0 + size);
	draw_bitmap_in_rect(bitmap, &icon, NULL, NULL, companion_mic_muted ? 0xFFE05A5A : 0x99FFFFFF, NULL, TRUE);
}

void companion_note_saved(
	void)
{
	companion_send_save("saved");
}

void companion_note_loaded(
	void)
{
	companion_send_save("loaded");
	/* (his body is the save's, if it has one, else he comes back now: he
	didn't die) */
	companion_unit_index = NONE;
	companion_respawn_time = 0;
}

/* scripted dialogue beginning to play, for mate to hear: Cortana and the crew
on the radio (no source), and the cutscenes' (heard, not answered) */
void companion_note_sound(
	long definition_index,
	struct sound_permutation const *permutation,
	long source_identifier)
{
	struct sound_definition *definition = sound_definition_get(definition_index);
	boolean cutscene = cinematic_in_progress();
	unsigned long frames = 0;
	unsigned long frame;
	short *samples;
	char speaker[48];
	char json[256];

	if ((definition->sound_class != _sound_class_scripted_dialog_to_player &&
		definition->sound_class != _sound_class_scripted_dialog_to_other &&
		definition->sound_class != _sound_class_scripted_dialog_force_unspatialized) ||
		permutation->compression != 1 /* Xbox ADPCM, the only one that plays */ ||
		game_engine_running() || network_game_distributed_client() || !mate_link_connected())
	{
		return;
	}
	samples = dsound_decode_xbox_adpcm((unsigned char const *)permutation->unknown1, permutation->samples.size,
		definition->encoding ? 2 : 1, &frames);
	if (!samples)
		return;
	if (definition->encoding)
	{
		for (frame = 0; frame < frames; frame++)
			samples[frame] = (short)((samples[frame * 2] + samples[frame * 2 + 1]) / 2);
	}
	companion_speaker(definition_index, speaker, sizeof(speaker));
	snprintf(json, sizeof(json), "{\"speaker\":\"%s\",\"radio\":%s,\"react\":%s,\"sample_rate\":%lu}",
		speaker, source_identifier == NONE && !cutscene ? "true" : "false", cutscene ? "false" : "true",
		sound_sample_rate_samples_per_second[definition->sample_rate ? 1 : 0]);
	mate_link_send_hear(json, samples, frames);
	dsound_free_decoded(samples);
}

boolean companion_console_command(
	char const *command)
{
	static real_argb_color const red = { 1.f, 1.f, 0.45f, 0.4f };
	char json[1024];
	long length = 0;
	char const *text;

	while (*command == ' ')
		command++;
	if (_strnicmp(command, "kevin", 5) != 0 || (command[5] != ' ' && command[5] != 0))
		return FALSE;
	command += 5;
	while (*command == ' ')
		command++;
	if (!*command)
	{
		terminal_printf(&red, "kevin <text>: tells Gem something as the game master would (\"kevin you are dead\")");
		return TRUE;
	}
	if (!mate_link_connected())
	{
		terminal_printf(&red, "Gem isn't listening (mate isn't connected)");
		return TRUE;
	}
	/* (the text as a JSON string) */
	text = command;
	length += snprintf(json, sizeof(json), "{\"type\":\"direct\",\"text\":\"");
	for (; *command && length < (long)sizeof(json) - 8; command++)
	{
		if (*command == '"' || *command == '\\')
			json[length++] = '\\';
		if ((unsigned char)*command >= ' ')
			json[length++] = *command;
	}
	snprintf(json + length, sizeof(json) - length, "\"}");
	mate_link_send(json);
	terminal_printf(&red, "(to Gem) %s", text);
	return TRUE;
}

void companion_update(
	void)
{
	long player_unit_index;
	struct unit_datum *player_unit;

	mate_link_start();
	if (game_engine_running() || network_game_distributed_client())
		return;

	companion_handle_messages();
	companion_update_mic();
	companion_update_settings();
	companion_place_voice();
	player_unit_index = companion_player_unit();
	player_unit = player_unit_index == NONE ? NULL : unit_try_and_get(player_unit_index);
	if (cinematic_in_progress())
		return;

	if (!player_unit || TEST_FLAG(player_unit->object.damage_flags, _object_dead_bit))
	{
		if (player_unit && !companion_player_dead)
			companion_send_event("{player} died.");
		companion_player_dead = player_unit != NULL;
		return;
	}
	companion_player_dead = FALSE;

	/* (a new level starts the game's clock again) */
	if (companion_respawn_time > game_time_get() + COMPANION_RESPAWN_TICKS)
		companion_respawn_time = 0;

	if (!companion_is_alive(companion_unit_index, player_unit->definition_index))
	{
		long found_unit_index = companion_find(player_unit->definition_index);

		if (found_unit_index == NONE && companion_unit_index != NONE)
		{
			companion_send_event("Gem died.");
			companion_unit_index = NONE;
			companion_respawn_time = game_time_get() + COMPANION_RESPAWN_TICKS;
		}
		else if (found_unit_index != NONE)
		{
			/* (a loaded game: his body as it was, his orders afresh) */
			companion_unit_index = found_unit_index;
			csmemset(&companion_body, 0, sizeof(companion_body));
			companion_body.order = _companion_order_follow;
			companion_body.target_unit_index = NONE;
			companion_body.aim = unit_get(found_unit_index)->object.forward;
			companion_body.strafe_sign = 1.f;
			companion_body.pace = 1.f;
			companion_body.player_moving_since = NONE;
			companion_body.act_target_unit_index = NONE;
			companion_body.goal_object_index = NONE;
		}
		else if (game_time_get() >= companion_respawn_time)
		{
			companion_unit_index = companion_spawn(player_unit_index);
		}
	}

	if (companion_unit_index != NONE)
	{
		companion_look_around(companion_unit_index, player_unit_index);
		companion_weapon_count = companion_look_for(companion_unit_index, _object_mask_weapon, COMPANION_WEAPON_DISTANCE, companion_weapons);
		companion_vehicle_count = companion_look_for(companion_unit_index, _object_mask_vehicle, COMPANION_VEHICLE_DISTANCE, companion_vehicles);
		companion_catch_up(companion_unit_index, player_unit_index);
		companion_control(companion_unit_index, player_unit_index);
	}
	companion_send_events(player_unit_index);
	if (game_time_get() % COMPANION_STATE_TICKS == 0)
		companion_send_state(player_unit_index);
}
