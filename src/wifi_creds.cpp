#include "wifi_creds.h"

#include <Preferences.h>

static Preferences prefs;
static const char *kNs = "moist_wifi";
static const char *kKeySsid = "ssid";
static const char *kKeyPass = "pass";

static String gSsid;
static String gPass;
static bool gConfigured = false;

void wifiCredsBegin() {
  prefs.begin(kNs, true);
  gSsid = prefs.getString(kKeySsid, "");
  gPass = prefs.getString(kKeyPass, "");
  prefs.end();
  gConfigured = gSsid.length() > 0;
}

bool wifiCredsConfigured() { return gConfigured; }

String wifiCredsSsid() { return gSsid; }

String wifiCredsPass() { return gPass; }

bool wifiCredsApply(const String &ssid, const String &pass) {
  String s = ssid;
  s.trim();
  if (s.length() == 0 || s.length() > 32) {
    return false;
  }
  if (pass.length() > 63) {
    return false;
  }
  prefs.begin(kNs, false);
  prefs.putString(kKeySsid, s);
  prefs.putString(kKeyPass, pass);
  prefs.end();
  gSsid = s;
  gPass = pass;
  gConfigured = true;
  return true;
}
