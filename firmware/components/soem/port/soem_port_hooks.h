/*
 * Hooks the ESP port adds to its copy of SOEM's ec_config.c (see SOEM_UPSTREAM.md).
 *
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file SOEM/LICENSE.md distributed with this software for
 * full license information.
 */

#pragma once

#include "soem/soem.h"

/**
 * Called by ecx_map_coe_soe() after SOEM has read the mapping over CoE/SoE.
 * If the application registered a layout for the slave (soem_esp_set_pdo_layout()),
 * overwrite the sizes and SM2/SM3 with it and return 1; otherwise leave them and return 0.
 */
int soem_port_apply_pdo_layout(ecx_contextt *context, uint16 slave, uint32 *Osize, uint32 *Isize);
