/*
 * Topology validation, comparison and naming (pure data processing, no bus access).
 * Design: docs/ETHERCAT_PLAN.md section 3.6.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "ecat_topology.h"
#include "esp_log.h"

static const char *TAG = "ecat_topo";

void ecat_issues_add(ecat_issues_t *issues, ecat_issue_code_t code, uint16_t slave, uint32_t detail)
{
    if (issues->n >= ECAT_TOPO_MAX_ISSUES) {
        /* Keep counting blocking problems even when the detail list is full. */
        if (ecat_issue_is_blocking(code)) {
            issues->n_blocking++;
        }
        return;
    }
    issues->item[issues->n++] = (ecat_issue_t) { .code = code, .slave = slave, .detail = detail };
    if (ecat_issue_is_blocking(code)) {
        issues->n_blocking++;
    }
}

bool ecat_issue_is_blocking(ecat_issue_code_t code)
{
    return code != ECAT_ISSUE_UNKNOWN_DEVICE;
}

const char *ecat_issue_text(ecat_issue_code_t code)
{
    switch (code) {
    case ECAT_ISSUE_UNKNOWN_MODULE:    return "coupler module is not in the catalog (add its ESI)";
    case ECAT_ISSUE_RACK_UNREADABLE:   return "coupler module list 0xF050 is not readable";
    case ECAT_ISSUE_RACK_GEOMETRY:     return "coupler slot distance 0xF000:1 differs from the ESI";
    case ECAT_ISSUE_TOO_MANY:          return "more slaves, couplers or modules than the firmware supports";
    case ECAT_ISSUE_UNKNOWN_DEVICE:    return "device is not in the catalog: kept on the bus, data not used";
    default:                           return "?";
    }
}

static bool is_io(uint8_t kind)
{
    return kind == ECAT_KIND_IO || kind == ECAT_KIND_COUPLER;
}

static bool same_model(const ecat_topo_slave_t *a, const ecat_topo_slave_t *b)
{
    return a->vendor == b->vendor && a->product == b->product;
}

void ecat_topology_validate(const ecat_topology_t *scan, ecat_issues_t *issues)
{
    for (uint16_t i = 0; i < scan->n_slaves; i++) {
        const ecat_topo_slave_t *s = &scan->slave[i];

        if (s->kind == ECAT_KIND_UNKNOWN) {
            ecat_issues_add(issues, ECAT_ISSUE_UNKNOWN_DEVICE, i, 0);
        }
        if (s->rack != ECAT_TOPO_NO_RACK && s->rack < scan->n_racks) {
            const ecat_rack_t *r = &scan->rack[s->rack];
            for (uint8_t m = 0; m < r->n_modules; m++) {
                if (ecat_catalog_module(s->product, r->ident[m]) == NULL) {
                    ecat_issues_add(issues, ECAT_ISSUE_UNKNOWN_MODULE, i, r->ident[m]);
                }
            }
        }
    }
}

static bool rack_equal(const ecat_topology_t *ta, const ecat_topo_slave_t *a,
                       const ecat_topology_t *tb, const ecat_topo_slave_t *b)
{
    bool ha = a->rack != ECAT_TOPO_NO_RACK && a->rack < ta->n_racks;
    bool hb = b->rack != ECAT_TOPO_NO_RACK && b->rack < tb->n_racks;
    if (!ha || !hb) {
        return ha == hb;
    }
    const ecat_rack_t *ra = &ta->rack[a->rack];
    const ecat_rack_t *rb = &tb->rack[b->rack];
    return ra->n_modules == rb->n_modules &&
           memcmp(ra->ident, rb->ident, ra->n_modules * sizeof(ra->ident[0])) == 0;
}

