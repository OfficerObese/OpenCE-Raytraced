/*
XGPU_HOST.H

The renderer process, as the game's side of the D3D12 renderer sees it:
started, fed and stopped by win32_renderer_host.c, which alone of these files
sees the Windows SDK (the others see the Xbox SDK's names), so everything
here is plain C.
*/

#ifndef __XGPU_HOST_H
#define __XGPU_HOST_H

#include <stddef.h>

#include "xgpu_protocol.h"

/* starts the renderer process for the window (an HWND), whose drawable size
is width by height; FALSE with the reason in error */
int xgpu_host_start(void *window, unsigned long width, unsigned long height, char *error, size_t error_size);
/* the shared section's header, NULL before the renderer has started */
struct xgpu_header *xgpu_host_header(void);
struct xgpu_results *xgpu_host_results(void);
unsigned char *xgpu_host_ring(void);
/* the renderer has read messages (room in the ring) or the timeout passed;
FALSE if the renderer has ended */
int xgpu_host_wait_space(unsigned long milliseconds);
/* the game has written messages */
void xgpu_host_signal_data(void);
/* waits until the renderer can take another frame (at most
XGPU_FRAMES_IN_FLIGHT behind); FALSE if the renderer has ended */
int xgpu_host_wait_frame(void);
/* the renderer has ended: its exit code, or -1 while it runs */
long xgpu_host_exit_code(void);
/* tells the renderer to finish (the caller has sent _xgpu_op_quit) and
waits for it, at most the milliseconds given */
void xgpu_host_stop(unsigned long milliseconds);

#endif
