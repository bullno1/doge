// Stopping a run and carrying it on: the suspensions an extcall or a debug hook
// can raise, the frames and continuations that carry a half finished host call
// across one, and the boundaries a suspension is not allowed to cross.
#include "program.h"

static shibe_status_t
suspending_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host; (void)called; (void)index;
	++num_extcalls;
	return SHIBE_SUSPENDED;
}

// Stops once at the start of every bundle. Resuming reports the opcode it
// stopped on again, so the hook has to remember where it last stopped or it
// would ask for the same suspension for ever.
static shibe_cell_t last_stop;
static int num_stops;

// Counts every point the host could stop at, and says whether to stop at this
// one. A target of 0 stops nowhere, which is the reference run; -1 stops
// everywhere it can.
static int sweep_step;
static int sweep_target;

// What the sweep actually reached. A property test that never enters the shapes
// it is meant to cover passes for the wrong reason, so these are asserted.
static int sweep_leaf_stops;
static int sweep_late_stops;
static int sweep_defers;
static int sweep_reentries;

// Host calls entered, and host calls that reached their end. Comparing vm state
// alone cannot see a call that was dropped half way, or done twice, when its
// effect on the stacks happens to cancel out. Every run has to finish what it
// starts, whether or not it stopped along the way.
static int sweep_started;
static int sweep_finished;

// Hook stops taken inside the run a continuation started. That is the only way
// to get a stop with no frame relayed through a continuation, which is the one
// shape that tells a relayed suspension apart from a continuation's own.
static int sweep_deep_stops;

static bool
sweep_should_stop(void) {
	++sweep_step;
	return sweep_target < 0 || sweep_step == sweep_target;
}

static shibe_status_t
stepping_hook(shibe_host_t* host, shibe_vm_t* hooked, const shibe_state_t* state, shibe_op_addr_t at) {
	(void)host; (void)hooked; (void)state;
	if (num_steps < MAX_STEPS) { steps[num_steps] = at; }
	++num_steps;

	if (at.slot != 0 || at.bundle.u32 == last_stop.u32) { return SHIBE_OK; }

	last_stop = at.bundle;
	++num_stops;
	return SHIBE_SUSPENDED;
}

// Suspends on one nominated opcode, named by where it sits rather than by a
// step count so that it can pick one out of a nested run
static shibe_op_addr_t suspend_at;

static shibe_status_t
suspending_hook(shibe_host_t* host, shibe_vm_t* hooked, const shibe_state_t* state, shibe_op_addr_t at) {
	(void)host; (void)hooked; (void)state;
	if (num_steps < MAX_STEPS) { steps[num_steps] = at; }
	++num_steps;
	return at.bundle.u32 == suspend_at.bundle.u32 && at.slot == suspend_at.slot
		? SHIBE_SUSPENDED
		: SHIBE_OK;
}

// A second stub, for a re-entry made from inside a continuation. It leaves the
// data stack as it found it.
static shibe_cell_t deferred_entry;

// Call 1 re-enters, and the nested run it starts is the one that suspends
static shibe_status_t
nested_suspending_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	return index.u32 == 1
		? reentrant_extcall(host, called, index)
		: suspending_extcall(host, called, index);
}

// Re-enters, lets the nested run finish, then suspends. The frame the re-entry
// opened is still this call's, so the suspension needs a continuation to finish
// it - which is call 9 below, standing in for the rest of this function.
static shibe_status_t
reenter_then_suspend_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host;
	++num_extcalls;

	// Call 9 is the rest of this function, reached after the resume
	if (index.u32 == 9) {
		shibe_push(called, (shibe_cell_t){ .i32 = 5 });
		return SHIBE_OK;
	}

	shibe_frame_t frame = shibe_alloc_frame(called, 0);
	shibe_set_continuation(called, frame, (shibe_cell_t){ .u32 = 9 });
	shibe_status_t status = shibe_execute(called, nested_entry);
	return status == SHIBE_OK ? SHIBE_SUSPENDED : status;
}

// Second halves reached through a frame, counted so that a continuation that
// never ran shows up
static int num_continuations;

// Call 1 re-enters with a continuation and a couple of context slots, call 2
// suspends the nested run, call 3 is the second half of call 1
static shibe_status_t
continuing_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host;
	++num_extcalls;

	switch (index.u32) {
		case 1: {
			shibe_frame_t frame = shibe_alloc_frame(called, 2);
			shibe_set_continuation(called, frame, (shibe_cell_t){ .u32 = 3 });
			shibe_status_t status = shibe_execute(called, nested_entry);
			// The frame is still this call's on the way out, so what the second
			// half needs goes in now, with the C locals still in scope
			if (status == SHIBE_SUSPENDED) {
				shibe_set_local(called, frame, 0, (shibe_cell_t){ .i32 = 42 });
			}
			return status;
		}
		case 2:
			return SHIBE_SUSPENDED;
		default: {
			++num_continuations;
			// Reached through call 1's frame, so its locals are right there
			shibe_frame_t frame = shibe_get_frame(called);
			observed_slot = shibe_get_local(called, frame, 0);
			shibe_push(called, (shibe_cell_t){ .i32 = 7 });
			return SHIBE_OK;
		}
	}
}

// As above, but the second half is not ready the first two times it is asked
static shibe_status_t
retrying_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host;
	++num_extcalls;

	switch (index.u32) {
		case 1: {
			shibe_frame_t frame = shibe_alloc_frame(called, 0);
			shibe_set_continuation(called, frame, (shibe_cell_t){ .u32 = 3 });
			return shibe_execute(called, nested_entry);
		}
		case 2:
			return SHIBE_SUSPENDED;
		default:
			if (++num_continuations < 3) {
				// Being called consumed the continuation, so asking to be
				// called again has to be said rather than inherited
				shibe_set_continuation(called, shibe_get_frame(called), index);
				return SHIBE_SUSPENDED;
			}
			shibe_push(called, (shibe_cell_t){ .i32 = 7 });
			return SHIBE_OK;
	}
}

