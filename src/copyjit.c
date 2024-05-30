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

#define DEBUG_GEN 0
#define USE_EXTRA 1

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

static intptr_t get_patch_target(ExprState *state, unsigned char *builtcode, int *offsets, size_t next_offset, struct ExprEvalStep *op, const struct Patch *patch)
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
			target = (intptr_t) builtcode + next_offset;
			break;
		case TARGET_JUMP_DONE:
			target = (intptr_t) builtcode + offsets[op->d.qualexpr.jumpdone];
			break;
		case TARGET_JUMP_NULL:
			if (op->opcode == EEOP_AGG_PLAIN_PERGROUP_NULLCHECK)
				target = (intptr_t) builtcode + offsets[op->d.agg_plain_pergroup_nullcheck.jumpnull];
			else if (op->opcode == EEOP_AGG_STRICT_INPUT_CHECK_ARGS)
				target = (intptr_t) builtcode + offsets[op->d.agg_strict_input_check.jumpnull];
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

static void apply_jump(unsigned char *builtcode, size_t offset, intptr_t target, const struct Patch *patch)
{
	// A LOT OF FUN !
	// target is an adress we need to jump to. we are playing with code with IP = offset+patch->offset
	if (DEBUG_GEN)
		elog(WARNING, "Asked to jump to %p, we are patching at %p", target, (intptr_t) builtcode + offset + patch->offset);
	int64_t relative_jump = target - ((intptr_t) builtcode + offset + patch->offset);
	// Note : one could build short jumps, but not sure it's worth the effort
	// I assert we have no insane jump, but... meh, should implement a check
	relative_jump -= 5;	// remove size of jump from offset
	int32_t near_jump = (int32_t) relative_jump;
	builtcode[offset + patch->offset] = 0xE9;
	memcpy(builtcode + offset + patch->offset + 1, &near_jump, 4);
}

static void apply_patch_with_target (unsigned char *builtcode, size_t offset, intptr_t target, const struct Patch *patch)
{
	switch (patch->relkind) {
		case RELKIND_R_X86_64_64:
			if (DEBUG_GEN) {
				elog(WARNING, "Patching %p at offset %02lu with relkind amd64", (void*) target, patch->offset);
				elog(WARNING, "builtcode at %p, offset is %02lu, patch offset is %02lu", (void *) builtcode, offset, patch->offset);
			}
			//targetLocation = (uintptr_t*) builtcode[offset + patch->offset];
			//*targetLocation = target;
			/*
			builtcode[offset + patch->offset + 7] = (target & 0xFF00000000000000) >> 56;
			builtcode[offset + patch->offset + 6] = (target & 0x00FF000000000000) >> 48;
			builtcode[offset + patch->offset + 5] = (target & 0x0000FF0000000000) >> 40;
			builtcode[offset + patch->offset + 4] = (target & 0x000000FF00000000) >> 32;
			builtcode[offset + patch->offset + 3] = (target & 0x00000000FF000000) >> 24;
			builtcode[offset + patch->offset + 2] = (target & 0x0000000000FF0000) >> 16;
			builtcode[offset + patch->offset + 1] = (target & 0x000000000000FF00) >> 8;
			builtcode[offset + patch->offset + 0] = (target & 0x00000000000000FF) >> 0;
			*/
			memcpy(builtcode + offset + patch->offset, &target, 8);
			break;
		case RELKIND_REJUMP:
			apply_jump(builtcode, offset, target, patch);
			break;
		default:
			elog(ERROR, "Unsupported relkind");
			break;
	}
}

static void apply_patch (ExprState *state, unsigned char *builtcode, int *offsets, size_t offset, size_t next_offset, struct ExprEvalStep *op, const struct Patch *patch)
{
	intptr_t target = get_patch_target(state, builtcode, offsets, next_offset, op, patch);

	apply_patch_with_target(builtcode, offset, target, patch);
}

