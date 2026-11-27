// SPDX-License-Identifier: BSD-2-Clause-Patent
// Synthetic boot arguments and display IDs. A disabled agent must never enter backendInit.
#define main rotated_main
#define sysctlbyname fixture_sysctlbyname
#define dlopen unexpected_dlopen
#define CGGetOnlineDisplayList fixture_display_list
#define CGDisplayVendorNumber fixture_display_vendor
#define CGDisplayModelNumber fixture_display_model
#define CGDisplayRotation fixture_display_rotation
#include "../daemon/piano-rotated.m"
#undef main
#include <math.h>
#include "check.h"

static char merged[PIANO_BOOT_ARGS_MAX];
static const char *legacy;
static int merged_error, legacy_reads;
static int panel_online;
static double panel_angle;

int fixture_sysctlbyname(const char *name, void *out, size_t *size, void *new_value, size_t new_size)
{
    CHECK(!new_value && !new_size);
    const char *line = merged;
    if (!strcmp(name, "kern.piano_bootargs")) {
        if (merged_error) {
            errno = merged_error;
            return -1;
        }
    } else {
        CHECK(!strcmp(name, "kern.bootargs"));
        ++legacy_reads;
        line = legacy;
    }
    const size_t bytes = strlen(line) + 1;
    CHECK(*size >= bytes);
    memcpy(out, line, bytes);
    *size = bytes;
    return 0;
}

void *unexpected_dlopen(const char *path, int mode)
{
    (void)path;
    (void)mode;
    CHECK(!"disabled agent entered the rotation backend");
    return NULL;
}

CGError fixture_display_list(uint32_t max, CGDirectDisplayID *ids, uint32_t *count)
{
    CHECK(max >= 2);
    ids[0] = 1;
    ids[1] = 2;
    *count = panel_online ? 2 : 1;
    return kCGErrorSuccess;
}

uint32_t fixture_display_vendor(CGDirectDisplayID display) { return display == 2 ? kPanelVendor : 0; }
uint32_t fixture_display_model(CGDirectDisplayID display) { return display == 2 ? kPanelModel : 0; }
double fixture_display_rotation(CGDirectDisplayID display) { CHECK(display == 2); return panel_angle; }

static void expect_off(void)
{
    char *argv[] = {"piano-rotated", NULL};
    CHECK(rotated_main(1, argv) == 0 && gLevel == 0);
}

int main(void)
{
    strcpy(merged, "piano-auto-rotate=0 piano-rotated=3 piano-rotate-test=1");
    expect_off();
    strcpy(merged, "piano-rotate=2 piano-rotate=0");
    expect_off();
    strcpy(merged, "piano-rotate=2junk");
    expect_off();
    strcpy(merged, "piano-rotate=99999999999999999999999999999");
    expect_off();

    // A full, near-limit line with the real keys after the legacy clip.
    for (unsigned i = 0; i < 3900; i += 10) memcpy(merged + i, "padding=1 ", 10);
    strcpy(merged + 3900, "piano-rotate=0\tpiano-rotate=2 piano-rotated=3 piano-rotate-map=0,270,180,90");
    readBootArgs();
    CHECK(bootNumber("piano-rotate", 0, 0, 3) == 2 && legacy_reads == 0);
    ParConfig config;
    configure(&config);
    CHECK(config.rot.degrees[PS_EDGE_LEFT] == 270 && config.rot.degrees[PS_EDGE_RIGHT] == 90);
    strcpy(merged + 3900, "piano-rotate=2 piano-rotate=0 piano-rotate-map=0,270,180,90junk");
    expect_off();
    configure(&config);
    CHECK(config.rot.degrees[PS_EDGE_LEFT] == 90 && config.rot.degrees[PS_EDGE_RIGHT] == 270);

    legacy = "piano-rotate";
    merged_error = ENOENT;
    readBootArgs();
    CHECK(bootNumber("piano-rotate", 0, 0, 3) == 1 && legacy_reads == 1);
    merged_error = EPERM;
    expect_off();
    CHECK(legacy_reads == 1);                  // a failing merged read cannot enable the agent via legacy args

    CHECK(panelDisplay() == 0);               // external-only configuration
    char why[100];
    CHECK(!applyRotation(panelDisplay(), 90, why, sizeof why) && strstr(why, "panel display is unavailable"));
    panel_online = 1;
    CHECK(panelDisplay() == 2);
    panel_angle = -90;
    CHECK(degreesShown(panelDisplay()) == 270);
    panel_angle = 360;
    CHECK(degreesShown(panelDisplay()) == 0);
    panel_angle = 45;
    CHECK(degreesShown(panelDisplay()) == -1);
    panel_angle = NAN;
    CHECK(degreesShown(panelDisplay()) == -1);
    gDisplayOverride = 1;
    CHECK(panelDisplay() == 1);
    printf("PASS daemon: default-off before backend setup, exact late arguments, legacy fallback, panel selection and read-back\n");
    return 0;
}