static int num_deep_calls;

// Call 1 re-enters and call 2 suspends that run. Call 3, the second half, grows
// the frame it was reached through and re-enters again, so when call 2 stops the
// second run the continuation is relaying rather than stopping on its own. Call
// 7 is what finally finishes call 1, and call 8 marks the far side of each
// suspension so an abandoned run shows up.
static shibe_status_t
reentering_continuation_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host;
	++num_extcalls;

	switch (index.u32) {
		case 1: {
			shibe_frame_t frame = shibe_alloc_frame(called, 1);
			shibe_set_local(called, frame, 0, (shibe_cell_t){ .i32 = 11 });
			shibe_set_continuation(called, frame, (shibe_cell_t){ .u32 = 3 });
			return shibe_execute(called, nested_entry);
		}
		case 2:
			return SHIBE_SUSPENDED;
		case 3: {
			++num_continuations;
			// Asking again inside a continuation grows the frame it was
			// reached through, since that frame is this call's
			shibe_frame_t frame = shibe_alloc_frame(called, 2);
			shibe_set_local(called, frame, 1, (shibe_cell_t){ .i32 = 22 });
			shibe_set_continuation(called, frame, (shibe_cell_t){ .u32 = 7 });
			return shibe_execute(called, nested_entry);
		}
		case 7: {
			++num_continuations;
			shibe_frame_t frame = shibe_get_frame(called);
			observed_slot = shibe_get_local(called, frame, 0);
			observed_frame_head = shibe_get_local(called, frame, 1);
			shibe_push(called, (shibe_cell_t){ .i32 = 7 });
			return SHIBE_OK;
		}
		default:
			++num_deep_calls;
			return SHIBE_OK;
	}
}

// A second half that stops again without naming the next one, leaving a frame
// nothing could ever take back
static shibe_status_t
forgetful_continuation_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host;
	++num_extcalls;

	switch (index.u32) {
		case 1: {
			shibe_frame_t frame = shibe_alloc_frame(called, 0);
			shibe_set_continuation(called, frame, (shibe_cell_t){ .u32 = 3 });
			return shibe_execute(called, nested_entry);
		}
		case 3:
			++num_continuations;
			return SHIBE_SUSPENDED;
		default:
			return SHIBE_SUSPENDED;
	}
}

// Call 1 stops the run; call 5 is the second half of the host function that
// started it from outside the vm
static shibe_status_t
top_level_continuation_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host;
	++num_extcalls;

	if (index.u32 == 5) {
		++num_continuations;
		observed_slot = shibe_get_local(called, shibe_get_frame(called), 0);
		return SHIBE_OK;
	}

	return SHIBE_SUSPENDED;
}

// Re-enters, is handed a suspension, and reports success anyway
static shibe_status_t
swallowing_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host;
	++num_extcalls;
	if (index.u32 != 1) { return SHIBE_SUSPENDED; }

	shibe_frame_t frame = shibe_alloc_frame(called, 0);
	shibe_set_continuation(called, frame, (shibe_cell_t){ .u32 = 3 });
	shibe_execute(called, nested_entry);
	return SHIBE_OK;
}

// Call 1 stops the run; call 3 finishes it, but the run left a frame of its own
// standing when it halted
static shibe_status_t
untidy_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host;
	++num_extcalls;
	if (index.u32 == 3) {
		++num_continuations;
		return SHIBE_OK;
	}
	return SHIBE_SUSPENDED;
}

// The plain async case: a leaf call that never re-enters, suspends, and is
// finished later by call 4. The frame is where it keeps both the continuation
// and the context that second half needs.
static shibe_status_t
deferring_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host;
	++num_extcalls;

	if (index.u32 == 4) {
		++num_continuations;
		shibe_push(called, shibe_get_local(called, shibe_get_frame(called), 0));
		return SHIBE_OK;
	}

	shibe_frame_t frame = shibe_alloc_frame(called, 1);
	shibe_set_local(called, frame, 0, (shibe_cell_t){ .i32 = 7 });
	shibe_set_continuation(called, frame, (shibe_cell_t){ .u32 = 4 });
	return SHIBE_SUSPENDED;
}

// Suspends holding a frame nobody could ever finish
static shibe_status_t
stranding_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host; (void)index;
	++num_extcalls;
	shibe_alloc_frame(called, 1);
	return SHIBE_SUSPENDED;
}

static void
suspend_init_per_test(void) {
	init_per_program_test();
	// No real opcode sits at address 0, so nothing matches until a test says so
	suspend_at = (shibe_op_addr_t){ 0 };
	deferred_entry = (shibe_cell_t){ 0 };
	num_continuations = 0;
	num_deep_calls = 0;
	last_stop = (shibe_cell_t){ 0 };
	num_stops = 0;
	sweep_step = 0;
	sweep_target = 0;
}

static btest_suite_t ssuspend = {
	.name = "shibe/suspend",

	.init_per_suite = init_per_suite,
	.cleanup_per_suite = cleanup_per_suite,
	.init_per_test = suspend_init_per_test,
	.cleanup_per_test = cleanup_per_program_test,
};

