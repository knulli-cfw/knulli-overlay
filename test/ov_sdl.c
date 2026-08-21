/* Checks the SDL_Renderer path: the applications that never touch GL
 * themselves (ScummVM, SDLPoP, ...) and present through SDL_RenderPresent.
 *
 * Draws a recognisable frame, presents it -- which is where the injected
 * library gets its chance -- and reads the result back into a PPM.
 *
 *   ov_sdl <w> <h> <out.ppm|-> [seconds]
 *
 * A "-" output skips the read-back, which not every driver survives; the
 * seconds argument keeps presenting the same frame for that long instead, so
 * the screen can be captured from outside (knulli-screenshot).
 *
 * SDL_VIDEODRIVER=offscreen keeps it off the display, so it can run on a
 * device that is busy with something else.
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

static void write_ppm(const char *path, int w, int h, const unsigned char *rgb)
{
    FILE *f = fopen(path, "wb");

    if (!f) {
        perror(path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 1, (size_t)w * h * 3, f);
    fclose(f);
    printf("wrote %s (%dx%d)\n", path, w, h);
}

int main(int argc, char **argv)
{
    int w = argc > 1 ? atoi(argv[1]) : 640;
    int h = argc > 2 ? atoi(argv[2]) : 480;
    const char *out = argc > 3 ? argv[3] : "sdl.ppm";
    int hold = argc > 4 ? atoi(argv[4]) : 0;
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_RendererInfo info;
    unsigned char *pixels;
    int x, y;

    setvbuf(stdout, NULL, _IONBF, 0);   /* so a crash keeps what got that far */
    {   /* which back-ends this SDL was built with, for when one is missing */
        int i, n = SDL_GetNumVideoDrivers();

        printf("video drivers:");
        for (i = 0; i < n; i++)
            printf(" %s", SDL_GetVideoDriver(i));
        printf("\n");
    }
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    printf("video driver: %s\n", SDL_GetCurrentVideoDriver());

    printf("creating window\n");
    win = SDL_CreateWindow("ov_sdl", SDL_WINDOWPOS_UNDEFINED,
                           SDL_WINDOWPOS_UNDEFINED, w, h, 0);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    printf("creating renderer\n");
    ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GetRendererInfo(ren, &info);
    printf("render driver: %s%s\n", info.name,
           (info.flags & SDL_RENDERER_SOFTWARE) ? " (software)" : "");

    /* Something with structure, so an overlay drawn on top is obvious. */
    SDL_SetRenderDrawColor(ren, 30, 60, 110, 255);
    SDL_RenderClear(ren);
    SDL_SetRenderDrawColor(ren, 200, 170, 90, 255);
    for (y = 0; y < h; y += 64)
        for (x = (y / 64 % 2) * 64; x < w; x += 128) {
            SDL_Rect r = { x, y, 64, 64 };
            SDL_RenderFillRect(ren, &r);
        }

    printf("presenting\n");
    SDL_RenderPresent(ren);         /* the injected library draws in here */
    printf("presented\n");

    /* Keep the frame up so it can be captured from another shell.  The scene
     * is redrawn each time: the overlay has to survive a real frame loop, not
     * just one lucky frame. */
    for (; hold > 0; hold--) {
        int i;

        for (i = 0; i < 60; i++) {
            SDL_PumpEvents();
            SDL_SetRenderDrawColor(ren, 30, 60, 110, 255);
            SDL_RenderClear(ren);
            SDL_SetRenderDrawColor(ren, 200, 170, 90, 255);
            for (y = 0; y < h; y += 64)
                for (x = (y / 64 % 2) * 64; x < w; x += 128) {
                    SDL_Rect r = { x, y, 64, 64 };
                    SDL_RenderFillRect(ren, &r);
                }
            SDL_RenderPresent(ren);
            SDL_Delay(16);
        }
    }

    /* 32bpp with a generous allocation: a back-end that ignores the pitch or
     * the format would otherwise scribble past a tight RGB24 buffer. */
    if (!strcmp(out, "-")) {        /* read-back skipped on request */
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 0;
    }
    pixels = calloc((size_t)w * h * 4 + 64, 1);
    if (pixels &&
        SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, pixels,
                             w * 4) != 0) {
        fprintf(stderr, "SDL_RenderReadPixels: %s\n", SDL_GetError());
    } else if (pixels) {
        unsigned char *rgb = malloc((size_t)w * h * 3);
        int i;

        for (i = 0; i < w * h; i++) {      /* ARGB8888 is BGRA in memory */
            rgb[i * 3 + 0] = pixels[i * 4 + 2];
            rgb[i * 3 + 1] = pixels[i * 4 + 1];
            rgb[i * 3 + 2] = pixels[i * 4 + 0];
        }
        write_ppm(out, w, h, rgb);
        free(rgb);
    }
    free(pixels);

    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
