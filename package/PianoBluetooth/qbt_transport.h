/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef QBT_TRANSPORT_H
#define QBT_TRANSPORT_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct qbt_transport_ops {
    void *context;
    void (*wake)(void *);
    /* Queue power transition on the UART worker; never call back inline. */
    int (*power)(void *, bool);
};
extern volatile uint32_t qbt_transport_activity[4];
extern bool qbt_bt_trace_enabled;
extern bool qbt_bt_wait_baseline;
/* piano-bt-lemask (docs/drivers/bluetooth.md, "LE link ready"): OR LE Data Length Change and LE PHY Update Complete into the host LE event mask; 0 off, 1 on.
 * The boot argument sets it; root may change it through machdep.piano_bt_lemask (applies to the next LE Set Event Mask). */
extern volatile uint32_t qbt_bt_lemask;
/* piano-bt-lerdy (HciH4.h LeReadyAssist): hand the host a no-change LE Data Length Change / PHY Update Complete for an LE link that
 * stalls before GATT; 0 off, 1 on; the boot argument sets it, root may change it through machdep.piano_bt_lerdy. */
extern volatile uint32_t qbt_bt_lerdy;
uint64_t qbt_bt_uptime_ms(void);
void qbt_transport_wait_for_baseline(void);
int qbt_transport_init(const struct qbt_transport_ops *);
void qbt_transport_unregister_sysctls(void);
void qbt_transport_boot_result(int error, const char *firmware, uint32_t baud);
void qbt_transport_power_result(bool enabled, int error, const char *firmware, uint32_t baud);
uint32_t qbt_transport_receive(const uint8_t *, uint32_t);
uint32_t qbt_transport_tx_pending(void);
uint32_t qbt_transport_tx_take(uint8_t *, uint32_t);
bool qbt_transport_active(void);
#ifdef __cplusplus
}
#endif
#endif