// Opens a frame inside a bundle, names a continuation for it, and re-enters.
// The nested run then stops, which is a suspension that would have to cross a
// frame recording a point no address can name.
static shibe_status_t
framed_reentering_hook(shibe_host_t* host, shibe_vm_t* hooked, const shibe_state_t* state, shibe_op_addr_t at) {
	(void)host; (void)state;
	++num_steps;
	if (at.bundle.u32 != suspend_at.bundle.u32 || at.slot != suspend_at.slot) {
		return SHIBE_OK;
	}

	shibe_frame_t frame = shibe_alloc_frame(hooked, 0);
	shibe_set_continuation(hooked, frame, (shibe_cell_t){ .u32 = 6 });
	return shibe_execute(hooked, nested_entry);
}

/* Suspension has to be invisible: a run that stops and is resumed must end up
 * exactly where the same run would have ended without stopping. That makes a
 * plain run the oracle for every stopped one, so suspension points can be swept
 * without writing an expectation for each. */

#define SWEEP_PRODUCE   1u  // pushes 10, possibly late
#define SWEEP_REENTER   2u  // runs `nested_entry`, possibly stopping after it
#define SWEEP_PRODUCED 11u  // the second half of SWEEP_PRODUCE
#define SWEEP_REENTERED 12u // the second half of SWEEP_REENTER
#define SWEEP_QUIET      3u // as SWEEP_PRODUCE, for the run a continuation starts
#define SWEEP_QUIETED   13u // the second half of SWEEP_QUIET
// Brackets around a run, so that dropping it anywhere inside shows up as a call
// that started and never finished
#define SWEEP_MARK_IN    4u
#define SWEEP_MARK_OUT   5u

static shibe_status_t
sweep_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host;
	++num_extcalls;

	switch (index.u32) {
		case SWEEP_MARK_IN:
			++sweep_started;
			return SHIBE_OK;

		case SWEEP_MARK_OUT:
			++sweep_finished;
			return SHIBE_OK;

		case SWEEP_QUIET:
		case SWEEP_PRODUCE:
			++sweep_started;
			if (sweep_should_stop()) {
				++sweep_leaf_stops;
				shibe_frame_t frame = shibe_alloc_frame(called, 2);
				shibe_set_local(called, frame, 0, (shibe_cell_t){ .i32 = 10 });
				shibe_set_continuation(called, frame, (shibe_cell_t){
					.u32 = index.u32 == SWEEP_PRODUCE ? SWEEP_PRODUCED : SWEEP_QUIETED,
				});
				return SHIBE_SUSPENDED;
			}
			++sweep_finished;
			shibe_push(called, (shibe_cell_t){ .i32 = 10 });
			return SHIBE_OK;

		case SWEEP_QUIETED:
		case SWEEP_PRODUCED: {
			shibe_frame_t frame = shibe_get_frame(called);
			// Local 1 makes this a one shot, so stopping everywhere still ends
			if (shibe_get_local(called, frame, 1).u32 == 0 && sweep_should_stop()) {
				++sweep_defers;
				shibe_set_local(called, frame, 1, (shibe_cell_t){ .u32 = 1 });
				// Being called consumed it, so ask to be called again
				shibe_set_continuation(called, frame, index);
				return SHIBE_SUSPENDED;
			}
			// What the first half would have left behind
			++sweep_finished;
			shibe_push(called, shibe_get_local(called, frame, 0));
			return SHIBE_OK;
		}

		case SWEEP_REENTER: {
			++sweep_started;
			shibe_frame_t frame = shibe_alloc_frame(called, 1);
			shibe_set_continuation(
				called, frame, (shibe_cell_t){ .u32 = SWEEP_REENTERED }
			);
			shibe_status_t status = shibe_execute(called, nested_entry);
			if (status != SHIBE_OK) { return status; }
			// Stopping here is the other shape: the run this call started has
			// already finished, so there is nothing to pick up but the call
			if (!sweep_should_stop()) { ++sweep_finished; return SHIBE_OK; }
			++sweep_late_stops;
			return SHIBE_SUSPENDED;
		}

		default: {
			// The nested run did the work, so there is nothing left to leave.
			// Starting another one from in here is the shape that matters: a
			// second half that runs something which then stops is relaying, not
			// stopping itself, and the run it started is what a resume owes.
			shibe_frame_t frame = shibe_get_frame(called);
			if (shibe_get_local(called, frame, 0).u32 == 0 && sweep_should_stop()) {
				++sweep_reentries;
				// Counted here rather than inside the run, because a hook can
				// stop at the head of its very first bundle - before anything
				// in it has run. A marker inside could never see that stop being
				// dropped; one taken before the run starts always can.
				++sweep_started;
				shibe_set_local(called, frame, 0, (shibe_cell_t){ .u32 = 1 });
				shibe_set_continuation(called, frame, index);
				// Leaves the data stack as it found it, so the run underneath
				// cannot tell it happened
				return shibe_execute(called, deferred_entry);
			}
			++sweep_finished;
			return SHIBE_OK;
		}
	}
}

// Stops at the start of a bundle, remembering where so that being reported
// again after the resume does not stop it a second time
static shibe_status_t
sweep_hook(shibe_host_t* host, shibe_vm_t* hooked, const shibe_state_t* state, shibe_op_addr_t at) {
	(void)host; (void)hooked; (void)state;
	++num_steps;

	if (at.slot != 0 || at.bundle.u32 == last_stop.u32) { return SHIBE_OK; }
	if (!sweep_should_stop()) { return SHIBE_OK; }

	if (deferred_entry.u32 != 0 && at.bundle.u32 >= deferred_entry.u32) {
		++sweep_deep_stops;
	}
	last_stop = at.bundle;
	return SHIBE_SUSPENDED;
}

typedef struct {
	shibe_status_t status;
	uint32_t depth;
	uint32_t asp;
	int panics;
	int started;
	int finished;
	shibe_cell_t ds[8];
} sweep_outcome_t;

