// SPDX-License-Identifier: BSD-2-Clause-Patent
// The small, private kernel boundary used by PianoWifi's apple80211 front end.
// It carries only the station's already-established association state; SSID and
// addresses are copied into the request answer and never logged or persisted.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PIANO_WIFI_ASSOC_SNAPSHOT_VERSION 3u
#define PIANO_WIFI_ASSOC_SSID_MAX 32u
#define PIANO_WIFI_ASSOC_RSN_MAX 64u

struct piano_wifi_assoc_snapshot {
  uint32_t version;
  uint8_t power;
  uint8_t associated;
  uint8_t authorized;
  uint8_t state;       // Apple80211 STATE: 0 INIT, 3 ASSOC, 4 RUN
  uint8_t ssid_length;
  uint8_t ssid[PIANO_WIFI_ASSOC_SSID_MAX];
  uint8_t bssid[6];
  uint16_t channel;
  uint32_t frequency_mhz;
  int32_t rssi_dbm;
  int32_t noise_dbm;
  uint32_t rate_mbps;
  uint8_t auth_type;  // 1=open, 2=WPA2-PSK
  uint8_t cipher;     // 0=open, 4=CCMP-128
  uint8_t op_mode;    // 1=station
  uint8_t reserved;
  uint32_t associate_result;
  uint32_t user_stop;  // public link hidden during user stop/replacement; local leave is voluntary
  uint32_t join_generation;  // accepted SET20 serial, zero for kernel-only diagnostic joins
  uint16_t capability; // the selected BSS's advertised capability field
  uint8_t rsn_length;
  uint8_t rsn[PIANO_WIFI_ASSOC_RSN_MAX]; // whole RSN IE of the selected BSS
};

// Returns 0 and fills a current-version snapshot. The function is safe before the
// station starts and reports power off/idle in that case.
// Version the symbol as well as the record so a mixed kext pair cannot write
// the larger snapshot into an older front end's buffer.
int piano_wifi_assoc_snapshot_v3(struct piano_wifi_assoc_snapshot *out);

// 1 while the radio can scan and join (a pass reached level 7, the chip is up and the user power is ON), else 0: the front end
// posts it to airportd as DRIVER_AVAILABLE (BSD event 55).
int piano_wifi_assoc_radio_usable(void);

enum { PIANO_WIFI_USER_OFF = 0, PIANO_WIFI_USER_ON = 1, PIANO_WIFI_USER_LEAVE = 2 };
// Leave and ON publish intent. OFF waits for bounded hardware confirmation
// from the radio worker, which owns WMI, DMA and physical power.
int piano_wifi_user_control(uint32_t operation);

#ifdef __cplusplus
}
#endif
