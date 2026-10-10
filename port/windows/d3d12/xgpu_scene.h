/*
XGPU_SCENE.H

display.path_tracing, the platform layer's side (xgpu_scene.c): what the
Direct3D 12 device (d3d8_d3d12.c) tells it.
*/

#ifndef __XGPU_SCENE_H
#define __XGPU_SCENE_H

/* the setting, each frame (path tracing is active while it is on and the
GPU can trace rays) */
void xgpu_scene_setting(int on);
/* the 3D view is drawn, before its anti-aliasing and the HUD */
void xgpu_scene_view_end(void);

#endif
