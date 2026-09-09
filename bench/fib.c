// Recursive fibonacci in shibe bytecode, to measure dispatch overhead.
//
// The word is written without an auxiliary frame: the argument and the result
// ride the data stack and CALL/RET carry the return address, so what is timed
// is the interpreter loop rather than frame setup. Every invocation runs a
// handful of opcodes and at most one branch, which makes the run almost pure
// dispatch.
//
// bench/fib.lua and bench/fib.py compute the same thing with the same call
// count, for comparison against a mature interpreter.

#if !defined(_WIN32)
#	define _POSIX_C_SOURCE 199309L
#endif

#include <barena.h>
#include <shibe.h>
#include <shibe/alloc.h>
#include <shibe/asm.h>
#include <shibe/opcode.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#	define WIN32_LEAN_AND_MEAN
#	include <windows.h>
#else
#	include <time.h>
#endif

// fib(47) is the last one that fits in a cell
#define FIB_MAX_N 47
// The word below assembles to 17 cells; the rest is slack for editing it
#define CODE_LEN 64u
#define DS_LEN 1024u
#define AS_LEN 1024u

static shibe_panic_t last_panic;
static int num_panics;
static uint64_t num_ops;

static void
report_panic(shibe_host_t* host, shibe_vm_t* vm, const shibe_panic_t* panic) {
	(void)host;
	(void)vm;
	last_panic = *panic;
	++num_panics;
}

// Attached only for the counting pass: it puts the interpreter on its hooked
// dispatch loop, so a run with it on says nothing about speed
static shibe_status_t
count_op(shibe_host_t* host, shibe_vm_t* vm, const shibe_state_t* state, shibe_op_addr_t at) {
	(void)host;
	(void)vm;
	(void)state;
	(void)at;
	++num_ops;
	return SHIBE_OK;
}

static shibe_host_t bench_host = {
	.panic = report_panic,
};

static double
now(void) {
#if defined(_WIN32)
	LARGE_INTEGER freq, counter;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&counter);
	return (double)counter.QuadPart / (double)freq.QuadPart;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

static uint32_t
fib_ref(int n) {
	uint32_t a = 0, b = 1;
	for (int i = 0; i < n; ++i) {
		uint32_t next = a + b;
		a = b;
		b = next;
	}
	return a;
}

// How many times the recursive definition enters fib:
// calls(0) = calls(1) = 1, calls(n) = 1 + calls(n - 1) + calls(n - 2)
static uint64_t
fib_calls(int n) {
	uint64_t prev = 1, cur = 1;
	for (int i = 2; i <= n; ++i) {
		uint64_t next = 1 + cur + prev;
		prev = cur;
		cur = next;
	}
	return n <= 1 ? 1 : cur;
}

#define EMIT(OPCODE)          shibe_asm_emit(sasm, SHIBE_OP_ ## OPCODE)
#define EMIT_IMM(OPCODE, V)   shibe_asm_emit_imm(sasm, SHIBE_OP_ ## OPCODE, (V))
#define EMIT_LABEL(OPCODE, L) shibe_asm_emit_imm_label(sasm, SHIBE_OP_ ## OPCODE, (L))
#define LIT(V)                EMIT_IMM(LIT, ((shibe_cell_t){ .i32 = (V) }))

// Lays down `fib(n)` followed by HALT, leaving the result on the data stack
static bool
assemble_fib(shibe_vm_t* vm, shibe_allocator_t* allocator, shibe_cell_t code, int n) {
	shibe_asm_t* sasm = shibe_asm_begin(vm, allocator, code);
	if (sasm == NULL) { return false; }

	shibe_asm_label_t fib = shibe_asm_make_label(sasm);
	shibe_asm_label_t recurse = shibe_asm_make_label(sasm);

	// entry: ( -- fib(n) )
	LIT(n);
	EMIT_LABEL(LIT, fib);
	EMIT(CALL);
	// CALL ends its bundle, so the return lands on this HALT
	EMIT(HALT);

	// fib: ( n -- fib(n) )
	shibe_asm_bind_label(sasm, fib);
	EMIT(DUP);                    // n n
	LIT(2);
	EMIT(SLT);                    // n n<2
	EMIT_LABEL(JZ, recurse);
	// n < 2, so n is already the answer
	EMIT(RET);

	shibe_asm_bind_label(sasm, recurse);
	EMIT(DUP);                    // n n
	LIT(1);
	EMIT(SUB);                    // n n-1
	EMIT_LABEL(LIT, fib);
	EMIT(CALL);                   // n fib(n-1)
	EMIT(SWP);                    // fib(n-1) n
	LIT(2);
	EMIT(SUB);                    // fib(n-1) n-2
	EMIT_LABEL(LIT, fib);
	EMIT(CALL);                   // fib(n-1) fib(n-2)
	EMIT(ADD);
	EMIT(RET);

	return shibe_asm_end(sasm);
}

