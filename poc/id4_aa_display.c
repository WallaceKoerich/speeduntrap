/* Snapshot of ConnectedDrive output/id4_aa_display.c for the radar overlay.
 * Canonical AA viewer still lives in the ConnectedDrive tree. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "id4_aa_display.h"

typedef struct _screen_window *screen_window_t;
extern int screen_set_window_property_iv(screen_window_t win, int pname, const int *param);
extern int screen_set_window_property_cv(screen_window_t win, int pname, int len, const char *param);

extern void kdInitializeNV(void);
extern void kdSetZoneNV(KDWindow *window, int zone);

#define MAX_WAL_IDS 64
#define HMI_WAL_FALLBACK 256

static volatile sig_atomic_t g_hmi_id = -1;
static volatile sig_atomic_t g_hmi_hidden;

static const char *k_shader_dirs[] = {
    "/opt/hmi/data/ro/common/shader",
    "/usr/Hmi/data/ro/common/shader",
    "/fs/sda0/opt/hmi/data/ro/common/shader",
    "/fs/sda0/Hmi/data/ro/common/shader",
};

void aa_walcontrol(int id, const char *args)
{
    char cmd[160];

    snprintf(cmd, sizeof(cmd), "WalControl %d %s", id, args);
    printf("%s\n", cmd);
    system(cmd);
}

/* Window ids from "WalControl info"; the KD window id property reads -1. */
static int wal_window_ids(int *ids, int max)
{
    FILE *p = popen("WalControl info", "r");
    char line[256];
    int n = 0;

    if (p == NULL)
        return 0;
    while (fgets(line, sizeof(line), p) != NULL) {
        const char *s = strstr(line, "Window id:");

        if (s != NULL && n < max)
            ids[n++] = atoi(s + 10);
    }
    pclose(p);
    return n;
}

static int find_new_window_id(const int *before, int n_before)
{
    int after[MAX_WAL_IDS];
    int n_after = wal_window_ids(after, MAX_WAL_IDS);
    int found = -1;
    int i;
    int j;

    for (i = 0; i < n_after; i++) {
        for (j = 0; j < n_before; j++) {
            if (after[i] == before[j])
                break;
        }
        if (j == n_before && found < 0)
            found = after[i];
    }
    return found;
}

static int find_hmi_window(void)
{
    FILE *p = popen("WalControl info", "r");
    char line[256];
    int last_id = -1;
    int named = -1;
    int saw_256 = 0;

    if (p == NULL)
        return HMI_WAL_FALLBACK;
    while (fgets(line, sizeof(line), p) != NULL) {
        const char *s = strstr(line, "Window id:");

        if (s != NULL) {
            last_id = atoi(s + 10);
            if (last_id == HMI_WAL_FALLBACK)
                saw_256 = 1;
            continue;
        }
        s = strstr(line, "Window name:");
        if (s != NULL && last_id >= 0 &&
            (strstr(s, "Hmi") != NULL || strstr(s, "HMI") != NULL)) {
            named = last_id;
            break;
        }
    }
    pclose(p);
    if (named >= 0)
        return named;
    if (saw_256)
        return HMI_WAL_FALLBACK;
    return HMI_WAL_FALLBACK;
}

void aa_hmi_prefetch(void)
{
    if (g_hmi_id < 0)
        g_hmi_id = find_hmi_window();
    if (g_hmi_id < 0)
        g_hmi_id = HMI_WAL_FALLBACK;
    printf("HMI WalServer window %d (prefetch, overlay still hidden)\n", (int)g_hmi_id);
}

void aa_hmi_hide(void)
{
    if (g_hmi_hidden)
        return;
    /* Never popen "WalControl info" after our overlay is visible — WalServer
     * serializes and that is the hang that ate Ctrl+C. 256 is the NBT HMI. */
    if (g_hmi_id < 0)
        g_hmi_id = HMI_WAL_FALLBACK;
    aa_walcontrol((int)g_hmi_id, "visible 0");
    g_hmi_hidden = 1;
    printf("HMI window %d hidden while overlay is on screen\n", (int)g_hmi_id);
}

void aa_hmi_restore(void)
{
    int id;

    if (!g_hmi_hidden)
        return;
    id = (int)g_hmi_id;
    g_hmi_hidden = 0;
    if (id >= 0)
        aa_walcontrol(id, "visible 1");
}

