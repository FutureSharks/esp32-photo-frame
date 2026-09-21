/*
 * artfeed.cpp: see artfeed.h.
 */
#include "artfeed.h"

#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <WiFi.h>
#include "el133.h"

/* The Mozilla root store that ships with arduino-esp32, embedded in the core's
 * mbedTLS build. Costs flash rather than RAM, and keeps working when the host
 * rotates its leaf or intermediate certificates - which it will. Pinning one
 * root instead would be smaller and considerably more fragile. */
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

/* Chunk pulled off the socket and pushed to the panel in one go. Small enough
 * to stay clear of the TLS buffers, large enough that per-write overhead is
 * irrelevant against a 960 KB frame. */
static const size_t ARTFEED_CHUNK = 2048;

/* A manifest is one filename per line; this caps how many we will consider. */
static const size_t ARTFEED_MAX_ENTRIES = 512;

/* Any clock at or past this (2023-11-14) is good enough to check a certificate
 * against. Below it, the clock has not been set since power-on. */
static const time_t ARTFEED_CLOCK_SANE = 1700000000;

static bool artfeed_verbose = false;

#define ARTFEED_LOG(...)                \
    do                                  \
    {                                   \
        if (artfeed_verbose)            \
            Serial.printf(__VA_ARGS__); \
    } while (0)

ArtfeedConfig artfeed_default_config()
{
    ArtfeedConfig cfg = {};
    cfg.sequential = false;
    cfg.avoid_recent = 8;
    cfg.insecure = false;
    cfg.wifi_timeout_ms = 30000;
    cfg.http_timeout_ms = 20000;
    return cfg;
}

void artfeed_set_verbose(bool on)
{
    artfeed_verbose = on;
}

bool artfeed_connect(const ArtfeedConfig &cfg)
{
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.ssid, cfg.password);
    ARTFEED_LOG("[feed] connecting to \"%s\"\n", cfg.ssid);

    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED)
    {
        if (millis() - t0 > cfg.wifi_timeout_ms)
        {
            ARTFEED_LOG("[feed] WiFi timed out after %lu ms\n",
                        (unsigned long)(millis() - t0));
            return false;
        }
        delay(200);
    }
    ARTFEED_LOG("[feed] WiFi up in %lu ms, ip %s, rssi %d dBm\n",
                (unsigned long)(millis() - t0), WiFi.localIP().toString().c_str(),
                (int)WiFi.RSSI());

    /* TLS rejects every certificate as not-yet-valid until the clock is roughly
     * right, and a device that has just woken thinks it is 1970.
     *
     * The RTC keeps running through deep sleep, so after the first sync the
     * clock is usually still good and we can skip this entirely. Its drift is
     * minutes per day at worst, against certificate lifetimes measured in
     * months - so plausible is as accurate as this needs to be. */
    if (time(nullptr) >= ARTFEED_CLOCK_SANE)
    {
        ARTFEED_LOG("[feed] clock survived deep sleep, skipping NTP\n");
        return true;
    }

    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    t0 = millis();
    while (time(nullptr) < ARTFEED_CLOCK_SANE)
    {
        if (millis() - t0 > 15000)
        {
            ARTFEED_LOG("[feed] NTP timed out; TLS would fail on cert dates\n");
            return false;
        }
        delay(200);
    }
    ARTFEED_LOG("[feed] clock synced in %lu ms\n", (unsigned long)(millis() - t0));
    return true;
}

void artfeed_disconnect()
{
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    ARTFEED_LOG("[feed] radio off\n");
}

/* Shared setup for both requests. The caller owns both objects. */
static void artfeed_prepare(const ArtfeedConfig &cfg, NetworkClientSecure &client,
                            HTTPClient &http)
{
    if (cfg.insecure)
    {
        client.setInsecure();
    }
    else
    {
        client.setCACertBundle(rootca_crt_bundle_start,
                               (size_t)(rootca_crt_bundle_end - rootca_crt_bundle_start));
    }
    client.setTimeout(cfg.http_timeout_ms / 1000);
    http.setTimeout(cfg.http_timeout_ms);
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
}

/* Report why a request failed in enough detail to tell the causes apart: a
 * negative code is a transport failure (DNS, TCP, TLS), a positive one is an
 * HTTP status and the server said something worth reading. A 404 body of
 * "404: Not Found" means the file is not on the host - most often because the
 * commit adding it has not been pushed. */
