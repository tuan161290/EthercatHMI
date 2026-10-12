/*
 * Lookups into the generated device catalog (ecat_catalog_gen.c).
 */

#include "ecat_catalog.h"

const ecat_device_info_t *ecat_catalog_device(uint32_t vendor, uint32_t product)
{
    for (size_t i = 0; i < ecat_catalog_device_count; i++) {
        const ecat_device_info_t *d = &ecat_catalog_devices[i];
        if (d->vendor == vendor && d->product == product) {
            return d;
        }
    }
    return NULL;
}

const ecat_module_info_t *ecat_catalog_module(uint32_t coupler_product, uint32_t ident)
{
    for (size_t i = 0; i < ecat_catalog_module_count; i++) {
        const ecat_module_info_t *m = &ecat_catalog_modules[i];
        if (m->coupler_product == coupler_product && m->ident == ident) {
            return m;
        }
    }
    return NULL;
}

const char *ecat_kind_name(ecat_kind_t kind)
{
    switch (kind) {
    case ECAT_KIND_SERVO:   return "servo";
    case ECAT_KIND_STEPPER: return "stepper";
    case ECAT_KIND_IO:      return "io";
    case ECAT_KIND_COUPLER: return "coupler";
    default:                return "unknown";
    }
}
