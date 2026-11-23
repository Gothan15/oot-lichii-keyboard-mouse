/* Mouse capture for the "native" mouse camera.

   In native mode the game reads the real mouse (Lichii setting mouse_enabled=true). Lichii gives it the pointer
   position inside the picture, so on its own the camera would stop turning at the edge of the picture.
   Once the game window is in front and the pointer is over the picture, and until the game loses focus, this helper:
     - keeps the pointer near the middle of the picture by moving it back by exactly 512 (or 384) game pixels
       when it gets half of that away. The game patch undoes jumps of exactly that size, so the camera never
       sees them, however fast the mouse moves;
     - turns off Windows "Enhance pointer precision" (mouse acceleration) and sets the pointer speed so that one
       mouse count is one pixel of the game's 1280x720 picture: the camera then follows the hand 1:1, at any
       window size. Both are restored as soon as the game loses focus or closes, and are never written to the
       Windows settings (the next start also restores them if a crash interrupted it).
   If a very fast move (or a busy PC) gets the pointer out of the picture before the helper sees it, the helper
   brings it straight back instead of letting go: otherwise Lichii would hold the game's mouse at the edge of the
   picture and the camera would stop as if against an invisible wall.
   It never hides, clips or locks the pointer: Lichii hides it itself once Ctrl+F9 is pressed in the game, and
   Alt+Tab (or any other window coming in front) always gives it back. */
#pragma once
#include <windows.h>
#include <wchar.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winmm.lib")

typedef struct { DWORD pid; HWND top; } McFind;

/* Is pid an emulator of the Lichii family (lichii, eden, yuzu and their forks)? */
static BOOL mc_is_emulator(DWORD pid) {
    static const wchar_t *names[] = {L"lichii", L"fritchii", L"eden", L"yuzu", L"citron", L"sudachi", L"suyu",
                                     L"torzu", L"eglantine"};
    wchar_t path[MAX_PATH];
    DWORD n = MAX_PATH;
    BOOL ok = FALSE;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return FALSE;
    if (QueryFullProcessImageNameW(p, 0, path, &n)) {
        wchar_t *base = wcsrchr(path, L'\\');
        base = base ? base + 1 : path;
        _wcslwr_s(base, wcslen(base) + 1);
        for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++)
            if (wcsncmp(base, names[i], wcslen(names[i])) == 0) ok = TRUE;
    }
    CloseHandle(p);
    return ok;
}

/* The emulator shows the game's name in its window title ("Ocarina", or "オカリナ" in Japanese). */
static BOOL mc_title_is_game(HWND h) {
    wchar_t title[512];
    return GetWindowTextW(h, title, 512) && (wcsstr(title, L"Ocarina") || wcsstr(title, L"\x30AA\x30AB\x30EA\x30CA"));
}

/* Every emulator of the family is a Qt program (window class "Qt5..."/"Qt6..."), whatever its exe is called;
   a browser tab with the game's name is not (Chrome_WidgetWin_1, MozillaWindowClass...). */
static BOOL mc_is_qt_window(HWND h) {
    wchar_t cls[64];
    return GetClassNameW(h, cls, 64) && wcsncmp(cls, L"Qt", 2) == 0;
}