static void artfeed_report_failure(NetworkClientSecure &client, HTTPClient &http,
                                   const char *what, const char *url, int code)
{
    if (!artfeed_verbose)
        return;

    Serial.printf("[feed] %s failed\n", what);
    Serial.printf("[feed]   url    %s\n", url);
    Serial.printf("[feed]   code   %d (%s)\n", code, http.errorToString(code).c_str());

    if (code < 0)
    {
        /* Transport-level: nothing HTTP to show, but mbedTLS usually has a
         * reason, and the WiFi state distinguishes "lost the AP" from
         * "handshake rejected". */
        char err[128] = {0};
        int tls = client.lastError(err, sizeof(err));
        Serial.printf("[feed]   tls    %d %s\n", tls, err[0] ? err : "(no detail)");
        Serial.printf("[feed]   wifi   %s, rssi %d dBm, dns %s\n",
                      WiFi.isConnected() ? "connected" : "DISCONNECTED",
                      (int)WiFi.RSSI(), WiFi.dnsIP().toString().c_str());
    }
    else
    {
        Serial.printf("[feed]   length %d\n", http.getSize());
        String body = http.getString();
        if (body.length() > 160)
            body = body.substring(0, 160) + "...";
        body.replace("\n", " ");
        Serial.printf("[feed]   body   %s\n", body.c_str());
    }
    Serial.printf("[feed]   heap   %u free\n", (unsigned)ESP.getFreeHeap());
}

/* Remember the last few choices so the same piece does not come round twice in
 * a row. Stored as a ring in NVS. */
static bool artfeed_recently_shown(Preferences &prefs, uint8_t depth, const char *name)
{
    char key[16];
    for (uint8_t i = 0; i < depth; i++)
    {
        snprintf(key, sizeof(key), "r%u", (unsigned)i);
        String seen = prefs.getString(key, "");
        if (seen.length() > 0 && seen.equals(name))
            return true;
    }
    return false;
}

static void artfeed_remember(Preferences &prefs, uint8_t depth, const char *name)
{
    if (depth == 0)
        return;

    uint8_t head = prefs.getUChar("head", 0) % depth;
    char key[16];
    snprintf(key, sizeof(key), "r%u", (unsigned)head);
    prefs.putString(key, name);
    prefs.putUChar("head", (uint8_t)((head + 1) % depth));
}

