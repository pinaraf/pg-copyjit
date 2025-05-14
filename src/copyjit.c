/*
 * Copy-patch JIT for PostgreSQL
 *
 * 2024, Pierre Ducroquet
 */
#include "postgres.h"
#include "fmgr.h"

#include "jit/jit.h"
#include "executor/execExpr.h"
#include "nodes/execnodes.h"
#include "utils/memutils.h"
#include "utils/resowner_private.h"
#include "utils/expandeddatum.h"
#include "utils/fmgrprotos.h"

#include <sys/mman.h>

void initialize_stencils();
void copyjit_reset_after_error(void);

#include "built-stencils.h"

PG_MODULE_MAGIC;

void _PG_init(void);
void _PG_fini(void);

#define DEBUG_GEN 1
#define SHOW_TIME 1

static const char *opcodeNames[] = {
	"EEOP_DONE",

	/* apply slot_getsomeattrs on corresponding tuple slot */
	"EEOP_INNER_FETCHSOME",
	"EEOP_OUTER_FETCHSOME",
	"EEOP_SCAN_FETCHSOME",

	/* compute non-system Var value */
	"EEOP_INNER_VAR",
	"EEOP_OUTER_VAR",
	"EEOP_SCAN_VAR",

	/* compute system Var value */
	"EEOP_INNER_SYSVAR",
	"EEOP_OUTER_SYSVAR",
	"EEOP_SCAN_SYSVAR",

	/* compute wholerow Var */
	"EEOP_WHOLEROW",

	/*
	 * Compute non-system Var value, assign it into ExprState's resultslot.
	 * These are not used if a CheckVarSlotCompatibility() check would be
	 * needed.
	 */
	"EEOP_ASSIGN_INNER_VAR",
	"EEOP_ASSIGN_OUTER_VAR",
	"EEOP_ASSIGN_SCAN_VAR",

	/* assign ExprState's resvalue/resnull to a column of its resultslot */
	"EEOP_ASSIGN_TMP",
	/* ditto", applying MakeExpandedObjectReadOnly() */
	"EEOP_ASSIGN_TMP_MAKE_RO",

	/* evaluate Const value */
	"EEOP_CONST",

	/*
	 * Evaluate function call (including OpExprs etc).  For speed", we
	 * distinguish in the opcode whether the function is strict and/or
	 * requires usage stats tracking.
	 */
	"EEOP_FUNCEXPR",
	"EEOP_FUNCEXPR_STRICT",
	"EEOP_FUNCEXPR_FUSAGE",
	"EEOP_FUNCEXPR_STRICT_FUSAGE",

	/*
	 * Evaluate boolean AND expression", one step per subexpression. FIRST/LAST
	 * subexpressions are special-cased for performance.  Since AND always has
	 * at least two subexpressions", FIRST and LAST never apply to the same
	 * subexpression.
	 */
	"EEOP_BOOL_AND_STEP_FIRST",
	"EEOP_BOOL_AND_STEP",
	"EEOP_BOOL_AND_STEP_LAST",

	/* similarly for boolean OR expression */
	"EEOP_BOOL_OR_STEP_FIRST",
	"EEOP_BOOL_OR_STEP",
	"EEOP_BOOL_OR_STEP_LAST",

	/* evaluate boolean NOT expression */
	"EEOP_BOOL_NOT_STEP",

	/* simplified version of BOOL_AND_STEP for use by ExecQual() */
	"EEOP_QUAL",

	/* unconditional jump to another step */
	"EEOP_JUMP",

	/* conditional jumps based on current result value */
	"EEOP_JUMP_IF_NULL",
	"EEOP_JUMP_IF_NOT_NULL",
	"EEOP_JUMP_IF_NOT_TRUE",

	/* perform NULL tests for scalar values */
	"EEOP_NULLTEST_ISNULL",
	"EEOP_NULLTEST_ISNOTNULL",

	/* perform NULL tests for row values */
	"EEOP_NULLTEST_ROWISNULL",
	"EEOP_NULLTEST_ROWISNOTNULL",

	/* evaluate a BooleanTest expression */
	"EEOP_BOOLTEST_IS_TRUE",
	"EEOP_BOOLTEST_IS_NOT_TRUE",
	"EEOP_BOOLTEST_IS_FALSE",
	"EEOP_BOOLTEST_IS_NOT_FALSE",

	/* evaluate PARAM_EXEC/EXTERN parameters */
	"EEOP_PARAM_EXEC",
	"EEOP_PARAM_EXTERN",
	"EEOP_PARAM_CALLBACK",

	/* return CaseTestExpr value */
	"EEOP_CASE_TESTVAL",

	/* apply MakeExpandedObjectReadOnly() to target value */
	"EEOP_MAKE_READONLY",

	/* evaluate assorted special-purpose expression types */
	"EEOP_IOCOERCE",
	"EEOP_DISTINCT",
	"EEOP_NOT_DISTINCT",
	"EEOP_NULLIF",
	"EEOP_SQLVALUEFUNCTION",
	"EEOP_CURRENTOFEXPR",
	"EEOP_NEXTVALUEEXPR",
	"EEOP_ARRAYEXPR",
	"EEOP_ARRAYCOERCE",
	"EEOP_ROW",

	/*
	 * Compare two individual elements of each of two compared ROW()
	 * expressions.  Skip to ROWCOMPARE_FINAL if elements are not equal.
	 */
	"EEOP_ROWCOMPARE_STEP",

	/* evaluate boolean value based on previous ROWCOMPARE_STEP operations */
	"EEOP_ROWCOMPARE_FINAL",

	/* evaluate GREATEST() or LEAST() */
	"EEOP_MINMAX",

	/* evaluate FieldSelect expression */
	"EEOP_FIELDSELECT",

	/*
	 * Deform tuple before evaluating new values for individual fields in a
	 * FieldStore expression.
	 */
	"EEOP_FIELDSTORE_DEFORM",

	/*
	 * Form the new tuple for a FieldStore expression.  Individual fields will
	 * have been evaluated into columns of the tuple deformed by the preceding
	 * DEFORM step.
	 */
	"EEOP_FIELDSTORE_FORM",

	/* Process container subscripts; possibly short-circuit result to NULL */
	"EEOP_SBSREF_SUBSCRIPTS",

	/*
	 * Compute old container element/slice when a SubscriptingRef assignment
	 * expression contains SubscriptingRef/FieldStore subexpressions. Value is
	 * accessed using the CaseTest mechanism.
	 */
	"EEOP_SBSREF_OLD",

	/* compute new value for SubscriptingRef assignment expression */
	"EEOP_SBSREF_ASSIGN",

	/* compute element/slice for SubscriptingRef fetch expression */
	"EEOP_SBSREF_FETCH",

	/* evaluate value for CoerceToDomainValue */
	"EEOP_DOMAIN_TESTVAL",

	/* evaluate a domain's NOT NULL constraint */
	"EEOP_DOMAIN_NOTNULL",

	/* evaluate a single domain CHECK constraint */
	"EEOP_DOMAIN_CHECK",

	/* evaluate assorted special-purpose expression types */
	"EEOP_CONVERT_ROWTYPE",
	"EEOP_SCALARARRAYOP",
	"EEOP_HASHED_SCALARARRAYOP",
	"EEOP_XMLEXPR",
	"EEOP_JSON_CONSTRUCTOR",
	"EEOP_IS_JSON",
	"EEOP_AGGREF",
	"EEOP_GROUPING_FUNC",
	"EEOP_WINDOW_FUNC",
	"EEOP_SUBPLAN",

	/* aggregation related nodes */
	"EEOP_AGG_STRICT_DESERIALIZE",
	"EEOP_AGG_DESERIALIZE",
	"EEOP_AGG_STRICT_INPUT_CHECK_ARGS",
	"EEOP_AGG_STRICT_INPUT_CHECK_NULLS",
	"EEOP_AGG_PLAIN_PERGROUP_NULLCHECK",
	"EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL",
	"EEOP_AGG_PLAIN_TRANS_STRICT_BYVAL",
	"EEOP_AGG_PLAIN_TRANS_BYVAL",
	"EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF",
	"EEOP_AGG_PLAIN_TRANS_STRICT_BYREF",
	"EEOP_AGG_PLAIN_TRANS_BYREF",
	"EEOP_AGG_PRESORTED_DISTINCT_SINGLE",
	"EEOP_AGG_PRESORTED_DISTINCT_MULTI",
	"EEOP_AGG_ORDERED_TRANS_DATUM",
	"EEOP_AGG_ORDERED_TRANS_TUPLE",

	/* non-existent operation, used e.g. to check array lengths */
	"EEOP_LAST"
};


