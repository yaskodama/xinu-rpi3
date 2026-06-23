/* apps/makina3d_stub.c — link shims that replace the native 3-D viewer
 * (makina3d.c, the "kernel Blender") on rpi3.  The full viewer baked a ~474 KB
 * MAKINA-7 mesh into the image and reserved ~3.3 MB of BSS (1.9 MB back buffer
 * + work arrays); rpi3 drops it entirely.  gwm.c (mouse/menu) and webactor.c
 * (HTTP mesh upload) still reference these symbols, so here they are no-ops:
 *   - hit tests miss (return -1/0) so the WM never routes into a 3-D window
 *   - load/open requests fail (return -1) so no window is created
 * Result: ~480 KB smaller image, 3.3 MB more free RAM, no 3-D code path. */
#ifdef _XINU_PLATFORM_ARM_RPI3_

void  makina_progress(int recv, int total)            { (void)recv; (void)total; }
int   makina_load_mesh(const unsigned char *d, int n) { (void)d; (void)n; return -1; }
int   makina_load_rig(const unsigned char *d, int n)  { (void)d; (void)n; return -1; }

void *makina_win_ptr(void)                            { return 0; }
int   makina_drag_active(void)                        { return 0; }
void  makina_drag_set(int on, int x, int y)           { (void)on; (void)x; (void)y; }
void  makina_drag_move(int x, int y)                  { (void)x; (void)y; }
int   makina_button_hit(int sx, int sy)               { (void)sx; (void)sy; return -1; }
void  makina_button_action(int i)                     { (void)i; }
int   makina_viewport_hit(int sx, int sy)             { (void)sx; (void)sy; return 0; }
int   makina_walk_toggle_hit(int sx, int sy)          { (void)sx; (void)sy; return 0; }
int   makina_window_open(void)                        { return -1; }
int   makina_walk_open(void)                          { return -1; }

#endif /* _XINU_PLATFORM_ARM_RPI3_ */
