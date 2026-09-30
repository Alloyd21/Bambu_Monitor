#ifndef BOARD_HAS_PSRAM
#error "Enable OPI PSRAM in Arduino IDE"
#endif

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <pngle.h>
#include <miniz.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <lwip/sockets.h>
#include "mbedtls/ssl.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"

#include "epd_driver.h"
#include "utilities.h"
#include "screen.h"   // status model + everything drawn on the panel

// ZIP metadata type must be declared before Arduino auto-generates function prototypes.
struct ZipEntryInfo {
    uint32_t localHeaderOffset = 0;
    uint32_t compressedSize = 0;
    uint32_t uncompressedSize = 0;
    uint16_t method = 0;
    String name = "";
};

// Decoded-thumbnail state, declared early for the same prototype reason.
struct PngRenderContext {
    uint32_t srcW = 0;
    uint32_t srcH = 0;
    uint8_t* grey = nullptr;        // srcW*srcH luminance, alpha composited onto white
    uint32_t minX = 0, minY = 0, maxX = 0, maxY = 0;   // bounding box of non-white pixels
    bool initialised = false;
};
// Declared early for the same reason (used in auto-generated prototypes).
class FtpCtrlClient;
class FtpDataClient;
class FtpSession;

// mbedtls handshakes for the FTPS data channel need more than the default 8 KB loop stack.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// ============================================================
// CONFIG
// ============================================================

// WiFi and printer details live in credentials.h (not committed).
// Copy credentials.example.h to credentials.h and fill it in.
#if __has_include("credentials.h")
#include "credentials.h"
#else
#error "Missing credentials.h: copy credentials.example.h to credentials.h and fill in your details"
#endif

const uint16_t MQTT_PORT = 8883;
const uint16_t FTPS_PORT = 990;

const unsigned long POLL_INTERVAL       = 10UL * 1000UL;   // check the printer for a new job
const unsigned long MIN_REDRAW_INTERVAL = 60UL * 1000UL;   // regular screen updates; a new job bypasses this

// Preview area while printing
const unsigned long THUMB_RETRY_MS         = 5UL * 1000UL;    // gap between failed preview fetches
const unsigned long THUMB_REFUSED_RETRY_MS = 30UL * 1000UL;   // first gap when the printer refuses FTP (doubles, max 2 min)
// The preview's position and size (PREVIEW_*) are part of the layout in screen.h.

// ============================================================
// MQTT / DISPLAY
// ============================================================

WiFiClientSecure secureClient;
PubSubClient mqtt(secureClient);

String printerSerial = "";
String requestTopic = "";
unsigned long lastPoll = 0;
bool firstFullStatusReceived = false;

uint8_t* framebuffer = nullptr;
bool screenDirty = false;
bool forceRender = false;   // bypass the redraw throttle (connect / first status)

// ============================================================
// PRINTER STATE (PrinterStatus is defined in screen.h)
// ============================================================

PrinterStatus status;

// ============================================================
// THUMBNAIL STATE
// ============================================================

uint8_t* thumbnailPng = nullptr;
size_t thumbnailPngSize = 0;
String thumbnailKey = "";
bool thumbnailReady = false;
bool thumbnailFetchPending = false;
bool thumbnailAttempted = false;
int thumbnailAttempts = 0;
unsigned long thumbnailRetryAt = 0;

// ============================================================
// LOGGING: mirrors everything to Serial and to /log.txt on the SD card
// ============================================================

class TeeLog : public Print {
public:
    bool sdOk = false;
    File file;

    void beginSD() {
        SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
        sdOk = SD.begin(SD_CS, SPI);
        if (sdOk) {
            file = SD.open("/log.txt", FILE_APPEND);
            if (!file) sdOk = false;
        }
        Serial.println(sdOk ? "SD card ready, logging to /log.txt" : "SD card not available");
        if (sdOk) file.println("\n===== boot =====");
    }

    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t* buf, size_t len) override {
        Serial.write(buf, len);
        if (sdOk && file) {
            file.write(buf, len);
            if (memchr(buf, '\n', len)) file.flush();
        }
        return len;
    }
};

TeeLog Log;

// Cache the current job's thumbnail (/thumb.png) and the job it belongs to
// (/thumb.key), so a restart mid-print reuses it instead of downloading again.
void saveThumbnailToSD(const uint8_t* data, size_t size, const String& key) {
    if (!Log.sdOk) return;
    SD.remove("/thumb.key");   // invalid until the PNG is fully written
    File f = SD.open("/thumb.png", FILE_WRITE);
    if (!f) return;
    size_t written = f.write(data, size);
    f.close();
    if (written != size) {
        Log.println("SD: thumbnail write incomplete");
        return;
    }
    File k = SD.open("/thumb.key", FILE_WRITE);
    if (!k) return;
    k.print(key);
    k.close();
    Log.printf("Saved /thumb.png (%u bytes) for %s\n", (unsigned)size, key.c_str());
}

// Loads the cached thumbnail into PSRAM if it belongs to `key`.
bool loadThumbnailFromSD(const String& key, uint8_t*& out, size_t& outSize) {
    if (!Log.sdOk || !SD.exists("/thumb.key") || !SD.exists("/thumb.png")) return false;

    File k = SD.open("/thumb.key", FILE_READ);
    if (!k) return false;
    String saved = k.readString();
    k.close();
    if (saved != key) return false;

    File f = SD.open("/thumb.png", FILE_READ);
    if (!f) return false;
    size_t size = f.size();
    if (size < 8 || size > 2UL * 1024UL * 1024UL) {
        f.close();
        return false;
    }
    uint8_t* buf = (uint8_t*)ps_malloc(size);
    if (!buf) {
        f.close();
        return false;
    }
    size_t got = f.read(buf, size);
    f.close();
    if (got != size) {
        free(buf);
        return false;
    }
    out = buf;
    outSize = size;
    return true;
}

// ============================================================
// HELPERS
// ============================================================

String cleanState(String s) {
    if (s == "RUNNING") return "PRINTING";
    if (s == "PAUSE")   return "PAUSED";
    if (s == "FINISH")  return "FINISHED";
    if (s == "PREPARE") return "PREPARING";
    if (s == "FAILED")  return "FAILED";
    if (s == "IDLE")    return "IDLE";
    return s;
}

bool isPrinting() {
    return isActiveState(status.state);
}

static uint16_t rd16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

void clearThumbnail() {
    if (thumbnailPng) {
        free(thumbnailPng);
        thumbnailPng = nullptr;
    }
    thumbnailPngSize = 0;
    thumbnailReady = false;
}

