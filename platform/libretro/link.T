/* Only the libretro entry points leave the .so.
 *
 * This matters more than it looks: LÖVE statically links Lua, zlib, FreeType and
 * friends, and so does the frontend. If our copies were globally visible, the
 * dynamic linker could bind the frontend's calls to our symbols (or ours to
 * theirs) and the result is a crash deep inside unrelated code, at a point that
 * gives no hint where it came from. Keeping everything but retro_* local makes
 * that class of bug impossible rather than merely unlikely.
 */
{
	global:
		retro_api_version;
		retro_cheat_reset;
		retro_cheat_set;
		retro_deinit;
		retro_get_memory_data;
		retro_get_memory_size;
		retro_get_region;
		retro_get_system_av_info;
		retro_get_system_info;
		retro_init;
		retro_load_game;
		retro_load_game_special;
		retro_reset;
		retro_run;
		retro_serialize;
		retro_serialize_size;
		retro_set_audio_sample;
		retro_set_audio_sample_batch;
		retro_set_controller_port_device;
		retro_set_environment;
		retro_set_input_poll;
		retro_set_input_state;
		retro_set_video_refresh;
		retro_unload_game;
		retro_unserialize;
	local:
		*;
};
