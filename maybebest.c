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
#define DECAY_INTERVAL 6000 // Ticks between decay operations
#define DECAY_FACTOR 0.80   // Multiply hits by this factor during decay

// Loop detection tuning parameters
#define MIN_LOOP_SIZE 2          // Minimum PC instructions in a loop to consider
#define MIN_LOOP_ITERATIONS 1    // Minimum times a loop must repeat to be confirmed
#define MAX_LOOP_PAGES 10         // Maximum pages to load for a loop
#define LOOP_CONFIDENCE_THRESHOLD 0.82 // How confident we need to be (match percentage)
#define LOOP_FAULT_THRESHOLD 1  // Disable loop handling after this many faults

// Thrashing protection parameters
#define THRASHING_THRESHOLD 800  // If a page was evicted less than this many ticks ago, consider it thrashing
#define THRASHING_BONUS 2.0     // Multiply page_hits by this factor for recently evicted pages

// Stride pattern detection parameters
#define STRIDE_HISTORY_SIZE 40      // Number of PC values to track for stride detection
#define MIN_STRIDE_SEQUENCE 4      // Minimum sequence length to confirm a stride pattern
#define MAX_STRIDE_DISTANCE 16     // Maximum stride distance to consider valid (in pages))
#define BASE_STRIDE_LOOKAHEAD 3    // Base number of pages for stride lookahead
#define MAX_STRIDE_LOOKAHEAD 8     // Maximum number of pages for stride lookahead

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
    
    // Page eviction timing tracking
    static int last_evicted_tick[MAXPROCESSES][MAXPROCPAGES]; // When was each page last evicted
    
    // Loop detection variables
    static int in_loop[MAXPROCESSES];         // Indicates if process is in a loop
    static int loop_start[MAXPROCESSES];      // Start position of detected loop
    static int loop_length[MAXPROCESSES];     // Length of detected loop
    static int loop_pages[MAXPROCESSES][MAXPROCPAGES]; // Pages accessed in loop
    static int loop_page_count[MAXPROCESSES]; // Number of unique pages in loop
    static int loop_iterations[MAXPROCESSES]; // Iterations of the loop observed
    static int loop_faults[MAXPROCESSES];     // Page faults during loop execution
    static int loop_disabled[MAXPROCESSES];   // Disable loop handling if not helping

    // Stride pattern detection variables
    static int page_history[MAXPROCESSES][STRIDE_HISTORY_SIZE]; // Recent page accesses
    static int page_history_idx[MAXPROCESSES];                  // Current index in page history
    static int stride_detected[MAXPROCESSES];                   // Whether a stride pattern is detected
    static int stride_value[MAXPROCESSES];                      // Current detected stride value
    static int stride_confidence[MAXPROCESSES];                 // How confident we are in the stride

    if (!initialized)
    {
        for (int i = 0; i < MAXPROCESSES; i++)
        {
            for (int j = 0; j < MAXPROCPAGES; j++)
            {
                page_hits[i][j] = 0.0f; // Initialize as float
                loop_pages[i][j] = 0;
                last_evicted_tick[i][j] = 0; // Initialize eviction tracking
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

            // Initialize stride detection variables
            for (int j = 0; j < STRIDE_HISTORY_SIZE; j++) {
                page_history[i][j] = -1;
            }
            page_history_idx[i] = 0;
            stride_detected[i] = 0;
            stride_value[i] = 0;
            stride_confidence[i] = 0;
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
        if (!q[proc].active){
            // Page out all pages for inactive processes
            for (int i = 0; i < MAXPROCPAGES; i++) {
                if (q[proc].pages[i]) {
                    pageout(proc, i);
                }
            }
            continue;
        }

        int pc = q[proc].pc;
        int page = pc / PAGESIZE;

        // Update history
        pc_history[proc][history_index[proc]] = pc;
        history_index[proc] = (history_index[proc] + 1) % HISTORY_SIZE;

        // Update page history for stride detection
        page_history[proc][page_history_idx[proc]] = page;
        page_history_idx[proc] = (page_history_idx[proc] + 1) % STRIDE_HISTORY_SIZE;

        // Attempt to detect stride pattern
        stride_detected[proc] = 0;
        
        // Only try to detect stride if not in a loop - loops take precedence
        if (!in_loop[proc]) {
            // Check for stride pattern - try various possible strides
            for (int stride = 1; stride <= MAX_STRIDE_DISTANCE; stride++) {
                int matches = 0;
                
                // Look for consistent differences between sequential accesses
                for (int i = 2; i <= MIN_STRIDE_SEQUENCE; i++) {
                    int idx1 = (page_history_idx[proc] - i + STRIDE_HISTORY_SIZE) % STRIDE_HISTORY_SIZE;
                    int idx2 = (page_history_idx[proc] - i + 1 + STRIDE_HISTORY_SIZE) % STRIDE_HISTORY_SIZE;
                    
                    if (page_history[proc][idx1] != -1 && page_history[proc][idx2] != -1) {
                        if (page_history[proc][idx2] - page_history[proc][idx1] == stride) {
                            matches++;
                        }
                    }
                }
                
                // If we found a consistent stride pattern
                if (matches >= MIN_STRIDE_SEQUENCE - 1) {
                    stride_detected[proc] = 1;
                    stride_value[proc] = stride;
                    stride_confidence[proc] = matches;
                    break;
                }
                
                // Also check for negative stride (decreasing addresses)
                matches = 0;
                for (int i = 2; i <= MIN_STRIDE_SEQUENCE; i++) {
                    int idx1 = (page_history_idx[proc] - i + STRIDE_HISTORY_SIZE) % STRIDE_HISTORY_SIZE;
                    int idx2 = (page_history_idx[proc] - i + 1 + STRIDE_HISTORY_SIZE) % STRIDE_HISTORY_SIZE;
                    
                    if (page_history[proc][idx1] != -1 && page_history[proc][idx2] != -1) {
                        if (page_history[proc][idx2] - page_history[proc][idx1] == -stride) {
                            matches++;
                        }
                    }
                }
                
                if (matches >= MIN_STRIDE_SEQUENCE - 1) {
                    stride_detected[proc] = 1;
                    stride_value[proc] = -stride;
                    stride_confidence[proc] = matches;
                    break;
                }
            }
        }

        // Check if this page was recently evicted (thrashing detection)
        if (!q[proc].pages[page] && 
            last_evicted_tick[proc][page] > 0 && 
            (tick - last_evicted_tick[proc][page]) < THRASHING_THRESHOLD) {
            // This page was recently evicted - apply a bonus to its hit count
            page_hits[proc][page] *= THRASHING_BONUS;
        }

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

        // Calculate working set priority using float arithmetic
        float avg_hits = 0.0f;
        float total_hits = 0.0f;
        for (int i = 0; i < MAXPROCPAGES; i++) {
            total_hits += page_hits[proc][i];
        }
        avg_hits = total_hits / (float)MAXPROCPAGES;

        // Prepare page decisions
        int should_be_loaded[MAXPROCPAGES] = {0}; // Pages that should be in memory
        int should_be_evicted[MAXPROCPAGES] = {0}; // Pages that should be evicted

        // Determine which pages should be loaded or evicted based on strategy
        if (in_loop[proc] && loop_page_count[proc] <= remaining_pages && loop_iterations[proc] >= MIN_LOOP_ITERATIONS) {
            // Loop-based strategy
            int current_offset = (history_index[proc] - loop_start[proc] + HISTORY_SIZE) % loop_length[proc];
            
            // First mark current and next pages in the loop sequence
            for (int i = 0; i < min(remaining_pages, loop_page_count[proc]); i++) {
                int offset = (current_offset + i) % loop_length[proc];
                int loop_pc = pc_history[proc][(loop_start[proc] + offset) % HISTORY_SIZE];
                int loop_page = loop_pc / PAGESIZE;
                if (loop_page < MAXPROCPAGES) {
                    should_be_loaded[loop_page] = 1;
                }
            }
            
            // Non-loop pages can be evicted if memory is needed
            if (loop_page_count[proc] > (PHYSICALPAGES / MAXPROCESSES) - 1) {
                for (int i = 0; i < MAXPROCPAGES; i++) {
                    if (!should_be_loaded[i] && !loop_pages[proc][i]) {
                        should_be_evicted[i] = 1;
                    }
                }
            }
        } 
        else if (stride_detected[proc] && !in_loop[proc]) {
            // Stride-based prefetching strategy
            
            // Always keep current page
            should_be_loaded[page] = 1;
            
            // Calculate dynamic lookahead based on confidence
            // Higher confidence = prefetch further ahead
            int stride_lookahead = BASE_STRIDE_LOOKAHEAD;
            if (stride_confidence[proc] > MIN_STRIDE_SEQUENCE) {
                // Add up to additional pages based on confidence level
                stride_lookahead += min(
                    stride_confidence[proc] - MIN_STRIDE_SEQUENCE,
                    MAX_STRIDE_LOOKAHEAD - BASE_STRIDE_LOOKAHEAD
                );
            }
            
            // Prefetch ahead based on stride pattern using dynamic lookahead
            for (int i = 1; i <= stride_lookahead; i++) {
                int predicted_page = page + (i * stride_value[proc]);
                if (predicted_page >= 0 && predicted_page < MAXPROCPAGES) {
                    should_be_loaded[predicted_page] = 1;
                }
            }
            
            // Keep high-priority pages regardless of stride pattern
            for (int i = 0; i < MAXPROCPAGES; i++) {
                if (page_hits[proc][i] >= avg_hits * LOCK_RATIO * 1.5) { // Higher threshold for stride mode
                    should_be_loaded[i] = 1;
                }
            }
            
            // Mark pages for eviction if they're not needed based on the stride
            for (int i = 0; i < MAXPROCPAGES; i++) {
                if (!should_be_loaded[i] && 
                    (i < page - MAX_STRIDE_DISTANCE || i > page + stride_lookahead * abs(stride_value[proc]))) {
                    should_be_evicted[i] = 1;
                }
            }
        }
        else {
            // Standard working set strategy
            
            // Mark pages that must stay loaded
            should_be_loaded[page] = 1; // Current page always needed
            
            // Mark high priority pages to load
            for (int i = 0; i < MAXPROCPAGES; i++) {
                if (page_hits[proc][i] >= avg_hits * LOCK_RATIO) {
                    should_be_loaded[i] = 1;
                }
            }
            
            // Mark pages in lookahead window to load
            for (int i = page + 1; i < page + process_lookahead[proc] && i < MAXPROCPAGES; i++) {
                should_be_loaded[i] = 1;
            }
            
            // Mark pages that should be evicted
            for (int i = 0; i < MAXPROCPAGES; i++) {
                if (!should_be_loaded[i] && (
                    page_hits[proc][i] < avg_hits * LOW_USAGE_THRESHOLD ||  // Very low usage
                    ((i < page || i >= page + process_lookahead[proc]) &&   // Outside lookahead
                     page_hits[proc][i] < avg_hits * LOCK_RATIO)            // Not high priority
                )) {
                    should_be_evicted[i] = 1;
                }
            }
        }
        
        // STEP 1: First handle all page evictions and track times
        for (int i = 0; i < MAXPROCPAGES; i++) {
            if (q[proc].pages[i] && should_be_evicted[i]) {
                // Record eviction time before calling pageout
                last_evicted_tick[proc][i] = tick;
                pageout(proc, i);
            }
        }
        
        // STEP 2: Then handle all page loads
        for (int i = 0; i < MAXPROCPAGES; i++) {
            if (!q[proc].pages[i] && should_be_loaded[i] && remaining_pages > 0) {
                if (pagein(proc, i)) {
                    remaining_pages--;
                }
            }
        }
    }

    tick++;
}