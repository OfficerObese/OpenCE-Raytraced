/*
XGPU_GL_TEXTURES.H

Texture numbers, shared by the GL texture calls carried out by the D3D12
renderer (xgpu_gl_textures.c) and the device (d3d8_d3d12.c), whose own
textures (the mip composites) are numbered from the same count.
*/

#ifndef __XGPU_GL_TEXTURES_H
#define __XGPU_GL_TEXTURES_H

unsigned long xgpu_texture_number_new(void);
/* deletes it in the renderer too */
void xgpu_texture_number_delete(unsigned long number);

#endif