static BOOL CALLBACK mc_find_top(HWND h, LPARAM lp) {
    McFind *f = (McFind *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    /* the game window: an emulator window whose title has the game's name */
    if ((f->pid == 0 || pid == f->pid) && IsWindowVisible(h) && mc_title_is_game(h) &&
        (f->pid != 0 || mc_is_qt_window(h) || mc_is_emulator(pid))) {
        f->top = h;
        return FALSE;
    }
    return TRUE;
}

typedef struct { HWND best; LONG area; } McChild;

static BOOL CALLBACK mc_largest_child(HWND h, LPARAM lp) {
    McChild *c = (McChild *)lp;
    RECT r;
    if (IsWindowVisible(h) && GetWindowRect(h, &r)) {
        LONG a = (r.right - r.left) * (r.bottom - r.top);
        if (a > c->area) { c->area = a; c->best = h; }
    }
    return TRUE;
}

/* ---- the emulator's "Aspect Ratio" setting: the game picture (and the mouse area the game sees) follows it ---- */

/* height / width of the picture; 0 = Stretch (the whole render area). Lichii: 0 16:9, 1 4:3, 2 21:9, 3 16:10, 4 Stretch */
static double mc_aspect = 9.0 / 16.0;
static wchar_t mc_config_dir[MAX_PATH] = L"";   /* Lichii's data folder (has config\qt-config.ini); empty = guess */

static int mc_read_aspect_setting(const wchar_t *ini, int *value) {
    FILE *f = _wfopen(ini, L"r");
    char line[512];
    int found = 0;
    if (!f) return 0;
    while (fgets(line, sizeof(line), f))
        if (strncmp(line, "aspect_ratio=", 13) == 0) { *value = atoi(line + 13); found = 1; }
    fclose(f);
    return found;
}

static int mc_try_folder(const wchar_t *dir, int *value) {
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%ls\\config\\custom\\0400486024DEC000.ini", dir);   /* per-game setting first */
    if (mc_read_aspect_setting(p, value)) return 1;
    swprintf(p, MAX_PATH, L"%ls\\config\\qt-config.ini", dir);
    return mc_read_aspect_setting(p, value);
}

/* OoT-MouseCapture.exe sits in the emulator's data folder (the installers put it there); else portable user\,
   or %APPDATA%\lichii / %APPDATA%\fritchii */
static void mc_load_aspect(void) {
    wchar_t dir[MAX_PATH], tmp[MAX_PATH];
    int v = 0, ok = 0;
    if (mc_config_dir[0]) ok = mc_try_folder(mc_config_dir, &v);
    if (!ok && GetModuleFileNameW(NULL, dir, MAX_PATH)) {
        wchar_t *s = wcsrchr(dir, L'\\');
        if (s) *s = 0;
        ok = mc_try_folder(dir, &v);
        if (!ok) { swprintf(tmp, MAX_PATH, L"%ls\\user", dir); ok = mc_try_folder(tmp, &v); }
    }
    if (!ok && GetEnvironmentVariableW(L"APPDATA", dir, MAX_PATH)) {
        swprintf(tmp, MAX_PATH, L"%ls\\lichii", dir);
        ok = mc_try_folder(tmp, &v);
        if (!ok) { swprintf(tmp, MAX_PATH, L"%ls\\fritchii", dir); ok = mc_try_folder(tmp, &v); }
    }
    static const double aspects[] = {9.0 / 16.0, 3.0 / 4.0, 9.0 / 21.0, 10.0 / 16.0, 0.0};
    mc_aspect = (ok && v >= 0 && v <= 4) ? aspects[v] : 9.0 / 16.0;
}

/* mouse_step in OoT-Keyboard-Mouse-Settings.ini (default 0.5): game pixels per mouse count. Below 1 a fast hand moves
   the pointer less between two position updates of the emulator, so the game patch can follow it; Apply-Settings
   raises the degrees per pixel by the same factor, so the camera speed does not change. */
static double mc_step = 0.5;

static int mc_read_step(const wchar_t *dir) {
    wchar_t p[MAX_PATH];
    char line[512];
    int found = 0;
    swprintf(p, MAX_PATH, L"%ls\\OoT-Keyboard-Mouse-Settings.ini", dir);
    FILE *f = _wfopen(p, L"r");
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        char *s = line, *semi = strchr(line, ';');
        double v;
        if (semi) *semi = 0;
        while (*s == ' ' || *s == '\t') s++;
        if (strncmp(s, "mouse_step", 10) != 0) continue;
        s += 10;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '=' && sscanf(s + 1, "%lf", &v) == 1 && v >= 0.125 && v <= 1.0) { mc_step = v; found = 1; }
    }
    fclose(f);
    return found;
}