// ============================================================
// MINIMAL IMPLICIT-FTPS CLIENT
// Reads only byte ranges from a remote file.
//
// The printer runs vsFTPd with require_ssl_reuse: a data connection is refused
// ("522 ... session reuse required") unless its TLS handshake resumes the
// control connection's session. WiFiClientSecure cannot do that, so the data
// connection is a plain socket driven by mbedtls with the control session
// copied into it.
// ============================================================

class FtpCtrlClient : public WiFiClientSecure {
public:
    mbedtls_ssl_context* tls() { return sslclient ? &sslclient->ssl_ctx : nullptr; }
};

class FtpDataClient {
public:
    FtpDataClient() {
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
        mbedtls_ctr_drbg_init(&drbg);
        mbedtls_entropy_init(&entropy);
    }

    ~FtpDataClient() {
        stop();
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
    }

    bool connect(FtpCtrlClient& ctrl, uint16_t port) {
        mbedtls_ssl_context* ctrlTls = ctrl.tls();
        if (!ctrlTls) return false;

        if (!tcp.connect(PRINTER_IP, port, 6000)) {
            Log.println("FTP data: TCP connect failed");
            return false;
        }
        net.fd = tcp.fd();
        struct timeval tv = {6, 0};
        setsockopt(net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(net.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, nullptr, 0) != 0) return false;
        if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) != 0) return false;
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
        if (mbedtls_ssl_setup(&ssl, &conf) != 0) return false;
        mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, mbedtls_net_recv, nullptr);

        mbedtls_ssl_session session;
        mbedtls_ssl_session_init(&session);
        if (mbedtls_ssl_get_session(ctrlTls, &session) == 0)
            mbedtls_ssl_set_session(&ssl, &session);
        mbedtls_ssl_session_free(&session);

        uint32_t start = millis();
        int ret;
        while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
            if ((ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) ||
                millis() - start > 8000) {
                Log.printf("FTP data: TLS handshake failed (-0x%04x)\n", -ret);
                return false;
            }
            delay(1);
        }
        handshaken = true;
        return true;
    }

    // Returns bytes read, or 0 when the server closed the connection / timed out.
    int read(uint8_t* buf, size_t len) {
        if (!handshaken) return 0;
        uint32_t start = millis();
        while (millis() - start < 6000) {
            int r = mbedtls_ssl_read(&ssl, buf, len);
            if (r > 0) return r;
            if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
            delay(1);
        }
        return 0;
    }

    void stop() {
        if (handshaken) mbedtls_ssl_close_notify(&ssl);
        handshaken = false;
        tcp.stop();
    }

private:
    WiFiClient tcp;
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context entropy;
    bool handshaken = false;
};

bool ftpReadLine(WiFiClientSecure& c, String& line, uint32_t timeoutMs = 5000) {
    line = "";
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        while (c.available()) {
            char ch = (char)c.read();
            if (ch == '\n') {
                line.trim();
                Log.println("FTP < " + line);
                return true;
            }
            if (ch != '\r') line += ch;
        }
        if (!c.connected()) return false;
        delay(1);
    }
    return false;
}

int ftpResponse(WiFiClientSecure& c, String* lastLine = nullptr, uint32_t timeoutMs = 5000) {
    String line;
    int code = -1;
    uint32_t start = millis();

    while (millis() - start < timeoutMs) {
        if (!ftpReadLine(c, line, timeoutMs)) return -1;
        if (line.length() >= 3 && isDigit(line[0]) && isDigit(line[1]) && isDigit(line[2])) {
            code = line.substring(0, 3).toInt();
            if (line.length() == 3 || line[3] == ' ') {
                if (lastLine) *lastLine = line;
                return code;
            }
        }
    }
    return code;
}

bool ftpCommand(WiFiClientSecure& c, const String& cmd, int expectedHundreds, String* reply = nullptr) {
    Log.println("FTP > " + (cmd.startsWith("PASS ") ? String("PASS ****") : cmd));   // keep the access code out of the log
    c.print(cmd + "\r\n");
    int code = ftpResponse(c, reply);
    return code >= expectedHundreds * 100 && code < (expectedHundreds + 1) * 100;
}

// Set when the printer refuses the control connection outright (as opposed to a
// missing file), so the retry loop can back off and let stale sessions expire.
bool ftpRefused = false;

bool ftpLogin(FtpCtrlClient& ctrl) {
    // FTP work blocks for a while; keep the MQTT session alive between steps.
    if (mqtt.connected()) mqtt.loop();

    ctrl.setInsecure();
    ctrl.setHandshakeTimeout(8);
    if (!ctrl.connect(PRINTER_IP, FTPS_PORT)) {
        Log.println("FTP: control connect failed");
        ftpRefused = true;
        return false;
    }
    ctrl.setTimeout(6);

    if (ftpResponse(ctrl) / 100 != 2) return false;
    if (!ftpCommand(ctrl, "USER bblp", 3)) return false;
    if (!ftpCommand(ctrl, String("PASS ") + ACCESS_CODE, 2)) return false;
    if (!ftpCommand(ctrl, "PBSZ 0", 2)) return false;
    if (!ftpCommand(ctrl, "PROT P", 2)) return false;
    if (!ftpCommand(ctrl, "TYPE I", 2)) return false;
    return true;
}

// One logged-in control connection shared by every command of a thumbnail fetch.
// The printer only allows a few FTP sessions, and ones dropped without QUIT
// linger on it for minutes, so logging in per request used up the limit.
class FtpSession {
public:
    ~FtpSession() { close(); }

    // Logs in unless already connected.
    bool ensure() {
        if (loggedIn && ctrl.connected()) return true;
        drop();
        loggedIn = ftpLogin(ctrl);
        if (!loggedIn) close();
        return loggedIn;
    }

    // Abandon the connection without QUIT, for when its state is unknown.
    void drop() {
        ctrl.stop();
        loggedIn = false;
    }

    void close() {
        if (ctrl.connected()) ftpCommand(ctrl, "QUIT", 2);
        drop();
    }

    FtpCtrlClient ctrl;

private:
    bool loggedIn = false;
};

bool ftpSize(FtpSession& s, const String& path, uint32_t& sizeOut) {
    if (!s.ensure()) return false;

    String reply;
    Log.println("FTP > SIZE " + path);
    s.ctrl.print("SIZE " + path + "\r\n");
    int code = ftpResponse(s.ctrl, &reply);
    if (code < 0) {
        s.drop();
        return false;
    }

    if (code != 213) return false;
    int sp = reply.indexOf(' ');
    if (sp < 0) return false;
    sizeOut = strtoul(reply.substring(sp + 1).c_str(), nullptr, 10);
    return sizeOut > 0;
}

