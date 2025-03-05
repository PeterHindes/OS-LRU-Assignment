#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include "simulator.h"
#include <limits.h>

// Add standard max function definition if not available
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

// Base paging parameters
#define LOW_USAGE_THRESHOLD 0   // Pages below this ratio of avg_hits will be evicted quickly
#define LOCK_RATIO 2.0
#define HISTORY_SIZE 40
#define MIN_LOOKAHEAD 2
#define MAX_LOOKAHEAD 10
#define DECAY_INTERVAL 8000 // Ticks between decay operations
#define DECAY_FACTOR 0.70   // Multiply hits by this factor during decay

// Loop detection tuning parameters
#define MIN_LOOP_SIZE 2          // Minimum PC instructions in a loop to consider
#define MIN_LOOP_ITERATIONS 1    // Minimum times a loop must repeat to be confirmed
#define MAX_LOOP_PAGES 10         // Maximum pages to load for a loop
#define LOOP_CONFIDENCE_THRESHOLD 0.82 // How confident we need to be (match percentage)
#define LOOP_FAULT_THRESHOLD 1  // Disable loop handling after this many faults

void pageit(Pentry q[MAXPROCESSES])
{
    /* Static vars */
    static int tick = 1;
    static int initialized = 0;
    static float page_hits[MAXPROCESSES][MAXPROCPAGES]; // Changed to float for more precise tracking
    static int pc_history[MAXPROCESSES][HISTORY_SIZE];
    static int history_index[MAXPROCESSES];
    static int process_lookahead[MAXPROCESSES];
    static int last_decay = 0; // Track last decay time
    
    // Loop detection variables
    static int in_loop[MAXPROCESSES];         // Indicates if process is in a loop
    static int loop_start[MAXPROCESSES];      // Start position of detected loop
    static int loop_length[MAXPROCESSES];     // Length of detected loop
    static int loop_pages[MAXPROCESSES][MAXPROCPAGES]; // Pages accessed in loop
    static int loop_page_count[MAXPROCESSES]; // Number of unique pages in loop
    static int loop_iterations[MAXPROCESSES]; // Iterations of the loop observed
    static int loop_faults[MAXPROCESSES];     // Page faults during loop execution
    static int loop_disabled[MAXPROCESSES];   // Disable loop handling if not helping

    if (!initialized)
    {
        for (int i = 0; i < MAXPROCESSES; i++)
        {
            for (int j = 0; j < MAXPROCPAGES; j++)
            {
                page_hits[i][j] = 0.0f; // Initialize as float
                loop_pages[i][j] = 0;
            }
            process_lookahead[i] = MIN_LOOKAHEAD;
            history_index[i] = 0;
            in_loop[i] = 0;
            loop_start[i] = -1;
            loop_length[i] = 0;
            loop_page_count[i] = 0;
            loop_iterations[i] = 0;
            loop_faults[i] = 0;
            loop_disabled[i] = 0;
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
                page_hits[p][i] *= DECAY_FACTOR; // Direct multiplication without casting
            }
            // Reset loop fault counter periodically to allow retrying
            if (loop_disabled[p] && (tick - last_decay >= DECAY_INTERVAL * 2)) {
                loop_disabled[p] = 0;
                loop_faults[p] = 0;
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

        // Update history
        pc_history[proc][history_index[proc]] = pc;
        history_index[proc] = (history_index[proc] + 1) % HISTORY_SIZE;

        // Update page hit counter - use float increment
        page_hits[proc][page] += 1.0f;

        // Count page faults during loop execution
        if (in_loop[proc] && !q[proc].pages[page]) {
            loop_faults[proc]++;
            
            // If too many faults during loop, disable loop handling for this process
            if (loop_faults[proc] > LOOP_FAULT_THRESHOLD) {
                in_loop[proc] = 0;
                loop_disabled[proc] = 1;
                // Reset loop data
                for (int i = 0; i < MAXPROCPAGES; i++) {
                    loop_pages[proc][i] = 0;
                }
                loop_page_count[proc] = 0;
                loop_length[proc] = 0;
            }
        }

        // Check if we're in a loop and if we've exited it
        if (in_loop[proc]) {
            int expected_pc = pc_history[proc][(loop_start[proc] + 
                                              (history_index[proc] - loop_start[proc]) % 
                                              loop_length[proc] + HISTORY_SIZE) % HISTORY_SIZE];
            
            // If PC doesn't match expected value in loop, we've exited the loop
            if (pc != expected_pc) {
                in_loop[proc] = 0;
                // Reset loop data
                for (int i = 0; i < MAXPROCPAGES; i++) {
                    loop_pages[proc][i] = 0;
                }
                loop_page_count[proc] = 0;
                loop_length[proc] = 0;
                loop_iterations[proc] = 0;
            } else {
                // We're still in the loop, increment iteration count if we completed the loop
                if ((history_index[proc] - loop_start[proc] + HISTORY_SIZE) % HISTORY_SIZE == 0) {
                    loop_iterations[proc]++;
                }
            }
        }

        // Check for loops by comparing current PC with history if not disabled
        if (!in_loop[proc] && !loop_disabled[proc]) {
            for (int i = MIN_LOOP_SIZE; i < HISTORY_SIZE / 2; i++) { // Looking back half the history
                int hist_idx = (history_index[proc] - i + HISTORY_SIZE) % HISTORY_SIZE;
                if (pc_history[proc][hist_idx] == pc) {
                    // Potential loop detected - verify by checking match percentage
                    int matches = 0;
                    int checks = min(i, HISTORY_SIZE/4); // Check up to 1/4 of history
                    
                    for (int j = 0; j < checks; j++) {
                        int current_idx = (history_index[proc] - j + HISTORY_SIZE) % HISTORY_SIZE;
                        int pattern_idx = (hist_idx - j + HISTORY_SIZE) % HISTORY_SIZE;
                        if (pc_history[proc][current_idx] == pc_history[proc][pattern_idx]) {
                            matches++;
                        }
                    }
                    
                    float confidence = (float)matches / checks;
                    
                    // We need good confidence and multiple iterations
                    if (confidence >= LOOP_CONFIDENCE_THRESHOLD && 
                        (history_index[proc] - hist_idx + HISTORY_SIZE) % HISTORY_SIZE >= i * MIN_LOOP_ITERATIONS) {
                        
                        // Found a reliable loop
                        in_loop[proc] = 1;
                        loop_start[proc] = hist_idx;
                        loop_length[proc] = i;
                        loop_iterations[proc] = 1;
                        loop_faults[proc] = 0;
                        
                        // Identify all pages in the loop
                        loop_page_count[proc] = 0;
                        for (int j = 0; j < MAXPROCPAGES; j++) {
                            loop_pages[proc][j] = 0;
                        }
                        
                        // Mark all unique pages accessed in the loop
                        for (int j = 0; j < i; j++) {
                            int loop_pc = pc_history[proc][(hist_idx + j) % HISTORY_SIZE];
                            int loop_page = loop_pc / PAGESIZE;
                            if (loop_page < MAXPROCPAGES && !loop_pages[proc][loop_page]) {
                                loop_pages[proc][loop_page] = 1;
                                loop_page_count[proc]++;
                                
                                // If too many pages, don't use loop optimization
                                if (loop_page_count[proc] > MAX_LOOP_PAGES) {
                                    in_loop[proc] = 0;
                                    loop_page_count[proc] = 0;
                                    break;
                                }
                            }
                        }
                        
                        if (in_loop[proc]) {
                            break;
                        }
                    }
                }
            }
        }

        // Adaptive page management
        int remaining_pages = PHYSICALPAGES / MAXPROCESSES;

        // If in a loop with manageable page count, prioritize loading loop pages
        if (in_loop[proc] && loop_page_count[proc] <= remaining_pages && loop_iterations[proc] >= MIN_LOOP_ITERATIONS) {
            // Enhanced loop page management: prioritize current and future pages
            int pages_to_load[MAXPROCPAGES] = {0};
            int current_offset = (history_index[proc] - loop_start[proc] + HISTORY_SIZE) % loop_length[proc];
            
            // First mark the current page and next few pages in the loop sequence
            for (int i = 0; i < min(remaining_pages, loop_page_count[proc]); i++) {
                int offset = (current_offset + i) % loop_length[proc];
                int loop_pc = pc_history[proc][(loop_start[proc] + offset) % HISTORY_SIZE];
                int loop_page = loop_pc / PAGESIZE;
                if (loop_page < MAXPROCPAGES) {
                    pages_to_load[loop_page] = 1;
                }
            }
            
            // Page in the prioritized loop pages
            for (int i = 0; i < MAXPROCPAGES; i++) {
                if (pages_to_load[i]) {
                    if (!q[proc].pages[i]) {
                        if (pagein(proc, i)) {
                            remaining_pages--;
                        }
                    }
                }
                // Only page out non-loop pages if we need the memory
                else if (q[proc].pages[i] && !loop_pages[proc][i] && 
                         loop_page_count[proc] > (PHYSICALPAGES / MAXPROCESSES) - 1) {
                    pageout(proc, i);
                }
            }
        } else {
            // Standard working set approach for non-loop or large loop scenarios
            
            // Calculate working set priority using float arithmetic
            float avg_hits = 0.0f;
            float total_hits = 0.0f;
            for (int i = 0; i < MAXPROCPAGES; i++) {
                total_hits += page_hits[proc][i];
            }
            avg_hits = total_hits / (float)MAXPROCPAGES;

            // First, aggressively evict pages with very low usage
            for (int i = 0; i < MAXPROCPAGES; i++) {
                if (i != page && // Don't evict current page
                    q[proc].pages[i] && // Page is loaded
                    page_hits[proc][i] < avg_hits * LOW_USAGE_THRESHOLD) { // Usage below threshold
                    pageout(proc, i);
                }
            }

            // Keep high-priority pages - compare with float threshold
            for (int i = 0; i < MAXPROCPAGES; i++) {
                if (page_hits[proc][i] >= avg_hits * LOCK_RATIO) {
                    if (!q[proc].pages[i] && remaining_pages > 0) {
                        pagein(proc, i);
                        remaining_pages--;
                    }
                } else if (i != page && (i < page || i >= page + process_lookahead[proc])) {
                    pageout(proc, i);
                }
            }

            // Page in lookahead window
            for (int i = page; i < page + process_lookahead[proc] && remaining_pages > 0; i++) {
                if (i < MAXPROCPAGES && !q[proc].pages[i]) {
                    if (pagein(proc, i)) {
                        remaining_pages--;
                    }
                }
            }
        }
    }

    tick++;
}