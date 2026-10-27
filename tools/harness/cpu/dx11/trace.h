// What reaches the real Direct3D 11 device from the DLL, as two hashes a frame:
// "strict" takes every call that changes something, in order; "effective"
// takes only the calls that do something (draws, dispatches, copies, clears,
// uploads) together with the state bound at that moment. Resources are named
// by what they hold, never by address, so two runs agree exactly when the
// same data flows to the same places.
#pragma once
#include <stdint.h>
#include <stdio.h>

struct TraceFrame {
	uint64_t strict, effective;
	unsigned calls, actions, draws, unknown;
};

// Replaces a real device and its immediate context, as a creation function
// of Direct3D returned them, by objects that pass every call on and listen.
void trace_wrap(void **device, void **context);
void trace_enable(bool on);
TraceFrame trace_frame_end();
// Lines for the actions of the frames first..last go to file
void trace_text(FILE *file, unsigned first, unsigned last);
void trace_note_frame(unsigned frame);
