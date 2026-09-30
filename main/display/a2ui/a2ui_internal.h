#pragma once

/** Private helpers shared inside the a2ui component. */

#ifdef __cplusplus
extern "C" {
#endif

/** Invoked by renderer when a Button is clicked. */
void a2ui_dispatch_action(const char *name);

#ifdef __cplusplus
}
#endif
