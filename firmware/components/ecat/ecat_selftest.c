/*
 * Built-in checks of the topology logic (validate, compare, names). No hardware needed:
 * run with the console command "selftest".
 *
 * Slaves are identified by cable position (ecat_topology.h): the checks below cover what that
 * detects (model, missing, extra, rack, revision) and what it cannot (two identical drives
 * swapped), and that station aliases play no part any more.
 */

#include <string.h>
#include "ecat_topology.h"
#include "esp_log.h"

static const char *TAG = "ecat_test";

#define V_LEADSHINE 0x00004321u
#define P_L5N       0x000000F0u
#define P_L3N       0x000000F8u
#define P_R1EC      0x61400015u
#define M_SC1600    0x71100025u /* 16 DI */
#define M_SC0016N   0x71100205u /* 16 DO */

static int s_fail;
static int s_pass;

#define CHECK(cond, what)                                          \
    do {                                                           \
        if (cond) {                                                \
            s_pass++;                                              \
        } else {                                                   \
            s_fail++;                                              \
            ESP_LOGE(TAG, "FAIL line %d: %s", __LINE__, what);     \
        }                                                          \
    } while (0)

/* Large structs stay out of the console task's stack. */
static ecat_topology_t s_saved;
static ecat_topology_t s_bus;
static ecat_compare_t s_cmp;
static ecat_issues_t s_iss;

static ecat_topo_slave_t *add(ecat_topology_t *t, uint32_t vendor, uint32_t product, uint32_t rev)
{
    ecat_topo_slave_t *s = &t->slave[t->n_slaves++];
    memset(s, 0, sizeof(*s));
    s->position = t->n_slaves;
    s->vendor = vendor;
    s->product = product;
    s->revision = rev;
    s->rack = ECAT_TOPO_NO_RACK;
    const ecat_device_info_t *d = ecat_catalog_device(vendor, product);
    s->kind = d ? d->kind : ECAT_KIND_UNKNOWN;
    return s;
}

static void add_rio(ecat_topology_t *t, uint32_t m0, uint32_t m1)
{
    ecat_topo_slave_t *s = add(t, V_LEADSHINE, P_R1EC, 0x22092010);
    ecat_rack_t *r = &t->rack[t->n_racks];
    memset(r, 0, sizeof(*r));
    r->n_modules = 2;
    r->ident[0] = m0;
    r->ident[1] = m1;
    s->rack = t->n_racks++;
}

/* 8 L5N drives in cable order (no station alias), then an R1EC with 16 DI + 16 DO. */
static void machine(ecat_topology_t *t)
{
    memset(t, 0, sizeof(*t));
    for (int a = 1; a <= 8; a++) {
        add(t, V_LEADSHINE, P_L5N, 1);
    }
    add_rio(t, M_SC1600, M_SC0016N);
}

static void compare(void)
{
    ecat_topology_compare(&s_saved, &s_bus, &s_cmp);
}

static void validate(const ecat_topology_t *t)
{
    memset(&s_iss, 0, sizeof(s_iss));
    ecat_topology_validate(t, &s_iss);
}

static int count_issue(ecat_issue_code_t code)
{
    int n = 0;
    for (uint8_t i = 0; i < s_iss.n; i++) {
        n += s_iss.item[i].code == code;
    }
    return n;
}