static void mc_load_step(void) {
    wchar_t dir[MAX_PATH], tmp[MAX_PATH];
    mc_step = 0.5;
    if (mc_config_dir[0] && mc_read_step(mc_config_dir)) return;
    if (GetModuleFileNameW(NULL, dir, MAX_PATH)) {
        wchar_t *s = wcsrchr(dir, L'\\');
        if (s) *s = 0;
        if (mc_read_step(dir)) return;
        swprintf(tmp, MAX_PATH, L"%ls\\user", dir);
        if (mc_read_step(tmp)) return;
    }
    if (GetEnvironmentVariableW(L"APPDATA", dir, MAX_PATH)) {
        swprintf(tmp, MAX_PATH, L"%ls\\lichii", dir);
        if (mc_read_step(tmp)) return;
        swprintf(tmp, MAX_PATH, L"%ls\\fritchii", dir);
        mc_read_step(tmp);
    }
}

/* OoT-MouseCapture-log.txt next to the exe: what the helper saw, so a problem can be diagnosed from the file. */
static FILE *mc_file = NULL;

static void mc_flog(const char *fmt, ...) {
    SYSTEMTIME t;
    va_list ap;
    if (!mc_file) return;
    GetLocalTime(&t);
    fprintf(mc_file, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_start(ap, fmt);
    vfprintf(mc_file, fmt, ap);
    va_end(ap);
    fputc('\n', mc_file);
    fflush(mc_file);
}

static void mc_logfile_open(void) {
    wchar_t p[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA ad;
    if (mc_file || !GetModuleFileNameW(NULL, p, MAX_PATH)) return;
    wchar_t *s = wcsrchr(p, L'\\');
    if (!s) return;
    wcscpy(s + 1, L"OoT-MouseCapture-log.txt");
    int big = GetFileAttributesExW(p, GetFileExInfoStandard, &ad) && ad.nFileSizeLow > 300000;   /* start over past 300 KB */
    mc_file = _wfopen(p, big ? L"w" : L"a");
}

/* The picture inside the render area, exactly like the emulator lays it out (framebuffer_layout.cpp). */
static void mc_fit_aspect(const RECT *r, double aspect, RECT *out) {
    LONG w = r->right - r->left, h = r->bottom - r->top, pw = w, ph = h;
    if (aspect > 0) {
        if ((double)h / w > aspect) ph = (LONG)(w * aspect + 0.5);   /* window taller than the picture: bars above and below */
        else pw = (LONG)(h / aspect + 0.5);                          /* wider: bars left and right */
    }
    out->left = r->left + (w - pw) / 2;
    out->top = r->top + (h - ph) / 2;
    out->right = out->left + pw;
    out->bottom = out->top + ph;
}

/* Screen rectangle of the game picture: the emulator draws into its largest child window. */
static BOOL mc_picture_rect(HWND top, RECT *out) {
    McChild c = {NULL, 0};
    RECT r, cr;
    EnumChildWindows(top, mc_largest_child, (LPARAM)&c);
    if (!(c.best && GetWindowRect(c.best, &r) && r.right - r.left > 64)) {
        if (!GetClientRect(top, &cr)) return FALSE;
        POINT a = {cr.left, cr.top}, b = {cr.right, cr.bottom};
        ClientToScreen(top, &a);
        ClientToScreen(top, &b);
        SetRect(&r, a.x, a.y, b.x, b.y);
    }
    if (r.right - r.left < 64 || r.bottom - r.top < 36) return FALSE;
    mc_fit_aspect(&r, mc_aspect, out);
    return TRUE;
}

/* Work in real screen pixels, like the emulator (otherwise Windows display scaling of 125 % / 150 % would
   round the positions and the clock-hand jumps would no longer be exact). */
static void mc_dpi_aware(void) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    typedef BOOL(WINAPI * SetCtx)(HANDLE);
    SetCtx set = u ? (SetCtx)GetProcAddress(u, "SetProcessDpiAwarenessContext") : NULL;
    if (!(set && set((HANDLE)-4))) SetProcessDPIAware();   /* -4 = per-monitor aware v2 */
}

/* ---- Windows pointer speed and acceleration, changed only while the game is in front ---- */

/* Pointer speed 1..20 -> pixels moved per mouse count (acceleration off). */
static const double mc_speed_mult[21] = {0, 1.0 / 32, 1.0 / 16, 1.0 / 8, 2.0 / 8, 3.0 / 8, 4.0 / 8, 5.0 / 8,
                                         6.0 / 8, 7.0 / 8, 1.0, 1.25, 1.5, 1.75, 2.0, 2.25, 2.5, 2.75, 3.0,
                                         3.25, 3.5};

typedef struct { int saved; int speed; int mouse[3]; } McPointer;
static McPointer mc_orig;

static void mc_restore_file(wchar_t *path) {
    wchar_t dir[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) GetTempPathW(MAX_PATH, dir);
    swprintf(path, MAX_PATH, L"%ls\\OoT-MouseCapture-restore.txt", dir);
}

static void mc_pointer_restore(void) {
    wchar_t path[MAX_PATH];
    if (mc_orig.saved) {
        SystemParametersInfoW(SPI_SETMOUSE, 0, mc_orig.mouse, 0);
        SystemParametersInfoW(SPI_SETMOUSESPEED, 0, (PVOID)(INT_PTR)mc_orig.speed, 0);
        mc_orig.saved = 0;
    }
    mc_restore_file(path);
    DeleteFileW(path);
}

/* A previous run that ended without restoring (crash) left its saved values: put them back. */
static void mc_pointer_recover(void) {
    wchar_t path[MAX_PATH];
    mc_restore_file(path);
    FILE *f = _wfopen(path, L"r");
    if (!f) return;
    McPointer p = {0};
    if (fscanf(f, "%d %d %d %d", &p.speed, &p.mouse[0], &p.mouse[1], &p.mouse[2]) == 4 && p.speed >= 1 &&
        p.speed <= 20) {
        SystemParametersInfoW(SPI_SETMOUSE, 0, p.mouse, 0);
        SystemParametersInfoW(SPI_SETMOUSESPEED, 0, (PVOID)(INT_PTR)p.speed, 0);
    }
    fclose(f);
    DeleteFileW(path);
}

static int mc_speed_for(LONG picture_width) {
    double want = (double)picture_width / 1280.0 * mc_step;   /* one count = mc_step pixels of the 1280-wide picture */
    int best = 10;
    for (int s = 1; s <= 20; s++)
        if (fabs(mc_speed_mult[s] - want) < fabs(mc_speed_mult[best] - want)) best = s;
    return best;
}

static void mc_pointer_apply(LONG picture_width) {
    wchar_t path[MAX_PATH];
    if (!mc_orig.saved) {
        if (!SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &mc_orig.speed, 0) ||
            !SystemParametersInfoW(SPI_GETMOUSE, 0, mc_orig.mouse, 0))
            return;
        mc_restore_file(path);
        FILE *f = _wfopen(path, L"w");
        if (f) {
            fprintf(f, "%d %d %d %d\n", mc_orig.speed, mc_orig.mouse[0], mc_orig.mouse[1], mc_orig.mouse[2]);
            fclose(f);
        }
        mc_orig.saved = 1;
    }
    int noaccel[3] = {mc_orig.mouse[0], mc_orig.mouse[1], 0};
    SystemParametersInfoW(SPI_SETMOUSE, 0, noaccel, 0);   /* last argument 0: not saved to the user's settings */
    int speed = mc_speed_for(picture_width);
    SystemParametersInfoW(SPI_SETMOUSESPEED, 0, (PVOID)(INT_PTR)speed, 0);
    mc_flog("pointer: picture %ld px wide, mouse_step %.3f: Windows pointer speed %d (%.3f screen px per count), "
            "was %d with acceleration %d/%d/%d", (long)picture_width, mc_step, speed, mc_speed_mult[speed], mc_orig.speed,
            mc_orig.mouse[0], mc_orig.mouse[1], mc_orig.mouse[2]);
}