void ecat_topology_compare(const ecat_topology_t *saved, const ecat_topology_t *bus, ecat_compare_t *out)
{
    bool used[ECAT_TOPO_MAX_SLAVES] = { false };

    memset(out, 0, sizeof(*out));
    for (uint16_t k = 0; k < saved->n_slaves; k++) {
        const ecat_topo_slave_t *s = &saved->slave[k];
        uint16_t idx = ECAT_TOPO_NONE;

        /* Identity is the cable position: the saved slave k must be at the same place. */
        for (uint16_t i = 0; i < bus->n_slaves && idx == ECAT_TOPO_NONE; i++) {
            if (!used[i] && bus->slave[i].position == s->position) {
                idx = i;
            }
        }

        uint8_t diff = 0;
        if (idx == ECAT_TOPO_NONE) {
            diff = ECAT_DIFF_MISSING;
        } else {
            const ecat_topo_slave_t *b = &bus->slave[idx];
            used[idx] = true;
            if (!same_model(b, s)) {
                diff |= ECAT_DIFF_MODEL;
            } else {
                if (b->revision != s->revision) {
                    diff |= ECAT_DIFF_REVISION;
                }
                if (!rack_equal(saved, s, bus, b)) {
                    diff |= ECAT_DIFF_RACK;
                }
            }
        }
        out->bus[k] = idx;
        out->diff[k] = diff;
        if (diff & ECAT_DIFF_BLOCKING) {
            out->n_blocking++;
        } else if (diff & ECAT_DIFF_REVISION) {
            out->n_warnings++;
        }
    }
    for (uint16_t i = 0; i < bus->n_slaves; i++) {
        if (!used[i]) {
            out->extra[out->n_extra++] = i;
        }
    }
    out->n_blocking += out->n_extra;
    out->ok = out->n_blocking == 0;
}

void ecat_topology_assign_names(ecat_topology_t *scan, const ecat_topology_t *saved, const ecat_compare_t *cmp)
{
    unsigned rio = 0, ax = 0;

    /* scan->slave[] is in cable order (ecat_topology_scan()), so AX1 is the first drive. */
    for (uint16_t i = 0; i < scan->n_slaves; i++) {
        ecat_topo_slave_t *s = &scan->slave[i];
        const char *keep = NULL;

        if (is_io(s->kind)) {
            rio++;
        } else if (ecat_kind_is_drive(s->kind)) {
            ax++;
        }
        if (saved != NULL && cmp != NULL) {
            for (uint16_t k = 0; k < saved->n_slaves; k++) {
                if (cmp->bus[k] == i && saved->slave[k].name[0] != '\0') {
                    keep = saved->slave[k].name;
                    break;
                }
            }
        }
        if (keep != NULL) {
            strlcpy(s->name, keep, sizeof(s->name));
        } else if (ecat_kind_is_drive(s->kind)) {
            snprintf(s->name, sizeof(s->name), "AX%u", ax % 100u);           /* <= 32 slaves */
        } else if (is_io(s->kind)) {
            snprintf(s->name, sizeof(s->name), "RIO%u", rio % 100u);         /* <= 32 slaves */
        } else {
            snprintf(s->name, sizeof(s->name), "DEV%u", s->position % 1000u);
        }
    }
}

const char *ecat_diff_text(uint8_t diff)
{
    if (diff & ECAT_DIFF_MISSING) {
        return "MISSING";
    }
    if (diff & ECAT_DIFF_MODEL) {
        return "WRONG MODEL";
    }
    if (diff & ECAT_DIFF_RACK) {
        return "RACK CHANGED";
    }
    if (diff & ECAT_DIFF_REVISION) {
        return "REV CHANGED";
    }
    return "MATCH";
}

const char *ecat_topology_device_name(const ecat_topo_slave_t *s)
{
    const ecat_device_info_t *d = ecat_catalog_device(s->vendor, s->product);
    return d ? d->name : "unknown";
}

/* ---- Logging ---- */

static void format_rack(char *buf, size_t len, const ecat_topology_t *t, const ecat_topo_slave_t *s)
{
    size_t used = 0;
    const ecat_rack_t *r = &t->rack[s->rack];

    buf[0] = '\0';
    if (r->n_modules == 0) {
        snprintf(buf, len, "(no modules)");
        return;
    }
    for (uint8_t m = 0; m < r->n_modules && used < len; m++) {
        const ecat_module_info_t *mi = ecat_catalog_module(s->product, r->ident[m]);
        int n;
        if (mi == NULL) {
            n = snprintf(buf + used, len - used, "%sslot%u 0x%08" PRIX32 " (unknown)", m ? " | " : "", m, r->ident[m]);
        } else if (mi->di || mi->dout) {
            n = snprintf(buf + used, len - used, "%sslot%u %s (%u DI %u DO)", m ? " | " : "", m, mi->name, mi->di, mi->dout);
        } else {
            n = snprintf(buf + used, len - used, "%sslot%u %s (%u in %u out bits)", m ? " | " : "", m, mi->name,
                         mi->in_bits, mi->out_bits);
        }
        if (n < 0) {
            break;
        }
        used += (size_t)n;
    }
}