bool ftpEnterPassive(WiFiClientSecure& ctrl, uint16_t& portOut) {
    String reply;
    Log.println("FTP > PASV");
    ctrl.print("PASV\r\n");
    int code = ftpResponse(ctrl, &reply);
    if (code != 227) return false;

    int l = reply.indexOf('(');
    int r = reply.indexOf(')', l + 1);
    if (l < 0 || r < 0) return false;

    String body = reply.substring(l + 1, r);
    int vals[6] = {0};
    int idx = 0;
    int start = 0;
    for (int i = 0; i <= (int)body.length() && idx < 6; i++) {
        if (i == (int)body.length() || body[i] == ',') {
            vals[idx++] = body.substring(start, i).toInt();
            start = i + 1;
        }
    }
    if (idx != 6) return false;
    portOut = (uint16_t)(vals[4] * 256 + vals[5]);
    return true;
}

// Sends REST for RETR, opens the passive data connection and sends `cmd`
// (RETR / LIST). On success the data connection is open and ready to read.
bool ftpOpenTransfer(FtpSession& s, FtpDataClient& data, const String& cmd, uint32_t offset) {
    if (!s.ensure()) return false;
    if (mqtt.connected()) mqtt.loop();

    // Always send REST for RETR so a stale offset from an earlier failure can't apply.
    if (cmd.startsWith("RETR ") && !ftpCommand(s.ctrl, "REST " + String(offset), 3)) return false;

    uint16_t dataPort = 0;
    if (!ftpEnterPassive(s.ctrl, dataPort)) return false;

    // vsFTPd sends the 150 reply, then waits for the data-channel TLS handshake.
    Log.println("FTP > " + cmd);
    s.ctrl.print(cmd + "\r\n");
    if (!data.connect(s.ctrl, dataPort)) return false;

    int code = ftpResponse(s.ctrl);
    return code / 100 == 1;
}

// Closes the data connection and reads the server's final reply (226, or 426
// when we stopped early) so the control connection can be reused. A transfer
// that never opened leaves the control connection in an unknown state, so it
// is dropped and the next command logs in again.
void ftpEndTransfer(FtpSession& s, FtpDataClient& data, bool opened) {
    data.stop();
    if (!opened || ftpResponse(s.ctrl, nullptr, 8000) < 0) s.drop();
}

bool ftpReadRange(FtpSession& s, const String& path, uint32_t offset, uint8_t* out, size_t length) {
    FtpDataClient data;

    size_t got = 0;
    bool opened = ftpOpenTransfer(s, data, "RETR " + path, offset);
    if (opened) {
        while (got < length) {
            int n = data.read(out + got, length - got);
            if (n <= 0) break;
            got += n;
        }
    }

    // We intentionally close the data connection after the requested range.
    ftpEndTransfer(s, data, opened);
    if (got != length) Log.printf("FTP: read %u of %u bytes\n", (unsigned)got, (unsigned)length);
    return got == length;
}

// ============================================================
// REMOTE ZIP READER
// Bambu plate PNGs are stored as PNG members in the .3mf ZIP.
// We only fetch the central directory + selected PNG bytes.
// ============================================================

bool findZipThumbnail(FtpSession& s, const String& remotePath, ZipEntryInfo& found) {
    uint32_t fileSize = 0;
    if (!ftpSize(s, remotePath, fileSize)) return false;

    size_t tailSize = min((uint32_t)65557, fileSize);
    uint8_t* tail = (uint8_t*)ps_malloc(tailSize);
    if (!tail) return false;

    if (!ftpReadRange(s, remotePath, fileSize - tailSize, tail, tailSize)) {
        free(tail);
        return false;
    }

    int eocd = -1;
    for (int i = (int)tailSize - 22; i >= 0; i--) {
        if (rd32(tail + i) == 0x06054b50UL) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        free(tail);
        return false;
    }

    uint32_t cdSize = rd32(tail + eocd + 12);
    uint32_t cdOffset = rd32(tail + eocd + 16);
    free(tail);

    if (cdSize == 0 || cdSize > 1024UL * 1024UL) return false;

    uint8_t* cd = (uint8_t*)ps_malloc(cdSize);
    if (!cd) return false;
    if (!ftpReadRange(s, remotePath, cdOffset, cd, cdSize)) {
        free(cd);
        return false;
    }

    // The printed plate is in gcode_file, e.g. "/data/Metadata/plate_3.gcode".
    String plate = "plate_1";
    int ps = status.gcodeFile.lastIndexOf("plate_");
    int pe = status.gcodeFile.lastIndexOf(".gcode");
    if (ps >= 0 && pe > ps) plate = status.gcodeFile.substring(ps, pe);

    // plate_N.png is 512x512 and suits the 250 px preview; the rest are fallbacks.
    String preferred[] = {
        "Metadata/" + plate + ".png",
        "Metadata/" + plate + "_small.png",
        "Metadata/plate_1.png",
        "Auxiliaries/.thumbnails/thumbnail_middle.png",
        "Auxiliaries/.thumbnails/thumbnail_3mf.png"
    };

    bool ok = false;
    for (const String& wanted : preferred) {
        size_t pos = 0;
        while (pos + 46 <= cdSize) {
            if (rd32(cd + pos) != 0x02014b50UL) break;
            uint16_t method   = rd16(cd + pos + 10);
            uint32_t compSize = rd32(cd + pos + 20);
            uint32_t rawSize  = rd32(cd + pos + 24);
            uint16_t nameLen  = rd16(cd + pos + 28);
            uint16_t extraLen = rd16(cd + pos + 30);
            uint16_t commLen  = rd16(cd + pos + 32);
            uint32_t localOfs = rd32(cd + pos + 42);

            if (pos + 46 + nameLen > cdSize) break;
            String name;
            name.reserve(nameLen);
            for (uint16_t i = 0; i < nameLen; i++) name += (char)cd[pos + 46 + i];

            if (name == wanted) {
                found.localHeaderOffset = localOfs;
                found.compressedSize = compSize;
                found.uncompressedSize = rawSize;
                found.method = method;
                found.name = name;
                ok = true;
                break;
            }
            pos += 46 + nameLen + extraLen + commLen;
        }
        if (ok) break;
    }

    free(cd);
    return ok;
}

