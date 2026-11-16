#ifndef PSX_LAUNCHER_VSYNC_H
#define PSX_LAUNCHER_VSYNC_H
/* Launcher ABI is 1=on, 2=off, 3=adaptive, 4=VRR; config is 1,0,-1,2.
 * Older launchers preserve config-only VRR when their row is unchanged. */
static inline int psx_vsync_to_launcher(int value) {
    return value == 0 ? 2 : value == -1 ? 3 : value == 2 ? 4 : 1;
}
static inline int psx_vsync_from_launcher(int value, int previous) {
    (void)previous;
    return value == 2 ? 0 : value == 3 ? -1 : value == 4 ? 2 : 1;
}
#endif
