#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#include "../schema_shm.h"
#include "../schema.h"

#define WIN_W     1100
#define WIN_H      600
#define GRID_COLS    8
#define NODE_W     104
#define NODE_H      62
#define NODE_GAP     6
#define GRID_X       8
#define GRID_Y      48
#define DETAIL_X   (GRID_X + GRID_COLS * (NODE_W + NODE_GAP) + 12)

static const char *FONTS[] = {
    "/usr/share/fonts/liberation-mono-fonts/LiberationMono-Regular.ttf",
    "/usr/share/fonts/adwaita-mono-fonts/AdwaitaMono-Regular.ttf",
    "/usr/share/fonts/liberation-mono/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    NULL
};

typedef struct { Uint8 r, g, b; } Col;

static Col state_col(uint8_t s) {
    switch (s) {
        case STATE_NEW_PROCESS:  return (Col){60,  60,  72};
        case STATE_FULL_TRUST:   return (Col){40,  200, 80};
        case STATE_FUNDAMENTAL:  return (Col){40,  130, 255};
        case STATE_RECOVERY:     return (Col){220, 180, 40};
        case STATE_FRICTION:     return (Col){240, 100, 20};
        case STATE_EXCISED:      return (Col){160, 20,  20};
        case STATE_DORMANT:      return (Col){100, 60,  140};
        case STATE_PERFECT:      return (Col){0,   210, 190};
        case STATE_SETTLED:      return (Col){140, 60,  220};
        default:                 return (Col){30,  30,  36};
    }
}

static void text(SDL_Renderer *r, TTF_Font *f, const char *s,
                 int x, int y, Uint8 rr, Uint8 gg, Uint8 bb) {
    SDL_Color c = {rr, gg, bb, 255};
    SDL_Surface *sur = TTF_RenderText_Blended(f, s, c);
    if (!sur) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, sur);
    SDL_Rect dst = {x, y, sur->w, sur->h};
    SDL_FreeSurface(sur);
    if (!tex) return;
    SDL_RenderCopy(r, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
}