// Runs the assembled code and takes the result off the data stack
static bool
run_fib(shibe_vm_t* vm, shibe_cell_t code, uint32_t* out) {
	if (shibe_execute(vm, code) != SHIBE_OK) { return false; }
	*out = shibe_pop(vm).u32;
	return num_panics == 0;
}

static void
usage(FILE* file, const char* prog) {
	fprintf(file, "usage: %s [n] [repeat] [--ops]\n", prog);
	fprintf(file, "\n");
	fprintf(file, "  n       fibonacci index, 0 to %d (default 30)\n", FIB_MAX_N);
	fprintf(file, "  repeat  timed runs to take the best of (default 3)\n");
	fprintf(file, "  --ops   count the opcodes one run dispatches, for ns/op\n");
	fprintf(file, "\n");
	fprintf(file, "bench/fib.lua and bench/fib.py take the same [n] [repeat].\n");
}

int
main(int argc, const char* argv[]) {
	int n = 30;
	int repeat = 3;
	bool with_ops = false;
	int positional = 0;

	for (int i = 1; i < argc; ++i) {
		const char* arg = argv[i];
		if (strcmp(arg, "--ops") == 0) {
			with_ops = true;
		} else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
			usage(stdout, argv[0]);
			return 0;
		} else if (arg[0] == '-') {
			fprintf(stderr, "unknown option: %s\n", arg);
			usage(stderr, argv[0]);
			return 1;
		} else if (positional == 0) {
			n = atoi(arg);
			++positional;
		} else if (positional == 1) {
			repeat = atoi(arg);
			++positional;
		} else {
			fprintf(stderr, "too many arguments\n");
			usage(stderr, argv[0]);
			return 1;
		}
	}

	if (n < 0 || n > FIB_MAX_N) {
		fprintf(stderr, "n must be between 0 and %d, got %d\n", FIB_MAX_N, n);
		return 1;
	}
	if (repeat < 1) {
		fprintf(stderr, "repeat must be at least 1, got %d\n", repeat);
		return 1;
	}

	barena_pool_t pool;
	barena_pool_init(&pool, 1);

	shibe_barena_t vm_alloc;
	shibe_vm_t* vm = shibe_create((shibe_config_t){
		.ds_len = DS_LEN,
		.as_len = AS_LEN,
		.allocator = shibe_barena_init(&vm_alloc, &pool),
		.host = &bench_host,
	});
	if (vm == NULL) {
		fprintf(stderr, "could not create the vm\n");
		barena_pool_cleanup(&pool);
		return 1;
	}

	// The assembler needs an allocator of its own: shibe_asm_end restores it
	shibe_barena_t asm_alloc;
	shibe_allocator_t* asm_allocator = shibe_barena_init(&asm_alloc, &pool);

	int exit_code = 1;
	uint32_t expected = fib_ref(n);
	uint64_t calls = fib_calls(n);
	double best = 0.0;

	shibe_cell_t code = shibe_alloc(vm, SHIBE_MEM_REGION_1, (shibe_cell_t){ .u32 = CODE_LEN });
	if (num_panics != 0) {
		fprintf(stderr, "could not allocate code memory\n");
		goto end;
	}

	if (!assemble_fib(vm, asm_allocator, code, n)) {
		fprintf(stderr, "could not assemble fib\n");
		goto end;
	}

	for (int i = 0; i < repeat; ++i) {
		uint32_t result = 0;
		double start = now();
		bool ok = run_fib(vm, code, &result);
		double elapsed = now() - start;

		if (!ok) {
			fprintf(
				stderr,
				"run %d panicked: error %d, arg %" PRIu32 "\n",
				i, (int)last_panic.error, last_panic.arg.u32
			);
			goto end;
		}
		if (result != expected) {
			fprintf(
				stderr,
				"run %d returned %" PRIu32 ", expected %" PRIu32 "\n",
				i, result, expected
			);
			goto end;
		}

		if (i == 0 || elapsed < best) { best = elapsed; }
	}

	printf("shibe    fib(%d) = %" PRIu32 "\n", n, expected);
	printf("  runs       %d\n", repeat);
	printf("  best       %.4f s\n", best);
	printf("  calls      %" PRIu64 "\n", calls);
	if (best > 0.0) {
		printf("  ns/call    %.2f\n", best * 1e9 / (double)calls);
		printf("  calls/s    %.2f M\n", (double)calls / best / 1e6);
	}

	if (with_ops) {
		uint32_t result = 0;
		num_ops = 0;
		bench_host.debug = count_op;
		bool ok = run_fib(vm, code, &result);
		bench_host.debug = NULL;
		if (!ok) {
			fprintf(stderr, "the counting run failed\n");
			goto end;
		}

		printf("  ops        %" PRIu64 "\n", num_ops);
		if (best > 0.0) {
			printf("  ns/op      %.2f\n", best * 1e9 / (double)num_ops);
			printf("  ops/s      %.2f M\n", (double)num_ops / best / 1e6);
		}
	}

	exit_code = 0;
end:
	shibe_destroy(vm);
	barena_pool_cleanup(&pool);
	return exit_code;
}