bool fetchZipMember(FtpSession& s, const String& remotePath, const ZipEntryInfo& e,
                    uint8_t*& out, size_t& outSize) {
    // Read the local ZIP header to find the actual member data offset.
    uint8_t localHeader[30];
    if (!ftpReadRange(s, remotePath, e.localHeaderOffset, localHeader, sizeof(localHeader))) return false;
    if (rd32(localHeader) != 0x04034b50UL) return false;

    uint16_t nameLen  = rd16(localHeader + 26);
    uint16_t extraLen = rd16(localHeader + 28);
    uint32_t dataOfs  = e.localHeaderOffset + 30UL + nameLen + extraLen;

    if (e.uncompressedSize == 0 || e.uncompressedSize > 2UL * 1024UL * 1024UL) {
        Log.println("Thumbnail member is empty or too large");
        return false;
    }

    // ZIP method 0 = stored/uncompressed.
    if (e.method == 0) {
        uint8_t* buf = (uint8_t*)ps_malloc(e.uncompressedSize);
        if (!buf) {
            Log.println("Could not allocate thumbnail buffer in PSRAM");
            return false;
        }

        if (!ftpReadRange(s, remotePath, dataOfs, buf, e.uncompressedSize)) {
            free(buf);
            return false;
        }

        out = buf;
        outSize = e.uncompressedSize;
        return true;
    }

    // ZIP method 8 = raw DEFLATE. pngle already bundles miniz, whose low-level
    // tinfl API uses mz_/tinfl symbols rather than LilyGo's zlib symbols. This
    // lets us support compressed 3MF members without recreating the PNGdec
    // duplicate-zlib problem.
    if (e.method == 8) {
        if (e.compressedSize == 0 || e.compressedSize > 2UL * 1024UL * 1024UL) {
            Log.println("Compressed thumbnail member is empty or too large");
            return false;
        }

        uint8_t* compressed = (uint8_t*)ps_malloc(e.compressedSize);
        uint8_t* decoded    = (uint8_t*)ps_malloc(e.uncompressedSize);

        if (!compressed || !decoded) {
            if (compressed) free(compressed);
            if (decoded) free(decoded);
            Log.println("Could not allocate ZIP inflate buffers in PSRAM");
            return false;
        }

        if (!ftpReadRange(s, remotePath, dataOfs, compressed, e.compressedSize)) {
            free(compressed);
            free(decoded);
            return false;
        }

        size_t produced = tinfl_decompress_mem_to_mem(
            decoded,
            e.uncompressedSize,
            compressed,
            e.compressedSize,
            0  // ZIP uses raw DEFLATE: no zlib header
        );

        free(compressed);

        if (produced == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED ||
            produced != e.uncompressedSize) {
            Log.printf("ZIP inflate failed: produced %lu, expected %lu\n",
                          (unsigned long)produced,
                          (unsigned long)e.uncompressedSize);
            free(decoded);
            return false;
        }

        out = decoded;
        outSize = e.uncompressedSize;
        return true;
    }

    Log.printf("Unsupported thumbnail ZIP compression method: %u\n", e.method);
    return false;
}

void addCandidate(String* arr, int& n, int maxN, const String& s) {
    if (!s.length() || n >= maxN) return;
    for (int i = 0; i < n; i++) if (arr[i] == s) return;
    arr[n++] = s;
}

// Lists a remote directory and returns the names of *.3mf files in it.
// Returns -1 if the listing itself failed (as opposed to an empty folder).
int ftpList3mf(FtpSession& s, const String& dir, String* out, int maxOut) {
    FtpDataClient data;

    String text;
    bool ok = ftpOpenTransfer(s, data, "LIST " + dir, 0);
    if (ok) {
        uint8_t buf[512];
        int n;
        while (text.length() < 16384 && (n = data.read(buf, sizeof(buf))) > 0)
            for (int i = 0; i < n; i++) text += (char)buf[i];
    }
    ftpEndTransfer(s, data, ok);
    if (!ok) return -1;

    int n = 0;
    int pos = 0;
    while (pos < (int)text.length() && n < maxOut) {
        int eol = text.indexOf('\n', pos);
        if (eol < 0) eol = text.length();
        String line = text.substring(pos, eol);
        line.trim();
        pos = eol + 1;

        // "-rw-r--r-- 1 user group size Mon DD HH:MM name": name follows 8 fields.
        if (line.length() == 0 || line[0] == 'd') continue;
        int idx = 0;
        for (int field = 0; field < 8 && idx < (int)line.length(); field++) {
            while (idx < (int)line.length() && line[idx] != ' ') idx++;
            while (idx < (int)line.length() && line[idx] == ' ') idx++;
        }
        String name = line.substring(idx);
        if (name.endsWith(".3mf")) {
            Log.println("FTP found: " + name);
            out[n++] = name;
        }
    }
    return n;
}

String normalizeName(const String& in) {
    String o;
    for (unsigned i = 0; i < in.length(); i++) {
        char c = in[i];
        if (isalnum((unsigned char)c)) o += (char)tolower((unsigned char)c);
    }
    return o;
}

bool fetchCurrentJobThumbnail() {
    clearThumbnail();
    String key = status.taskId + "|" + status.jobName;
    String job = normalizeName(status.jobName);

    // Same job as the cached image (e.g. after a restart): skip the download.
    uint8_t* cached = nullptr;
    size_t cachedSize = 0;
    if (loadThumbnailFromSD(key, cached, cachedSize)) {
        thumbnailPng = cached;
        thumbnailPngSize = cachedSize;
        thumbnailReady = true;
        thumbnailKey = key;
        Log.printf("Thumbnail loaded from SD cache: %u bytes\n", (unsigned)cachedSize);
        return true;
    }

    ftpRefused = false;
    FtpSession ftp;   // sends QUIT when it goes out of scope
    if (!ftp.ensure()) {
        Log.println("Thumbnail: printer refused the FTP connection");
        return false;
    }

    String candidates[24];
    int n = 0;
    bool listed = false;

    // Look at what is actually on the printer's storage: the archive name
    // rarely equals the job name (e.g. "<name>_4_Colors_(PrintByObject).gcode.3mf").
    // Files whose name contains the job name are tried first, then the rest.
    const char* listDirs[] = {"/", "/cache"};
    for (const char* d : listDirs) {
        String found[16];
        int count = ftpList3mf(ftp, d, found, 16);
        if (count < 0) continue;
        listed = true;
        String prefix = String(d) == "/" ? "/" : String(d) + "/";
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < count; i++) {
                String f = normalizeName(found[i]);
                bool match = job.length() && (f.indexOf(job) >= 0 || job.indexOf(f) >= 0);
                if ((pass == 0) == match) addCandidate(candidates, n, 24, prefix + found[i]);
            }
        }
    }

    // Cloud jobs sometimes keep a task-id-named archive in /cache.
    if (status.taskId.length() && status.taskId != "0")
        addCandidate(candidates, n, 24, "/cache/" + status.taskId + ".gcode.3mf");

    if (!listed) Log.println("Thumbnail: could not list printer storage");

    for (int i = 0; i < n; i++) {
        // Stop if the printer started refusing us part-way; retrying every
        // candidate would only open more connections.
        if (ftpRefused) break;
        Log.println("Thumbnail: trying " + candidates[i]);
        ZipEntryInfo entry;
        if (!findZipThumbnail(ftp, candidates[i], entry)) continue;

        Log.printf("Thumbnail member: %s (%lu bytes, method %u)\n",
                      entry.name.c_str(), (unsigned long)entry.uncompressedSize, entry.method);

        uint8_t* png = nullptr;
        size_t pngSize = 0;
        if (fetchZipMember(ftp, candidates[i], entry, png, pngSize)) {
            // MQTT was serviced during the download; drop the result if the job changed.
            if (key != status.taskId + "|" + status.jobName) {
                free(png);
                Log.println("Thumbnail: job changed during download, discarding");
                return false;
            }
            clearThumbnail();
            thumbnailPng = png;
            thumbnailPngSize = pngSize;
            thumbnailReady = true;
            thumbnailKey = key;
            Log.printf("Thumbnail ready: %u bytes\n", (unsigned)thumbnailPngSize);
            saveThumbnailToSD(thumbnailPng, thumbnailPngSize, key);
            return true;
        }
    }

    Log.println("Thumbnail not available locally for this job.");
    return false;
}