/* ---- main loop ---- */

#define MC_WRAP_X 1024  /* game pixels; must match WRAP_X / WRAP_Y in the game patch (cave.c) */
#define MC_WRAP_Y 576

static volatile LONG mc_active = 0;
static LONG mc_active_width = 0;
static void (*mc_log)(const wchar_t *msg) = NULL;   /* status lines; the standalone exe prints them */

static void mc_say(const wchar_t *msg) {
    if (mc_log) mc_log(msg);
    mc_flog("%ls", msg);
}

static void mc_release(void) {
    if (InterlockedExchange(&mc_active, 0)) mc_pointer_restore();
    mc_active_width = 0;
}

/* Clock hand: a pointer more than half a period (512 x 288 pixels of the game picture) from the middle of picture
   r goes back by whole periods, so it ends within half a period of the middle, inside the picture. The game patch
   undoes jumps of exactly one period (cave.c, WRAP_X / WRAP_Y), so the camera never sees them. A pointer that got
   out of the picture comes back the same way. Returns TRUE when *p changed. */
static BOOL mc_wrap(const RECT *r, POINT *p) {
    LONG cx = (r->left + r->right) / 2, cy = (r->top + r->bottom) / 2;
    LONG wrap_x = (LONG)((r->right - r->left) * MC_WRAP_X / 1280.0 + 0.5);
    LONG wrap_y = (LONG)((r->bottom - r->top) * MC_WRAP_Y / 720.0 + 0.5);
    LONG dx = p->x - cx, dy = p->y - cy;
    if (wrap_x < 2 || wrap_y < 2) return FALSE;
    while (dx > wrap_x / 2) dx -= wrap_x;
    while (dx < -(wrap_x / 2)) dx += wrap_x;
    while (dy > wrap_y / 2) dy -= wrap_y;
    while (dy < -(wrap_y / 2)) dy += wrap_y;
    if (cx + dx == p->x && cy + dy == p->y) return FALSE;
    p->x = cx + dx;
    p->y = cy + dy;
    return TRUE;
}