typedef struct CodeGen {
	union Code {
		uint32_t *as_u32;
		void *as_void;
		unsigned char *as_char;
	} code;
	int code_size;
	int *offsets;
	int trampoline_count;	// count the number of initialized trampolines
	intptr_t *trampoline_targets;
} CodeGen;

void
copyjit_reset_after_error(void)
{
}

typedef struct CopyJitContext
{
	JitContext base;
	void *code;
	size_t code_size;
} CopyJitContext;

CopyJitContext *
copyjit_create_context(int jitFlags)
{
	CopyJitContext *context;

	ResourceOwnerEnlargeJIT(CurrentResourceOwner);

	context = MemoryContextAllocZero(TopMemoryContext,
									 sizeof(CopyJitContext));
	context->base.flags = jitFlags;

	/* ensure cleanup */
	context->base.resowner = CurrentResourceOwner;
	context->code = NULL;
	ResourceOwnerRememberJIT(CurrentResourceOwner, PointerGetDatum(context));

	return context;
}

void
copyjit_release_context(JitContext *context)
{
	CopyJitContext *copyjit_context = (CopyJitContext *) context;
	if (copyjit_context->code)
		munmap(copyjit_context->code, copyjit_context->code_size);
}


static Datum
ExecRunCompiledExpr(ExprState *state, ExprContext *econtext, bool *isNull)
{
	return ((ExprStateEvalFunc) state->evalfunc_private) (state, econtext, isNull);
}