void aa_hmi_force_restore(void)
{
    int id = (int)g_hmi_id;

    g_hmi_hidden = 0;
    if (id < 0)
        id = HMI_WAL_FALLBACK;
    aa_walcontrol(id, "visible 1");
    if (id != HMI_WAL_FALLBACK)
        aa_walcontrol(HMI_WAL_FALLBACK, "visible 1");
}

int aa_hmi_window_id(void)
{
    return (int)g_hmi_id;
}

static unsigned char *read_shader_file(const char *name, long *len)
{
    char path[256];
    unsigned int i;

    for (i = 0; i < sizeof(k_shader_dirs) / sizeof(k_shader_dirs[0]); i++) {
        FILE *f;
        unsigned char *buf;

        snprintf(path, sizeof(path), "%s/%s", k_shader_dirs[i], name);
        f = fopen(path, "rb");
        if (f == NULL)
            continue;
        fseek(f, 0, SEEK_END);
        *len = ftell(f);
        fseek(f, 0, SEEK_SET);
        buf = (unsigned char *)malloc((size_t)*len);
        if (buf != NULL && fread(buf, 1, (size_t)*len, f) != (size_t)*len) {
            free(buf);
            buf = NULL;
        }
        fclose(f);
        if (buf != NULL)
            return buf;
    }
    printf("shader %s not found\n", name);
    return NULL;
}

static GLuint load_shader_binary(GLenum type, const char *name)
{
    GLint formats[8];
    GLint n_formats = 0;
    GLuint s;
    long len = 0;
    unsigned char *buf = read_shader_file(name, &len);
    int i;

    if (buf == NULL)
        return 0;
    glGetIntegerv(GL_NUM_SHADER_BINARY_FORMATS, &n_formats);
    if (n_formats > 8)
        n_formats = 8;
    glGetIntegerv(GL_SHADER_BINARY_FORMATS, formats);
    s = glCreateShader(type);
    for (i = 0; i < n_formats; i++) {
        while (glGetError() != GL_NO_ERROR)
            ;
        glShaderBinary(1, &s, (GLenum)formats[i], buf, (GLsizei)len);
        if (glGetError() == GL_NO_ERROR) {
            free(buf);
            return s;
        }
    }
    printf("shader %s: no binary format accepted (%d offered)\n", name, n_formats);
    free(buf);
    glDeleteShader(s);
    return 0;
}

