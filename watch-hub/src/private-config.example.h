#pragma once
// Copy to private-config.h (gitignored) and fill in. The firmware builds
// without it: Wi-Fi stays offline until Settings -> Reconnect Wi-Fi.

#define WIFI_SSID "your-ssid"
#define WIFI_PASS "your-pass"
// Tried only when the first network does not associate. Leave empty to skip.
#define WIFI_SSID2 ""
#define WIFI_PASS2 ""
#define DEFAULT_SERVER "http://192.168.1.10:8896"
#define INBOX_SECRET ""

// #define DEVICE_ROLE "mom"
// #define SEND_CONTACT "mom"
// #define LISTEN_CONTACT "dad"
