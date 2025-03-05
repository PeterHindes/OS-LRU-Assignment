import subprocess
import re
import numpy as np
import threading
import time
from queue import Queue
import datetime
import csv
import os.path

# Add a global lock to synchronize test runs
run_lock = threading.Lock()
# Track the last execution time globally
last_execution_time = 0

def run_test_predict():
    """Runs ./test-final and parses the output."""
    result = subprocess.run("./test-final", shell=True, capture_output=True, text=True)
    output = (result.stdout + result.stderr).strip()  # Combine stdout and stderr
    
    print("Raw output from ./test-final:")
    print(repr(output))  # Print output with special characters visible
    
    if not output:
        print("Error: No output received from ./test-final. Ensure the program is running correctly.")
        return None, None, None
    
    # Extract numerical values with error handling
    blocked_match = re.search(r"(\d+):\s+(\d+)\s+blocked cycles", output)
    compute_match = re.search(r"(\d+):\s+(\d+)\s+compute cycles", output)
    ratio_match = re.search(r"(\d+):\s+ratio blocked/compute=([\d\.]+)", output)
    
    if not (blocked_match and compute_match and ratio_match):
        print("Error: Failed to parse output. Check the format.")
        return None, None, None
    
    blocked_cycles = int(blocked_match.group(2))
    compute_cycles = int(compute_match.group(2))
    ratio = float(ratio_match.group(2))
    
    return blocked_cycles, compute_cycles, ratio

def compute_statistics(data):
    """Computes mean, standard deviation, and max swing."""
    if not data:
        return 0, 0, 0, 0
    mean = np.mean(data)
    stddev = np.std(data)
    max_swing = max(data) - min(data)
    min_value = min(data)
    return mean, stddev, max_swing, min_value

def thread_run_test(thread_id, runs, result_queue, start_delay=0):
    """Function to be run in each thread"""
    global last_execution_time
    
    print(f"Thread {thread_id} starting after {start_delay} seconds delay")
    time.sleep(start_delay)  # Staggered start
    
    blocked_cycles_list = []
    compute_cycles_list = []
    ratio_list = []
    alert_file = f"high_ratios_thread_{thread_id}.log"
    
    for i in range(runs):
        # Acquire lock before running the test
        with run_lock:
            # Ensure 10ns have passed since the last execution
            # Note: Python can't actually sleep for 10ns, but we'll set the threshold very low
            current_time = time.time()
            if current_time - last_execution_time < 10e-9:
                wait_time = max(10e-9, (10e-9 - (current_time - last_execution_time)))
                print(f"Thread {thread_id} waiting {wait_time:.9f}s before starting run {i+1}")
                # Use minimal sleep - system will use smallest possible value
                time.sleep(0.001)  # 1ms is typically the minimum reliable sleep
            
            print(f"Thread {thread_id} starting run {i+1}")
            # Update the last execution time
            last_execution_time = time.time()
            
            blocked, compute, ratio = run_test_predict()
            if blocked is None:
                continue  # Skip this iteration if parsing failed
            
            blocked_cycles_list.append(blocked)
            compute_cycles_list.append(compute)
            ratio_list.append(ratio)
            
            print(f"Thread {thread_id}, Run {i+1}: Blocked Cycles={blocked}, Compute Cycles={compute}, Ratio={ratio:.6f}")
            
            # Check if ratio exceeds 0.04 and log it
            if ratio > 0.04:
                print(f"Thread {thread_id}: High ratio detected! Check {alert_file} for details.")
                with open(alert_file, "a") as f:
                    f.write(f"Run {i+1}: Blocked Cycles={blocked}, Compute Cycles={compute}, Ratio={ratio:.6f}\n")
            
            # Print running statistics
            mean, stddev, max_swing, min_value = compute_statistics(ratio_list)
            print(f"  Thread {thread_id} - Ratio - Mean: {mean:.6f}, Std Dev: {stddev:.6f}, Max Swing: {max_swing:.6f}")
            print(f"  Thread {thread_id} - Ratio - Max: {max(ratio_list):.6f}, Min: {min_value:.6f}")
            print("--------------------------------------------------")
        
        # Minimal wait between runs
        time.sleep(0.001)  # Reduced from 1.5s to 1ms
    
    # Put the results in the queue
    result_queue.put((thread_id, blocked_cycles_list, compute_cycles_list, ratio_list))

def save_statistics(label, total_runs, stats_dict):
    """Save statistics to a CSV file."""
    timestamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    csv_file = "performance_stats.csv"
    file_exists = os.path.isfile(csv_file)
    
    with open(csv_file, 'a', newline='') as f:
        fieldnames = ['timestamp', 'label', 'total_runs', 
                      'mean_ratio', 'stddev_ratio', 'max_swing_ratio', 'max_ratio', 'min_ratio']
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        
        if not file_exists:
            writer.writeheader()
        
        writer.writerow({
            'timestamp': timestamp,
            'label': label,
            'total_runs': total_runs,
            'mean_ratio': f"{stats_dict['mean']:.6f}",
            'stddev_ratio': f"{stats_dict['stddev']:.6f}",
            'max_swing_ratio': f"{stats_dict['max_swing']:.6f}",
            'max_ratio': f"{stats_dict['max_value']:.6f}",
            'min_ratio': f"{stats_dict['min_value']:.6f}"
        })
    
    print(f"Statistics saved to {csv_file}")

def main():
    # Initialize the last execution time
    global last_execution_time
    last_execution_time = time.time()
    
    # Ask user for a label for this test run
    run_label = input("Enter a label for this test run: ")
    
    runs = 10  # Increased from 5 to 10 runs per thread
    num_threads = 100  # Increased from 24 to 100 threads
    stagger_time = 0.001  # Reduced from 1.5s to 1ms (10ns not practical with time.sleep)
    
    threads = []
    result_queue = Queue()
    
    # Create and start threads with staggered start times
    for i in range(num_threads):
        start_delay = i * stagger_time
        t = threading.Thread(
            target=thread_run_test,
            args=(i+1, runs, result_queue, start_delay)
        )
        threads.append(t)
        t.start()
        print(f"Started thread {i+1}")
    
    # Wait for all threads to complete
    for t in threads:
        t.join()
    
    # Collect all results
    all_results = []
    while not result_queue.empty():
        all_results.append(result_queue.get())
    
    # Process and display aggregate results
    print("\nAggregate Results:")
    all_blocked = []
    all_compute = []
    all_ratio = []
    
    for thread_id, blocked, compute, ratio in all_results:
        print(f"Thread {thread_id} completed {len(ratio)} runs")
        all_blocked.extend(blocked)
        all_compute.extend(compute)
        all_ratio.extend(ratio)
    
    total_runs = len(all_ratio)  # The actual number of successful runs
    
    print("\nFinal Aggregate Statistics:")
    print(f"Total successful runs: {total_runs}")
    print("Statistics for Blocked/Compute Ratio:")
    mean, stddev, max_swing, min_value = compute_statistics(all_ratio)
    max_ratio = max(all_ratio) if all_ratio else 0
    print(f"  Mean: {mean:.6f}, Std Dev: {stddev:.6f}, Max Swing: {max_swing:.6f}")
    print(f"  Max Ratio: {max_ratio:.6f}, Min Ratio: {min_value:.6f}")
    
    # Save statistics to file
    stats_dict = {
        'mean': mean,
        'stddev': stddev,
        'max_swing': max_swing,
        'max_value': max_ratio,
        'min_value': min_value
    }
    save_statistics(run_label, total_runs, stats_dict)

if __name__ == "__main__":
    main()