#if defined(__aarch64__) || defined(_M_ARM64)

#define TRAMPOLINE_SIZE 16

static void build_aarch64_trampoline(uint32_t *code, intptr_t target)
{
	// Unlike x86, arm has a fixed instruction width.
	// When it switched to 64 bits, instead of killing code density and thus performance,
	// it stayed on 32 bits instruction width. This makes jumping to arbitrary address harder.
	// We create a small "trampoline", containing the following code:
	// ldr x8, 8
	// br x8
	// XXXX
	// YYYY
	// where XXXX and YYYY are the two 32-bits parts of the 64 bits target.
	// x8 is a scratch register, we are free to trash it.
	// Note that code is a uint32_t here to use this fixed-instruction property...
	code[0] = 0x58000048;
	code[1] = 0xD61F0100;
	code[2] = target & 0xffffffff;
	code[3] = target >> 32;
}

static void apply_arm64_x26 (CodeGen *codeGen, size_t u32offset, intptr_t target)
{
	uint32_t *beginning_trampoline_area = codeGen->code.as_u32 + codeGen->code_size / 4;
	int t;
	for (t = 0 ; t < codeGen->trampoline_count ; t++) {
		if (codeGen->trampoline_targets[t] == target)
			break;
	}
	intptr_t trampoline_address = beginning_trampoline_area + t * TRAMPOLINE_SIZE / 4;
	if (t == codeGen->trampoline_count) {
		t--;
		trampoline_address -= TRAMPOLINE_SIZE / 4;
		// The target has not yet been 'trampolined', let's do it
		build_aarch64_trampoline(trampoline_address, target);
		codeGen->trampoline_targets[t] = target;
		codeGen->trampoline_count++;
	}
	// Now we can code a 26bits delta using the offset between codeGen->code+u32offset and trampoline_address
	intptr_t current_address = &(codeGen->code.as_u32[u32offset]);
	int32_t delta = (trampoline_address - current_address) / 4;
	if (delta > (1 << 26) || delta < -(1 << 26))
		elog(WARNING, "Computed delta, %p, from %p to %p, is far too big", delta, current_address, trampoline_address);

	// Force instruction target bits to 0, for safety
	codeGen->code.as_u32[u32offset] &= 0xFC000000;
	// Now encode the delta in there
	codeGen->code.as_u32[u32offset] |= (delta & ~0xFC000000);
}