bool artfeed_pick(const ArtfeedConfig &cfg, char *name, size_t name_len)
{
    String url = String(cfg.base_url) + "manifest.txt";

    NetworkClientSecure client;
    HTTPClient http;
    artfeed_prepare(cfg, client, http);

    ARTFEED_LOG("[feed] GET %s\n", url.c_str());
    if (!http.begin(client, url))
    {
        ARTFEED_LOG("[feed] cannot parse url %s\n", url.c_str());
        return false;
    }

    uint32_t t0 = millis();
    int code = http.GET();
    if (code != HTTP_CODE_OK)
    {
        artfeed_report_failure(client, http, "manifest GET", url.c_str(), code);
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();
    ARTFEED_LOG("[feed] manifest: %u bytes in %lu ms\n", (unsigned)body.length(),
                (unsigned long)(millis() - t0));

    /* Collect the line offsets rather than copying the names; a manifest of a
     * few hundred entries is only a few KB, but there is no reason to double
     * it. */
    int starts[ARTFEED_MAX_ENTRIES];
    int lengths[ARTFEED_MAX_ENTRIES];
    size_t count = 0;

    int i = 0;
    const int len = body.length();
    while (i < len && count < ARTFEED_MAX_ENTRIES)
    {
        int end = body.indexOf('\n', i);
        if (end < 0)
            end = len;

        int lineEnd = end;
        while (lineEnd > i && (body[lineEnd - 1] == '\r' || body[lineEnd - 1] == ' '))
            lineEnd--;

        if (lineEnd > i && body[i] != '#')
        {
            starts[count] = i;
            lengths[count] = lineEnd - i;
            count++;
        }
        i = end + 1;
    }

    if (count == 0)
    {
        /* The request succeeded but produced no usable lines - normally an
         * error page served with a 200, or a manifest of only comments. */
        String preview = body.substring(0, body.length() < 160 ? body.length() : 160);
        preview.replace("\n", " ");
        ARTFEED_LOG("[feed] manifest has no usable entries; body was: %s\n",
                    preview.c_str());
        return false;
    }
    ARTFEED_LOG("[feed] manifest lists %u image(s)\n", (unsigned)count);

    Preferences prefs;
    prefs.begin("artfeed", false);

    size_t chosen = 0;
    uint8_t depth = 0;

    if (cfg.sequential)
    {
        /* Resume where the last wake left off. Taken modulo the current count
         * so the position stays valid when images are added or removed. */
        uint32_t pos = prefs.getUInt("seq", 0) % count;
        chosen = (size_t)pos;
        prefs.putUInt("seq", (uint32_t)((pos + 1) % count));
    }
    else
    {
        /* esp_random() is a real hardware RNG, so no seeding needed. Try a
         * handful of times to dodge a recent repeat, then accept whatever we
         * have - with fewer images than the avoid depth, every choice is a
         * repeat. */
        depth = cfg.avoid_recent;
        if (depth >= count)
            depth = (count > 1) ? (uint8_t)(count - 1) : 0;

        for (int attempt = 0; attempt < 12; attempt++)
        {
            chosen = (size_t)(esp_random() % count);
            String candidate =
                body.substring(starts[chosen], starts[chosen] + lengths[chosen]);
            if (depth == 0 || !artfeed_recently_shown(prefs, depth, candidate.c_str()))
                break;
        }
    }

    String pick = body.substring(starts[chosen], starts[chosen] + lengths[chosen]);
    if (!cfg.sequential)
        artfeed_remember(prefs, depth, pick.c_str());
    prefs.end();

    if (pick.length() + 1 > name_len)
    {
        ARTFEED_LOG("[feed] name \"%s\" too long for buffer\n", pick.c_str());
        return false;
    }
    strncpy(name, pick.c_str(), name_len - 1);
    name[name_len - 1] = '\0';

    ARTFEED_LOG("[feed] chose \"%s\" (%u of %u, %s)\n", name, (unsigned)(chosen + 1),
                (unsigned)count, cfg.sequential ? "in order" : "random");
    return true;
}

bool artfeed_show(const ArtfeedConfig &cfg, const char *name)
{
    String url = String(cfg.base_url) + name;

    /* Init the panel first. It takes ~7 s of blocking delays, and doing that
     * after the response headers arrive leaves the socket unread for long
     * enough that the receive window fills and the connection stalls or the
     * server drops it. Nothing is displayed until the refresh, so paying this
     * cost before we know the download will succeed is harmless. */
    uint32_t t0 = millis();
    el133_init_panel();

    NetworkClientSecure client;
    HTTPClient http;
    artfeed_prepare(cfg, client, http);

    ARTFEED_LOG("[feed] GET %s\n", url.c_str());
    if (!http.begin(client, url))
    {
        ARTFEED_LOG("[feed] cannot parse url %s\n", url.c_str());
        return false;
    }

    int code = http.GET();
    if (code != HTTP_CODE_OK)
    {
        artfeed_report_failure(client, http, "image GET", url.c_str(), code);
        http.end();
        return false;
    }

    int size = http.getSize();
    if (size >= 0 && (size_t)size != EL133_STREAM_BYTES)
    {
        /* Almost always an HTML error page served with a 200, or a frame packed
         * for different geometry. */
        ARTFEED_LOG("[feed] wrong length: %d bytes, expected %u - not a packed frame?\n",
                    size, (unsigned)EL133_STREAM_BYTES);
        http.end();
        return false;
    }

    ARTFEED_LOG("[feed] streaming %u bytes\n", (unsigned)EL133_STREAM_BYTES);
    t0 = millis();

    WiFiClient *stream = http.getStreamPtr();
    uint8_t buf[ARTFEED_CHUNK];
    bool ok = true;

    for (int half = 0; half < 2 && ok; half++)
    {
        el133_stream_begin(half);

        size_t remaining = EL133_HALF_BYTES;
        uint32_t lastData = millis();
        size_t nextMark = EL133_HALF_BYTES - EL133_HALF_BYTES / 4;

        while (remaining > 0)
        {
            size_t want = remaining < sizeof(buf) ? remaining : sizeof(buf);
            int got = stream->readBytes(buf, want);

            if (got > 0)
            {
                el133_stream_write(buf, (size_t)got);
                remaining -= (size_t)got;
                lastData = millis();

                if (remaining <= nextMark)
                {
                    ARTFEED_LOG("[feed]   half %d: %u of %u bytes\n", half,
                                (unsigned)(EL133_HALF_BYTES - remaining),
                                (unsigned)EL133_HALF_BYTES);
                    nextMark = (nextMark > EL133_HALF_BYTES / 4)
                                   ? nextMark - EL133_HALF_BYTES / 4
                                   : 0;
                }
                continue;
            }

            /* An empty read is not automatically the end: TLS records arrive in
             * bursts and the window can stall briefly. Only give up once the
             * peer has gone and there is nothing buffered, or nothing has
             * arrived for a while. */
            if (!stream->connected() && stream->available() == 0)
            {
                ARTFEED_LOG("[feed] connection closed with %u bytes of half %d "
                            "outstanding\n",
                            (unsigned)remaining, half);
                ok = false;
                break;
            }
            if (millis() - lastData > cfg.http_timeout_ms)
            {
                ARTFEED_LOG("[feed] stalled for %lu ms with %u bytes of half %d "
                            "outstanding\n",
                            (unsigned long)(millis() - lastData), (unsigned)remaining,
                            half);
                ok = false;
                break;
            }
            delay(10);
        }

        el133_stream_end();
    }

    http.end();

    if (!ok)
    {
        /* Skipping the refresh leaves whatever is on the panel alone. */
        ARTFEED_LOG("[feed] download incomplete, not refreshing\n");
        return false;
    }
    ARTFEED_LOG("[feed] streamed %u bytes in %lu ms\n", (unsigned)EL133_STREAM_BYTES,
                (unsigned long)(millis() - t0));

    /* The radio is the expensive part of the wake cycle and the refresh is the
     * long part; no reason to overlap them. */
    artfeed_disconnect();

    return el133_refresh();
}
