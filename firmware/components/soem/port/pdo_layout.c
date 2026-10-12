/*
 * Application-supplied process data layouts (see soem_esp_set_pdo_layout()).
 *
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file SOEM/LICENSE.md distributed with this software for
 * full license information.
 */

#include <stdbool.h>
#include <string.h>
#include "soem/soem.h"
#include "soem_esp.h"
#include "soem_port_hooks.h"

/* SM flags as SOEM builds them from the SII: control byte + "activate" bit 16. */
#define SM_ACTIVATE 0x00010000UL

static soem_esp_pdo_layout_t s_layout[EC_MAXSLAVE + 1];
static bool s_valid[EC_MAXSLAVE + 1];

esp_err_t soem_esp_set_pdo_layout(uint16_t slave, const soem_esp_pdo_layout_t *layout)
{
    if (slave == 0 || slave > EC_MAXSLAVE || layout == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_layout[slave] = *layout;
    s_valid[slave] = true;
    return ESP_OK;
}

void soem_esp_clear_pdo_layouts(void)
{
    memset(s_valid, 0, sizeof(s_valid));
}

static void apply_sm(ec_slavet *sl, int sm, uint16_t bits, uint16_t addr, uint8_t control, uint8_t type)
{
    if (sl->SM[sm].StartAddr == 0) {
        sl->SM[sm].StartAddr = htoes(addr);
        sl->SM[sm].SMflags = htoel(control | SM_ACTIVATE);
    }
    sl->SM[sm].SMlength = htoes((uint16)((bits + 7) / 8));
    sl->SMtype[sm] = type;
}

int soem_port_apply_pdo_layout(ecx_contextt *context, uint16 slave, uint32 *Osize, uint32 *Isize)
{
    if (slave == 0 || slave > EC_MAXSLAVE || !s_valid[slave]) {
        return 0;
    }
    const soem_esp_pdo_layout_t *l = &s_layout[slave];
    ec_slavet *sl = &context->slavelist[slave];

    apply_sm(sl, 2, l->out_bits, l->sm2_addr, l->sm2_control, 3);
    apply_sm(sl, 3, l->in_bits, l->sm3_addr, l->sm3_control, 4);
    *Osize = l->out_bits;
    *Isize = l->in_bits;
    return 1;
}
