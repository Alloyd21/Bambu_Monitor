// ============================================================
// SCREEN LAYOUT
//
// Everything that decides what the 960x540 panel shows. It only draws into
// `framebuffer`; pushing it to the panel stays in the sketch. Kept free of
// WiFi / MQTT / FTP so tools/preview can compile it on a PC and render PNGs.
// ============================================================
#pragma once

#include <Arduino.h>
#include "epd_driver.h"
#include "font_numeral.h"
#include "font_title.h"
#include "font_body.h"
#include "font_label.h"

extern uint8_t* framebuffer;
bool drawThumbnailToFramebuffer();   // defined by the sketch

struct PrinterStatus {
    String state = "CONNECTING";
    int stage = -1;            // stg_cur: what the printer is doing within the job
    String printerName = "";   // e.g. "Bambu Lab X2D", from the printer's get_version reply
    String jobName = "";
    String taskId = "";
    String gcodeFile = "";

    int progress = -1;
    int layer = -1;
    int totalLayers = -1;
    int remainingMinutes = -1;

    float chamberTemp = NAN;
    float bedTemp = NAN;
    float leftNozzleTemp = NAN;
    float rightNozzleTemp = NAN;

    float amsTemp = NAN;
    int amsHumidity = -1;
    int amsHumidityRaw = -1;

    String printerWifi = "";
    unsigned long lastMessage = 0;
};

bool isActiveState(const String& state) {
    return state == "PRINTING" || state == "PREPARING" || state == "PAUSED";
}

// ------------------------------------------------------------
// Design tokens
// ------------------------------------------------------------

const int MARGIN  = 32;
const int LEFT_X  = MARGIN;
const int RIGHT_X = EPD_WIDTH - MARGIN;
const int CONTENT_W = RIGHT_X - LEFT_X;

// Text greys are panel levels 0 (black) .. 15 (white); shapes take 0..255.
const uint8_t INK   = 0;
const uint8_t MUTED = 7;      // labels, units, secondary text
const uint8_t PAPER = 15;
const uint8_t RULE  = 0xB0;   // hairlines
const uint8_t TRACK = 0xD0;   // empty part of the progress bar

const int LABEL_TRACKING = 2; // extra px between letters of small caps labels

// Vertical rhythm
const int HEADER_BASE = 50;   // wordmark baseline
const int HEADER_RULE = 80;
const int TELEMETRY_RULE = 392;

// Plate preview, drawn by the sketch at this position. It spans the first two
// of the six telemetry columns, so the text beside it starts on the third.
const int PREVIEW_X = LEFT_X;
const int PREVIEW_Y = HEADER_RULE + 24;
const int PREVIEW_W = 264;
const int PREVIEW_H = 264;
const int PREVIEW_TEXT_X = LEFT_X + CONTENT_W / 3;

// ------------------------------------------------------------
// Text
// ------------------------------------------------------------

static uint32_t nextCodepoint(const char*& p) {
    uint8_t c = (uint8_t)*p;
    if (!c) return 0;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t cp = extra == 0 ? c : c & (0x3F >> extra);
    p++;
    for (int i = 0; i < extra && ((uint8_t)*p & 0xC0) == 0x80; i++, p++)
        cp = (cp << 6) | ((uint8_t)*p & 0x3F);
    return cp;
}

static GFXglyph* glyphFor(const GFXfont* font, uint32_t cp) {
    GFXglyph* g = nullptr;
    get_glyph(font, cp, &g);
    return g;
}

// Drop characters the font has no glyph for, so measuring and drawing agree.
String printable(const GFXfont* font, const String& s) {
    String out;
    const char* p = s.c_str();
    while (*p) {
        const char* start = p;
        uint32_t cp = nextCodepoint(p);
        if (glyphFor(font, cp))
            for (const char* q = start; q < p; q++) out += *q;
    }
    return out;
}

// Horizontal advance of a string, which is what alignment needs.
int textWidth(const GFXfont* font, const String& s, int tracking = 0) {
    int w = 0, n = 0;
    const char* p = s.c_str();
    while (uint32_t cp = nextCodepoint(p)) {
        GFXglyph* g = glyphFor(font, cp);
        if (!g) continue;
        w += g->advance_x;
        n++;
    }
    return n ? w + tracking * (n - 1) : 0;
}