#elif defined(__x86_64__)

// No trampoline on amd64
#define TRAMPOLINE_SIZE 0

#else

#error "Unsupported CPU architecture. Please, please, please, contact me so we can work on it!"

#endif

static intptr_t get_patch_target(ExprState *state, CodeGen *codeGen, size_t next_offset, struct ExprEvalStep *op, const struct Patch *patch)
{
	intptr_t target;
	switch (patch->target) {
		case TARGET_CONST_ISNULL:
			target = op->d.constval.isnull;
			break;
		case TARGET_CONST_VALUE:
			target = op->d.constval.value;
			break;
		case TARGET_RESULTNUM:
			target = op->d.assign_tmp.resultnum;
			break;
		case TARGET_OP:
			target = (intptr_t) op;
			break;
		case TARGET_MakeExpandedObjectReadOnlyInternal:
			target = (intptr_t) &MakeExpandedObjectReadOnlyInternal;
			break;
		case TARGET_ExecEvalScalarArrayOp:
			target = (intptr_t) &ExecEvalScalarArrayOp;
			break;
		case TARGET_ExecEvalSysVar:
			target = (intptr_t) &ExecEvalSysVar;
			break;
		case TARGET_ExecEvalSQLValueFunction:
			target = (intptr_t) &ExecEvalSQLValueFunction;
			break;
		case TARGET_ExecEvalParamExec:
			target = (intptr_t) &ExecEvalParamExec;
			break;
		case TARGET_ExecEvalParamExtern:
			target = (intptr_t) &ExecEvalParamExtern;
			break;
		case TARGET_slot_getsomeattrs_int:
			target = (intptr_t) &slot_getsomeattrs_int;
			break;
		case TARGET_FORCE_NEXT_CALL:
		case TARGET_NEXT_CALL:
			target = (intptr_t) codeGen->code.as_void + next_offset;
			break;
		case TARGET_JUMP_DONE:
			target = (intptr_t) codeGen->code.as_void + codeGen->offsets[op->d.qualexpr.jumpdone];
			break;
		case TARGET_JUMP_NULL:
			if (op->opcode == EEOP_AGG_PLAIN_PERGROUP_NULLCHECK)
				target = (intptr_t) codeGen->code.as_void + codeGen->offsets[op->d.agg_plain_pergroup_nullcheck.jumpnull];
			else if (op->opcode == EEOP_AGG_STRICT_INPUT_CHECK_ARGS)
				target = (intptr_t) codeGen->code.as_void + codeGen->offsets[op->d.agg_strict_input_check.jumpnull];
			else
				elog(ERROR, "Unsupported target TARGET_JUMP_NULL in opcode %s", opcodeNames[op->opcode]);
			break;
		case TARGET_RESULTSLOT_VALUES:
			if (op->opcode == EEOP_ASSIGN_TMP || op->opcode == EEOP_ASSIGN_TMP_MAKE_RO)
				target = (intptr_t) &(state->resultslot->tts_values[op->d.assign_tmp.resultnum]);
			else if (op->opcode == EEOP_ASSIGN_SCAN_VAR || op->opcode == EEOP_ASSIGN_INNER_VAR || op->opcode == EEOP_ASSIGN_OUTER_VAR)
				target = (intptr_t) &(state->resultslot->tts_values[op->d.assign_var.resultnum]);
			else
				elog(ERROR, "Unsupported target TARGET_RESULTSLOT_VALUES in opcode %s", opcodeNames[op->opcode]);
			break;
		case TARGET_RESULTSLOT_ISNULL:
			if (op->opcode == EEOP_ASSIGN_TMP || op->opcode == EEOP_ASSIGN_TMP_MAKE_RO)
				target = (intptr_t) &(state->resultslot->tts_isnull[op->d.assign_tmp.resultnum]);
			else if (op->opcode == EEOP_ASSIGN_SCAN_VAR || op->opcode == EEOP_ASSIGN_INNER_VAR || op->opcode == EEOP_ASSIGN_OUTER_VAR)
				target = (intptr_t) &(state->resultslot->tts_isnull[op->d.assign_var.resultnum]);
			else
				elog(ERROR, "Unsupported target TARGET_RESULTSLOT_ISNULL in opcode %s", opcodeNames[op->opcode]);
			break;
		case TARGET_FUNC_CALL:
			target = (intptr_t) op->d.func.fn_addr;
			break;
		case TARGET_FUNC_NARGS:
			target = (intptr_t) op->d.func.nargs;
			break;
		case TARGET_ATTNUM:
			if (op->opcode == EEOP_ASSIGN_SCAN_VAR || op->opcode == EEOP_ASSIGN_INNER_VAR || op->opcode == EEOP_ASSIGN_OUTER_VAR)
				target = op->d.assign_var.attnum;
			else if (op->opcode == EEOP_SCAN_VAR)
				target = op->d.var.attnum;
			else
				elog(ERROR, "Unsupported target TARGET_ATTNUM in opcode %s", opcodeNames[op->opcode]);
			break;
		case TARGET_CurrentMemoryContext:
			target = (intptr_t) &CurrentMemoryContext;
			break;
		default:
			elog(ERROR, "Unsupported target");
			break;
	};
	return target;
}

