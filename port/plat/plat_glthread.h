/* plat_glthread.h - the GL thread (RALLY_GL_THREAD=1).
 *
 * plat_gl.c is included with these renames, so its bodies become the
 * implementations (plat_glimpl_*) that run on the GL thread; plat_glthread.c
 * defines the public names as wrappers that either call the implementation
 * directly (no thread) or put the call, with a copy of its data, in a ring. */
#ifndef PLAT_GLTHREAD_H
#define PLAT_GLTHREAD_H

#define plat_gfx_set_state          plat_glimpl_set_state
#define plat_gfx_reset_state        plat_glimpl_reset_state
#define plat_gfx_draw               plat_glimpl_draw
#define plat_gfx_draw_env           plat_glimpl_draw_env
#define plat_gfx_draw_indexed       plat_glimpl_draw_indexed
#define plat_gfx_read_pixel         plat_glimpl_read_pixel
#define plat_gfx_cull_mode          plat_glimpl_cull_mode
#define plat_gfx_pillar plat_glimpl_pillar
#define plat_gfx_inset              plat_glimpl_inset
#define plat_gfx_state_save         plat_glimpl_state_save
#define plat_gfx_state_restore      plat_glimpl_state_restore
#define plat_gfx_read_frame         plat_glimpl_read_frame
#define plat_gfx_clear_depth        plat_glimpl_clear_depth
#define plat_gfx_frame_tex_create   plat_glimpl_frame_tex_create
#define plat_gfx_frame_copy         plat_glimpl_frame_copy
#define plat_gfx_tex_create         plat_glimpl_tex_create
#define plat_gfx_tex_destroy        plat_glimpl_tex_destroy
#define plat_gfx_tex_update         plat_glimpl_tex_update
#define plat_gfx_tex_bgra           plat_glimpl_tex_bgra
#define plat_gfx_tex_create_bgra    plat_glimpl_tex_create_bgra
#define plat_gfx_tex_update_bgra    plat_glimpl_tex_update_bgra
#define plat_gfx_tex_smooth         plat_glimpl_tex_smooth
#define plat_gfx_tex_bind           plat_glimpl_tex_bind
#define plat_video_frame_begin      plat_glimpl_frame_begin
#define plat_video_scene_begin      plat_glimpl_scene_begin
#define plat_video_scene_end        plat_glimpl_scene_end

#endif
