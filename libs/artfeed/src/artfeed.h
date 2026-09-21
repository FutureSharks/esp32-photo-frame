/*
 * artfeed.h: pull a pre-packed e-paper frame over HTTPS and stream it to the
 * panel.
 *
 * The frames live as static files on a web host - in this project, in the repo
 * itself served through raw.githubusercontent.com. A manifest lists what is
 * available; the device picks one at random, avoiding whatever it showed
 * recently, and streams it into the controllers without ever holding the
 * 960 KB frame in memory.
 *
 * Nothing reaches the panel until the refresh, so a failed download leaves the
 * previous image in place. Callers should simply try again later.
 */
#pragma once

#include <Arduino.h>

struct ArtfeedConfig
{
    const char *ssid;
    const char *password;

    /* Base URL the image files sit under, with a trailing slash. The manifest
     * is expected at <base_url>manifest.txt. */
    const char *base_url;

    /* Walk the manifest in order instead of picking at random, resuming where
     * the last wake left off. Useful for reviewing every image in turn;
     * avoid_recent is ignored in this mode. The position is kept in NVS, so it
     * survives deep sleep and power loss. */
    bool sequential;

    /* How many recently shown images to avoid repeating in random mode. Kept in
     * NVS, so it survives deep sleep and power loss. 0 disables the check. */
    uint8_t avoid_recent;

    /* Skip TLS certificate validation. Convenient while bringing the network
     * path up; leave false in a real build. */
    bool insecure;

    uint32_t wifi_timeout_ms;
    uint32_t http_timeout_ms;
};

/* Sensible defaults; ssid, password and base_url still have to be filled in. */
ArtfeedConfig artfeed_default_config();

/* Mirror progress to Serial. Off by default. */
void artfeed_set_verbose(bool on);

/* Bring up WiFi and sync the clock over NTP. TLS certificate validation needs a
 * correct date, and a freshly woken device has no idea what time it is.
 * Returns false if either step fails. */
bool artfeed_connect(const ArtfeedConfig &cfg);

/* Drop the radio. Worth doing before the ~35 s refresh - it is the single
 * biggest saving in the whole wake cycle. */
void artfeed_disconnect();

/* Fetch the manifest and choose an image - the next one in order if
 * cfg.sequential, otherwise at random avoiding recent repeats. The chosen name
 * is written to `name`. Returns false if the manifest could not be read or is
 * empty. */
bool artfeed_pick(const ArtfeedConfig &cfg, char *name, size_t name_len);

/* Download `name` and stream it into the panel. Does the panel init, streams
 * both halves, and refreshes. Returns false without refreshing if the download
 * fails or is the wrong length, leaving the current image untouched. */
bool artfeed_show(const ArtfeedConfig &cfg, const char *name);
