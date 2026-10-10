/*
SCENE_TEST.C

Fixed scenes for comparing renderers (debug.scene_test in config.toml,
HALO_SCENE_TEST; tools/test_d3d12_parity.py): a level started with
init.txt's map_name has each cinematic skipped as soon as it can be, as the
player's accept button does, and once the player has had control, without a
break, for debug.scene_test ticks, the frame drawn is saved as the screenshot key saves
it (the data root's screenshots folder) and the game exits half a second
later. Counted in the game's ticks, not frames or seconds, so that every run
saves the same moment of the game however fast it draws; with
display.interpolation false the frame is that tick's. The game ignores the
mouse, keyboard and gamepads meanwhile (xinput_sdl.c), in the menus too.

Called from the main loop every frame, by network_test_update.
*/

#include "cseries.h"
#include "main/main.h"
#include "game/game.h"
#include "game/players.h"
#include "cutscene/cinematics.h"

#include <stdlib.h>

/* the platform layer's (port/linux/src) */
long config_integer(char const *name);
void platform_log(char const *format, ...);
void platform_screenshot_request(void);

static struct
{
	boolean checked;
	long ticks;
	boolean skipping;
	/* the game time the player took control, NONE before */
	long control_time;
	/* the game time the screenshot was asked for, NONE before */
	long screenshot_time;
} scene_test;

void scene_test_update(
	boolean main_menu_loaded)
{
	long local_player_index;
	long now;

	if (!scene_test.checked)
	{
		scene_test.checked = TRUE;
		scene_test.ticks = config_integer("debug.scene_test");
		scene_test.control_time = NONE;
		scene_test.screenshot_time = NONE;
	}
	if (scene_test.ticks <= 0 || main_menu_loaded || !game_in_progress())
		return;

	/* each cinematic skipped once, as soon as its script allows */
	if (cinematic_can_be_skipped())
	{
		if (!scene_test.skipping)
		{
			scene_test.skipping = TRUE;
			platform_log("scene test: skipping a cinematic at tick %ld", game_time_get());
			main_skip_cinematic();
		}
		return;
	}
	scene_test.skipping = FALSE;

	/* control: the player's input on, with no cinematic, no letterbox (the
	scripted sequences that are not cinematics, a10's waking, show one) and a
	unit; counted from when it last began */
	now = game_time_get();
	local_player_index = local_player_get_player_index(0);
	if (scene_test.screenshot_time == NONE &&
		(!player_input_enabled() || cinematic_in_progress() || cinematic_globals->show_letterbox ||
		cinematic_globals->letterbox_amount > 0.0f || local_player_index == NONE ||
		player_get(local_player_index)->unit_index == NONE))
	{
		scene_test.control_time = NONE;
		return;
	}
	if (scene_test.control_time == NONE)
	{
		scene_test.control_time = now;
		platform_log("scene test: the player has control at tick %ld", now);
	}
	if (scene_test.screenshot_time == NONE && now - scene_test.control_time >= scene_test.ticks)
	{
		scene_test.screenshot_time = now;
		platform_log("scene test: screenshot at tick %ld", now);
		platform_screenshot_request();
	}
	if (scene_test.screenshot_time != NONE && now - scene_test.screenshot_time >= TICKS_PER_SECOND / 2)
	{
		platform_log("scene test: done");
		exit(EXIT_SUCCESS);
	}
}
