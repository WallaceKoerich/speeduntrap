/*
 * CID output window for the Android Auto viewer: a KD/EGL window above
 * HmiMain3D with one texture drawn full screen through the HMI's
 * precompiled ContentShader (the GLES2 driver has no GLSL compiler).
 */

#ifndef ID4_AA_DISPLAY_H
#define ID4_AA_DISPLAY_H

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <KD/kd.h>

typedef struct aa_display {
    EGLDisplay egl;
    EGLSurface surface;
    EGLContext context;
    KDWindow *window;
    GLuint program;
    GLuint texture;
    GLenum format;
    GLenum type;
    int width;
    int height;
    int wal_id;          /* WalServer window id, -1 if not found */
} aa_display;

/* Opens a width x height window and a texture of the same size.
 * format/type: GL_RGB + GL_UNSIGNED_SHORT_5_6_5, GL_RGBA + GL_UNSIGNED_BYTE
 * or GL_LUMINANCE + GL_UNSIGNED_BYTE. Returns 0 on success. */
int aa_display_open(aa_display *d, int width, int height, GLenum format, GLenum type);

/* Uploads rows [y, y + rows) of a full-width image into the texture. */
void aa_display_upload_rows(aa_display *d, int y, int rows, const void *pixels);

void aa_display_present(aa_display *d);
void aa_display_set_visible(aa_display *d, int visible);
void aa_display_close(aa_display *d);

/* Runs "WalControl <id> <args>". */
void aa_walcontrol(int id, const char *args);

/* Hides HmiMain3D (WalServer id 256, or the window named Hmi*) so the
 * knob is not driving a visible iDrive menu under Android Auto. Restore
 * on close, atexit, or SIGINT. CCS still belongs to HMI until a PluginApp
 * takes the focused RHMI state; this only covers the CID. */
void aa_hmi_hide(void);
void aa_hmi_restore(void);
void aa_hmi_prefetch(void);
void aa_hmi_force_restore(void);
int aa_hmi_window_id(void);

#endif