// ============================================================
// PNGLE PNG DECODER -> E-PAPER FRAMEBUFFER
//
// Bambu's plate thumbnails are PNG files. pngle uses its own miniz backend
// rather than exporting the same zlib symbols as LilyGo-EPD47, so it avoids
// the duplicate-symbol linker problem caused by PNGdec.
// ============================================================

void pngInitCallback(pngle_t* png, uint32_t w, uint32_t h) {
    PngRenderContext* ctx = (PngRenderContext*)pngle_get_user_data(png);
    if (!ctx || w == 0 || h == 0 || w > 1024 || h > 1024) return;

    ctx->grey = (uint8_t*)ps_malloc((size_t)w * h);
    if (!ctx->grey) {
        Log.println("Could not allocate thumbnail grey buffer");
        return;
    }
    memset(ctx->grey, 255, (size_t)w * h);

    ctx->srcW = w;
    ctx->srcH = h;
    ctx->minX = w;  ctx->minY = h;
    ctx->maxX = 0;  ctx->maxY = 0;
    ctx->initialised = true;
}

void pngDrawCallback(pngle_t* png,
                     uint32_t x, uint32_t y,
                     uint32_t w, uint32_t h,
                     const uint8_t rgba[4]) {
    PngRenderContext* ctx = (PngRenderContext*)pngle_get_user_data(png);
    if (!ctx || !ctx->initialised) return;

    // pngle gives RGBA irrespective of the PNG's original colour type.
    uint16_t lum = (uint16_t)((30UL * rgba[0] + 59UL * rgba[1] + 11UL * rgba[2]) / 100UL);
    uint8_t gray8 = (uint8_t)(255U - ((uint32_t)rgba[3] * (255U - lum) / 255U));

    for (uint32_t yy = y; yy < y + h && yy < ctx->srcH; yy++) {
        for (uint32_t xx = x; xx < x + w && xx < ctx->srcW; xx++) {
            ctx->grey[(size_t)yy * ctx->srcW + xx] = gray8;
        }
    }
    if (gray8 < 240) {
        if (x < ctx->minX) ctx->minX = x;
        if (y < ctx->minY) ctx->minY = y;
        if (x + w - 1 > ctx->maxX) ctx->maxX = x + w - 1;
        if (y + h - 1 > ctx->maxY) ctx->maxY = y + h - 1;
    }
}

// Crop the decoded image to its content, area-average it down to the preview
// size, stretch the contrast and error-diffuse it onto the panel's 16 grey levels.
void renderGreyPreview(PngRenderContext& ctx) {
    int cx0 = 0, cy0 = 0, cw = ctx.srcW, ch = ctx.srcH;
    if (ctx.maxX >= ctx.minX && ctx.maxY >= ctx.minY) {
        const int margin = 8;
        cx0 = max((int)ctx.minX - margin, 0);
        cy0 = max((int)ctx.minY - margin, 0);
        cw = min((int)ctx.maxX + margin + 1, (int)ctx.srcW) - cx0;
        ch = min((int)ctx.maxY + margin + 1, (int)ctx.srcH) - cy0;
    }

    int drawW = PREVIEW_W;
    int drawH = (int)((uint64_t)ch * PREVIEW_W / cw);
    if (drawH > PREVIEW_H) {
        drawH = PREVIEW_H;
        drawW = (int)((uint64_t)cw * PREVIEW_H / ch);
    }
    if (drawW < 1) drawW = 1;
    if (drawH < 1) drawH = 1;
    int offX = PREVIEW_X + (PREVIEW_W - drawW) / 2;
    int offY = PREVIEW_Y + (PREVIEW_H - drawH) / 2;
    Log.printf("PNG thumbnail: %lux%lu, crop %dx%d -> %dx%d\n",
               (unsigned long)ctx.srcW, (unsigned long)ctx.srcH, cw, ch, drawW, drawH);

    // Stretch the contrast of the content (not the white plate): the darkest 1%
    // becomes black and the lightest 1% near-white, so a dark model still spans
    // all 16 grey levels instead of collapsing to black.
    uint32_t hist[256] = {0};
    uint32_t content = 0;
    for (int yy = 0; yy < ch; yy++) {
        const uint8_t* row = ctx.grey + (size_t)(cy0 + yy) * ctx.srcW + cx0;
        for (int xx = 0; xx < cw; xx++) {
            if (row[xx] < 240) { hist[row[xx]]++; content++; }
        }
    }
    int lo = 0, hi = 239;
    if (content > 0) {
        uint32_t acc = 0;
        bool haveLo = false;
        for (int v = 0; v < 240; v++) {
            acc += hist[v];
            if (!haveLo && acc * 100 >= content)      { lo = v; haveLo = true; }
            if (acc * 100 >= content * 99)            { hi = v; break; }
        }
        if (lo > 215) lo = 215;
        if (hi - lo < 24) hi = lo + 24;               // near-flat model: don't blow up noise
    }
    // Tone curve: e-paper renders mid-greys darker than nominal, so lift the mid-tones
    // (gamma < 1) and keep the darkest tone off pure black so shading stays visible.
    const float GAMMA = 0.5f;
    const int FLOOR = 50;    // darkest model tone
    const int TOP = 235;     // lightest model tone; still distinguishable from the white plate
    uint8_t curve[256];
    for (int i = 0; i < 256; i++)
        curve[i] = (uint8_t)(FLOOR + powf(i / 255.0f, GAMMA) * (TOP - FLOOR) + 0.5f);

    int16_t* errCur  = (int16_t*)calloc(drawW + 2, sizeof(int16_t));
    int16_t* errNext = (int16_t*)calloc(drawW + 2, sizeof(int16_t));
    if (!errCur || !errNext) {
        free(errCur);
        free(errNext);
        return;
    }

    for (int dy = 0; dy < drawH; dy++) {
        int sy0 = cy0 + (int)((uint64_t)dy * ch / drawH);
        int sy1 = max(cy0 + (int)((uint64_t)(dy + 1) * ch / drawH), sy0 + 1);
        memset(errNext, 0, (drawW + 2) * sizeof(int16_t));

        for (int dx = 0; dx < drawW; dx++) {
            int sx0 = cx0 + (int)((uint64_t)dx * cw / drawW);
            int sx1 = max(cx0 + (int)((uint64_t)(dx + 1) * cw / drawW), sx0 + 1);

            uint32_t sum = 0;
            for (int yy = sy0; yy < sy1; yy++) {
                const uint8_t* row = ctx.grey + (size_t)yy * ctx.srcW;
                for (int xx = sx0; xx < sx1; xx++) sum += row[xx];
            }
            int v = (int)(sum / ((uint32_t)(sx1 - sx0) * (sy1 - sy0)));

            if (v >= 245) v = 255;                        // keep the plate background pure white
            else v = curve[constrain((v - lo) * 255 / (hi - lo), 0, 255)];

            v = constrain(v + errCur[dx + 1], 0, 255);
            int q = (v + 8) / 17;                         // nearest of the 16 panel levels
            if (q > 15) q = 15;
            int out = q * 17;
            int err = v - out;

            errCur[dx + 2]  += err * 7 / 16;
            errNext[dx]     += err * 3 / 16;
            errNext[dx + 1] += err * 5 / 16;
            errNext[dx + 2] += err * 1 / 16;

            epd_draw_pixel(offX + dx, offY + dy, (uint8_t)out, framebuffer);
        }
        int16_t* t = errCur; errCur = errNext; errNext = t;
    }

    free(errCur);
    free(errNext);
}