// Runs `entry` to a stop that is not a suspension, however many resumes that
// takes, and reports where it ended up
static sweep_outcome_t
sweep_run(shibe_cell_t entry, int target) {
	shibe_reset(vm);
	num_panics = 0;
	last_panic = (shibe_panic_t){ 0 };
	last_stop = (shibe_cell_t){ 0 };
	sweep_step = 0;
	sweep_target = target;
	sweep_started = sweep_finished = 0;
	last_stop = (shibe_cell_t){ 0 };

	shibe_status_t status = shibe_execute(vm, entry);
	// The bound is what turns a resume that never gets anywhere into a failure
	for (int i = 0; status == SHIBE_SUSPENDED && i < 64; ++i) {
		status = shibe_resume(vm);
	}

	sweep_outcome_t outcome = {
		.status = status,
		.depth = shibe_inspect(vm)->dsp.u32,
		.asp = shibe_inspect(vm)->asp.u32,
		.panics = num_panics,
		.started = sweep_started,
		.finished = sweep_finished,
	};
	for (uint32_t i = 0; i < outcome.depth && i < 8; ++i) {
		outcome.ds[i] = shibe_inspect(vm)->ds[i];
	}
	return outcome;
}

static void
sweep(shibe_cell_t entry, const char* what) {
	sweep_leaf_stops = sweep_late_stops = sweep_defers = sweep_reentries = 0;
	sweep_deep_stops = 0;

	sweep_outcome_t want = sweep_run(entry, 0);
	BTEST_ASSERT_EQUAL("%d", want.status, SHIBE_OK);
	BTEST_ASSERT_EQUAL("%d", want.panics, 0);
	int num_points = sweep_step;
	BTEST_EXPECT(num_points > 0);

	// Every point on its own, then all of them at once
	for (int target = 1; target <= num_points + 1; ++target) {
		sweep_outcome_t got = sweep_run(entry, target <= num_points ? target : -1);

		BTEST_EXPECT_EQUAL("%s", what, what);
		BTEST_EXPECT_EQUAL("%d", got.status, want.status);
		BTEST_EXPECT_EQUAL("%d", got.panics, want.panics);
		BTEST_EXPECT_EQUAL("%u", got.depth, want.depth);
		BTEST_EXPECT_EQUAL("%u", got.asp, want.asp);
		// A call dropped half way, or done twice, shows up here even when its
		// effect on the stacks cancelled out
		BTEST_EXPECT_EQUAL("%d", got.finished, got.started);
		for (uint32_t i = 0; i < want.depth && i < 8; ++i) {
			BTEST_EXPECT_EQUAL("%d", got.ds[i].i32, want.ds[i].i32);
		}
	}

	// Every shape a stop can take has to have been reached, or the sweep is
	// only reporting that it did not look
	BTEST_EXPECT(sweep_leaf_stops > 0);
	BTEST_EXPECT(sweep_late_stops > 0);
	BTEST_EXPECT(sweep_defers > 0);
	BTEST_EXPECT(sweep_reentries > 0);
}

// Assembles a program that calls out, re-enters, and calls out again from
// inside the nested run, so one sweep covers leaf stops, stops taken after a
// nested run finished, and stops raised underneath one
static void
sweep_program(void) {
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = SWEEP_PRODUCE }));
	LIT(5);
	EMIT(ADD);
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = SWEEP_REENTER }));
	EMIT(ADD);
	EMIT(HALT);

	shibe_asm_align(sasm);
	nested_entry = shibe_asm_here(sasm);
	shibe_asm_label_t sub = shibe_asm_make_label(sasm);
	EMIT_LABEL(LIT, sub);
	EMIT(CALL);
	EMIT(HALT);

	shibe_asm_bind_label(sasm, sub);
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = SWEEP_MARK_IN }));
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = SWEEP_PRODUCE }));
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = SWEEP_PRODUCE }));
	EMIT(ADD);
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = SWEEP_MARK_OUT }));
	EMIT(RET);

	shibe_asm_align(sasm);
	deferred_entry = shibe_asm_here(sasm);
	shibe_asm_label_t sub2 = shibe_asm_make_label(sasm);
	EMIT_LABEL(LIT, sub2);
	EMIT(CALL);
	EMIT(HALT);

	// Only the far end is marked: the near end was counted before the run began
	shibe_asm_bind_label(sasm, sub2);
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = SWEEP_QUIET }));
	EMIT(DRP);
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = SWEEP_MARK_OUT }));
	EMIT(RET);

	BTEST_ASSERT(shibe_asm_end(sasm));
}

BTEST(ssuspend, stopping_anywhere_leaves_the_same_result) {
	test_host.extcall = sweep_extcall;
	sweep_program();
	sweep(code, "extcall stops");
}

BTEST(ssuspend, stopping_anywhere_with_a_hook_leaves_the_same_result) {
	// The hook adds a stop at the head of every bundle, and puts the whole
	// thing through the other interpreter build
	test_host.extcall = sweep_extcall;
	test_host.debug = sweep_hook;
	sweep_program();
	sweep(code, "extcall and hook stops");
	BTEST_EXPECT(sweep_deep_stops > 0);
}

BTEST(ssuspend, extcall_can_suspend) {
	test_host.extcall = suspending_extcall;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 3 }));
	EMIT(HALT);

	BTEST_EXPECT_EQUAL("%d", run(), SHIBE_SUSPENDED);
	BTEST_EXPECT_EQUAL("%d", num_extcalls, 1);
	// Suspending is not a failure, so nothing panicked
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	BTEST_EXPECT(shibe_inspect(vm)->exec_state == SHIBE_EXEC_SUSPENDED);
}