static size_t apply_stencil (struct Stencil *stencil, ExprState *state, unsigned char *builtcode, int *offsets, size_t offset, size_t next_offset, struct ExprEvalStep *op)
{
	memcpy(builtcode + offset, stencil->code, stencil->code_size);
	for (int p = 0 ; p < stencil->patch_size ; p++) {
		const struct Patch *patch = &stencil->patches[p];
		apply_patch(state, builtcode, offsets, offset, next_offset, op, patch);
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
	size_t neededsize = 0;
	unsigned char *builtcode;
	size_t offset = 0;
	int mprotect_res;
	int *offsets;

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
	offsets = malloc(sizeof(int) * state->steps_len);
	for (int opno = 0; opno < state->steps_len; opno++)
	{
		struct ExprEvalStep *op = &state->steps[opno];
		ExprEvalOp opcode = op->opcode;
		if (DEBUG_GEN)
			elog(WARNING, "Need to build an %s - %i opcode at %p", opcodeNames[opcode], opcode, op);

		offsets[opno] = neededsize;
#if USE_EXTRA
		if (opcode == EEOP_FUNCEXPR_STRICT) {
			neededsize += stencils[EEOP_FUNCEXPR].code_size + op->d.func.nargs * extra_EEOP_FUNCEXPR_STRICT_CHECKER.code_size;
		} else
#endif
		if (opcode == EEOP_FUNCEXPR_STRICT && op->d.func.fn_addr == &int4eq) {
			elog(WARNING, "Found a call to int4eq, inlining the hard way!");
			neededsize += extra_EEOP_FUNCEXPR_STRICT_int4eq.code_size;
		} else if (opcode == EEOP_FUNCEXPR_STRICT && op->d.func.fn_addr == &int4lt) {
			elog(WARNING, "Found a call to int4lt, inlining the hard way!");
			neededsize += extra_EEOP_FUNCEXPR_STRICT_int4lt.code_size;
		} else if (stencils[opcode].code_size == -1) {
			elog(WARNING, "UNSUPPORTED OPCODE %s", opcodeNames[opcode]);
			canbuild = false;
		} else {
			neededsize += stencils[opcode].code_size;
		}
	}

	// All opcodes are accounted for, we can proceed
	if (canbuild) {
		builtcode = mmap(0, neededsize, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
		context->code = builtcode;
		context->code_size = neededsize;

		for (int opno = 0 ; opno < state->steps_len ; opno++)
		{
			struct ExprEvalStep *op = &state->steps[opno];
			ExprEvalOp opcode = ExecEvalStepOp(state, op);
			size_t next_offset = offsets[opno+1];
			if (DEBUG_GEN)
				elog(WARNING, "Adding stencil for %s, op address is %p", opcodeNames[opcode], op);
#if USE_EXTRA
			if (opcode == EEOP_FUNCEXPR_STRICT) {
				if (DEBUG_GEN)
					elog(WARNING, "Adding %i extra_EEOP_FUNCEXPR_STRICT_CHECKER stencils", op->d.func.nargs);
				for (int narg = 0 ; narg < op->d.func.nargs ; narg++) {
					memcpy(builtcode + offset, extra_EEOP_FUNCEXPR_STRICT_CHECKER.code, extra_EEOP_FUNCEXPR_STRICT_CHECKER.code_size);
					for (int p = 0 ; p < extra_EEOP_FUNCEXPR_STRICT_CHECKER.patch_size ; p++) {
						const struct Patch *patch = &extra_EEOP_FUNCEXPR_STRICT_CHECKER.patches[p];
						if (patch->target == TARGET_FUNC_ARG) {
							NullableDatum *func_arg = &(op->d.func.fcinfo_data->args[narg]);
							apply_patch_with_target(builtcode, offset, (intptr_t) func_arg, patch);
						} else {
							apply_patch(state, builtcode, offsets, offset, next_offset, op, patch);
						}
					}
					offset += extra_EEOP_FUNCEXPR_STRICT_CHECKER.code_size;
				}
				// Now we can land back on normal func call
				offset += apply_stencil(&stencils[EEOP_FUNCEXPR], state, builtcode, offsets, offset, next_offset, op);
			} else
#endif

			if (opcode == EEOP_FUNCEXPR_STRICT && op->d.func.fn_addr == &int4eq) {
				offset += apply_stencil(&extra_EEOP_FUNCEXPR_STRICT_int4eq, state, builtcode, offsets, offset, next_offset, op);
			} else if (opcode == EEOP_FUNCEXPR_STRICT && op->d.func.fn_addr == &int4lt) {
				offset += apply_stencil(&extra_EEOP_FUNCEXPR_STRICT_int4lt, state, builtcode, offsets, offset, next_offset, op);
			} else {
				offset += apply_stencil(&stencils[opcode], state, builtcode, offsets, offset, next_offset, op);
			}
		}
		mprotect_res = mprotect(builtcode, neededsize, PROT_EXEC);
		if (DEBUG_GEN)
			elog(WARNING, "Result of mprotect is %i", mprotect_res);
		state->evalfunc_private = builtcode;
		state->evalfunc = (ExprStateEvalFunc) builtcode; // When this one starts being usefull, we can bring it back. ExecRunCompiledExpr;
		state->evalfunc = ExecRunCompiledExpr;
	}
	free(offsets);

	INSTR_TIME_SET_CURRENT(endtime);
	INSTR_TIME_SET_ZERO(context->base.instr.generation_counter);
	INSTR_TIME_ACCUM_DIFF(context->base.instr.generation_counter,
						  endtime, starttime);

	if (DEBUG_GEN)
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