Rect_t emptyRect() {
    Rect_t r;
    r.x = r.y = r.width = r.height = 0;
    return r;
}

Rect_t unionRect(const Rect_t& a, const Rect_t& b) {
    if (a.width <= 0 || a.height <= 0) return b;
    if (b.width <= 0 || b.height <= 0) return a;
    Rect_t r;
    r.x = min(a.x, b.x);
    r.y = min(a.y, b.y);
    r.width = max(a.x + a.width, b.x + b.width) - r.x;
    r.height = max(a.y + a.height, b.y + b.height) - r.y;
    return r;
}

const int TEXT_PAD = 4;   // margin around the ink in returned areas

// Draws `s` with its baseline at y and returns the area its ink covers (plus
// TEXT_PAD), for partial refresh. `bg` must match what is underneath:
// the renderer paints every glyph's box in that colour.
Rect_t drawText(const GFXfont* font, int32_t x, int32_t y, const String& text,
                uint8_t fg = INK, int tracking = 0, uint8_t bg = PAPER) {
    String s = printable(font, text);
    if (!s.length()) return emptyRect();

    FontProperties props;
    props.fg_color = fg;
    props.bg_color = bg;
    props.fallback_glyph = 0;
    props.flags = 0;

    // Ink extent, so neighbours can be placed against the visible glyphs.
    int32_t top = 0, bottom = 0, left = 100000, right = 0, pen = 0;
    const char* p = s.c_str();
    while (uint32_t cp = nextCodepoint(p)) {
        GFXglyph* g = glyphFor(font, cp);
        left = min(left, pen + (int32_t)g->left);
        right = max(right, pen + (int32_t)g->left + (int32_t)g->width);
        top = max(top, (int32_t)g->top);
        bottom = max(bottom, (int32_t)g->height - (int32_t)g->top);
        pen += g->advance_x + tracking;
    }

    int32_t cx = x, cy = y;
    if (tracking == 0) {
        write_mode(font, s.c_str(), &cx, &cy, framebuffer, BLACK_ON_WHITE, &props);
    } else {
        p = s.c_str();
        while (*p) {
            const char* start = p;
            nextCodepoint(p);
            char one[5] = {0};
            memcpy(one, start, p - start);
            write_mode(font, one, &cx, &cy, framebuffer, BLACK_ON_WHITE, &props);
            cx += tracking;
        }
    }

    Rect_t r;
    r.x = x + left - TEXT_PAD;
    r.y = y - top - TEXT_PAD;
    r.width = right - left + 2 * TEXT_PAD;
    r.height = top + bottom + 2 * TEXT_PAD;
    return r;
}

// Grow an area by `pad` on every side. The big numbers leave faint edge pixels
// behind when only their tight ink box is flashed, so they get extra margin.
Rect_t padRect(Rect_t r, int pad) {
    r.x -= pad;  r.y -= pad;
    r.width += 2 * pad;  r.height += 2 * pad;
    return r;
}

const int NUMBER_FLASH_PAD = 12;
const int NOZZLE_EXTRA_RIGHT = 16;   // on top of NUMBER_FLASH_PAD
const int COLUMN_FLASH_GAP = 4;      // clear space kept before the next telemetry column

Rect_t drawTextRight(const GFXfont* font, int32_t right, int32_t y, const String& text,
                     uint8_t fg = INK, int tracking = 0, uint8_t bg = PAPER) {
    String s = printable(font, text);
    return drawText(font, right - textWidth(font, s, tracking), y, s, fg, tracking, bg);
}

// Small uppercase label, letter-spaced.
Rect_t drawLabel(int32_t x, int32_t y, const String& text, uint8_t fg = MUTED, uint8_t bg = PAPER) {
    return drawText(&InterLabel, x, y, text, fg, LABEL_TRACKING, bg);
}

Rect_t drawLabelRight(int32_t right, int32_t y, const String& text) {
    return drawTextRight(&InterLabel, right, y, text, MUTED, LABEL_TRACKING);
}

// Shorten to fit maxWidth, ending in an ellipsis.
String fitText(const GFXfont* font, const String& text, int maxWidth) {
    String s = printable(font, text);
    if (textWidth(font, s) <= maxWidth) return s;
    const String dots = "\xE2\x80\xA6";
    while (s.length() && textWidth(font, s + dots) > maxWidth) {
        int end = s.length() - 1;
        while (end > 0 && ((uint8_t)s[end] & 0xC0) == 0x80) end--;   // whole UTF-8 characters
        s = s.substring(0, end);
        s.trim();
    }
    return s + dots;
}

