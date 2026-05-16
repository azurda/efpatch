#include "stdafx.h"
#include "spectator_overlay.h"
#include "rec.h"
#include "registry.h"
#include "log.h"
#include <vector>

// Font 26 is the HUD font, cycled through the largeText override in textrender.cpp.
static const int SP_FONT_ID = 26;

static const int STRIPE_W     = 4;
static const int SP_CELL_GAP  = 3;
static const int TAB_H        = 18;
static const int SP_MAX_COL_W = 280;

static int label_h() { return cd.largeText ? 20 : 16; }

// Indexed by color_table->vfptr->get_id() (1..8). Slot 0 = gaia.
static const COLORREF PLAYER_COLORS[] =
{
    RGB(  0,   0,   0),
    RGB( 36,  73, 255),
    RGB(255,   0,   0),
    RGB(  0, 204,   0),
    RGB(255, 255,   0),
    RGB(  0, 220, 220),
    RGB(220, 120,   0),
    RGB(180,   0, 220),
    RGB(160, 160, 160),
};
static HBRUSH s_br_player[9] = {};

static const COLORREF TAB_VIEW_BG[] = {
    RGB( 30,  55, 110),
    RGB( 20,  85,  35),
    RGB( 90,  35,  90),
    RGB(100,  60,  10),
};

// Slot index doesn't match the visible colour in MP with shuffled colours.
static int player_color_id(TRIBE_Player* player, int fallback_idx)
{
    if (player && player->color_table && player->color_table->vfptr
        && player->color_table->vfptr->get_id)
    {
        int id = player->color_table->vfptr->get_id(player->color_table);
        if (id >= 0 && id <= 8) return id;
    }
    return (fallback_idx >= 0 && fallback_idx <= 8) ? fallback_idx : 0;
}

COLORREF get_player_color(int idx)
{
    if (idx < 0 || idx > 8) idx = 0;
    return PLAYER_COLORS[idx];
}

HBRUSH get_player_brush(int idx)
{
    if (idx < 0 || idx > 8) idx = 0;
    if (!s_br_player[idx])
        s_br_player[idx] = CreateSolidBrush(PLAYER_COLORS[idx]);
    return s_br_player[idx];
}

struct PalCacheEntry { COLORREF rgb; unsigned __int8 idx; };
static std::vector<PalCacheEntry> s_pal_cache;

static unsigned __int8 pal_lookup_uncached(COLORREF rgb)
{
    if (!*base_game || !(*base_game)->draw_system) return 0;
    PALETTEENTRY* pal = (*base_game)->draw_system->palette;
    BYTE r = GetRValue(rgb), g = GetGValue(rgb), b = GetBValue(rgb);
    int best_i = 0, best_d = 0x7fffffff;
    for (int i = 0; i < 256; i++) {
        int dr = (int)pal[i].peRed   - (int)r;
        int dg = (int)pal[i].peGreen - (int)g;
        int db = (int)pal[i].peBlue  - (int)b;
        int d  = dr*dr + dg*dg + db*db;
        if (d < best_d) { best_d = d; best_i = i; if (d == 0) break; }
    }
    return (unsigned __int8)best_i;
}

unsigned __int8 pal_index(COLORREF rgb)
{
    for (size_t i = 0; i < s_pal_cache.size(); i++)
        if (s_pal_cache[i].rgb == rgb) return s_pal_cache[i].idx;
    PalCacheEntry e; e.rgb = rgb; e.idx = pal_lookup_uncached(rgb);
    s_pal_cache.push_back(e);
    return e.idx;
}

unsigned __int8 pal_player(int color_id)
{
    if (color_id < 0 || color_id > 8) color_id = 0;
    return pal_index(PLAYER_COLORS[color_id]);
}

