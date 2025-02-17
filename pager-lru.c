/*
 * This file contains the LRU pager
 */

 #include <stdio.h>
 #include <signal.h>
 #include <stdlib.h>
 #include "simulator.h"
 #include <limits.h>
 
 #define new_max(x,y) (((x) >= (y)) ? (x) : (y))
 
 void pageit(Pentry q[MAXPROCESSES]) { 
 
     /* Static vars */
     static int initialized = 0;
     static int tick = 1; // artificial time
     static int timestamps[MAXPROCESSES][MAXPROCPAGES];
 
     /* Local vars */
     int proc;
     int page;
 
     /* initialize timestamp array on first run */
     if (!initialized) {
         for (proc=0; proc < MAXPROCESSES; proc++) {
             for (page=0; page < MAXPROCPAGES; page++) {
                 timestamps[proc][page] = INT_MAX; 
             }
         }
         initialized = 1;
     }
     
     /* TODO: Implement LRU Paging */
     // fprintf(stderr, "pager-lru not yet implemented. Exiting...\n");
     // exit(EXIT_FAILURE);
     proc = (tick-1) % MAXPROCESSES;
 
     int pc = q[proc].pc; 		            	// program counter for process
     page = pc/PAGESIZE; 		        	// current page the PC is on
     
     // select only active processes
     // if the page is already in memory, then we're done
     // if not in memory, then page it in
     if (q[proc].active == 1 && !q[proc].pages[page]) {
         if (pagein(proc,page)){ // If it becomes paged set the timestamp
             timestamps[proc][page] = tick;
         }
         else { // If it doesn't become paged then find the least recently used page and evict it!
             int minTime = INT_MAX;
             int minProc = -1;
             int minPage = -1;
             for (int lproc=0; lproc < MAXPROCESSES; lproc++) {
                 for (int lpage=0; lpage < MAXPROCPAGES; lpage++) {
                     if (q[lproc].pages[lpage] == 1 && minTime > timestamps[lproc][lpage] ){ // only interested in pages that are in memory and with a lower timestamp
                         minTime = timestamps[lproc][lpage];
                         minProc = lproc;
                         minPage = lpage;
                     }
                 }
             }
             if (pageout(minProc,minPage) == 1) {
                 timestamps[minProc][minPage] = INT_MAX;
                 pagein(proc,page);
                 timestamps[proc][page] = tick;
             }
             else { // Might happen if everything is currently swapping in.
                 fprintf(stderr, "Error, couldnt evict page selected in lru!\n");
                 raise(SIGSEGV);
             }
 
         }
     }
     
     
     /* advance time for next pageit iteration */
     tick++;
 }