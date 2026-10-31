/*
 * General App: the EtherCAT HMI with no machine sequence. Everything it does is in
 * components/app_core; a machine app starts as a copy of this folder and adds its own code after
 * app_core_start() (docs/SETUP.md, "App mới").
 */

#include "app_core.h"

void app_main(void)
{
    app_core_start();
}