BTEST(ssuspend, extcall_resumes_after_the_call) {
	test_host.extcall = suspending_extcall;

	// EXTCALL closes its bundle, so the work below it is in the next one
	LIT(2);
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 3 }));
	LIT(5);
	EMIT(ADD);
	EMIT(ADD);
	EMIT(HALT);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_SUSPENDED);
	// Everything the run had done is still there
	BTEST_ASSERT_EQUAL("%u", depth(), 1u);

	// What a handler that suspends to wait on something does when the answer
	// turns up: leave the result behind, then let the run carry on
	shibe_push(vm, (shibe_cell_t){ .i32 = 10 });

	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_OK);
	// The call is not made a second time
	BTEST_EXPECT_EQUAL("%d", num_extcalls, 1);
	BTEST_EXPECT_EQUAL("%u", depth(), 1u);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 17);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	BTEST_EXPECT(shibe_inspect(vm)->exec_state == SHIBE_EXEC_IDLE);
}

BTEST(ssuspend, a_hook_can_stop_at_the_start_of_a_bundle) {
	test_host.debug = suspending_hook;

	// Slot 0 is the only place the run can be come back to: nothing in the
	// bundle has run and no operand has been consumed, so the bundle's own
	// address says all of it
	LIT(9);
	shibe_asm_align(sasm);
	shibe_cell_t second = shibe_asm_here(sasm);
	LIT(1);
	LIT(2);
	EMIT(ADD);
	EMIT(HALT);
	suspend_at = (shibe_op_addr_t){ .bundle = second, .slot = 0 };

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_SUSPENDED);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	// The hook reports an opcode before it runs, so the bundle is untouched
	BTEST_EXPECT_EQUAL("%u", depth(), 1u);
	// and `ip` is wound back to the bundle, which is where it carries on
	BTEST_EXPECT_EQUAL("%u", shibe_inspect(vm)->ip.u32, second.u32);

	// Resuming reports that opcode again, so a hook that stops on a condition
	// has to account for having just been resumed past it
	suspend_at = (shibe_op_addr_t){ 0 };

	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	BTEST_EXPECT_EQUAL("%u", depth(), 2u);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 3);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 9);
}

BTEST(ssuspend, a_frame_opened_inside_a_bundle_cannot_be_resumed_through) {
	test_host.debug = framed_reentering_hook;
	test_host.extcall = suspending_extcall;

	// The frame records the bundle, which is not where a run two opcodes into
	// it carries on. Nothing may reach that record: the suspension is refused
	// as it crosses the frame, not left to be resumed into the wrong place.
	suspend_at = (shibe_op_addr_t){ .bundle = shibe_asm_here(sasm), .slot = 2 };
	LIT(1);
	LIT(2);
	EMIT(ADD);
	EMIT(HALT);

	shibe_asm_align(sasm);
	nested_entry = shibe_asm_here(sasm);
	shibe_asm_label_t sub = shibe_asm_make_label(sasm);
	EMIT_LABEL(LIT, sub);
	EMIT(CALL);
	EMIT(HALT);

	shibe_asm_bind_label(sasm, sub);
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 2 }));
	EMIT(RET);

	BTEST_EXPECT_EQUAL("%d", run(), SHIBE_ERROR);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_NOT_SUSPENDABLE);
}

BTEST(ssuspend, a_hook_may_not_stop_inside_a_bundle) {
	test_host.debug = suspending_hook;

	// No address describes a point inside a bundle: two opcodes have run and
	// their operands are consumed, so there is nothing to come back to
	suspend_at = (shibe_op_addr_t){ .bundle = shibe_asm_here(sasm), .slot = 2 };
	LIT(1);
	LIT(2);
	EMIT(ADD);
	EMIT(HALT);

	BTEST_EXPECT_EQUAL("%d", run(), SHIBE_ERROR);
	BTEST_EXPECT_EQUAL("%d", num_steps, 3);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_NOT_SUSPENDABLE);
	BTEST_EXPECT(shibe_inspect(vm)->exec_state == SHIBE_EXEC_PANIC);
}

BTEST(ssuspend, a_hook_steps_a_bundle_at_a_time) {
	test_host.debug = stepping_hook;

	// Two bundles, so two stops. Resuming reports the opcode it stopped on
	// again, which is why the hook has to remember where it last stopped -
	// without that it would ask for the same suspension for ever.
	LIT(1);
	LIT(2);
	EMIT(ADD);
	EMIT(DUP);
	LIT(3);
	EMIT(ADD);
	EMIT(HALT);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_SUSPENDED);

	int num_resumes = 0;
	shibe_status_t status;
	while ((status = shibe_resume(vm)) == SHIBE_SUSPENDED && num_resumes < MAX_STEPS) {
		++num_resumes;
	}
	// The bound is what fails this if a resume ever stands still
	BTEST_EXPECT_EQUAL("%d", status, SHIBE_OK);
	BTEST_EXPECT_EQUAL("%d", num_stops, 2);

	BTEST_EXPECT_EQUAL("%u", depth(), 2u);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 6);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 3);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(ssuspend, the_hook_can_be_dropped_while_suspended) {
	test_host.debug = suspending_hook;

	suspend_at = (shibe_op_addr_t){ .bundle = shibe_asm_here(sasm), .slot = 0 };
	LIT(1);
	LIT(2);
	EMIT(ADD);
	EMIT(HALT);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_SUSPENDED);
	BTEST_ASSERT_EQUAL("%d", num_steps, 1);

	// The two interpreter builds have to agree about where a suspended run is,
	// and since that is only ever `ip`, this one is picked up by the build
	// without a hook
	test_host.debug = NULL;

	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%d", num_steps, 1);
	BTEST_EXPECT_EQUAL("%u", depth(), 1u);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 3);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(ssuspend, resume_without_a_suspension_is_rejected) {
	LIT(1);
	EMIT(HALT);

	// Idle: there is no activation to come back to
	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_ERROR);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_INVALID);
	clear_panic();

	// And a run that halted is over for good
	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_ERROR);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_INVALID);
}