static void log_slave_line(const char *prefix, const ecat_topology_t *t, const ecat_topo_slave_t *s, const char *suffix)
{
    ESP_LOGI(TAG, "%s%3u %-7s %-12s %-8s 0x%08" PRIX32 " 0x%08" PRIX32 " %s", prefix, (unsigned)s->position,
             s->name[0] ? s->name : "?", ecat_topology_device_name(s), ecat_kind_name((ecat_kind_t)s->kind), s->product,
             s->revision, suffix ? suffix : "");
    if (s->rack != ECAT_TOPO_NO_RACK && s->rack < t->n_racks) {
        char buf[320];
        format_rack(buf, sizeof(buf), t, s);
        ESP_LOGI(TAG, "%s      rack: %s", prefix, buf);
    }
}

void ecat_topology_log(const char *title, const ecat_topology_t *t)
{
    ESP_LOGI(TAG, "%s: %u slave(s)", title, (unsigned)t->n_slaves);
    ESP_LOGI(TAG, "  pos name    device       kind     product    revision");
    for (uint16_t i = 0; i < t->n_slaves; i++) {
        log_slave_line("  ", t, &t->slave[i], NULL);
    }
}

void ecat_issues_log(const ecat_topology_t *t, const ecat_issues_t *issues)
{
    for (uint8_t n = 0; n < issues->n; n++) {
        const ecat_issue_t *is = &issues->item[n];
        const ecat_topo_slave_t *s = is->slave < t->n_slaves ? &t->slave[is->slave] : NULL;
        bool block = ecat_issue_is_blocking((ecat_issue_code_t)is->code);
        if (block) {
            ESP_LOGW(TAG, "  [BLOCK] pos %u: %s (detail 0x%08" PRIX32 ")", s ? (unsigned)s->position : 0,
                     ecat_issue_text((ecat_issue_code_t)is->code), is->detail);
        } else {
            ESP_LOGI(TAG, "  [WARN]  pos %u: %s", s ? (unsigned)s->position : 0,
                     ecat_issue_text((ecat_issue_code_t)is->code));
        }
    }
    if (issues->n_blocking > issues->n) {
        ESP_LOGW(TAG, "  ... more blocking issues than listed");
    }
}

void ecat_compare_log(const ecat_topology_t *saved, const ecat_topology_t *bus, const ecat_compare_t *cmp)
{
    ESP_LOGI(TAG, "Saved topology vs bus:");
    ESP_LOGI(TAG, "  pos name    device       result");
    for (uint16_t k = 0; k < saved->n_slaves; k++) {
        const ecat_topo_slave_t *s = &saved->slave[k];
        const ecat_topo_slave_t *b = cmp->bus[k] == ECAT_TOPO_NONE ? NULL : &bus->slave[cmp->bus[k]];
        const char *dev = b && (cmp->diff[k] & ECAT_DIFF_MODEL) ? ecat_topology_device_name(b) : ecat_topology_device_name(s);
        if (cmp->diff[k] & ECAT_DIFF_BLOCKING) {
            ESP_LOGW(TAG, "  %3u %-7s %-12s %s", (unsigned)s->position, s->name, dev, ecat_diff_text(cmp->diff[k]));
        } else {
            ESP_LOGI(TAG, "  %3u %-7s %-12s %s", (unsigned)s->position, s->name, dev, ecat_diff_text(cmp->diff[k]));
        }
    }
    for (uint16_t n = 0; n < cmp->n_extra; n++) {
        const ecat_topo_slave_t *b = &bus->slave[cmp->extra[n]];
        ESP_LOGW(TAG, "  %3u EXTRA   %-12s not in the saved topology", (unsigned)b->position,
                 ecat_topology_device_name(b));
    }
    if (cmp->ok) {
        ESP_LOGI(TAG, "Topology OK (%u warning(s))", (unsigned)cmp->n_warnings);
    } else {
        ESP_LOGW(TAG, "Topology MISMATCH: %u blocking difference(s). Bus held in PRE-OP; fix the hardware "
                 "and 'recheck', or 'scan' and 'accept' a new topology.", (unsigned)cmp->n_blocking);
    }
}
