#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

import sys
import re
from datetime import datetime

def parse_timestamp(line):
    """Extract timestamp from log line and convert to datetime object."""
    match = re.search(r'\[(.*?)\.(\d+)\]', line)
    if match:
        # Combine the timestamp and microseconds/nanoseconds
        timestamp_str = match.group(1)
        nano_part = match.group(2)

        # Parse the main timestamp part
        dt = datetime.strptime(timestamp_str, '%Y-%m-%d %H:%M:%S')

        # Convert nanoseconds to microseconds (truncate if necessary)
        # Standard datetime only supports microseconds (6 digits)
        microseconds = int(nano_part[:6])

        # Create a new datetime with microseconds
        dt = dt.replace(microsecond=microseconds)
        return dt
    return None

def find_gaps(log_file, threshold_seconds=59.0):
    """Find log entries with gaps exceeding the threshold."""
    previous_timestamp = None

    with open(log_file, 'r') as f:
        for line in f:
            timestamp = parse_timestamp(line)

            if timestamp and previous_timestamp:
                time_diff = (timestamp - previous_timestamp).total_seconds()

                if time_diff > threshold_seconds:
                    print(f"Gap of {time_diff:.6f} seconds found before:")
                    print(line.strip())

            if timestamp:
                previous_timestamp = timestamp

def main():
    log_file = sys.argv[1] if len(sys.argv) > 1 else None

    if not log_file:
        print("Usage: python log_gap_finder.py <log_file> [threshold_seconds]")
        sys.exit(1)

    try:
        threshold_seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 59.0
    except ValueError:
        print("Error: threshold must be a number in seconds")
        sys.exit(1)

    find_gaps(log_file, threshold_seconds)

if __name__ == "__main__":
    main()