// A value made of runs in different fonts on one baseline, e.g. a number in
// the title font followed by its unit in grey.
struct Span {
    const GFXfont* font;
    String text;
    uint8_t fg;
};

int spansWidth(const Span* spans, int n) {
    int w = 0;
    for (int i = 0; i < n; i++) w += textWidth(spans[i].font, spans[i].text);
    return w;
}

Rect_t drawSpans(int32_t x, int32_t y, const Span* spans, int n) {
    Rect_t r = emptyRect();
    for (int i = 0; i < n; i++) {
        r = unionRect(r, drawText(spans[i].font, x, y, spans[i].text, spans[i].fg));
        x += textWidth(spans[i].font, spans[i].text);
    }
    return r;
}

Rect_t drawSpansRight(int32_t right, int32_t y, const Span* spans, int n) {
    return drawSpans(right - spansWidth(spans, n), y, spans, n);
}

// ------------------------------------------------------------
// Shapes
// ------------------------------------------------------------

// Rounded-end bar filling exactly x..x+w-1, y..y+h-1. epd_fill_circle(r) is
// 2r+1 px tall, so r = (h-1)/2; an even height needs each end drawn as two
// circles one row apart to cover all h rows.
void fillPill(int32_t x, int32_t y, int32_t w, int32_t h, uint8_t color) {
    if (w < h) w = h;
    int32_t r = (h - 1) / 2;
    int32_t cyTop = y + r, cyBottom = y + h - 1 - r;
    int32_t cxLeft = x + r, cxRight = x + w - 1 - r;
    epd_fill_rect(cxLeft, y, cxRight - cxLeft + 1, h, color, framebuffer);
    for (int32_t cy = cyTop; cy <= cyBottom; cy++) {
        epd_fill_circle(cxLeft, cy, r, color, framebuffer);
        epd_fill_circle(cxRight, cy, r, color, framebuffer);
    }
}

void hairline(int32_t y) {
    epd_draw_hline(LEFT_X, y, CONTENT_W, RULE, framebuffer);
}

// ------------------------------------------------------------
// Partial refresh bookkeeping
//
// Every dynamic element is registered as a "field" while drawing. On the next
// update only fields whose content changed are flashed and redrawn. Within one
// layout the list of fields must stay the same length and order, so optional
// elements register an empty field when absent.
//
// A field with a `level` (>= 0) only ever adds black ink as the level rises,
// like the progress bar filling up. When it rises it is drawn over the old
// content without a flash; when it falls it is flashed like any other field.
// ------------------------------------------------------------

struct FieldState {
    String sig;
    Rect_t rect;
    int level = -1;
};

const int MAX_FIELDS = 40;

FieldState curFields[MAX_FIELDS];
FieldState prevFields[MAX_FIELDS];
int curFieldCount = 0;
int prevFieldCount = 0;

void addField(const String& sig, const Rect_t& r, int level = -1) {
    if (curFieldCount >= MAX_FIELDS) return;
    curFields[curFieldCount].sig = sig;
    curFields[curFieldCount].rect = r;
    curFields[curFieldCount].level = level;
    curFieldCount++;
}

// ------------------------------------------------------------
// Formatting
// ------------------------------------------------------------

// Label for the printer's current stage (stg_cur), or "" for none/unknown.
String stageLabel(int stage) {
    switch (stage) {
        case 0:  return "PRINTING";
        case 1:  return "BED LEVELING";
        case 2:  return "PREHEATING BED";
        case 3:  return "VIBRATION COMP";
        case 4:  return "CHANGING FILAMENT";
        case 5:  return "PAUSED";
        case 6:  return "FILAMENT RUNOUT";
        case 7:  return "HEATING HOTEND";
        case 8:  return "CALIBRATING EXTRUSION";
        case 9:  return "SCANNING BED";
        case 10: return "INSPECTING 1ST LAYER";
        case 11: return "IDENTIFYING PLATE";
        case 12: return "CALIBRATING LIDAR";
        case 13: return "HOMING";
        case 14: return "CLEANING NOZZLE";
        case 15: return "CHECKING TEMP";
        case 16: return "PAUSED BY USER";
        case 17: return "FRONT COVER FELL";
        case 19: return "CALIBRATING FLOW";
        case 22: return "UNLOADING FILAMENT";
        case 24: return "LOADING FILAMENT";
        case 25: return "CALIBRATING MOTOR";
        case 29: return "COOLING CHAMBER";
        default: return "";   // -1 / 255 idle, or a stage we don't know
    }
}

