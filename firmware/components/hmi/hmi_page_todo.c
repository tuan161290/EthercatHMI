/*
 * Placeholder for the Settings pages that have no mockup and no firmware yet (Recipe, User &
 * password, Maintenance, Backup / restore), so navigation works end to end.
 */

#include "hmi_internal.h"

void hmi_todo_card(lv_obj_t *page)
{
    hmi_info_card(page, "Not in the firmware yet",
                  "This page has no mockup yet. It will be designed with the machine (recipes, users, maintenance "
                  "counters, SD card).",
                  NULL, NULL);
}
