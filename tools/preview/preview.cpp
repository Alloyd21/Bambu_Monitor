// Renders the sketch's screens on a PC using screen.h and the real fonts.
// Built and run by render.py; writes one PGM per scenario.
#include <cstdio>
#include <vector>

#include "../../Bambu_Monitor_ESP32/screen.h"

uint8_t* framebuffer = nullptr;
static std::vector<uint8_t> thumb;   // PREVIEW_W x PREVIEW_H grey, from render.py

bool drawThumbnailToFramebuffer() {
    if (thumb.empty()) return false;
    for (int y = 0; y < PREVIEW_H; y++)
        for (int x = 0; x < PREVIEW_W; x++)
            epd_draw_pixel(PREVIEW_X + x, PREVIEW_Y + y, thumb[y * PREVIEW_W + x], framebuffer);
    return true;
}

static void clear() { memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2); }

static void save(const char* name) {
    char path[256];
    snprintf(path, sizeof path, "out/%s.pgm", name);
    FILE* f = fopen(path, "wb");
    fprintf(f, "P5\n%d %d\n255\n", EPD_WIDTH, EPD_HEIGHT);
    for (int i = 0; i < EPD_WIDTH * EPD_HEIGHT / 2; i++) {
        fputc((framebuffer[i] & 0x0F) * 17, f);
        fputc((framebuffer[i] >> 4) * 17, f);
    }
    fclose(f);
    printf("%s: %d fields\n", name, curFieldCount);
}

static PrinterStatus printing() {
    PrinterStatus s;
    s.state = "PRINTING";
    s.printerName = "Bambu Lab X2D";
    s.jobName = "Mini Turtle";
    s.progress = 67;
    s.layer = 142;
    s.totalLayers = 380;
    s.remainingMinutes = 134;
    s.chamberTemp = 42.3f;
    s.bedTemp = 60.0f;
    s.leftNozzleTemp = 220.4f;
    s.rightNozzleTemp = 38.0f;
    s.amsTemp = 28.4f;
    s.amsHumidityRaw = 18;
    return s;
}

int main() {
    std::vector<uint8_t> fb(EPD_WIDTH * EPD_HEIGHT / 2);
    framebuffer = fb.data();

    if (FILE* f = fopen("out/thumb.raw", "rb")) {
        thumb.resize(PREVIEW_W * PREVIEW_H);
        if (fread(thumb.data(), 1, thumb.size(), f) != thumb.size()) thumb.clear();
        fclose(f);
    }

    PrinterStatus s = printing();
    clear(); drawMainScreen(s, true, false); save("1_printing");
    clear(); drawMainScreen(s, false, true); save("2_printing_no_preview");

    PrinterStatus e = printing();
    e.progress = 4; e.layer = 3; e.remainingMinutes = 47; e.state = "PREPARING";
    e.jobName = "Mini Turtle";
    clear(); drawMainScreen(e, true, false); save("3_preparing_short");

    PrinterStatus d = printing();
    d.state = "FINISHED"; d.progress = 100; d.remainingMinutes = 0;
    d.leftNozzleTemp = 61.0f; d.bedTemp = 44.8f;
    clear(); drawMainScreen(d, true, false); save("4_finished");

    PrinterStatus i;
    i.state = "IDLE"; i.printerName = "Bambu Lab X2D"; i.chamberTemp = 24.1f; i.bedTemp = 23.6f; i.leftNozzleTemp = 26.0f;
    i.rightNozzleTemp = 25.2f; i.amsTemp = 23.0f; i.amsHumidity = 2;
    clear(); drawMainScreen(i, false, false); save("5_idle");

    PrinterStatus c;
    clear(); drawMainScreen(c, false, false); save("6_waiting");

    // Single-nozzle printer without chamber sensor or AMS: those readings are hidden.
    PrinterStatus one = printing();
    one.printerName = "Bambu Lab P1S";
    one.chamberTemp = NAN; one.leftNozzleTemp = NAN; one.rightNozzleTemp = 218.6f;
    one.amsTemp = NAN; one.amsHumidityRaw = -1;
    clear(); drawMainScreen(one, true, false); save("8_single_nozzle");

    clear();
    drawHeader("CONNECTING");
    drawBootLine(0, "Connecting to WiFi\xE2\x80\xA6", false);
    drawBootLine(0, "WiFi connected", true);
    drawBootLine(1, "Connecting to printer\xE2\x80\xA6", false);
    save("7_boot");
    return 0;
}