BTEST(ssuspend, resuming_a_panicked_vm_reports_nothing_new) {
	test_host.extcall = failing_extcall;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 3 }));
	EMIT(HALT);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_ERROR);
	BTEST_ASSERT_EQUAL("%d", num_panics, 1);

	// Same as shibe_execute: the panic already fired, so this only relays it
	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_ERROR);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
}

BTEST(ssuspend, re_entry_while_suspended_is_rejected) {
	test_host.extcall = suspending_extcall;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	EMIT(HALT);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_SUSPENDED);
	BTEST_ASSERT_EQUAL("%d", num_panics, 0);

	// Only IDLE and RUNNING are re-entrant; a suspended run has to be resumed
	BTEST_EXPECT_EQUAL("%d", shibe_execute(vm, code), SHIBE_ERROR);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_INVALID);
}

BTEST(ssuspend, nested_run_can_suspend_with_a_continuation) {
	test_host.extcall = continuing_extcall;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	LIT(5);
	EMIT(ADD);
	EMIT(HALT);

	shibe_asm_align(sasm);
	nested_entry = shibe_asm_here(sasm);
	shibe_asm_label_t sub = shibe_asm_make_label(sasm);
	EMIT_LABEL(LIT, sub);
	EMIT(CALL);
	EMIT(HALT);

	shibe_asm_bind_label(sasm, sub);
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 2 }));
	EMIT(RET);

	// Call 1 named a continuation, so the suspension may cross its boundary
	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_SUSPENDED);
	BTEST_EXPECT_EQUAL("%d", num_extcalls, 2);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	BTEST_EXPECT(shibe_inspect(vm)->exec_state == SHIBE_EXEC_SUSPENDED);

	// The nested run finishes first, then call 1 is finished by call 3, and
	// only then does the run underneath carry on into its LIT 5; ADD
	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%d", num_continuations, 1);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	BTEST_EXPECT_EQUAL("%u", depth(), 1u);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 12);

	// The second half read back what the first half left in the frame
	BTEST_EXPECT_EQUAL("%d", observed_slot.i32, 42);

	// Everything was handed back on the way out
	BTEST_EXPECT_EQUAL("%u", shibe_inspect(vm)->asp.u32, 0u);
	BTEST_EXPECT(shibe_inspect(vm)->exec_state == SHIBE_EXEC_IDLE);
}

BTEST(ssuspend, a_continuation_that_names_no_successor_is_rejected) {
	test_host.extcall = forgetful_continuation_extcall;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	EMIT(HALT);

	shibe_asm_align(sasm);
	nested_entry = shibe_asm_here(sasm);
	shibe_asm_label_t sub = shibe_asm_make_label(sasm);
	EMIT_LABEL(LIT, sub);
	EMIT(CALL);
	EMIT(HALT);

	shibe_asm_bind_label(sasm, sub);
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 2 }));
	EMIT(RET);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_SUSPENDED);

	// Being called consumed the continuation, so stopping again without naming
	// one leaves a frame nothing can finish. Inheriting the value that reached
	// it would call the same second half over and over instead.
	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_ERROR);
	BTEST_EXPECT_EQUAL("%d", num_continuations, 1);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_NOT_SUSPENDABLE);
	// `arg` names the frame that named nothing, so a walker can say which call
	BTEST_EXPECT_EQUAL("%u", last_panic.arg.u32, shibe_inspect(vm)->fp.u32);
	BTEST_EXPECT(shibe_inspect(vm)->exec_state == SHIBE_EXEC_PANIC);
}

BTEST(ssuspend, a_continuation_can_re_enter_and_suspend) {
	test_host.extcall = reentering_continuation_extcall;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	LIT(5);
	EMIT(ADD);
	EMIT(HALT);

	shibe_asm_align(sasm);
	nested_entry = shibe_asm_here(sasm);
	shibe_asm_label_t sub = shibe_asm_make_label(sasm);
	EMIT_LABEL(LIT, sub);
	EMIT(CALL);
	EMIT(HALT);

	// Call 8 sits on the far side of the suspension, so it only runs if the run
	// that stopped there was actually carried on
	shibe_asm_bind_label(sasm, sub);
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 2 }));
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 8 }));
	EMIT(RET);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_SUSPENDED);
	BTEST_EXPECT_EQUAL("%d", num_deep_calls, 0);

	// The first run finishes, then the continuation starts a second one that
	// stops in the same place. It is relaying, so the next resume has to carry
	// that run on rather than call the continuation over again.
	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_SUSPENDED);
	BTEST_EXPECT_EQUAL("%d", num_continuations, 1);
	BTEST_EXPECT_EQUAL("%d", num_deep_calls, 1);

	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%d", num_continuations, 2);
	// Both runs reached the far side, so neither was dropped
	BTEST_EXPECT_EQUAL("%d", num_deep_calls, 2);

	// The frame grew inside the continuation rather than being replaced, so
	// what the first half put in it is still there
	BTEST_EXPECT_EQUAL("%d", observed_slot.i32, 11);
	BTEST_EXPECT_EQUAL("%d", observed_frame_head.i32, 22);

	BTEST_EXPECT_EQUAL("%u", depth(), 1u);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 12);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	BTEST_EXPECT_EQUAL("%u", shibe_inspect(vm)->asp.u32, 0u);
}

