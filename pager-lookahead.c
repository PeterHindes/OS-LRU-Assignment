/*
 * This file contains the Look Ahead pager
 */

#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include "simulator.h"
#include <limits.h>

#define new_max(x, y) (((x) >= (y)) ? (x) : (y))

void pageit(Pentry q[MAXPROCESSES])
{
    /* Static vars */
    static int tick = 1; // artificial time

    /* Local vars */
    int page;

/* LookAhead Paging */
#define LOOKAHEAD 5
    int remainingProcesses = PHYSICALPAGES / LOOKAHEAD;
    for (int proc = 0; proc < MAXPROCESSES; proc++)
    {

        int pc = q[proc].pc;  // program counter for process
        page = pc / PAGESIZE; // current page the PC is on

        // select only active processes
        if (q[proc].active == 1 && remainingProcesses > 0)
        {
            for (int i = 0; i < page; i++)
            {
                pageout(proc, i);
            }

            for (int i = page; i < page + LOOKAHEAD; i++)
            {
                if (!q[proc].pages[page])
                {
                    if (!pagein(proc, i))
                    {
                        // fprintf(stderr, "Error, couldn't allocate page selected!\n");
                        // raise(SIGINT);
                        break;
                    }
                }
            }

            for (int i = page + LOOKAHEAD; i < MAXPROCPAGES; i++)
            {
                pageout(proc, i);
            }

            remainingProcesses--;
        }
    }

    /* advance time for next pageit iteration */
    tick++;
}
