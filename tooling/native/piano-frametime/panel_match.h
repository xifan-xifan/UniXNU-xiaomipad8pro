// SPDX-License-Identifier: MIT
#ifndef FT_PANEL_MATCH_H
#define FT_PANEL_MATCH_H
#include <stdbool.h>
#include <stdint.h>

enum { FT_PANEL_VENDOR = 0x61a9, FT_PANEL_PRODUCT = 0x8750 };

// A CG identity can be synthesized for an internal IOMFB display. Never accept
// that generic identity without the panel UUID + product IDs in IORegistry.
static inline unsigned ft_panel_match(uint32_t vendor, uint32_t product,
                                     unsigned width, unsigned height, bool builtin,
                                     bool driver_verified, unsigned driver_width,
                                     unsigned driver_height, bool driver_builtin,
                                     bool product_name_matches)
{
    if (vendor == FT_PANEL_VENDOR && product == FT_PANEL_PRODUCT)
        return 2;
    if (!driver_verified || !driver_width || !driver_height ||
        width != driver_width || height != driver_height)
        return 0;
    if (product_name_matches)
        return 1;
    // SkyLight 959.4's internal-display fallback: Apple / unknown model.
    if (driver_builtin && builtin && vendor == 0x610 && product == 0x756e6b6e)
        return 1;
    return 0;
}
#endif