bool drawPngToFramebuffer(const uint8_t* pngData, size_t pngSize) {
    if (!pngData || pngSize < 8) return false;

    static const uint8_t pngSignature[8] = {
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A
    };

    if (memcmp(pngData, pngSignature, sizeof(pngSignature)) != 0) {
        Log.println("Thumbnail member is not a PNG");
        return false;
    }

    pngle_t* png = pngle_new();
    if (!png) {
        Log.println("pngle_new() failed");
        return false;
    }

    PngRenderContext ctx;
    pngle_set_user_data(png, &ctx);
    pngle_set_init_callback(png, pngInitCallback);
    pngle_set_draw_callback(png, pngDrawCallback);

    // Do not enable gamma correction: the e-paper only has 16 grey levels and
    // avoiding the gamma LUT saves memory.

    size_t offset = 0;
    while (offset < pngSize) {
        int fed = pngle_feed(png, pngData + offset, pngSize - offset);

        if (fed < 0) {
            Log.print("PNG decode failed: ");
            Log.println(pngle_error(png));
            pngle_destroy(png);
            if (ctx.grey) free(ctx.grey);
            return false;
        }

        if (fed == 0) {
            // With the entire PNG already in memory, zero progress means pngle
            // cannot consume the remaining bytes.
            Log.println("PNG decode stopped before end of file");
            pngle_destroy(png);
            if (ctx.grey) free(ctx.grey);
            return false;
        }

        offset += (size_t)fed;
    }

    bool ok = ctx.initialised;
    pngle_destroy(png);

    if (ok) {
        renderGreyPreview(ctx);
        Log.println("PNG thumbnail rendered");
    }
    if (ctx.grey) free(ctx.grey);
    return ok;
}

bool drawThumbnailToFramebuffer() {
    if (!thumbnailReady || !thumbnailPng || thumbnailPngSize == 0) return false;
    return drawPngToFramebuffer(thumbnailPng, thumbnailPngSize);
}

// ============================================================
// DISPLAY
// ============================================================

// The layout itself is in screen.h. Every dynamic element registers a "field"
// while drawing; on the next update only fields whose content changed are
// flashed black -> white on the panel and redrawn, instead of the whole screen.
const int FULL_REFRESH_EVERY = 100;   // partial updates between full clears (ghosting)

int prevLayout = -1;
int partialCount = 0;

// Flash one region black then white to clear it, then draw the new content.
// Without the flash the panel can only darken, so inkOnly sends just the black
// pixels: redrawing grey ones (the bar's track) would darken them a step each time.
void pushRegion(Rect_t r, bool flash, bool inkOnly = false) {
    int32_t x0 = max((int32_t)0, r.x) & ~1;
    int32_t y0 = max((int32_t)0, r.y);
    int32_t x1 = min((int32_t)EPD_WIDTH, r.x + r.width);
    int32_t y1 = min((int32_t)EPD_HEIGHT, r.y + r.height);
    x1 = min((int32_t)EPD_WIDTH, (x1 + 1) & ~1);
    if (x1 <= x0 || y1 <= y0) return;

    Rect_t area;
    area.x = x0;
    area.y = y0;
    area.width = x1 - x0;
    area.height = y1 - y0;

    size_t rowBytes = area.width / 2;
    uint8_t* buf = (uint8_t*)ps_malloc(rowBytes * area.height);
    if (!buf) return;
    for (int32_t y = 0; y < area.height; y++)
        memcpy(buf + y * rowBytes, framebuffer + (size_t)(y0 + y) * (EPD_WIDTH / 2) + x0 / 2, rowBytes);
    if (inkOnly) {
        for (size_t i = 0; i < rowBytes * area.height; i++) {
            uint8_t b = buf[i];
            if (b & 0x0F) b |= 0x0F;
            if (b & 0xF0) b |= 0xF0;
            buf[i] = b;
        }
    }

    if (flash) epd_clear_area_cycles(area, 2, 50);
    epd_draw_grayscale_image(area, buf);
    free(buf);
}

