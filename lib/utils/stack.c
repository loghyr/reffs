/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <execinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#include "reffs/stack.h"

#define MAX_STACK_FRAMES 64

void print_stack_trace(void)
{
	void *stack_frames[MAX_STACK_FRAMES];
	int frame_count;
	char **frame_strings;

	// Get the stack frames
	frame_count = backtrace(stack_frames, MAX_STACK_FRAMES);

	// Convert addresses to strings
	frame_strings = backtrace_symbols(stack_frames, frame_count);
	if (frame_strings == NULL) {
		perror("backtrace_symbols failed");
		return;
	}

	// Print the stack trace
	fprintf(stderr, "Stack trace (%d frames):\n", frame_count);
	for (int i = 0; i < frame_count; i++) {
		fprintf(stderr, "  %s\n", frame_strings[i]);
	}

	// Free the strings
	free(frame_strings);
}