// Pill text: the current stage while a job is active, else the state.
String stateLabel(const String& state, int stage = -1) {
    if (isActiveState(state)) {
        String s = stageLabel(stage);
        if (s.length()) return s;
    }
    return state;
}

String idleHeadline(const String& state) {
    if (state == "FINISHED")   return "Print complete";
    if (state == "FAILED")     return "Print failed";
    if (state == "IDLE")       return "Ready";
    if (state == "CONNECTING") return "Waiting for printer\xE2\x80\xA6";
    String s = state;
    s.toLowerCase();
    if (s.length()) s.setCharAt(0, toupper(s[0]));
    return s;
}

// Number in the title font with a grey unit, e.g. 220 degrees or 18%.
int valueSpans(Span* out, float value, const String& unit) {
    if (isnan(value)) {
        out[0] = {&InterTitle, "--", MUTED};
        return 1;
    }
    out[0] = {&InterTitle, String((int)lroundf(value)), INK};
    out[1] = {&InterTitle, unit, MUTED};
    return 2;
}

// 2h 05m / 45m, numbers in ink and units in grey.
int durationSpans(Span* out, int minutes) {
    if (minutes < 0) {
        out[0] = {&InterTitle, "--", MUTED};
        return 1;
    }
    int n = 0;
    int h = minutes / 60, m = minutes % 60;
    if (h > 0) {
        out[n++] = {&InterTitle, String(h), INK};
        out[n++] = {&InterBody, "h ", MUTED};
        out[n++] = {&InterTitle, (m < 10 ? "0" : "") + String(m), INK};
    } else {
        out[n++] = {&InterTitle, String(m), INK};
    }
    out[n++] = {&InterBody, "m", MUTED};
    return n;
}

// ------------------------------------------------------------
// Screen sections
// ------------------------------------------------------------

// Wordmark on the left, state pill on the right, hairline below.
// The wordmark is the printer's name once known ("Bambu Lab" in grey, the
// model in ink), and "Bambu Monitor" until then.
void drawHeader(const String& state, const String& printerName = "", int stage = -1) {
    String muted = "Bambu", ink = "Monitor";
    if (printerName.startsWith("Bambu Lab ")) {
        muted = "Bambu Lab";
        ink = printerName.substring(10);
    } else if (printerName.length()) {
        muted = "";
        ink = printerName;
    }
    int inkX = LEFT_X;
    Rect_t brand = {};
    if (muted.length()) {
        brand = drawText(&InterBody, LEFT_X, HEADER_BASE, muted, MUTED);
        inkX = brand.x + brand.width + 6;
    }
    Rect_t model = drawText(&InterBody, inkX, HEADER_BASE, ink, INK);
    addField("name" + printerName, muted.length() ? unionRect(brand, model) : model);

    String label = stateLabel(state, stage);
    const int pillH = 38, padX = 20;
    int textW = textWidth(&InterLabel, label, LABEL_TRACKING);
    int pillW = textW + 2 * padX;
    int pillX = RIGHT_X - pillW;
    int pillY = HEADER_BASE - 12 - pillH / 2;   // centred on the wordmark's cap height
    fillPill(pillX, pillY, pillW, pillH, 0);
    drawLabel(pillX + padX, pillY + pillH / 2 + 7, label, PAPER, INK);

    Rect_t r;
    r.x = pillX - 4;  r.y = pillY - 4;
    r.width = pillW + 8;  r.height = pillH + 8;
    addField("state" + label, r);

    hairline(HEADER_RULE);
}

void fieldBar(int x, int y, int w, int h, int progress) {
    fillPill(x, y, w, h, TRACK);
    if (progress > 0) fillPill(x, y, w * constrain(progress, 0, 100) / 100, h, 0);
    Rect_t r;
    r.x = x - 2;  r.y = y - 2;
    r.width = w + 4;  r.height = h + 4;
    addField("bar" + String(progress), r, max(progress, 0));
}

