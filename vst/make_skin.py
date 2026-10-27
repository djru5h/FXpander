#!/usr/bin/env python3
"""FXpander: writes vst/params.json, vst/layout.conf and vst/art/header.svg (edit this, not the outputs).

Parameter order is the VST index. Saved projects and automation store values by index, so only ever APPEND:
    0              status                          readout (param 0 is harmless: JUCE sets it to 0 and back at load)
    1..32          s1..s4 _pick _pick_prev _pick_next _load _clear _name _on _mix      (v1000)
    33..288        s1..s4 _p01.._p64                the hosted plugin's parameters
    289..296       s1..s4 _up _down                 (v1001 from here on)
    297..336       s5..s8 slot controls, _up and _down included
    337..592       s5..s8 _p01.._p64
    593..600       pr_slot _slot_prev _slot_next _recall _save _delete _name _info     chain presets
    601..640       kb_*                             the preset-name keyboard
    641..648       s1..s8 _drop                     (v1002) the drop-down field: opens the plugin list for that slot
    649..653       dd_shown dd_prev dd_next dd_close dd_title     the open list (dd_shown drives its when=)
    654..705       dd_1..dd_52                      the list rows (4 columns x 13, read down the columns)
src/fxpander.c finds every index by its key in the generated params.h, so it follows this order by itself.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SLOTS, OLD_SLOTS, POOL, PRESETS = 8, 4, 64, 32
DD_COLS, DD_ROWS = 4, 13              # the drop-down list: 52 plugins a page
KEYS = ["1", "2", "3", "4", "5", "6", "7", "8", "9", "0",
        "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P",
        "A", "S", "D", "F", "G", "H", "J", "K", "L", "-",
        "Z", "X", "C", "V", "B", "N", "M", "SPACE", "DEL", "CLEAR"]
KEY_ID = {"-": "dash", "SPACE": "space", "DEL": "del", "CLEAR": "clear"}

THEME = """theme_bg=101418
theme_panel=171c22
theme_line=2a323b
theme_ink=e6edf2
theme_ink_dim=93a1ad
theme_ink_faint=5c6873
theme_accent=35c9d6
theme_accent_hi=7fe3ea
theme_lcd=0b0e11
theme_seg_active=35c9d6
theme_seg_inactive=151a1f
theme_seg_active_tx=0b0e11
theme_box=0e1215
theme_knob_face=222a31
theme_knob_ring=34404a
theme_knob_dot=35c9d6
"""
INK, DIM, ACC, PANEL, LINE, LCD = "e6edf2", "93a1ad", "35c9d6", "171c22", "2a323b", "0b0e11"
BG = "101418"   # theme_bg
# the browser renderer's artwork, restyled (layout art_css=): button labels in the page's background colour (the user
# asked for it, 2026-10-26), on the accent fill whether up or pressed
SKIN_CSS = ".button-tx { fill: #%s; }\n" % BG


def trig(key, name):
    return {"key": key, "name": name, "min": 0, "max": 1, "default": 0, "momentary": True, "type": "trigger"}


def slot_controls(s):
    n = "FX %d" % s
    return [
        {"key": "s%d_pick" % s, "name": n + " Browse", "min": 0, "max": 127, "default": 0, "display": "string"},
        trig("s%d_pick_prev" % s, n + " Prev"), trig("s%d_pick_next" % s, n + " Next"),
        trig("s%d_load" % s, n + " Load"), trig("s%d_clear" % s, n + " Clear"),
        {"key": "s%d_name" % s, "name": n + " Plugin", "type": "readout", "display": "string"},
        {"key": "s%d_on" % s, "name": n + " On", "options": ["Off", "On"], "default": 1},
        {"key": "s%d_mix" % s, "name": n + " Mix", "min": 0, "max": 100, "default": 100, "unit": "%"},
    ]


def moves(s):
    return [trig("s%d_up" % s, "FX %d Move Up" % s), trig("s%d_down" % s, "FX %d Move Down" % s)]


def pool(s):
    return [{"key": "s%d_p%02d" % (s, k), "name": "FX %d P%d" % (s, k), "min": 0, "max": 1, "default": 0,
             "dynamic_name": True, "dynamic_display": True} for k in range(1, POOL + 1)]


def params():
    p = [{"key": "status", "name": "Status", "type": "readout", "display": "string"}]
    for s in range(1, OLD_SLOTS + 1):
        p += slot_controls(s)
    for s in range(1, OLD_SLOTS + 1):
        p += pool(s)
    for s in range(1, OLD_SLOTS + 1):
        p += moves(s)
    for s in range(OLD_SLOTS + 1, SLOTS + 1):
        p += slot_controls(s) + moves(s)
    for s in range(OLD_SLOTS + 1, SLOTS + 1):
        p += pool(s)
    p += [
        {"key": "pr_slot", "name": "Preset Slot", "min": 0, "max": PRESETS - 1, "default": 0, "display": "string"},
        trig("pr_slot_prev", "Preset Prev"), trig("pr_slot_next", "Preset Next"),
        trig("pr_recall", "Recall Preset"), trig("pr_save", "Save Preset"), trig("pr_delete", "Delete Preset"),
        {"key": "pr_name", "name": "Preset Name", "type": "readout", "display": "string"},
        {"key": "pr_info", "name": "Preset Contents", "type": "readout", "display": "string"},
    ]
    p += [trig("kb_" + KEY_ID.get(k, k.lower()), "Key " + k.title()) for k in KEYS]
    p += [trig("s%d_drop" % s, "FX %d Choose" % s) for s in range(1, SLOTS + 1)]
    p += [{"key": "dd_shown", "name": "Plugin List", "options": ["Off", "On"], "default": 0},
          trig("dd_prev", "List Prev Page"), trig("dd_next", "List Next Page"), trig("dd_close", "List Close"),
          {"key": "dd_title", "name": "List Title", "type": "readout", "display": "string"}]
    p += [{"key": "dd_%d" % n, "name": "List %d" % n, "min": 0, "max": 1, "default": 0, "display": "string"}
          for n in range(1, DD_COLS * DD_ROWS + 1)]
    return {"name": "FXpander", "params": p}


def pk(s, k):
    return "s%d_p%02d" % (s, k)


# ---- the banner (every page, y 96-152) ------------------------------------------------------------------------------
def header_svg():
    """1264 x 56: a panel with an accent rule along its foot and, at the left, the emblem: one slot (the tile's left
    bar) opening into a chain of four stacked modules, the signal snaking through them."""
    w, h = 1264, 56
    mods = "".join('<rect x="21" y="%d" width="22" height="6" rx="2" fill="#%s" fill-opacity="%.2f"/>' % (11 + 9 * i, ACC, 0.95 - 0.15 * i)
                   for i in range(4))
    path = "M 13 14 H 21 M 43 14 Q 48 14 48 18.5 Q 48 23 43 23 M 21 23 Q 16 23 16 27.5 Q 16 32 21 32 " \
           "M 43 32 Q 48 32 48 36.5 Q 48 41 43 41 M 21 41 H 13"
    g = ['<defs><linearGradient id="rule" x1="0" x2="1" y1="0" y2="0">'
         '<stop offset="0" stop-color="#%s"/><stop offset="0.6" stop-color="#%s" stop-opacity="0.35"/>'
         '<stop offset="1" stop-color="#%s" stop-opacity="0"/></linearGradient></defs>' % (ACC, ACC, ACC),
         '<rect x="0.5" y="0.5" width="%d" height="%d" rx="6" fill="#%s" stroke="#%s"/>' % (w - 1, h - 1, PANEL, LINE),
         '<rect x="62" y="%d" width="%d" height="2" fill="url(#rule)"/>' % (h - 6, w - 70),
         '<rect x="6" y="6" width="44" height="44" rx="8" fill="#%s" stroke="#%s" stroke-width="1.5"/>' % (LCD, ACC),
         '<rect x="10" y="10" width="3" height="36" rx="1.5" fill="#%s"/>' % INK,
         '<path d="%s" fill="none" stroke="#%s" stroke-width="1.6" stroke-linecap="round"/>' % (path, INK),
         mods]
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">%s</svg>\n'
            % (w, h, w, h, "".join(g)))


def field_svg(w, h, on):
    """a drop-down field: a dark box with a caret at the right (on: lit border, while pressed)"""
    cx, cy = w - 26, h // 2
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">'
            '<rect x="1" y="1" width="%d" height="%d" rx="6" fill="#%s" stroke="#%s" stroke-width="%s"/>'
            '<rect x="%d" y="8" width="1" height="%d" fill="#%s"/>'
            '<path d="M %d %d L %d %d L %d %d Z" fill="#%s"/></svg>\n'
            % (w, h, w, h, w - 2, h - 2, LCD, ACC if on else LINE, "2" if on else "1.5", w - 52, h - 16, LINE,
               cx - 8, cy - 4, cx + 8, cy - 4, cx, cy + 6, ACC))


FIELDS = [(580, 52), (560, 56)]   # the CHAIN page's and the FX pages' fields


def field(out, s, cx, cy, w, h, tsize, extra=""):
    out.append('button cx=%d cy=%d w=%d h=%d label="" key=s%d_drop img=art/field_%dx%d.svg img_on=art/field_%dx%d_on.svg '
               'get=s%d_name tsize=%d tcolor=%s talign=left%s' % (cx, cy, w, h, s, w, h, w, h, s, tsize, INK, extra))


def dropdown(out):
    """the plugin list, last on the page so it covers it (and takes its touches) while open"""
    out.append('list x=8 y=158 w=1264 h=%d cols=%d rows=%d th=42 gap=0 key=dd order=cols tsize=22 tcolor=%s when=dd_shown:On'
               % (42 * DD_ROWS, DD_COLS, DD_ROWS, INK))


def banner(out, sub=""):
    out.append('art file="art/header.svg" x=8 y=96 w=1264 h=56')
    out.append('text cx=72 cy=124 label="FX" size=3.4 weight=700 spacing=4 align=left color=%s' % ACC)
    out.append('text cx=119 cy=124 label="PANDER" size=3.4 weight=700 spacing=4 align=left color=%s' % INK)
    out.append('text cx=330 cy=125 label="%d-SLOT EFFECT CHAIN%s" size=1.5 weight=600 spacing=3 align=left color=%s'
               % (SLOTS, sub, DIM))
    out.append('readout cx=980 cy=124 w=540 h=40 label="" key=status tsize=20 tcolor=%s talign=right when=dd_shown:Off' % DIM)
    out.append('readout cx=765 cy=124 w=430 h=40 label="" key=dd_title tsize=22 tcolor=%s talign=left when=dd_shown:On' % ACC)
    out.append('button cx=1030 cy=124 w=90 h=44 label="PREV" key=dd_prev when=dd_shown:On')
    out.append('button cx=1126 cy=124 w=90 h=44 label="NEXT" key=dd_next when=dd_shown:On')
    out.append('button cx=1218 cy=124 w=84 h=44 label="CLOSE" key=dd_close when=dd_shown:On')


# ---- pages -------------------------------------------------------------------------------------------------------
def chain_tab(out):
    out.append("[tab CHAIN]")
    banner(out)
    for half in range(2):
        bank = "FX %d-%d" % (half * 4 + 1, half * 4 + 4)
        b = ' banks="%s"' % bank
        for r in range(4):
            s, y0 = half * 4 + r + 1, 158 + r * 138
            cy = y0 + 80
            out.append('frame x=8 y=%d w=1264 h=132 title="FX %d"%s' % (y0, s, b))
            for cx, lab in ((310, "PLUGIN  (TAP TO CHOOSE)"), (840, "ORDER"), (1010, "ON"), (1190, "MIX")):
                out.append('text cx=%d cy=%d label="%s" size=1.5 weight=600 spacing=2 align=center color=%s%s' % (cx, y0 + 24, lab, DIM, b))
            field(out, s, 310, cy, 580, 52, 26, b)
            out.append('button cx=680 cy=%d w=110 h=52 label="CLEAR" key=s%d_clear%s' % (cy, s, b))
            out.append('button cx=800 cy=%d w=70 h=52 label="UP" key=s%d_up%s' % (cy, s, b))
            out.append('button cx=880 cy=%d w=70 h=52 label="DN" key=s%d_down%s' % (cy, s, b))
            out.append('toggle cx=1010 cy=%d label="ON" key=s%d_on bw=90 ns=0%s' % (cy, s, b))
            out.append('knob cx=1190 cy=%d r=24 label="MIX" key=s%d_mix bw=100 ns=0%s' % (y0 + 70, s, b))
    dropdown(out)
    for half in range(2):
        ss = range(half * 4 + 1, half * 4 + 5)
        keys = ["s%d_mix" % s for s in ss] + ["s%d_on" % s for s in ss]
        out.append('qlinks "FX %d-%d" = %s' % (half * 4 + 1, half * 4 + 4, ",".join(keys)))
    out.append("")


def slot_tab(out, s):
    out.append("[tab FX %d]" % s)
    banner(out)
    out.append('frame x=8 y=158 w=1264 h=116 title="FX %d"' % s)
    field(out, s, 330, 234, 560, 56, 32)
    for cx, lab in ((990, "ON"), (1180, "MIX")):
        out.append('text cx=%d cy=178 label="%s" size=1.5 weight=600 spacing=2 align=center color=%s' % (cx, lab, DIM))
    out.append('toggle cx=990 cy=234 label="ON" key=s%d_on bw=100 ns=0' % s)
    out.append('knob cx=1180 cy=224 r=20 label="MIX" key=s%d_mix bw=100 ns=0' % s)
    out.append('frame x=8 y=280 w=1264 h=428 title="PARAMETERS"')
    for b in range(POOL // 16):
        rng = "%d-%d" % (b * 16 + 1, b * 16 + 16)
        bank = "FX %d: %s" % (s, rng)
        out.append('text cx=780 cy=234 label="PARAMS %s" size=1.8 weight=600 spacing=2 align=center color=%s banks="%s"' % (rng, ACC, bank))
        for j in range(16):
            row, col = divmod(j, 8)
            k = b * 16 + j + 1
            out.append('knob cx=%d cy=%d r=40 label="P%d" key=%s bw=150 banks="%s"' % (87 + col * 158, 384 + row * 200, k, pk(s, k), bank))
    dropdown(out)
    for b in range(POOL // 16):
        bank = "FX %d: %d-%d" % (s, b * 16 + 1, b * 16 + 16)
        out.append('qlinks "%s" = %s' % (bank, ",".join(pk(s, b * 16 + j + 1) for j in range(16))))
    out.append("")


def presets_tab(out):
    out.append("[tab PRESETS]")
    banner(out)
    out.append('frame x=8 y=158 w=1264 h=180 title="CHAIN PRESET"')
    out.append('stepper cx=340 cy=222 w=620 h=56 label="" key=pr_slot tsize=28')
    out.append('readout cx=340 cy=294 w=620 h=44 label="" key=pr_info tsize=20 tcolor=%s' % DIM)
    out.append('button cx=780 cy=222 w=150 h=56 label="RECALL" key=pr_recall')
    out.append('button cx=950 cy=222 w=150 h=56 label="SAVE" key=pr_save')
    out.append('button cx=1120 cy=222 w=150 h=56 label="DELETE" key=pr_delete')
    out.append('text cx=950 cy=294 label="SAVE STORES ALL %d SLOTS UNDER THE NAME BELOW" size=1.3 weight=600 spacing=1 align=center color=%s' % (SLOTS, DIM))
    out.append('frame x=8 y=344 w=1264 h=364 title="NAME"')
    out.append('readout cx=640 cy=402 w=920 h=52 label="" key=pr_name tsize=30 tcolor=%s' % INK)
    for i, k in enumerate(KEYS):
        row, col = divmod(i, 10)
        out.append('button cx=%d cy=%d w=94 h=54 label="%s" key=kb_%s' % (235 + col * 101, 474 + row * 62, k, KEY_ID.get(k, k.lower())))
    # no drop-down list here: this page has no field, and a list over it spoiled the stepper's arrow pictures
    # (they are cut from the rendered page; v1002-v1004)
    out.append('qlinks "PRESETS" = pr_slot')
    out.append("")


def layout():
    out = ["# FXpander skin: written by vst/make_skin.py (edit that, not this file).", THEME.rstrip(), "art_css=skin.css", ""]
    chain_tab(out)
    for s in range(1, SLOTS + 1):
        slot_tab(out, s)
    presets_tab(out)
    out.append("qlinks_track = " + ",".join(pk(s, k) for s in range(1, 5) for k in range(1, 5)))
    return "\n".join(out) + "\n"


if __name__ == "__main__":
    with open(os.path.join(HERE, "params.json"), "w") as f:
        json.dump(params(), f, indent=1)
        f.write("\n")
    with open(os.path.join(HERE, "layout.conf"), "w") as f:
        f.write(layout())
    with open(os.path.join(HERE, "skin.css"), "w") as f:
        f.write(SKIN_CSS)
    os.makedirs(os.path.join(HERE, "art"), exist_ok=True)
    with open(os.path.join(HERE, "art", "header.svg"), "w") as f:
        f.write(header_svg())
    for w, h in FIELDS:
        for on in (0, 1):
            with open(os.path.join(HERE, "art", "field_%dx%d%s.svg" % (w, h, "_on" if on else "")), "w") as f:
                f.write(field_svg(w, h, on))
    print("params.json: %d params; layout.conf, art/header.svg and field art written" % len(params()["params"]))
