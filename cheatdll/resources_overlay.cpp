#include "stdafx.h"
#include "resources_overlay.h"
#include "spectator_overlay.h"

static const int ATTR_FOOD     = 0;
static const int ATTR_CARBON   = 1;
static const int ATTR_ORE      = 2;
static const int ATTR_NOVA     = 3;
static const int ATTR_POP_ROOM = 4;   // free pop slots (cap - current)
static const int ATTR_CIV_POP  = 11;  // civilian pop count
static const int ATTR_MIL_POP  = 12;  // military pop count

static const int CARD_W      = 40;   // wide enough for "999/200"
static const int CARD_ICN_H  = 24;
static const int CARD_VAL_H  = 10;
static const int CARD_TOTAL  = CARD_ICN_H + CARD_VAL_H;
static const int N_CARDS     = 5;

// Match the queue view's compact_h so switching views doesn't resize the panel.
static const int ROW_H_RES   = 37;

struct ResInfo { const char* letter; COLORREF color; int attr; };

static const ResInfo RES[4] = {
    { "F", RGB( 60, 200,  70), ATTR_FOOD   },
    { "C", RGB(200, 140,  45), ATTR_CARBON },
    { "O", RGB(180, 180, 190), ATTR_ORE    },
    { "N", RGB( 90, 150, 230), ATTR_NOVA   },
};
static const COLORREF POP_COLOR = RGB(255, 215, 80);

static void draw_res_card_fills(TDrawArea* da, int cx, int cy,
                                 unsigned __int8 pi_dark,
                                 unsigned __int8 pi_tint,
                                 unsigned __int8 pi_val)
{
    TDrawArea__FillRect(da, cx,     cy,     cx + CARD_W, cy + CARD_ICN_H, pi_dark);
    TDrawArea__FillRect(da, cx + 1, cy + 1, cx + CARD_W - 1, cy + CARD_ICN_H - 1, pi_tint);
    TDrawArea__FillRect(da, cx,     cy + CARD_ICN_H,
                            cx + CARD_W, cy + CARD_ICN_H + CARD_VAL_H, pi_val);
}

static void draw_res_card_text(HDC hdc, int cx, int cy,
                                COLORREF text_color,
                                const char* letter, const char* value)
{
    RECT rcard = { cx, cy, cx + CARD_W, cy + CARD_ICN_H };
    SetTextColor(hdc, text_color);
    DrawTextA(hdc, letter, -1, &rcard, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT rval = { cx, cy + CARD_ICN_H, cx + CARD_W, cy + CARD_ICN_H + CARD_VAL_H };
    DrawTextA(hdc, value, -1, &rval, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static bool res_need_redraw()
{
    return true;  // resources change every tick; container gates on isRec()
}

// Dim tint of the resource colour for the card interior.
static unsigned __int8 res_tint_pal(int card_idx)
{
    COLORREF base = (card_idx < 4) ? RES[card_idx].color : POP_COLOR;
    COLORREF dim  = RGB(GetRValue(base) / 6, GetGValue(base) / 6, GetBValue(base) / 6);
    return pal_index(dim);
}

static void res_render(TDrawArea* da, HRGN /*clip*/,
                       TRIBE_Player* player, int /*player_idx*/,
                       int x, int y, int w,
                       SpectatorLayout /*layout*/, SpectatorPass pass)
{
    if (!player || !player->attributes) return;

    int strip_w = N_CARDS * CARD_W;
    int cx_base = x + w - strip_w;
    if (cx_base < x) cx_base = x;

    if (pass == SP_PASS_SLP)
    {
        unsigned __int8 pi_dark = pal_index(RGB(15, 15, 15));
        unsigned __int8 pi_val  = pal_index(RGB( 0,  0,  0));
        for (int i = 0; i < N_CARDS; i++)
            draw_res_card_fills(da, cx_base + i * CARD_W, y, pi_dark, res_tint_pal(i), pi_val);
        return;
    }

    HDC hdc = da->DrawDc;
    if (!hdc) return;

    for (int i = 0; i < 4; i++)
    {
        int val = (int)player->attributes[RES[i].attr];
        char buf[12];
        if (val >= 10000)
            snprintf(buf, sizeof(buf), "%.1fk", val / 1000.0f);
        else
            snprintf(buf, sizeof(buf), "%d", val);

        draw_res_card_text(hdc, cx_base + i * CARD_W, y, RES[i].color, RES[i].letter, buf);
    }

    int pop_cur = (int)(player->attributes[ATTR_CIV_POP] + player->attributes[ATTR_MIL_POP]);
    int pop_cap = (int)(pop_cur + player->attributes[ATTR_POP_ROOM]);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d/%d", pop_cur, pop_cap);
    draw_res_card_text(hdc, cx_base + 4 * CARD_W, y, POP_COLOR, "P", buf);
}

static const SpectatorViewDef s_res_view_def = {
    "Resources",
    ROW_H_RES,
    ROW_H_RES,
    res_render,
    res_need_redraw,
    NULL
};

void register_resources_spectator_view()
{
    register_spectator_view(s_res_view_def);
}
