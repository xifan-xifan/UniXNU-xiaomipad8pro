// SPDX-License-Identifier: BSD-2-Clause-Patent
// One boot addon for every user (docs/kc-space.md section 6): the container tooling/persist/firmware.py makes for a RAM image with
// PianoSensors' pack entry and firmware entries is accepted by both readers, each finding only its own entry.
//   shared_addon_host ADDON FIRMWARE_PATH
#include <stdio.h>
#include "SensorsAddon.h"
#include "PianoFirmwareCore.h"

static int readers_agree(const uint8_t *buf, size_t bytes, const char *path)
{
    SaEntry sensors;
    PfwAddonEntry fw;
    const int a = sa_find(buf, bytes, path, &sensors), b = pfw_addon_find(buf, bytes, path, &fw);
    if (a == SA_OK && b == PIANO_FW_OK && sensors.path == fw.path && sensors.mode == fw.mode &&
        sensors.size == fw.size && sensors.compressed == fw.packed_bytes && sensors.data == fw.packed) return 1;
    fprintf(stderr, "FAIL shared addon: readers disagree for %s (sensors %d, firmware %d)\n", path, a, b);
    return 0;
}

int main(int argc, char **argv)
{
    static uint8_t buf[8 << 20];
    FILE *f = argc == 3 ? fopen(argv[1], "rb") : NULL;
    if (!f) return 2;
    const size_t n = fread(buf, 1, sizeof buf, f);
    const int complete = fgetc(f) == EOF && !ferror(f);
    fclose(f);
    if (!complete || !readers_agree(buf, n, SA_PACK_PATH) || !readers_agree(buf, n, argv[2])) return 1;
    printf("PASS shared addon: PianoSensors' reader and the firmware loader both accept one container with both kinds of entries\n");
    return 0;
}