static void apply_jump(CodeGen *codeGen, size_t offset, intptr_t target, const struct Patch *patch)
{
	// Note: this is amd64 only
	// A LOT OF FUN !
	// target is an address we need to jump to. we are playing with code with IP = offset+patch->offset
	if (DEBUG_GEN)
		elog(WARNING, "Asked to jump to %p, we are patching at %p", target, (intptr_t) codeGen->code.as_void + offset + patch->offset);
	int64_t relative_jump = target - ((intptr_t) codeGen->code.as_void + offset + patch->offset);
	// Note : one could build short jumps, but not sure it's worth the effort
	// I assert we have no insane jump, but... meh, should implement a check
	relative_jump -= 5;	// remove size of jump from offset
	int32_t near_jump = (int32_t) relative_jump;
	codeGen->code.as_char[offset + patch->offset] = 0xE9;
	memcpy(codeGen->code.as_void + offset + patch->offset + 1, &near_jump, 4);
}

static void apply_patch_with_target (CodeGen *codeGen, size_t offset, intptr_t target, const struct Patch *patch)
{
#if defined(__aarch64__) || defined(_M_ARM64)
	size_t u32offset = (offset + patch->offset) / 4;
	uint32_t value;
#endif
	target += patch->addend;
	if (DEBUG_GEN)
		elog(WARNING, "Applying a patch at offset %i+%i, target %p, kind %i", offset, patch->offset, target, patch->relkind);
	switch (patch->relkind) {
#if defined(__x86_64__)
		case RELKIND_R_X86_64_64:
			memcpy(codeGen->code.as_void + offset + patch->offset, &target, 8);
			break;
		case RELKIND_REJUMP: // Reminder: this is an artificial one we created
			apply_jump(codeGen, offset, target, patch);
			break;
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
		case RELKIND_R_AARCH64_MOVW_UABS_G0_NC:
			value = target & 0xFFFF;
			if (DEBUG_GEN)
				elog(WARNING, "Patching 0x%08x with value %p (moved to %p)", codeGen->code.as_u32[u32offset], value, value << 5);
			codeGen->code.as_u32[u32offset] = (codeGen->code.as_u32[u32offset] | (value << 5));
			if (DEBUG_GEN)
				elog(WARNING, "Patched 0x%08x with value %p (moved to %p)", codeGen->code.as_u32[u32offset], value, value << 5);
			break;
		case RELKIND_R_AARCH64_MOVW_UABS_G1_NC:
			value = (target & 0xFFFF0000) >> 16;
			if (DEBUG_GEN)
				elog(WARNING, "Patching 0x%08x with value %p (moved to %p)", codeGen->code.as_u32[u32offset], value, value << 5);
			codeGen->code.as_u32[u32offset] = (codeGen->code.as_u32[u32offset] | (value << 5));
			if (DEBUG_GEN)
				elog(WARNING, "Patched 0x%08x with value %p (moved to %p)", codeGen->code.as_u32[u32offset], value, value << 5);
			break;
		case RELKIND_R_AARCH64_MOVW_UABS_G2_NC:
			value = (target & 0xFFFF00000000) >> 32;
			if (DEBUG_GEN)
				elog(WARNING, "Patching 0x%08x with value %p (moved to %p)", codeGen->code.as_u32[u32offset], value, value << 5);
			codeGen->code.as_u32[u32offset] = (codeGen->code.as_u32[u32offset] | (value << 5));
			if (DEBUG_GEN)
				elog(WARNING, "Patched 0x%08x with value %p (moved to %p)", codeGen->code.as_u32[u32offset], value, value << 5);
			break;
		case RELKIND_R_AARCH64_MOVW_UABS_G3:
			value = (target & 0xFFFF000000000000) >> 48;
			if (DEBUG_GEN)
				elog(WARNING, "Patching 0x%08x with value %p (moved to %p)", codeGen->code.as_u32[u32offset], value, value << 5);
			codeGen->code.as_u32[u32offset] = (codeGen->code.as_u32[u32offset] | (value << 5));
			if (DEBUG_GEN)
				elog(WARNING, "Patched 0x%08x with value %p (moved to %p)", codeGen->code.as_u32[u32offset], value, value << 5);
			break;
		// These two require trampolines
		case RELKIND_R_AARCH64_JUMP26:
		case RELKIND_R_AARCH64_CALL26:
			apply_arm64_x26(codeGen, u32offset, target);
			break;
#endif
		default:
			elog(ERROR, "Unsupported relkind");
			break;
	}
}

