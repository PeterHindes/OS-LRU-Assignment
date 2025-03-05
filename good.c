#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include "simulator.h"
#include <limits.h>

// Add standard max function definition if not available
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif

#define LOCK_RATIO 2.0
#define HISTORY_SIZE 40
#define MIN_LOOKAHEAD 2
#define MAX_LOOKAHEAD 5
#define DECAY_INTERVAL 8000 // Ticks between decay operations
#define DECAY_FACTOR 0.70   // Multiply hits by this factor during decay

void pageit(Pentry q[MAXPROCESSES])
{
    /* Static vars */
    static int tick = 1;
    static int initialized = 0;
    static int page_hits[MAXPROCESSES][MAXPROCPAGES];
    static int pc_history[MAXPROCESSES][HISTORY_SIZE];
    static int history_index[MAXPROCESSES];
    static int process_lookahead[MAXPROCESSES];
    static int last_decay = 0; // Track last decay time

    if (!initialized)
    {
        for (int i = 0; i < MAXPROCESSES; i++)
        {
            for (int j = 0; j < MAXPROCPAGES; j++)
            {
                page_hits[i][j] = 0;
            }
            process_lookahead[i] = MIN_LOOKAHEAD;
            history_index[i] = 0;
        }
        initialized = 1;
    }

    // Perform periodic decay of hit counts
    if (tick - last_decay >= DECAY_INTERVAL)
    {
        for (int p = 0; p < MAXPROCESSES; p++)
        {
            for (int i = 0; i < MAXPROCPAGES; i++)
            {
                page_hits[p][i] = (int)(page_hits[p][i] * DECAY_FACTOR);
            }
        }
        last_decay = tick;
    }

    for (int proc = 0; proc < MAXPROCESSES; proc++)
    {
        if (!q[proc].active)
            continue;

        int pc = q[proc].pc;
        int page = pc / PAGESIZE;

        // Update history and detect loops
        pc_history[proc][history_index[proc]] = pc;
        history_index[proc] = (history_index[proc] + 1) % HISTORY_SIZE;

        // Check for loops by comparing current PC with history
        for (int i = 1; i < HISTORY_SIZE; i++)
        {
            if (pc_history[proc][(history_index[proc] - i + HISTORY_SIZE) % HISTORY_SIZE] == pc)
            {
                // Loop detected - increase lookahead to cover loop
                process_lookahead[proc] = max(process_lookahead[proc], i / PAGESIZE + 1);
                if (process_lookahead[proc] > MAX_LOOKAHEAD)
                    process_lookahead[proc] = MAX_LOOKAHEAD;
                break;
            }
        }

        page_hits[proc][page]++;

        // Calculate working set priority
        float avg_hits = 0;
        int total_hits = 0;
        for (int i = 0; i < MAXPROCPAGES; i++)
        {
            total_hits += page_hits[proc][i];
        }
        avg_hits = total_hits / (float)MAXPROCPAGES;

        // Adaptive page management
        int remaining_pages = PHYSICALPAGES / MAXPROCESSES;

        if (remaining_pages > 0)
        {
            // Keep high-priority pages
            for (int i = 0; i < MAXPROCPAGES; i++)
            {
                if (page_hits[proc][i] >= avg_hits * LOCK_RATIO)
                {
                    if (!q[proc].pages[i])
                    {
                        pagein(proc, i);
                        remaining_pages--;
                    }
                }
                else if (i < page || i >= page + process_lookahead[proc])
                {
                    pageout(proc, i);
                }
            }

            // Page in lookahead window
            for (int i = page; i < page + process_lookahead[proc] && remaining_pages > 0; i++)
            {
                if (i < MAXPROCPAGES && !q[proc].pages[i])
                {
                    if (pagein(proc, i))
                    {
                        remaining_pages--;
                    }
                }
            }
        }
    }

    tick++;
}