// A hook that opens a frame and then stops. At slot 0 the frame's record of
// where the run underneath carries on is the bundle itself, since `ip` is wound
// back to it for the length of the call.
static shibe_status_t
framed_suspending_hook(shibe_host_t* host, shibe_vm_t* hooked, const shibe_state_t* state, shibe_op_addr_t at) {
	(void)host; (void)state;
	++num_steps;
	if (at.bundle.u32 != suspend_at.bundle.u32 || at.slot != suspend_at.slot) {
		return SHIBE_OK;
	}

	shibe_frame_t frame = shibe_alloc_frame(hooked, 1);
	shibe_set_local(hooked, frame, 0, (shibe_cell_t){ .i32 = 55 });
	shibe_set_continuation(hooked, frame, (shibe_cell_t){ .u32 = 6 });
	return SHIBE_SUSPENDED;
}

// The second half of the hook call above
static shibe_status_t
hook_continuation_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host; (void)index;
	++num_continuations;
	observed_slot = shibe_get_local(called, shibe_get_frame(called), 0);
	return SHIBE_OK;
}

BTEST(ssuspend, a_hook_can_hold_a_frame_across_a_stop) {
	test_host.debug = framed_suspending_hook;
	test_host.extcall = hook_continuation_extcall;

	// The hook is handed the bundle's own address, so a frame it opens records
	// a point the run can come back to, exactly as an extcall's does
	shibe_cell_t bundle = shibe_asm_here(sasm);
	suspend_at = (shibe_op_addr_t){ .bundle = bundle, .slot = 0 };
	LIT(1);
	LIT(2);
	EMIT(ADD);
	EMIT(HALT);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_SUSPENDED);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	BTEST_EXPECT_EQUAL("%u", depth(), 0u);

	suspend_at = (shibe_op_addr_t){ 0 };

	// The hook's own second half runs first, then the bundle it stopped in
	// front of
	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%d", num_continuations, 1);
	BTEST_EXPECT_EQUAL("%d", observed_slot.i32, 55);
	BTEST_EXPECT_EQUAL("%u", depth(), 1u);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 3);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	BTEST_EXPECT_EQUAL("%u", shibe_inspect(vm)->asp.u32, 0u);
}

BTEST(ssuspend, a_continuation_can_suspend_again) {
	test_host.extcall = retrying_extcall;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	LIT(5);
	EMIT(ADD);
	EMIT(HALT);

	shibe_asm_align(sasm);
	nested_entry = shibe_asm_here(sasm);
	shibe_asm_label_t sub = shibe_asm_make_label(sasm);
	EMIT_LABEL(LIT, sub);
	EMIT(CALL);
	EMIT(HALT);

	shibe_asm_bind_label(sasm, sub);
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 2 }));
	EMIT(RET);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_SUSPENDED);

	// A second half that is not ready is asked again rather than dragging the
	// run under it along, so the frame has to still be there each time
	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_SUSPENDED);
	BTEST_EXPECT_EQUAL("%d", num_continuations, 1);
	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_SUSPENDED);
	BTEST_EXPECT_EQUAL("%d", num_continuations, 2);
	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%d", num_continuations, 3);

	BTEST_EXPECT_EQUAL("%u", depth(), 1u);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 12);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	BTEST_EXPECT_EQUAL("%u", shibe_inspect(vm)->asp.u32, 0u);
}

BTEST(ssuspend, a_leaf_call_can_suspend_and_be_finished_later) {
	test_host.extcall = deferring_extcall;

	// No re-entry anywhere: the call simply stops, and the rest of it runs
	// after the resume
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	LIT(5);
	EMIT(ADD);
	EMIT(HALT);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_SUSPENDED);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	BTEST_EXPECT_EQUAL("%u", depth(), 0u);

	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%d", num_continuations, 1);
	// The second half left what the interrupted EXTCALL had promised, so the
	// run underneath cannot tell that anything happened in between
	BTEST_EXPECT_EQUAL("%u", depth(), 1u);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 12);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	BTEST_EXPECT_EQUAL("%u", shibe_inspect(vm)->asp.u32, 0u);
}

BTEST(ssuspend, suspending_on_a_frame_with_no_continuation_is_rejected) {
	test_host.extcall = stranding_extcall;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	EMIT(HALT);

	// Nothing could ever take the frame back, so the suspension is refused even
	// though the run underneath is a top level one that could have carried on
	BTEST_EXPECT_EQUAL("%d", run(), SHIBE_ERROR);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_NOT_SUSPENDABLE);
	BTEST_EXPECT(shibe_inspect(vm)->exec_state == SHIBE_EXEC_PANIC);
}

BTEST(ssuspend, a_call_from_outside_the_vm_can_be_resumable) {
	test_host.extcall = top_level_continuation_extcall;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	EMIT(HALT);
	BTEST_ASSERT(shibe_asm_end(sasm));

	// Standing in for a host function the host called directly. It cannot tell
	// that from being called by the vm, and it does not have to: it takes a
	// frame and names its second half either way.
	shibe_frame_t frame = shibe_alloc_frame(vm, 1);
	shibe_set_local(vm, frame, 0, (shibe_cell_t){ .i32 = 77 });
	shibe_set_continuation(vm, frame, (shibe_cell_t){ .u32 = 5 });

	BTEST_ASSERT_EQUAL("%d", shibe_execute(vm, code), SHIBE_SUSPENDED);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);

	// The run it started finishes first, then its own second half runs, and
	// there is nothing underneath that one to carry on
	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%d", num_continuations, 1);
	BTEST_EXPECT_EQUAL("%d", observed_slot.i32, 77);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	BTEST_EXPECT(shibe_inspect(vm)->exec_state == SHIBE_EXEC_IDLE);
	BTEST_EXPECT_EQUAL("%u", shibe_inspect(vm)->asp.u32, 0u);
}