void renderDisplay() {
    screenDirty = false;
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

    bool printing = isPrinting();
    // The preview stays up after the print ends, as long as it belongs to the last job.
    bool withThumb = thumbnailReady && thumbnailKey == status.taskId + "|" + status.jobName;
    bool retrying = printing && !withThumb && thumbnailAttempts > 0;
    int layout = screenLayout(printing, withThumb, status.jobName.length() > 0, telemetryMask(status));

    drawMainScreen(status, withThumb, retrying);

    bool full = layout != prevLayout ||
                curFieldCount != prevFieldCount ||
                partialCount >= FULL_REFRESH_EVERY;

    epd_poweron();
    if (full) {
        epd_clear();
        epd_draw_grayscale_image(epd_full_screen(), framebuffer);
        partialCount = 0;
        Log.println("Display updated (full)");
    } else {
        int changed = 0;
        for (int i = 0; i < curFieldCount; i++) {
            if (curFields[i].sig == prevFields[i].sig) continue;
            bool grew = curFields[i].level >= 0 && prevFields[i].level >= 0 &&
                        curFields[i].level >= prevFields[i].level;
            if (grew) pushRegion(curFields[i].rect, false, true);
            else pushRegion(unionRect(curFields[i].rect, prevFields[i].rect), true);
            changed++;
        }
        if (changed) partialCount++;
        Log.printf("Display updated (partial, %d fields)\n", changed);
    }
    epd_poweroff();

    for (int i = 0; i < curFieldCount; i++) prevFields[i] = curFields[i];
    prevFieldCount = curFieldCount;
    prevLayout = layout;
}

// ============================================================
// PARSE BAMBU STATUS
// ============================================================

void parsePrinterStatus(const byte* payload, unsigned int length) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err) {
        Log.print("JSON error: ");
        Log.println(err.c_str());
        return;
    }

    // Reply to get_version: the "ota" module carries the printer's product name.
    JsonObject info = doc["info"];
    if (!info.isNull() && info["command"] == "get_version") {
        for (JsonObject module : info["module"].as<JsonArray>()) {
            if (module["name"] != "ota") continue;
            String name = module["product_name"] | "";
            if (name.length() && name != status.printerName) {
                status.printerName = name;
                Log.println("Printer name: " + name);
                screenDirty = true;
            }
        }
        return;
    }

    JsonObject p = doc["print"];
    if (p.isNull()) return;

    String oldKey = status.taskId + "|" + status.jobName;
    bool wasPrinting = isPrinting();

    if (!p["gcode_state"].isNull()) status.state = cleanState(p["gcode_state"].as<String>());
    if (!p["subtask_name"].isNull()) status.jobName = p["subtask_name"].as<String>();
    if (!p["task_id"].isNull()) status.taskId = p["task_id"].as<String>();
    if (!p["gcode_file"].isNull()) status.gcodeFile = p["gcode_file"].as<String>();

    if (!p["mc_percent"].isNull()) status.progress = p["mc_percent"].as<int>();
    if (!p["layer_num"].isNull()) status.layer = p["layer_num"].as<int>();
    if (!p["total_layer_num"].isNull()) status.totalLayers = p["total_layer_num"].as<int>();
    if (!p["mc_remaining_time"].isNull()) status.remainingMinutes = p["mc_remaining_time"].as<int>();
    if (!p["wifi_signal"].isNull()) status.printerWifi = p["wifi_signal"].as<String>();

    // The X2D packs temps as (target << 16) | current, so keep only the low 16 bits.
    auto unpackTemp = [](float raw) {
        return raw > 65535.0f ? (float)((uint32_t)raw & 0xFFFF) : raw;
    };

    JsonVariant bed = p["device"]["bed"]["info"]["temp"];
    if (!bed.isNull()) status.bedTemp = unpackTemp(bed.as<float>());

    JsonVariant chamber = p["device"]["ctc"]["info"]["temp"];
    if (!chamber.isNull()) status.chamberTemp = unpackTemp(chamber.as<float>());

    JsonArray extruders = p["device"]["extruder"]["info"].as<JsonArray>();
    for (JsonObject extruder : extruders) {
        if (extruder["temp"].isNull()) continue;
        int id = extruder["id"] | -1;
        float temp = unpackTemp(extruder["temp"].as<float>());
        if (id == 0) status.rightNozzleTemp = temp;
        else if (id == 1) status.leftNozzleTemp = temp;
    }

    // Older single-nozzle models (X1, P1, A1) have no "device" block and report
    // flat fields instead. Newer ones send both, so the flat fields are only
    // used until a "device" block has been seen.
    static bool hasDeviceTemps = false;
    if (!bed.isNull() || !extruders.isNull()) hasDeviceTemps = true;
    if (!hasDeviceTemps) {
        if (!p["bed_temper"].isNull()) status.bedTemp = p["bed_temper"].as<float>();
        if (!p["nozzle_temper"].isNull()) status.rightNozzleTemp = p["nozzle_temper"].as<float>();
    }

    JsonArray amsUnits = p["ams"]["ams"].as<JsonArray>();
    if (amsUnits.size() > 0) {
        JsonObject ams = amsUnits[0];
        if (!ams["temp"].isNull()) status.amsTemp = ams["temp"].as<float>();
        if (!ams["humidity"].isNull()) status.amsHumidity = ams["humidity"].as<int>();
        if (!ams["humidity_raw"].isNull()) status.amsHumidityRaw = ams["humidity_raw"].as<int>();
    }

    String newKey = status.taskId + "|" + status.jobName;

    // New job (or a print just started): refresh the screen now, not on the 60 s timer.
    if (firstFullStatusReceived && ((newKey != oldKey && status.taskId.length()) || (isPrinting() && !wasPrinting))) {
        Log.println("New job detected: " + newKey);
        forceRender = true;
    }

    if (isPrinting() && newKey != oldKey && newKey != thumbnailKey) {
        clearThumbnail();
        thumbnailAttempted = false;
        thumbnailAttempts = 0;
        thumbnailRetryAt = 0;
        thumbnailFetchPending = true;
    }

    // If we boot while a print is already running, schedule the first attempt.
    if (isPrinting() && !thumbnailReady && !thumbnailAttempted)
        thumbnailFetchPending = true;

    status.lastMessage = millis();
    if (!firstFullStatusReceived) forceRender = true;
    firstFullStatusReceived = true;
    screenDirty = true;
}

// ============================================================
// MQTT
// ============================================================

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String topicString(topic);

    if (printerSerial.length() == 0 &&
        topicString.startsWith("device/") &&
        topicString.endsWith("/report")) {
        int firstSlash = topicString.indexOf('/');
        int secondSlash = topicString.indexOf('/', firstSlash + 1);
        printerSerial = topicString.substring(firstSlash + 1, secondSlash);
        requestTopic = "device/" + printerSerial + "/request";
        Log.println("Detected printer serial: " + printerSerial);
    }

    parsePrinterStatus(payload, length);
}

void requestPrinterStatus() {
    if (!mqtt.connected() || printerSerial.length() == 0) return;

    static unsigned int sequence = 1;
    String payload;

    // Ask for the printer's name until it has answered once.
    if (status.printerName.length() == 0) {
        JsonDocument info;
        info["info"]["sequence_id"] = String(sequence++);
        info["info"]["command"] = "get_version";
        serializeJson(info, payload);
        mqtt.publish(requestTopic.c_str(), payload.c_str());
        payload = "";
    }

    JsonDocument doc;
    doc["pushing"]["sequence_id"] = String(sequence++);
    doc["pushing"]["command"] = "pushall";
    doc["pushing"]["version"] = 1;
    doc["pushing"]["push_target"] = 1;

    serializeJson(doc, payload);
    mqtt.publish(requestTopic.c_str(), payload.c_str());
    lastPoll = millis();
    Log.println("Requested printer status");
}