/* Windows 11 slows the timers of background programs (EcoQoS); the helper runs minimized, so it opts out, and it
   waits with a high-resolution timer (about 0.5 ms) where Windows has one, else Sleep(1). */
typedef struct { ULONG Version; ULONG ControlMask; ULONG StateMask; } McPowerThrottling;
static HANDLE mc_hr_timer = NULL;

static void mc_scheduling(void) {
    typedef BOOL (WINAPI *SetProcInfo)(HANDLE, int, LPVOID, DWORD);
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    SetProcInfo spi = k ? (SetProcInfo)(void *)GetProcAddress(k, "SetProcessInformation") : NULL;
    McPowerThrottling st = {1, 0x1 | 0x4, 0};   /* EXECUTION_SPEED | IGNORE_TIMER_RESOLUTION: both off */
    if (spi) spi(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &st, sizeof st);
    if (!mc_hr_timer) mc_hr_timer = CreateWaitableTimerExW(NULL, NULL, 0x2 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
}

static void mc_tick(void) {
    LARGE_INTEGER due;
    due.QuadPart = -5000;   /* 0.5 ms, relative */
    if (mc_hr_timer && SetWaitableTimer(mc_hr_timer, &due, 0, NULL, NULL, FALSE)) {
        WaitForSingleObject(mc_hr_timer, 5);
        return;
    }
    Sleep(1);
}

/* Runs until process `pid` exits (pid 0: until `*stop` becomes non-zero). */
static void mc_run(DWORD pid, volatile LONG *stop) {
    HANDLE proc = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : NULL;
    if (pid && !proc) return;
    mc_logfile_open();
    mc_dpi_aware();
    mc_load_aspect();
    mc_load_step();
    mc_scheduling();
    mc_flog("---- helper started (game pid %lu): mouse_step %.3f, picture aspect %.4f, high-resolution timer %s ----",
            (unsigned long)pid, mc_step, mc_aspect, mc_hr_timer ? "yes" : "no");
    mc_pointer_recover();
    timeBeginPeriod(1);
    /* The emulator keeps every CPU core busy. At normal priority Windows can leave this 1 ms loop asleep long
       enough for a fast move to carry the pointer out of the picture; it only sleeps, so this costs nothing. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    McFind f = {pid, NULL};
    DWORD last_search = 0, rect_time = 0, escapes = 0;
    HWND rect_of = NULL;
    RECT r = {0};
    BOOL have_rect = FALSE;
    /* written to the log every 5 s while active: the slowest look and the biggest pointer move between two looks */
    LARGE_INTEGER qf, qlast = {0}, qnow;
    QueryPerformanceFrequency(&qf);
    DWORD st_t0 = 0, st_wraps = 0;
    double st_maxgap = 0;
    LONG st_mx = 0, st_my = 0;
    POINT lastpt = {0};
    BOOL have_last = FALSE;
    for (;;) {
        if (proc && WaitForSingleObject(proc, 0) == WAIT_OBJECT_0) break;
        if (stop && *stop) break;
        DWORD now = GetTickCount();
        if (!f.top || !IsWindow(f.top)) {
            f.top = NULL;
            if (now - last_search > 500) {
                last_search = now;
                EnumWindows(mc_find_top, (LPARAM)&f);
                if (f.top) mc_load_aspect();               /* the game (re)started: read Aspect Ratio again */
            }
        }
        /* The game is in front: a window of the emulator with the game's name (the main window, or the separate
           game window when Lichii's "Single Window Mode" is off), or something inside it. */
        HWND fg = GetForegroundWindow(), root = fg ? GetAncestor(fg, GA_ROOT) : NULL;
        DWORD fg_pid = 0, game_pid = pid;
        if (root) GetWindowThreadProcessId(root, &fg_pid);
        if (!game_pid && f.top) GetWindowThreadProcessId(f.top, &game_pid);
        BOOL focused = f.top && root && fg_pid == game_pid && !IsIconic(root) &&
                       (root == f.top || mc_title_is_game(root));
        if (focused && (root != rect_of || now - rect_time > 200)) {
            have_rect = mc_picture_rect(root, &r);
            rect_of = root;
            rect_time = now;
        }
        POINT pt = {0};
        if (!focused || !have_rect || !GetCursorPos(&pt)) {   /* another window in front: leave the pointer alone */
            if (mc_active) mc_say(L"Paused: the game is not in front (the mouse is normal again).");
            have_last = FALSE;
            mc_release();
            rect_of = NULL;
            Sleep(f.top ? 20 : 250);
            continue;
        }
        if (!mc_active) {
            if (!PtInRect(&r, pt)) {   /* starts once the pointer is over the picture (not the menu or title bar) */
                Sleep(10);
                continue;
            }
            InterlockedExchange(&mc_active, 1);
            mc_say(L"Active: the mouse turns the camera.");
        }
        LONG width = r.right - r.left;
        if (width != mc_active_width) {
            mc_pointer_apply(width);   /* again when the window changes size */
            mc_active_width = width;
        }
        BOOL escaped = !PtInRect(&r, pt);
        QueryPerformanceCounter(&qnow);
        if (have_last) {
            double gap = (double)(qnow.QuadPart - qlast.QuadPart) * 1000.0 / (double)qf.QuadPart;
            LONG mx = labs(pt.x - lastpt.x), my = labs(pt.y - lastpt.y);
            if (gap > st_maxgap) st_maxgap = gap;
            if (mx > st_mx) st_mx = mx;
            if (my > st_my) st_my = my;
        } else {
            st_t0 = now;
        }
        qlast = qnow;
        if (mc_wrap(&r, &pt)) {
            SetCursorPos(pt.x, pt.y);
            st_wraps++;
            if (escaped) {
                if (escapes < 30 || escapes % 100 == 0)
                    mc_flog("pointer was outside the picture; brought back to (%ld,%ld)", (long)pt.x, (long)pt.y);
                if (escapes++ % 50 == 0)
                    mc_say(L"The pointer got out of the picture (very fast move or busy PC): brought it back.");
            }
        }
        lastpt = pt;
        have_last = TRUE;
        if (now - st_t0 >= 5000) {
            if (st_wraps || st_mx > 4 || st_my > 4)
                mc_flog("5 s: %lu jumps back, slowest look %.1f ms, biggest pointer move per look %ld x %ld screen px, "
                        "%lu escapes in total", (unsigned long)st_wraps, st_maxgap, (long)st_mx, (long)st_my,
                        (unsigned long)escapes);
            st_t0 = now;
            st_wraps = 0;
            st_maxgap = 0;
            st_mx = st_my = 0;
        }
        mc_tick();
    }
    mc_release();
    timeEndPeriod(1);
    if (proc) CloseHandle(proc);
}
