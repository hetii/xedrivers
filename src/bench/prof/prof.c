/* Where bench=prof keeps its sums; the hooks themselves are src/core/prof.h. */

#include "prof.h"

volatile NET_PROF net_prof;

/* Read off ordinal 131 on 17559; the frequency fits the low word. */
extern u32 KeQueryPerformanceFrequency(void);

void prof_init(void)
{
    net_prof.freq = KeQueryPerformanceFrequency();
    net_prof.enabled = 1;
}