static int build_program(aa_display *d)
{
    static const GLfloat identity[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    GLuint vs = load_shader_binary(GL_VERTEX_SHADER, "ContentShader_vertex.shd");
    GLuint fs = load_shader_binary(GL_FRAGMENT_SHADER, "ContentShader_fragment.shd");
    GLint linked = 0;
    char log[512];

    if (vs == 0 || fs == 0)
        return -1;
    d->program = glCreateProgram();
    glAttachShader(d->program, vs);
    glAttachShader(d->program, fs);
    glBindAttribLocation(d->program, 0, "a_Position");
    glBindAttribLocation(d->program, 1, "a_TextureCoordinate");
    glLinkProgram(d->program);
    glGetProgramiv(d->program, GL_LINK_STATUS, &linked);
    if (!linked) {
        log[0] = '\0';
        glGetProgramInfoLog(d->program, (GLsizei)sizeof(log), NULL, log);
        printf("program link failed: %s\n", log);
        return -1;
    }
    glUseProgram(d->program);
    glUniformMatrix4fv(glGetUniformLocation(d->program, "u_MVPMatrix"), 1, GL_FALSE, identity);
    glUniform1f(glGetUniformLocation(d->program, "u_ConstAlpha"), 1.0f);
    glUniform1i(glGetUniformLocation(d->program, "tex0"), 0);
    return 0;
}

int aa_display_open(aa_display *d, int width, int height, GLenum format, GLenum type)
{
    /* ContentShader flips v (1 - v), so v = 1 is the top of the image. */
    static const GLfloat quad[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };
    EGLint attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };
    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLConfig config;
    EGLint num_configs = 0;
    screen_window_t native_win = NULL;
    KDint32 kd_size[2];
    int visible = 1;
    int pipeline = 2;
    int usage = 1 << 8;
    int size[2];
    int before[MAX_WAL_IDS];
    int n_before;
    char args[64];

    memset(d, 0, sizeof(*d));
    d->width = width;
    d->height = height;
    d->format = format;
    d->type = type;
    d->wal_id = -1;
    kd_size[0] = size[0] = width;
    kd_size[1] = size[1] = height;

    n_before = wal_window_ids(before, MAX_WAL_IDS);

    kdInitializeNV();
    d->egl = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(d->egl, NULL, NULL);
    if (!eglChooseConfig(d->egl, attribs, &config, 1, &num_configs) || num_configs < 1) {
        printf("no EGL config\n");
        return -1;
    }
    d->window = kdCreateWindow(d->egl, config, NULL);
    if (d->window == NULL) {
        printf("kdCreateWindow failed\n");
        return -1;
    }
    /* The size has to be set before realize, or the surface stays square. */
    kdSetWindowPropertyiv(d->window, KD_WINDOWPROPERTY_SIZE, kd_size);
    if (kdRealizeWindow(d->window, (EGLNativeWindowType *)&native_win) != 0) {
        printf("kdRealizeWindow failed\n");
        return -1;
    }
    screen_set_window_property_iv(native_win, 51, &visible);
    screen_set_window_property_iv(native_win, 35, &pipeline);
    screen_set_window_property_iv(native_win, 48, &usage);
    screen_set_window_property_iv(native_win, 40, size);
    screen_set_window_property_cv(native_win, 21, 4, "fb1");
    screen_set_window_property_cv(native_win, 7, 12, "framebuffer1");
    kdSetZoneNV(d->window, 1);

    d->surface = eglCreateWindowSurface(d->egl, config, (EGLNativeWindowType)native_win, NULL);
    d->context = eglCreateContext(d->egl, config, EGL_NO_CONTEXT, ctx_attribs);
    if (d->surface == EGL_NO_SURFACE || d->context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(d->egl, d->surface, d->surface, d->context)) {
        printf("EGL surface or context failed\n");
        return -1;
    }
    if (eglSwapInterval(d->egl, 0) == EGL_TRUE)
        printf("display: swap interval 0\n");
    if (build_program(d) != 0)
        return -1;

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad + 2);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    glGenTextures(1, &d->texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, d->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)format, width, height, 0, format, type, NULL);
    glViewport(0, 0, width, height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(d->egl, d->surface);

    d->wal_id = find_new_window_id(before, n_before);
    printf("display %dx%d, WalServer window %d\n", width, height, d->wal_id);
    if (d->wal_id >= 0) {
        snprintf(args, sizeof(args), "size %d %d", width, height);
        aa_walcontrol(d->wal_id, args);
        snprintf(args, sizeof(args), "move %d 0", (1280 - width) / 2);
        aa_walcontrol(d->wal_id, args);
        /* NBT WalServer: lower order is in front of HMI (256). */
        snprintf(args, sizeof(args), "zorder %d 1 256 256", d->wal_id);
        printf("WalControl %s\n", args);
        {
            char cmd[80];

            snprintf(cmd, sizeof(cmd), "WalControl %s", args);
            system(cmd);
        }
    }
    return 0;
}

void aa_display_upload_rows(aa_display *d, int y, int rows, const void *pixels)
{
    if (rows <= 0)
        return;
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, d->width, rows, d->format, d->type, pixels);
}

void aa_display_present(aa_display *d)
{
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    eglSwapBuffers(d->egl, d->surface);
}

void aa_display_set_visible(aa_display *d, int visible)
{
    if (d->wal_id >= 0)
        aa_walcontrol(d->wal_id, visible ? "visible 1" : "visible 0");
}

void aa_display_close(aa_display *d)
{
    /* Hide via WalControl only. kdDestroyWindow / eglDestroy* on this HU
     * have SIGSEGV'd inside ldqnx _list_release during session teardown. */
    aa_display_set_visible(d, 0);
    d->window = NULL;
    d->surface = EGL_NO_SURFACE;
    d->context = EGL_NO_CONTEXT;
    d->egl = EGL_NO_DISPLAY;
    d->wal_id = -1;
}