// Job name, big percentage, time remaining, progress bar and layer count,
// laid out between x0 and RIGHT_X.
void drawProgress(const PrinterStatus& s, int x0, bool showRetryNote) {
    const int nameY = 136, numberY = 290, barY = 314, barH = 12, footY = 366;

    addField("job" + s.jobName,
             drawText(&InterBody, x0, nameY, fitText(&InterBody, s.jobName, RIGHT_X - x0)));

    // 67% : numerals in the display face, the percent sign small and grey.
    String pct = s.progress >= 0 ? String(s.progress) : "--";
    Rect_t a = drawText(&InterNumeral, x0 - 4, numberY, pct, s.progress >= 0 ? INK : MUTED);
    Rect_t b = drawText(&InterTitle, a.x + a.width - TEXT_PAD + 6, numberY, "%", MUTED);
    addField("pct" + pct, padRect(unionRect(a, b), NUMBER_FLASH_PAD));

    drawLabelRight(RIGHT_X, numberY - 62, "REMAINING");
    Span t[4];
    int n = durationSpans(t, s.remainingMinutes);
    addField("left" + String(s.remainingMinutes),
             padRect(drawSpansRight(RIGHT_X, numberY, t, n), NUMBER_FLASH_PAD));

    fieldBar(x0, barY, RIGHT_X - x0, barH, s.progress);

    Rect_t lbl = drawLabel(x0, footY, "LAYER");
    int vx = lbl.x + lbl.width + 8;
    if (s.layer >= 0 && s.totalLayers > 0) {
        Span v[2] = {{&InterBody, String(s.layer), INK},
                     {&InterBody, " / " + String(s.totalLayers), MUTED}};
        addField("layer" + String(s.layer) + "/" + String(s.totalLayers), drawSpans(vx, footY, v, 2));
    } else {
        addField("layer--", drawText(&InterBody, vx, footY, "--", MUTED));
    }

    if (showRetryNote)
        addField("note", drawLabelRight(RIGHT_X, footY, "PREVIEW UNAVAILABLE, RETRYING"));
    else
        addField("", emptyRect());
}

// Not printing: a headline for the state, then the last job if there is one.
void drawIdle(const PrinterStatus& s, int x0) {
    bool hasJob = s.jobName.length() > 0;
    int headY = hasJob ? 190 : 226;

    addField("head" + s.state, drawText(&InterTitle, x0, headY, idleHeadline(s.state)));

    if (hasJob) {
        drawLabel(x0, headY + 70, "LAST PRINT");
        addField("job" + s.jobName,
                 drawText(&InterBody, x0, headY + 114, fitText(&InterBody, s.jobName, RIGHT_X - x0)));
    } else {
        String sub = s.state == "CONNECTING" ? "No status from the printer yet" : "No active print";
        addField("sub" + sub, drawText(&InterBody, x0, headY + 56, sub, MUTED));
    }
}

// Which of the six readings to show, one bit each: chamber, bed, left nozzle,
// right nozzle, AMS temperature, humidity. Readings the printer doesn't report
// (one nozzle, no AMS, no chamber sensor) are left out. Until the first status
// arrives nothing is known yet, so all six show "--".
int telemetryMask(const PrinterStatus& s) {
    if (s.state == "CONNECTING") return 0x3F;
    int m = 0;
    if (!isnan(s.chamberTemp))     m |= 1 << 0;
    if (!isnan(s.bedTemp))         m |= 1 << 1;
    if (!isnan(s.leftNozzleTemp))  m |= 1 << 2;
    if (!isnan(s.rightNozzleTemp)) m |= 1 << 3;
    if (!isnan(s.amsTemp))         m |= 1 << 4;
    if (s.amsHumidityRaw >= 0 || s.amsHumidity >= 0) m |= 1 << 5;
    return m;
}