void connectMQTT() {
    while (!mqtt.connected()) {
        Log.print("Connecting to printer MQTT...");
        String clientID = "BambuMonitor-" + String((uint32_t)ESP.getEfuseMac(), HEX);

        if (mqtt.connect(clientID.c_str(), "bblp", ACCESS_CODE)) {
            Log.println("connected");
            mqtt.subscribe("device/+/report");
            // Ask for a full status right away rather than waiting for the printer to publish.
            if (printerSerial.length() > 0) requestPrinterStatus();
        } else {
            Log.print("failed, rc=");
            Log.println(mqtt.state());
            delay(5000);
        }
    }
}

void connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Log.print("Connecting WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        Log.print(".");
        delay(500);
    }
    Log.println();
    Log.print("WiFi connected: ");
    Log.println(WiFi.localIP());
}

// ============================================================
// BOOT SCREEN: one row per step, grey while in progress, ticked when done
// ============================================================

unsigned long bootDoneAt = 0;

// Draw a boot step into its row. A finished step replaces its in-progress
// line, so that row is flashed clean first.
void bootLine(int row, const String& text, bool done) {
    Rect_t area = drawBootLine(row, text, done);
    epd_poweron();
    pushRegion(area, done);
    epd_poweroff();
}

// ============================================================
// SETUP / LOOP
// ============================================================

void setup() {
    Serial.begin(115200);
    delay(1000);
    Log.beginSD();

    framebuffer = (uint8_t*)ps_calloc(sizeof(uint8_t), EPD_WIDTH * EPD_HEIGHT / 2);
    if (!framebuffer) {
        Log.println("Framebuffer allocation failed");
        while (true) delay(1000);
    }

    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
    epd_init();
    drawHeader(status.state);
    epd_poweron();
    epd_clear();
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
    epd_poweroff();

    bootLine(0, "Connecting to WiFi\xE2\x80\xA6", false);
    connectWiFi();
    bootLine(0, "WiFi connected", true);

    secureClient.setInsecure();
    mqtt.setServer(PRINTER_IP, MQTT_PORT);
    mqtt.setCallback(mqttCallback);
    mqtt.setBufferSize(49152);
    mqtt.setKeepAlive(60);
    if (strlen(PRINTER_SERIAL) > 0) {
        printerSerial = PRINTER_SERIAL;
        requestTopic = "device/" + printerSerial + "/request";
    }

    bootLine(1, "Connecting to printer\xE2\x80\xA6", false);
    connectMQTT();
    bootLine(1, "Printer connected", true);
    bootDoneAt = millis();

    // The main screen replaces this one when the first printer status arrives
    // (parsePrinterStatus sets forceRender / screenDirty).
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) connectWiFi();
    if (!mqtt.connected()) connectMQTT();
    mqtt.loop();

    // Until the first status arrives, re-ask every 3 s.
    if (printerSerial.length() > 0 && !firstFullStatusReceived && millis() - lastPoll >= 3000UL)
        requestPrinterStatus();

    if (printerSerial.length() > 0 && millis() - lastPoll >= POLL_INTERVAL)
        requestPrinterStatus();

    // Keep retrying a failed thumbnail fetch until it works or the job changes.
    if (thumbnailRetryAt && (long)(millis() - thumbnailRetryAt) >= 0) {
        thumbnailRetryAt = 0;
        thumbnailFetchPending = true;
    }

    static unsigned long lastRender = 0;
    static bool mainScreenShown = false;

    // If no status has arrived shortly after connecting, show the main screen anyway.
    if (!firstFullStatusReceived && bootDoneAt && millis() - bootDoneAt > 15000UL) {
        bootDoneAt = 0;
        forceRender = true;
        screenDirty = true;
    }

    static String lastSignature = "";

    // Draw before doing any FTPS work: the thumbnail search can take a while and
    // must not hold the boot screen up.
    if (screenDirty &&
        (forceRender || lastRender == 0 || millis() - lastRender >= MIN_REDRAW_INTERVAL)) {
        forceRender = false;

        String sig =
            status.printerName + "|" +
            status.state + "|" + status.jobName + "|" +
            String(status.progress) + "|" +
            String(status.layer) + "|" +
            String(status.totalLayers) + "|" +
            String(status.remainingMinutes) + "|" +
            String((int)status.chamberTemp) + "|" +
            String((int)status.bedTemp) + "|" +
            String((int)status.leftNozzleTemp) + "|" +
            String((int)status.rightNozzleTemp) + "|" +
            String((int)status.amsTemp) + "|" +
            String(status.amsHumidity) + "|" +
            String(status.amsHumidityRaw) + "|thumb=" +
            String(thumbnailReady ? 1 : 0) + "|fail=" +
            String((thumbnailAttempts > 0 && !thumbnailReady) ? 1 : 0);

        if (sig != lastSignature) {
            lastSignature = sig;
            renderDisplay();
            lastRender = millis();
            mainScreenShown = true;
        } else {
            screenDirty = false;
        }
    }

    // Do FTPS work outside the MQTT callback, and only once the main screen is up.
    if (mainScreenShown && thumbnailFetchPending && isPrinting()) {
        thumbnailFetchPending = false;
        thumbnailAttempted = true;
        thumbnailAttempts++;

        Log.printf("Attempting local print thumbnail (attempt %d)...\n", thumbnailAttempts);
        static int refusedStreak = 0;
        if (fetchCurrentJobThumbnail()) {
            forceRender = true;   // show the preview immediately, skip the redraw throttle
            refusedStreak = 0;
        } else {
            unsigned long wait = THUMB_RETRY_MS;
            if (ftpRefused) {
                // The printer is refusing connections: give its stale sessions
                // time to expire (30 s, 60 s, then 2 min).
                int shift = refusedStreak < 2 ? refusedStreak : 2;
                wait = THUMB_REFUSED_RETRY_MS << shift;
                refusedStreak++;
            } else {
                refusedStreak = 0;
            }
            Log.printf("Thumbnail: retrying in %lu s\n", wait / 1000UL);
            thumbnailRetryAt = millis() + wait;
            if (thumbnailAttempts == 1) forceRender = true;   // show the "unavailable" note now
        }
        screenDirty = true;

        // The FTPS operation can take a few seconds, service MQTT immediately after.
        if (mqtt.connected()) mqtt.loop();
    }

    delay(10);
}