int main(void) {
    int fd = shm_open(SCHEMA_SHM_NAME, O_RDONLY, 0);
    if (fd < 0) {
        fprintf(stderr, "schema-desktop: shm not found — is schema-init running?\n");
        return 1;
    }
    schema_shm_t *shm = mmap(NULL, sizeof(schema_shm_t), PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (shm == MAP_FAILED) { perror("mmap"); return 1; }

    SDL_Init(SDL_INIT_VIDEO);
    TTF_Init();

    SDL_Window   *win = SDL_CreateWindow("schema-init",
                            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                            WIN_W, WIN_H,
                            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1,
                            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    TTF_Font *fsm = NULL, *flg = NULL;
    for (int i = 0; FONTS[i]; i++) {
        if (!fsm) fsm = TTF_OpenFont(FONTS[i], 11);
        if (!flg) flg = TTF_OpenFont(FONTS[i], 14);
        if (fsm && flg) break;
    }
    if (!fsm || !flg) {
        fprintf(stderr, "schema-desktop: no monospace font found\n");
        return 1;
    }

    int selected = -1;
    uint32_t last_seq = UINT32_MAX;
    int running = 1;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = 0;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_q) running = 0;
            if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT) {
                int mx = ev.button.x, my = ev.button.y;
                selected = -1;
                int cnt = shm->count;
                for (int i = 0; i < cnt; i++) {
                    int col = i % GRID_COLS, row = i / GRID_COLS;
                    int nx = GRID_X + col * (NODE_W + NODE_GAP);
                    int ny = GRID_Y + row * (NODE_H + NODE_GAP);
                    if (mx >= nx && mx < nx+NODE_W && my >= ny && my < ny+NODE_H)
                        selected = i;
                }
            }
        }

        if (shm->seq == last_seq) { SDL_Delay(16); continue; }
        last_seq = shm->seq;

        SDL_SetRenderDrawColor(ren, 14, 14, 18, 255);
        SDL_RenderClear(ren);

        /* header bar */
        uint8_t ss = shm->system_state;
        Uint8 hbr = 28, hbg = 28, hbb = 36;
        if      (ss == 13) { hbr = 60; hbg = 14; hbb = 14; }
        else if (ss == 14) { hbr = 14; hbg = 30; hbb = 60; }
        SDL_SetRenderDrawColor(ren, hbr, hbg, hbb, 255);
        SDL_Rect hbar = {0, 0, WIN_W, GRID_Y - 4};
        SDL_RenderFillRect(ren, &hbar);

        if (ss == 13)
            text(ren, flg, "13 — shutdown", 12, 10, 220, 80, 80);
        else if (ss == 14)
            text(ren, flg, "14 — restart",  12, 10, 80, 160, 220);
        else
            text(ren, flg, "schema-init",   12, 10, 160, 160, 200);

        char seq_buf[32];
        snprintf(seq_buf, sizeof(seq_buf), "tick:%u", shm->seq);
        text(ren, fsm, seq_buf, WIN_W - 100, 14, 80, 80, 100);

        /* service grid */
        int cnt = shm->count;
        for (int i = 0; i < cnt; i++) {
            shm_svc_t sv = shm->svc[i];
            Col col = state_col(sv.state);
            int c = i % GRID_COLS, row = i / GRID_COLS;
            int nx = GRID_X + c * (NODE_W + NODE_GAP);
            int ny = GRID_Y + row * (NODE_H + NODE_GAP);

            /* dim background */
            SDL_SetRenderDrawColor(ren, col.r/5, col.g/5, col.b/5, 255);
            SDL_Rect bg = {nx, ny, NODE_W, NODE_H};
            SDL_RenderFillRect(ren, &bg);

            /* state color bar — top 4px */
            SDL_SetRenderDrawColor(ren, col.r, col.g, col.b, 255);
            SDL_Rect bar = {nx+1, ny+1, NODE_W-2, 4};
            SDL_RenderFillRect(ren, &bar);

            /* border */
            if (i == selected)
                SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
            else
                SDL_SetRenderDrawColor(ren, col.r, col.g, col.b, 180);
            SDL_RenderDrawRect(ren, &bg);

            text(ren, fsm, sv.name,            nx+5, ny+8,  210, 210, 225);
            text(ren, fsm, state_name(sv.state),nx+5, ny+24, col.r, col.g, col.b);

            char wt[12]; snprintf(wt, sizeof(wt), "wt:%d", sv.weight);
            text(ren, fsm, wt, nx+5, ny+40, 90, 90, 110);
        }

        /* detail panel */
        if (selected >= 0 && selected < cnt) {
            shm_svc_t sv = shm->svc[selected];
            Col col = state_col(sv.state);
            int px = DETAIL_X, py = GRID_Y;

            SDL_SetRenderDrawColor(ren, 28, 28, 36, 255);
            SDL_Rect panel = {px - 8, py - 6, WIN_W - px + 4, 160};
            SDL_RenderFillRect(ren, &panel);

            text(ren, flg, sv.name, px, py, 220, 220, 240); py += 22;
            text(ren, fsm, state_name(sv.state), px, py, col.r, col.g, col.b); py += 18;

            char buf[64];
            snprintf(buf, sizeof(buf), "weight:   %d", sv.weight);
            text(ren, fsm, buf, px, py, 150, 150, 170); py += 16;
            snprintf(buf, sizeof(buf), "pid:      %d", sv.child_pid);
            text(ren, fsm, buf, px, py, 150, 150, 170); py += 16;
            snprintf(buf, sizeof(buf), "restarts: %d", sv.restart_count);
            text(ren, fsm, buf, px, py, 150, 150, 170);
        }

        SDL_RenderPresent(ren);
    }

    TTF_CloseFont(fsm);
    TTF_CloseFont(flg);
    TTF_Quit();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    munmap(shm, sizeof(schema_shm_t));
    return 0;
}
