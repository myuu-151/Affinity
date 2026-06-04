// Affinity PSP runtime — visual-script runtime (node graph hooks).
#pragma once
void script_start(void);     // OnStart + blueprint start (once)
void script_tick(void);      // OnUpdate + OnKey* each frame (sets node variables)
int  script_present(void);