static void apply_patch (ExprState *state, CodeGen *codeGen, size_t offset, size_t next_offset, struct ExprEvalStep *op, const struct Patch *patch)
{
	intptr_t target = get_patch_target(state, codeGen, next_offset, op, patch);

	apply_patch_with_target(codeGen, offset, target, patch);
}

static size_t apply_stencil (struct Stencil *stencil, ExprState *state, CodeGen *codeGen, size_t offset, size_t next_offset, struct ExprEvalStep *op)
{
	memcpy(codeGen->code.as_void + offset, stencil->code, stencil->code_size);
	for (int p = 0 ; p < stencil->patch_size ; p++) {
		const struct Patch *patch = &stencil->patches[p];
		apply_patch(state, codeGen, offset, next_offset, op, patch);
	}
	return stencil->code_size;
}

bool
copyjit_compile_expr(ExprState *state)
{
	CopyJitContext *context = NULL;
	instr_time	starttime;
	instr_time	endtime;
	bool canbuild = true;
	int required_trampolines = 0;
	size_t neededsize = 0;
	size_t offset = 0;

	CodeGen codeGen;
	memset(&codeGen, 0, sizeof(codeGen));

	int mprotect_res;

	PlanState  *parent = state->parent;
	Assert(parent);
	/* get or create JIT context */
	if (parent->state->es_jit)
		context = (CopyJitContext *) parent->state->es_jit;
	else
	{
		context = copyjit_create_context(parent->state->es_jit_flags);
		parent->state->es_jit = &context->base;
	}

	INSTR_TIME_SET_CURRENT(starttime);

	// This offset array is usefull later when jumps appear...
	codeGen.offsets = malloc(sizeof(int) * state->steps_len);
	for (int opno = 0; opno < state->steps_len; opno++)
	{
		struct ExprEvalStep *op = &state->steps[opno];
		ExprEvalOp opcode = op->opcode;
		if (DEBUG_GEN)
			elog(WARNING, "Need to build an %s - %i opcode at %p", opcodeNames[opcode], opcode, op);

		codeGen.offsets[opno] = neededsize;

		if (opcode == EEOP_FUNCEXPR_STRICT && op->d.func.fn_addr == &int4eq) {
			if (DEBUG_GEN)
				elog(WARNING, "Found a call to int4eq, inlining the hard way!");
			neededsize += extra_EEOP_FUNCEXPR_STRICT_int4eq.code_size;
		} else if (opcode == EEOP_FUNCEXPR_STRICT && op->d.func.fn_addr == &int4lt) {
			if (DEBUG_GEN)
				elog(WARNING, "Found a call to int4lt, inlining the hard way!");
			neededsize += extra_EEOP_FUNCEXPR_STRICT_int4lt.code_size;
		} else if (opcode == EEOP_FUNCEXPR_STRICT) {
			neededsize += stencils[EEOP_FUNCEXPR].code_size + op->d.func.nargs * extra_EEOP_FUNCEXPR_STRICT_CHECKER.code_size;
		} else if (opcode == EEOP_CONST) {
			if (DEBUG_GEN)
				elog(WARNING, "Replacing EEOP_CONST with null/nonnull eeop_const");
			if (op->d.constval.isnull)
				neededsize += extra_EEOP_CONST_NULL.code_size;
			else
				neededsize += extra_EEOP_CONST_NOTNULL.code_size;
		} else if (stencils[opcode].code_size == -1) {
			elog(WARNING, "UNSUPPORTED OPCODE %s", opcodeNames[opcode]);
			canbuild = false;
		} else {
			neededsize += stencils[opcode].code_size;
			if (TRAMPOLINE_SIZE) {
				// Check for patches that require trampolines to be built
				for (int p = 0 ; p < stencils[opcode].patch_size ; p++) {
					if (stencils[opcode].patches[p].relkind == RELKIND_R_AARCH64_CALL26)
						required_trampolines++;
				}
			}
		}
	}

	// All opcodes are accounted for, we can proceed
	if (canbuild) {
		// Initialize the various codeGen fields
		codeGen.code_size = neededsize;
		// We will need required_trampolines * TRAMPOLINE_SIZE of memory, appended at the end of the code
		codeGen.code.as_void = mmap(0, neededsize + required_trampolines * TRAMPOLINE_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
		if (TRAMPOLINE_SIZE) {
			codeGen.trampoline_count = 0;
			codeGen.trampoline_targets = malloc(sizeof(void*) *required_trampolines);
			memset(codeGen.trampoline_targets, 0, TRAMPOLINE_SIZE * required_trampolines);
		}
		context->code = codeGen.code.as_void;
		context->code_size = neededsize + required_trampolines * TRAMPOLINE_SIZE;

		for (int opno = 0 ; opno < state->steps_len ; opno++)
		{
			struct ExprEvalStep *op = &state->steps[opno];
			ExprEvalOp opcode = ExecEvalStepOp(state, op);
			size_t next_offset = codeGen.offsets[opno+1];
			if (DEBUG_GEN)
				elog(WARNING, "Adding stencil for %s, op address is %p", opcodeNames[opcode], op);

			if (opcode == EEOP_FUNCEXPR_STRICT && op->d.func.fn_addr == &int4eq) {
				offset += apply_stencil(&extra_EEOP_FUNCEXPR_STRICT_int4eq, state, &codeGen, offset, next_offset, op);
			} else if (opcode == EEOP_FUNCEXPR_STRICT && op->d.func.fn_addr == &int4lt) {
				offset += apply_stencil(&extra_EEOP_FUNCEXPR_STRICT_int4lt, state, &codeGen, offset, next_offset, op);
			} else if (opcode == EEOP_FUNCEXPR_STRICT) {
				// Prepend {op->d.func.nargs} extra_EEOP_FUNCEXPR_STRICT_CHECKER stencils before falling back on a FUNCEXPR
				for (int narg = 0 ; narg < op->d.func.nargs ; narg++) {
					memcpy(codeGen.code.as_void + offset, extra_EEOP_FUNCEXPR_STRICT_CHECKER.code, extra_EEOP_FUNCEXPR_STRICT_CHECKER.code_size);
					for (int p = 0 ; p < extra_EEOP_FUNCEXPR_STRICT_CHECKER.patch_size ; p++) {
						const struct Patch *patch = &extra_EEOP_FUNCEXPR_STRICT_CHECKER.patches[p];
						if (patch->target == TARGET_FUNC_ARG) {
							NullableDatum *func_arg = &(op->d.func.fcinfo_data->args[narg]);
							apply_patch_with_target(&codeGen, offset, (intptr_t) func_arg, patch);
						} else {
							apply_patch(state, &codeGen, offset, next_offset, op, patch);
						}
					}
					offset += extra_EEOP_FUNCEXPR_STRICT_CHECKER.code_size;
				}
				// Now we can land back on normal func call
				offset += apply_stencil(&stencils[EEOP_FUNCEXPR], state, &codeGen, offset, next_offset, op);
			} else if (opcode == EEOP_CONST) {
				if (DEBUG_GEN)
					elog(WARNING, "Replacing EEOP_CONST with null/nonnull eeop_const");
				if (op->d.constval.isnull)
					offset += apply_stencil(&extra_EEOP_CONST_NULL, state, &codeGen, offset, next_offset, op);
				else
					offset += apply_stencil(&extra_EEOP_CONST_NOTNULL, state, &codeGen, offset, next_offset, op);
			} else {
				offset += apply_stencil(&stencils[opcode], state, &codeGen, offset, next_offset, op);
			}
		}
		mprotect_res = mprotect(codeGen.code.as_void, neededsize, PROT_EXEC);
		if (DEBUG_GEN)
			elog(WARNING, "Result of mprotect is %i", mprotect_res);
		state->evalfunc_private = codeGen.code.as_void;
//		state->evalfunc = (ExprStateEvalFunc) codeGen.code.as_void; // We jump through ExecRunCompiledExpr so we can breakpoint, if needed...
		state->evalfunc = ExecRunCompiledExpr;
		if (DEBUG_GEN)
			elog(WARNING, "Code generated is located at %p for %i bytes (with %i trampolines)", codeGen.code.as_void, codeGen.code_size, required_trampolines);
	}
	free(codeGen.offsets);
	if (codeGen.trampoline_targets)
		free(codeGen.trampoline_targets);

	INSTR_TIME_SET_CURRENT(endtime);
	INSTR_TIME_SET_ZERO(context->base.instr.generation_counter);
	INSTR_TIME_ACCUM_DIFF(context->base.instr.generation_counter,
						  endtime, starttime);

	if (DEBUG_GEN || SHOW_TIME)
		elog(WARNING, "Total JIT duration is %lius", INSTR_TIME_GET_MICROSEC(context->base.instr.generation_counter));
	return canbuild;
}

/*
 * Initialize copy-and-patch JIT provider.
 */
void
_PG_jit_provider_init(JitProviderCallbacks *cb)
{
	cb->reset_after_error = copyjit_reset_after_error;
	cb->release_context = copyjit_release_context;
	cb->compile_expr = copyjit_compile_expr;
}

void
_PG_init(void)
{
	initialize_stencils();
}

void
_PG_fini(void)
{
}