BTEST(ssuspend, a_swallowed_suspension_still_stops_the_run) {
	test_host.extcall = swallowing_extcall;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	LIT(5);
	EMIT(ADD);
	EMIT(HALT);

	shibe_asm_align(sasm);
	nested_entry = shibe_asm_here(sasm);
	shibe_asm_label_t sub = shibe_asm_make_label(sasm);
	EMIT_LABEL(LIT, sub);
	EMIT(CALL);
	EMIT(HALT);

	shibe_asm_bind_label(sasm, sub);
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 2 }));
	EMIT(RET);

	// The callback reported success on top of a vm that was already suspended.
	// Carrying on would abandon the run that stopped, so the vm's state wins,
	// the same way a panic raised underneath a callback does.
	BTEST_EXPECT_EQUAL("%d", run(), SHIBE_SUSPENDED);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	BTEST_EXPECT(shibe_inspect(vm)->exec_state == SHIBE_EXEC_SUSPENDED);
}

BTEST(ssuspend, a_run_that_halts_untidily_still_finds_its_host_frame) {
	test_host.extcall = untidy_extcall;

	// Nothing makes a top level run balance its frames before halting, and the
	// call underneath must not go unfinished because one was left standing
	shibe_asm_label_t frame_slots = shibe_asm_make_label(sasm);
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	shibe_asm_emit_imm_label(sasm, SHIBE_OP_ENTER, frame_slots);
	shibe_asm_bind_value(sasm, frame_slots, (shibe_cell_t){ .u32 = 0 });
	EMIT(HALT);
	BTEST_ASSERT(shibe_asm_end(sasm));

	shibe_frame_t frame = shibe_alloc_frame(vm, 0);
	shibe_set_continuation(vm, frame, (shibe_cell_t){ .u32 = 3 });

	BTEST_ASSERT_EQUAL("%d", shibe_execute(vm, code), SHIBE_SUSPENDED);

	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%d", num_continuations, 1);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	// The stray frame went with the run that left it
	BTEST_EXPECT_EQUAL("%u", shibe_inspect(vm)->asp.u32, 0u);
}

BTEST(ssuspend, nested_extcall_suspension_is_rejected) {
	test_host.extcall = nested_suspending_extcall;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	LIT(5);
	EMIT(ADD);
	EMIT(HALT);

	shibe_asm_align(sasm);
	nested_entry = shibe_asm_here(sasm);
	shibe_asm_label_t sub = shibe_asm_make_label(sasm);
	EMIT_LABEL(LIT, sub);
	EMIT(CALL);
	EMIT(HALT);

	// A leaf extcall, but the run it belongs to was re-entered, and there is no
	// way back into the host call frame underneath it
	shibe_asm_bind_label(sasm, sub);
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 2 }));
	EMIT(RET);

	BTEST_EXPECT_EQUAL("%d", run(), SHIBE_ERROR);
	BTEST_EXPECT_EQUAL("%d", num_extcalls, 2);
	// Raised once, at the boundary the suspension could not cross, and only
	// relayed by the level above it
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_NOT_SUSPENDABLE);
	BTEST_EXPECT(shibe_inspect(vm)->exec_state == SHIBE_EXEC_PANIC);
	// Ends the nest like any other panic, host frame left to walk
	BTEST_EXPECT_EQUAL("%u", shibe_inspect(vm)->asp.u32, 8u);

	// And there is nothing to come back to
	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_ERROR);
}

BTEST(ssuspend, nested_hook_suspension_is_rejected) {
	test_host.extcall = reentrant_extcall;
	test_host.debug = suspending_hook;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	EMIT(HALT);

	shibe_asm_align(sasm);
	nested_entry = shibe_asm_here(sasm);
	shibe_asm_label_t sub = shibe_asm_make_label(sasm);
	EMIT_LABEL(LIT, sub);
	EMIT(CALL);
	EMIT(HALT);

	// The hook is the other way to suspend, and it is refused in a nested run
	// for the same reason, wherever inside a bundle it stops
	shibe_asm_bind_label(sasm, sub);
	suspend_at = (shibe_op_addr_t){ .bundle = shibe_asm_here(sasm), .slot = 1 };
	LIT(7);
	EMIT(DUP);
	EMIT(ADD);
	EMIT(RET);

	BTEST_EXPECT_EQUAL("%d", run(), SHIBE_ERROR);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_NOT_SUSPENDABLE);
	BTEST_EXPECT(shibe_inspect(vm)->exec_state == SHIBE_EXEC_PANIC);
}

BTEST(ssuspend, host_call_can_suspend_after_its_nested_run_finished) {
	test_host.extcall = reenter_then_suspend_extcall;

	// What matters is which host call suspends, not whether it had re-entered
	// the vm before it did: the nested run is over by then, and the frame it
	// opened is what the continuation is called through
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	EMIT(ADD);
	EMIT(HALT);

	shibe_asm_align(sasm);
	nested_entry = shibe_asm_here(sasm);
	shibe_asm_label_t sub = shibe_asm_make_label(sasm);
	EMIT_LABEL(LIT, sub);
	EMIT(CALL);
	EMIT(HALT);

	shibe_asm_bind_label(sasm, sub);
	LIT(7);
	EMIT(RET);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_SUSPENDED);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	// The nested run gave its activation back, but the frame is the host call's
	// and that call has not finished
	BTEST_EXPECT_EQUAL("%u", depth(), 1u);

	// The continuation pushes the 5 the first half never got to, and only then
	// does the interrupted run carry on into its ADD
	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%d", num_extcalls, 2);
	BTEST_EXPECT_EQUAL("%u", depth(), 1u);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 12);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	// And the frame went with it
	BTEST_EXPECT_EQUAL("%u", shibe_inspect(vm)->asp.u32, 0u);
}