// The shown readings under the lower hairline, packed from the left on a
// six-column grid.
void drawTelemetry(const PrinterStatus& s) {
    hairline(TELEMETRY_RULE);

    int mask = telemetryMask(s);
    bool oneNozzle = (mask & 0x0C) != 0x0C;   // "NOZZLE" rather than "NOZZLE L" / "NOZZLE R"
    const char* labels[] = {"CHAMBER", "BED", oneNozzle ? "NOZZLE" : "NOZZLE L",
                            oneNozzle ? "NOZZLE" : "NOZZLE R", "AMS", "HUMIDITY"};
    const float temps[] = {s.chamberTemp, s.bedTemp, s.leftNozzleTemp, s.rightNozzleTemp, s.amsTemp};
    const int cols = 6;
    const int labelY = TELEMETRY_RULE + 46;
    const int valueY = TELEMETRY_RULE + 110;

    int col = 0;
    for (int i = 0; i < cols; i++) {
        if (!(mask & (1 << i))) continue;
        int x = LEFT_X + CONTENT_W * col++ / cols;
        drawLabel(x, labelY, labels[i]);

        Span v[2];
        int n;
        String sig;
        if (i < 5) {
            n = valueSpans(v, temps[i], "\xC2\xB0");
            sig = isnan(temps[i]) ? String("--") : String((int)lroundf(temps[i]));
        } else if (s.amsHumidityRaw >= 0) {
            // "humidity_raw" is the real percentage the app shows.
            n = valueSpans(v, s.amsHumidityRaw, "%");
            sig = String(s.amsHumidityRaw) + "%";
        } else if (s.amsHumidity >= 0) {
            // "humidity" is only a 1-5 level index.
            v[0] = {&InterTitle, String(s.amsHumidity), INK};
            v[1] = {&InterBody, " / 5", MUTED};
            n = 2;
            sig = "lvl" + String(s.amsHumidity);
        } else {
            n = valueSpans(v, NAN, "");
            sig = "--";
        }
        Rect_t r = padRect(drawSpans(x, valueY, v, n), NUMBER_FLASH_PAD);
        // Three-digit nozzle temps still left stray pixels on the right edge.
        if (i == 2 || i == 3) r.width += NOZZLE_EXTRA_RIGHT;
        // ...but never reach into the next column, or a refresh here wipes its text.
        int nextX = LEFT_X + CONTENT_W * col / cols - COLUMN_FLASH_GAP;
        if (col < cols && r.x + r.width > nextX) r.width = nextX - r.x;
        addField("t" + String(i) + sig, r);
    }
}

// Layout id: a change of layout forces a full refresh. It includes which
// readings are shown, since hiding one moves the labels of the others.
int screenLayout(bool active, bool withThumb, bool hasJob, int telemetry) {
    return (active ? 1 : 0) | (withThumb ? 2 : 0) | (hasJob ? 4 : 0) | (telemetry << 3);
}

// Draws the whole main screen into the (already white) framebuffer.
void drawMainScreen(const PrinterStatus& s, bool withThumb, bool showRetryNote) {
    curFieldCount = 0;
    drawHeader(s.state, s.printerName, s.stage);

    int x0 = LEFT_X;
    if (withThumb) {
        drawThumbnailToFramebuffer();
        x0 = PREVIEW_TEXT_X;
    }

    if (isActiveState(s.state)) drawProgress(s, x0, showRetryNote && !withThumb);
    else drawIdle(s, x0);

    drawTelemetry(s);
}

// One step of the boot screen: grey while in progress, then redrawn in the
// same row in black with a tick when done. Returns the area to push.
Rect_t drawBootLine(int row, const String& text, bool done) {
    const int textX = LEFT_X + 44;
    int y = 160 + row * 60;

    Rect_t area;
    area.x = LEFT_X - 4;
    area.y = y - 42;
    area.width = 640;
    area.height = 60;
    epd_fill_rect(area.x, area.y, area.width, area.height, 0xFF, framebuffer);

    if (done) {
        for (int t = 0; t < 4; t++)
            for (int d = 0; d < 2; d++) {
                epd_draw_line(LEFT_X + 1 + d, y - 13 + t, LEFT_X + 9 + d, y - 5 + t, 0, framebuffer);
                epd_draw_line(LEFT_X + 9 + d, y - 5 + t, LEFT_X + 25 + d, y - 24 + t, 0, framebuffer);
            }
        drawText(&InterBody, textX, y, text, INK);
    } else {
        epd_fill_circle(LEFT_X + 13, y - 12, 5, 0x80, framebuffer);
        drawText(&InterBody, textX, y, text, MUTED);
    }
    return area;
}