int ecat_topology_selftest(void)
{
    s_fail = 0;
    s_pass = 0;

    /* The catalog must know the devices used below. */
    CHECK(ecat_catalog_device(V_LEADSHINE, P_L5N) != NULL, "catalog has L5N");
    CHECK(ecat_catalog_device(V_LEADSHINE, P_R1EC) != NULL, "catalog has R1EC");
    CHECK(ecat_catalog_module(P_R1EC, M_SC1600) != NULL, "catalog has SC-1600");

    /* 1. Identical bus: OP allowed, nothing reported, drives need no alias. */
    machine(&s_saved);
    machine(&s_bus);
    compare();
    CHECK(s_cmp.ok && s_cmp.n_blocking == 0 && s_cmp.n_warnings == 0 && s_cmp.n_extra == 0, "identical -> ok");
    validate(&s_saved);
    CHECK(s_iss.n == 0, "drives without alias: no issues");

    /* 2. Aliases are ignored: odd or duplicate aliases change nothing. */
    machine(&s_bus);
    s_bus.slave[2].alias = 7;
    s_bus.slave[3].alias = 7;
    compare();
    CHECK(s_cmp.ok && s_cmp.bus[2] == 2 && s_cmp.bus[3] == 3, "aliases ignored by compare");
    validate(&s_bus);
    CHECK(s_iss.n_blocking == 0, "aliases ignored by validate");

    /* 3. Every saved slave is matched with the bus slave at the same position. */
    machine(&s_bus);
    compare();
    bool same_pos = true;
    for (uint16_t k = 0; k < s_saved.n_slaves; k++) {
        same_pos &= s_cmp.bus[k] == k && s_cmp.diff[k] == 0;
    }
    CHECK(same_pos, "matched position by position");

    /* 4. AX6 replaced by another model: blocking. */
    machine(&s_bus);
    s_bus.slave[5].product = P_L3N;
    compare();
    CHECK(!s_cmp.ok && s_cmp.n_blocking == 1, "wrong model -> blocked");
    CHECK(s_cmp.diff[5] & ECAT_DIFF_MODEL, "AX6 WRONG MODEL");

    /* 5. AX7 missing: everything after it shifts; the R1EC lands on AX8's position. */
    memset(&s_bus, 0, sizeof(s_bus));
    for (int a = 1; a <= 7; a++) {
        add(&s_bus, V_LEADSHINE, P_L5N, 1);
    }
    add_rio(&s_bus, M_SC1600, M_SC0016N);
    compare();
    CHECK(!s_cmp.ok, "missing drive -> blocked");
    CHECK(s_cmp.diff[6] == 0, "position 7 still holds an L5N (AX8 moved up): same model, cannot tell");
    CHECK(s_cmp.diff[7] & ECAT_DIFF_MODEL, "position 8: R1EC where AX8 was -> WRONG MODEL");
    CHECK(s_cmp.diff[8] == ECAT_DIFF_MISSING && s_cmp.bus[8] == ECAT_TOPO_NONE, "position 9 (R1EC) MISSING");

    /* 6. Two identical drives swapped in the chain: not detectable by design. */
    machine(&s_bus);
    const ecat_topo_slave_t tmp = s_bus.slave[2];
    s_bus.slave[2] = s_bus.slave[3];
    s_bus.slave[3] = tmp;
    s_bus.slave[2].position = 3; /* same model and revision: nothing tells them apart */
    s_bus.slave[3].position = 4;
    compare();
    CHECK(s_cmp.ok, "identical drives swapped -> accepted (documented limitation)");

    /* 7. Extra unknown device at the end: blocking. */
    machine(&s_bus);
    add(&s_bus, 0x00000ABCu, 0x1234u, 1);
    compare();
    CHECK(!s_cmp.ok && s_cmp.n_extra == 1 && s_cmp.extra[0] == 9, "extra slave -> blocked");

    /* 8. New revision of AX2: warning only. */
    machine(&s_bus);
    s_bus.slave[1].revision = 2;
    compare();
    CHECK(s_cmp.ok && s_cmp.n_warnings == 1 && s_cmp.diff[1] == ECAT_DIFF_REVISION, "revision change -> warning");

    /* 9. A module swapped in the R1EC rack: blocking. */
    machine(&s_bus);
    s_bus.rack[0].ident[1] = M_SC1600;
    compare();
    CHECK(!s_cmp.ok && (s_cmp.diff[8] & ECAT_DIFF_RACK), "rack change -> blocked");

    /* 10. Unknown module in the rack. */
    machine(&s_bus);
    s_bus.rack[0].ident[0] = 0xDEADBEEFu;
    validate(&s_bus);
    CHECK(count_issue(ECAT_ISSUE_UNKNOWN_MODULE) == 1, "validate: unknown module");

    /* 11. Two identical couplers: told apart by position, no issue. */
    machine(&s_bus);
    add_rio(&s_bus, M_SC1600, M_SC0016N);
    validate(&s_bus);
    CHECK(s_iss.n_blocking == 0, "validate: two couplers are fine");

    /* 12. Unknown device is a warning, not a block. */
    machine(&s_bus);
    add(&s_bus, 0x00000ABCu, 0x1234u, 1);
    validate(&s_bus);
    CHECK(count_issue(ECAT_ISSUE_UNKNOWN_DEVICE) == 1 && s_iss.n_blocking == 0, "validate: unknown device warns");

    /* 13. Names: drives AX1..AX8 in cable order whatever their alias, the coupler RIO1. */
    machine(&s_saved);
    s_saved.slave[0].alias = 5;
    s_saved.slave[1].alias = 3;
    ecat_topology_assign_names(&s_saved, NULL, NULL);
    CHECK(strcmp(s_saved.slave[0].name, "AX1") == 0 && strcmp(s_saved.slave[1].name, "AX2") == 0 &&
              strcmp(s_saved.slave[7].name, "AX8") == 0 && strcmp(s_saved.slave[8].name, "RIO1") == 0,
          "default names in cable order");

    /* 14. A renamed axis keeps its name at its position after a new scan. */
    strlcpy(s_saved.slave[2].name, "LIFT", sizeof(s_saved.slave[2].name));
    machine(&s_bus);
    compare();
    ecat_topology_assign_names(&s_bus, &s_saved, &s_cmp);
    CHECK(strcmp(s_bus.slave[2].name, "LIFT") == 0 && strcmp(s_bus.slave[3].name, "AX4") == 0,
          "renamed axis keeps its name");

    if (s_fail == 0) {
        ESP_LOGI(TAG, "selftest PASS (%d checks)", s_pass);
    } else {
        ESP_LOGE(TAG, "selftest FAILED: %d of %d checks", s_fail, s_fail + s_pass);
    }
    return s_fail;
}