static HFONT sp_font()
{
    RGE_Font* f = RGE_Base_Game__get_font(*base_game, SP_FONT_ID);
    return f ? f->font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

static int sp_font_h()
{
    RGE_Font* f = RGE_Base_Game__get_font(*base_game, SP_FONT_ID);
    return (f && f->font_hgt > 0) ? f->font_hgt : 12;
}

static std::vector<SpectatorViewDef> s_views;
static int s_active = 0;
static unsigned int s_view_generation = 0;

// Re-render every Nth need_redraw call; off-frames reuse the panel buffer.
// Lower = smoother, higher = cheaper. Container ticks at display rate.
static const int SP_REDRAW_EVERY_N = 15;

extern void __stdcall handle_overlay_size();

void register_spectator_view(const SpectatorViewDef& def)
{
    s_views.push_back(def);
}

void spectator_next_view()
{
    if ((int)s_views.size() <= 1) return;
    s_active = (s_active + 1) % (int)s_views.size();
    ++s_view_generation;
    handle_overlay_size();
}

static int active_player_count()
{
    if (!*base_game || !(*base_game)->world) return 8;
    return max(1, (int)(*base_game)->world->player_num - 1);
}

static int current_cell_h()
{
    if (s_views.empty()) return label_h();
    return label_h() + s_views[s_active].compact_h;
}

static int current_tab_h() { return TAB_H; }

static void draw_tab_bg(TDrawArea* da, int x, int w, int y, int h)
{
    if (s_views.empty() || !da) return;
    COLORREF bg_col = (s_active < (int)(sizeof(TAB_VIEW_BG) / sizeof(TAB_VIEW_BG[0])))
                      ? TAB_VIEW_BG[s_active] : RGB(40, 40, 60);
    TDrawArea__FillRect(da, x, y, x + w, y + h, pal_index(bg_col));
}

static void draw_tab_text(HDC hdc, int x, int w, int y, int h)
{
    if (s_views.empty() || !hdc) return;
    RECT rtab = { x, y, x + w, y + h };
    SetTextColor(hdc, RGB(255, 255, 255));
    DrawTextA(hdc, s_views[s_active].label, -1, &rtab, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

struct SpectatorUserData
{
    bool had_content;
    unsigned int seen_view_generation;
    int tick_counter;
};

static void* sp_create(TRIBE_Panel_Screen_Overlay* /*panel*/, const void* /*user_init*/)
{
    // Palette may differ between games; flush cached indices.
    s_pal_cache.clear();
    SpectatorUserData* d = new SpectatorUserData;
    d->had_content = false;
    d->seen_view_generation = s_view_generation;
    d->tick_counter = 0;
    return d;
}

static void sp_destroy(TRIBE_Panel_Screen_Overlay* /*panel*/, void* user_data) { delete (SpectatorUserData*)user_data; }

static bool sp_need_redraw(TRIBE_Panel_Screen_Overlay* /*panel*/, void* user_data)
{
    if (!isRec() || s_views.empty()) return false;
    SpectatorUserData* d = (SpectatorUserData*)user_data;
    // View switch bypasses the throttle so Alt+Q feels instant.
    if (d->seen_view_generation != s_view_generation)
    {
        d->seen_view_generation = s_view_generation;
        d->had_content = true;
        d->tick_counter = 0;
        return true;
    }
    if (++d->tick_counter < SP_REDRAW_EVERY_N) return false;
    d->tick_counter = 0;

    bool any   = s_views[s_active].need_redraw();
    bool needs = any || d->had_content;
    d->had_content = any;
    return needs;
}

static panel_size sp_handle_size(TRIBE_Panel_Screen_Overlay* /*panel*/, void* /*user_data*/)
{
    int active = active_player_count();
    int h      = current_tab_h() + active * (current_cell_h() + SP_CELL_GAP);

    panel_size s;
    s.left_border_in = s.right_border_in = s.bottom_border_in = 0;
    s.top_border_in  = 2;
    s.min_wid_in     = 200;
    s.max_wid_in     = 10000;
    s.min_hgt_in     = s.max_hgt_in = h;
    return s;
}

static void draw_player_stripe(TDrawArea* da, int col_x, int col_w, int cell_y, int cell_h,
                                TRIBE_Player* player, int player_idx)
{
    int color_id = player_color_id(player, player_idx);
    TDrawArea__FillRect(da,
        col_x + col_w - STRIPE_W, cell_y,
        col_x + col_w,            cell_y + cell_h,
        pal_player(color_id));
}

// ExtTextOutA over DrawTextA: skips the DT_END_ELLIPSIS measurement pass.
// Overflow is clipped by the rect instead of ellipsized.
static void draw_player_name(HDC hdc, int col_x, int col_w, int cell_y, int font_h,
                              TRIBE_Player* player, int player_idx)
{
    int color_id = player_color_id(player, player_idx);
    const char* name = (player->name && player->name[0]) ? player->name : NULL;
    char name_buf[64];
    int name_len;
    if (!name)
    {
        name_len = snprintf(name_buf, sizeof(name_buf), "Player %d", player_idx);
        name = name_buf;
    }
    else
    {
        name_len = (int)strlen(name);
        if (name_len > 63) name_len = 63;
    }

    SetTextColor(hdc, get_player_color(color_id));
    int content_w = col_w - STRIPE_W;
    int lh        = label_h();
    int text_y    = cell_y + (lh - font_h) / 2;
    if (text_y < cell_y) text_y = cell_y;
    RECT clip = { col_x + 3, cell_y, col_x + content_w - 2, cell_y + lh };
    ExtTextOutA(hdc, col_x + 3, text_y, ETO_CLIPPED, &clip, name, name_len, NULL);
}

static RECT sp_render_to_image_buffer(TRIBE_Panel_Screen_Overlay* /*panel*/, void* /*user_data*/,
                                       TDrawArea* render_area, RECT* render_rect, HRGN clip_region)
{
    if (!isRec() || s_views.empty()) return *render_rect;
    if (!*base_game || !(*base_game)->world)  return *render_rect;

    int panel_w = render_rect->right;
    int pnum    = (*base_game)->world->player_num;
    int active  = pnum - 1;
    if (active <= 0) return *render_rect;

    const SpectatorViewDef& view = s_views[s_active];
    int tab_h     = current_tab_h();
    int lh        = label_h();
    int cell_h    = lh + view.compact_h;
    int col_w     = min(panel_w, SP_MAX_COL_W);
    int col_x     = panel_w - col_w;
    int cell_step = cell_h + SP_CELL_GAP;
    int content_x = col_x;
    int content_w = col_w - STRIPE_W;

    if (view.begin_frame) view.begin_frame();

    if (TDrawArea__Lock(render_area, "sp_slp", 1))
    {
        TDrawArea__SetClipRect(render_area, NULL);
        draw_tab_bg(render_area, col_x, col_w, 0, tab_h);

        for (int i = 1; i < pnum; i++)
        {
            TRIBE_Player* player = (*base_game)->world->players[i];
            if (!player) continue;
            int cell_y = tab_h + (i - 1) * cell_step;

            // Stripe spans label + content; per-view content clip is set after.
            RECT full_cell = { col_x, cell_y, col_x + col_w, cell_y + cell_h };
            TDrawArea__SetClipRect(render_area, &full_cell);
            draw_player_stripe(render_area, col_x, col_w, cell_y, cell_h, player, i);

            RECT view_cell = { content_x, cell_y + lh,
                               content_x + content_w, cell_y + cell_h };
            TDrawArea__SetClipRect(render_area, &view_cell);
            view.render(render_area, clip_region, player, i,
                        content_x, cell_y + lh, content_w,
                        SP_COMPACT, SP_PASS_SLP);
        }
        TDrawArea__SetClipRect(render_area, NULL);
        TDrawArea__Unlock(render_area, "sp_slp");
    }

    if (TDrawArea__GetDc(render_area, "sp_gdi"))
    {
        HDC hdc = render_area->DrawDc;
        // Views and headers must not change clip rgn or font in their renders.
        SetBkMode(hdc, TRANSPARENT);
        SelectClipRgn(hdc, clip_region);
        HGDIOBJ old_font = SelectObject(hdc, sp_font());

        draw_tab_text(hdc, col_x, col_w, 0, tab_h);

        int font_h = sp_font_h();
        for (int i = 1; i < pnum; i++)
        {
            TRIBE_Player* player = (*base_game)->world->players[i];
            if (!player) continue;
            int cell_y = tab_h + (i - 1) * cell_step;

            draw_player_name(hdc, col_x, col_w, cell_y, font_h, player, i);

            view.render(render_area, clip_region, player, i,
                        content_x, cell_y + lh, content_w,
                        SP_COMPACT, SP_PASS_GDI);
        }

        SelectObject(hdc, old_font);
        SelectClipRgn(hdc, 0);
        TDrawArea__ReleaseDc(render_area, "sp_gdi");
    }

    return *render_rect;
}

static void sp_handle_hotkey(TRIBE_Panel_Screen_Overlay* panel, void* /*user_data*/, int hotkey)
{
    switch (hotkey)
    {
    case 0x63:  // F8 – toggle spectator overlay on/off
        panel->vfptr->set_active((TPanel*)panel, panel->active ? 0 : 1);
        break;
    case 0x64:  // Alt+Q – cycle to next view
        spectator_next_view();
        break;
    }
}

void register_spectator_overlay()
{
    TRIBE_Panel_Screen_Overlay_User_Callbacks cb = {};
    cb.render_to_image_buffer = sp_render_to_image_buffer;
    cb.need_redraw            = sp_need_redraw;
    cb.handle_size            = sp_handle_size;
    cb.handle_hotkey          = sp_handle_hotkey;
    cb.create                 = sp_create;
    cb.destroy                = sp_destroy;
    register_screen_overlay(cb, NULL);
}
