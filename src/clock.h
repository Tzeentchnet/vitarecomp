/* clock.h — the one clock everything in the runtime reads.
 *
 * A title asks the time in a dozen ways: the process clock, the real-time
 * clock, the C library's, the timeouts of its waits. They all come from
 * here, so that they agree with each other, and so that the host can
 * choose what time is. By default it is the host's clock. A host that
 * wants a run to repeat exactly asks for a virtual one (host.h), which
 * moves only with what the program does: a sixtieth of a second at each
 * vertical blank, the length of a wait when every thread is waiting, and a
 * microsecond each time it is read, so that a loop spinning on the clock
 * still sees it move.
 */

#ifndef VITARECOMP_CLOCK_H
#define VITARECOMP_CLOCK_H

#include "vitarecomp/runtime.h"

#endif /* VITARECOMP_CLOCK_H */
