// Affinity NDS — visual scripting runtime glue (Phase 3).
//
// Bridges the per-frame dispatchers main.c calls (afn_script_init / tick)
// to the actual emitted script bodies in mapdata.h (afn_emitted_script_*).
// Until full per-node emission lands, mapdata.h supplies stubs so this
// file compiles and the rest of the runtime keeps working unchanged.

#include "affinity.h"
#include "mapdata.h"

void afn_script_init(void)
{
    afn_emitted_script_init();
}

void afn_script_tick(void)
{
    afn_emitted_script_update();
    afn_emitted_script_key_held();
    afn_emitted_script_key_pressed();
    afn_emitted_script_key_released();
}
