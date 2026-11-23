/* android_main.c - the Android entry point.
 *
 * SDLActivity loads libmain.so and calls SDL_main.  There is no command line
 * and no executable folder, so this builds the arguments rally's own main()
 * (renamed rally_main for this target, see game/CMakeLists.txt) would have got:
 *
 *   <external files dir>/game   the game's files (RAL.EXE and FILES/), which
 *                               the player copies there, e.g. with adb push
 *   <external files dir>/save   var/ and savedata/
 *   <external files dir>/rally.log   stdout and stderr, which Android drops
 *
 * The game's resolution is the screen's aspect at up to 720 lines, so a
 * 20:9 phone gets 1600x720: the picture fills the screen, not a 4:3 box. */
#include <SDL.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int rally_main(int argc, char **argv);

int main(int argc, char **argv)
{
    char base[512], game[512], save[512], logp[512], win[32], hl[16];
    const char *ext, *av[16];
    int n = 0, w = 1280, h = 720, dw, dh;
    SDL_DisplayMode dm;
    FILE *f;

    (void)argc; (void)argv;
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");   /* Back = Esc */

    /* Android/media/<package>, not Android/data: a file manager (and adb, and
     * the app, with no permission) can all reach it. */
    ext = SDL_AndroidGetExternalStoragePath();
    if (ext && strstr(ext, "/Android/data/")) {
        char *d, *tail;
        size_t pre;
        d = strstr(ext, "/Android/data/");
        pre = (size_t)(d - ext);
        tail = d + strlen("/Android/data/");
        snprintf(base, sizeof base, "%.*s/Android/media/%s", (int)pre, ext, tail);
        d = strrchr(base, '/');                          /* drop "/files" */
        if (d && strcmp(d, "/files") == 0) *d = '\0';
        ext = base;
    }
    if (!ext) ext = SDL_AndroidGetInternalStoragePath();
    snprintf(game, sizeof game, "%s/game", ext);
    snprintf(save, sizeof save, "%s/save", ext);
    snprintf(logp, sizeof logp, "%s/rally.log", ext);
    freopen(logp, "w", stdout);
    dup2(fileno(stdout), 2);                 /* one file offset for both */
    setvbuf(stdout, NULL, _IOLBF, 0);

    {
        char probe[600];
        snprintf(probe, sizeof probe, "%s/RAL.EXE", game);
        f = fopen(probe, "rb");        /* fopen is the case-insensitive one */
        if (!f) {
            char msg[700];
            snprintf(msg, sizeof msg,
                     "The game's files are missing.\n\nCopy RAL.EXE and the "
                     "FILES folder from your copy of Rally Championship to:\n\n%s\n\n(%s)",
                     game, strerror(errno));
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Rally Championship",
                                     msg, NULL);
            return 1;
        }
        fclose(f);
    }

    if (SDL_Init(SDL_INIT_VIDEO) == 0 && SDL_GetDesktopDisplayMode(0, &dm) == 0) {
        dw = dm.w > dm.h ? dm.w : dm.h;      /* landscape, whatever it is now */
        dh = dm.w > dm.h ? dm.h : dm.w;
        h = dh < 720 ? dh : 720;
        w = (int)((long)h * dw / dh) & ~1;
    }
    snprintf(win, sizeof win, "%dx%d", w, h);
    snprintf(hl, sizeof hl, "%d", h);
    printf("android: game %s, %s, display mode %s\n", game, win, SDL_GetError());

    av[n++] = "rally";
    av[n++] = "--assets";   av[n++] = game;
    av[n++] = "--user-dir"; av[n++] = save;
    av[n++] = "--window";   av[n++] = win;
    av[n] = NULL;
    return rally_main(n, (char **)av);
}
