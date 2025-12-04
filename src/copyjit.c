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

#if PG_VERSION_NUM < 180000
#include "utils/resowner_private.h"
#endif

#include "utils/expandeddatum.h"
#include "utils/fmgrprotos.h"

#include <stdio.h>
#include <sys/mman.h>
#include <errno.h>


#define DEBUG_GEN 0
#define SHOW_TIME 1

void initialize_stencils();
void copyjit_reset_after_error(void);

/** Registers contract "API" **/

#define REG_COUNT 2

typedef struct LostVariable {
	void *isnull;
	void *value;
	int build_step_id;
} LostVariable;

typedef struct RegisterContent {
	void *isnull;
	void *value;
} RegisterContent;

typedef struct FillRequest {
	void *isnull;
	void *value;
} FillRequest;

typedef struct SpillRequest {
	void *isnull;
	void *value;
} SpillRequest;

typedef struct CurrentState {
	RegisterContent registers[REG_COUNT];
	LostVariable lost_variables[100];
	int lost_variable_count;
	int current_build_step_id;
	FillRequest fill_requests[REG_COUNT];
	SpillRequest spill_requests[REG_COUNT];
} CurrentState;

void copyjit_register_fcinfo_access(FunctionCallInfo fcinfo, CurrentState *context);
void copyjit_register_memory_read_access(char register_id, void *isnull, void *value, CurrentState *context);
void copyjit_register_memory_write_access(char register_id, void *isnull, void *value, CurrentState *context);

void copyjit_register_fcinfo_access(FunctionCallInfo fcinfo, CurrentState *context) {

	if (DEBUG_GEN)
		elog(INFO, "Checking a fcinfo contract");
	/* Step 1: make sure the function only has IN parameters, handling OUT is out of reach so far. */
	// TODO: how to do that, fcinfo does give us a flinfo that doesn't have this info

	/* Step 2: iterate through arguments, make each one as a memory access */
	for (int i = 0 ; i < fcinfo->nargs ; i++) {
		copyjit_register_memory_read_access(-1, &(fcinfo->args[i].isnull), &(fcinfo->args[i].value), context);
	}
}

void copyjit_register_memory_read_access(char register_id, void *isnull, void *value, CurrentState *context) {
	if (DEBUG_GEN)
		elog(INFO, "Checking a memory read contract for register %i", register_id);
	if (register_id < 0) {
		/* Check all registers, if they had this adress, we must spill them */
		for (int reg_id = 0 ; reg_id < REG_COUNT ; reg_id++) {
			if ((context->registers[reg_id].isnull == isnull) && (context->registers[reg_id].value == value)) {
				if (DEBUG_GEN)
					elog(INFO, "Need to spill register %i", reg_id);
				context->spill_requests[reg_id].isnull = isnull;
				context->spill_requests[reg_id].value = value;
			}
		}
		/* Check if the variable was previously lost, if so, bring it back to live */
		for (int lost_var = 0 ; lost_var < context->lost_variable_count ; lost_var++) {
			if ((context->lost_variables[lost_var].isnull == isnull) && (context->lost_variables[lost_var].value == value)) {
				elog(ERROR, "Need to resurrect a lost variable !");
			}
		}
		/* Else nothing to do */
	} else {
		if (context->registers[register_id].isnull == isnull && context->registers[register_id].value == value) {
			if (DEBUG_GEN)
				elog(INFO, "Contract ok for register %i, continue !", register_id);
		} else {
			// Check if it is in a lost variable
			for (int lost_var = 0 ; lost_var < context->lost_variable_count ; lost_var++) {
				if ((context->lost_variables[lost_var].isnull == isnull) && (context->lost_variables[lost_var].value == value)) {
					elog(ERROR, "Need to resurrect a lost variable !");
				}
			}
			if (DEBUG_GEN)
				elog(INFO, "Need to inject a fill register in %i, current is %p/%p, need %p/%p", register_id, context->registers[register_id].isnull, context->registers[register_id].value, isnull, value);
			context->registers[register_id].isnull = isnull;
			context->registers[register_id].value = value;
			context->fill_requests[register_id].isnull = isnull;
			context->fill_requests[register_id].value = value;
		}
	}
}

void copyjit_register_memory_write_access(char register_id, void *isnull, void *value, CurrentState *context) {
	if (DEBUG_GEN)
		elog(INFO, "Checking a memory write contract for register %i", register_id);

	if (register_id < 0) {
		/* Check all registers, if they had this adress, we must empty them */
		for (int reg_id = 0 ; reg_id < REG_COUNT ; reg_id++) {
			if ((context->registers[reg_id].isnull == isnull) && (context->registers[reg_id].value == value)) {
				if (DEBUG_GEN)
					elog(INFO, "Flushing register %i", reg_id);
				context->registers[reg_id].isnull = NULL;
				context->registers[reg_id].value = NULL;
			}
		}
	} else {
		// Ignore writes to a spilled register
		if (context->registers[register_id].isnull != NULL && context->spill_requests[register_id].isnull == NULL) {
			if (DEBUG_GEN)
				elog(INFO, "Register %i contains a variable that is now lost, save it", register_id);
			LostVariable *newLost = &(context->lost_variables[context->lost_variable_count]);
			newLost->isnull = context->registers[register_id].isnull;
			newLost->value = context->registers[register_id].value;
			newLost->build_step_id = context->current_build_step_id;
		}
		if (DEBUG_GEN)
			elog(INFO, "Register %i now contains %p/%p", register_id, isnull, value);
		context->registers[register_id].isnull = isnull;
		context->registers[register_id].value = value;
	}
}

#include "built-stencils.h"

PG_MODULE_MAGIC;

void _PG_init(void);
void _PG_fini(void);

static void ResOwnerReleaseJitContext(Datum res);

typedef struct CopyJitContext
{
	JitContext base;
        ResourceOwner resowner;
	void *code;
	size_t code_size;
} CopyJitContext;


static const ResourceOwnerDesc jit_resowner_desc =
{
        .name = "Copyjit context",
        .release_phase = RESOURCE_RELEASE_BEFORE_LOCKS,
        .release_priority = RELEASE_PRIO_JIT_CONTEXTS,
        .ReleaseResource = ResOwnerReleaseJitContext,
        .DebugPrint = NULL                      /* the default message is fine */
};

/* Convenience wrappers over ResourceOwnerRemember/Forget */
static inline void
ResourceOwnerRememberJIT(ResourceOwner owner, CopyJitContext *handle)
{
        ResourceOwnerRemember(owner, PointerGetDatum(handle), &jit_resowner_desc);
}
static inline void
ResourceOwnerForgetJIT(ResourceOwner owner, CopyJitContext *handle)
{
        ResourceOwnerForget(owner, PointerGetDatum(handle), &jit_resowner_desc);
}


static const char *opcodeNames[] = {
	"EEOP_DONE_RETURN",
	"EEOP_DONE_NO_RETURN",

	/* apply slot_getsomeattrs on corresponding tuple slot */
	"EEOP_INNER_FETCHSOME",
	"EEOP_OUTER_FETCHSOME",
	"EEOP_SCAN_FETCHSOME",
	"EEOP_OLD_FETCHSOME",
	"EEOP_NEW_FETCHSOME",

	/* compute non-system Var value */
	"EEOP_INNER_VAR",
	"EEOP_OUTER_VAR",
	"EEOP_SCAN_VAR",
	"EEOP_OLD_VAR",
	"EEOP_NEW_VAR",

	/* compute system Var value */
	"EEOP_INNER_SYSVAR",
	"EEOP_OUTER_SYSVAR",
	"EEOP_SCAN_SYSVAR",
	"EEOP_OLD_SYSVAR",
	"EEOP_NEW_SYSVAR",

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
	"EEOP_ASSIGN_OLD_VAR",
	"EEOP_ASSIGN_NEW_VAR",

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
	"EEOP_FUNCEXPR_STRICT_1",
	"EEOP_FUNCEXPR_STRICT_2",
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

static const char *opcodeName(int opcode) {
	if (opcode > EEOP_LAST)
		return "EEOP_LAST+n";
	else
		return opcodeNames[opcode];
}

typedef struct CopyJitBuildStep {
	int build_opcode;
	int source_step;
	/* TODO: move these elsewhere */
	intptr_t register_value;
	intptr_t register_isnull;
} CopyJitBuildStep;

typedef struct CodeGen {
	union Code {
		uint32_t *as_u32;
		void *as_void;
		unsigned char *as_char;
	} code;
	int code_size;
	int *offsets;	//TODO: merge with build steps
	CopyJitBuildStep *build_steps;
	int allocated_build_steps;
	int count_build_steps;
	int trampoline_count;	// count the number of initialized trampolines
	intptr_t *trampoline_targets;
} CodeGen;

void
copyjit_reset_after_error(void)
{
}

CopyJitContext *
copyjit_create_context(int jitFlags)
{
	CopyJitContext *context;

	ResourceOwnerEnlarge(CurrentResourceOwner);

	context = MemoryContextAllocZero(TopMemoryContext,
									 sizeof(CopyJitContext));
	context->base.flags = jitFlags;

	/* ensure cleanup */
	context->resowner = CurrentResourceOwner;
	context->code = NULL;
	ResourceOwnerRememberJIT(CurrentResourceOwner, context);

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
	Datum result = ((ExprStateEvalFunc) state->evalfunc_private) (state, econtext, isNull);
	elog(INFO, "Result is value=%i, isNull=%i", result, *isNull);
	return result;
}

static void apply_patch_with_target (CodeGen *codeGen, size_t offset, intptr_t target, const struct Patch *patch);

#if defined(__aarch64__) || defined(_M_ARM64)

#include "copyjit-arm64.c"

#elif defined(__x86_64__)

#include "copyjit-amd64.c"

#else

#error "Unsupported CPU architecture. Please, please, please, contact me so we can work on it!"

#endif

static intptr_t get_patch_target(ExprState *state, CopyJitBuildStep *step, CodeGen *codeGen, size_t next_offset, struct ExprEvalStep *op, const struct Patch *patch)
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
				elog(ERROR, "Unsupported target TARGET_JUMP_NULL in opcode %s", opcodeName(op->opcode));
			break;
		case TARGET_RESULTSLOT_VALUES:
			if (op->opcode == EEOP_ASSIGN_TMP || op->opcode == EEOP_ASSIGN_TMP_MAKE_RO)
				target = (intptr_t) &(state->resultslot->tts_values[op->d.assign_tmp.resultnum]);
			else if (op->opcode == EEOP_ASSIGN_SCAN_VAR || op->opcode == EEOP_ASSIGN_INNER_VAR || op->opcode == EEOP_ASSIGN_OUTER_VAR)
				target = (intptr_t) &(state->resultslot->tts_values[op->d.assign_var.resultnum]);
			else
				elog(ERROR, "Unsupported target TARGET_RESULTSLOT_VALUES in opcode %s", opcodeName(op->opcode));
			break;
		case TARGET_RESULTSLOT_ISNULL:
			if (op->opcode == EEOP_ASSIGN_TMP || op->opcode == EEOP_ASSIGN_TMP_MAKE_RO)
				target = (intptr_t) &(state->resultslot->tts_isnull[op->d.assign_tmp.resultnum]);
			else if (op->opcode == EEOP_ASSIGN_SCAN_VAR || op->opcode == EEOP_ASSIGN_INNER_VAR || op->opcode == EEOP_ASSIGN_OUTER_VAR)
				target = (intptr_t) &(state->resultslot->tts_isnull[op->d.assign_var.resultnum]);
			else
				elog(ERROR, "Unsupported target TARGET_RESULTSLOT_ISNULL in opcode %s", opcodeName(op->opcode));
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
				elog(ERROR, "Unsupported target TARGET_ATTNUM in opcode %s", opcodeName(op->opcode));
			break;
		case TARGET_CurrentMemoryContext:
			target = (intptr_t) &CurrentMemoryContext;
			break;
		case TARGET_REGISTER_VALUE:
			target = step->register_value;
			break;
		case TARGET_REGISTER_ISNULL:
			target = step->register_isnull;
			break;
		default:
			elog(ERROR, "Unsupported target");
			break;
	};
	return target + patch->addend;
}

static void apply_patch (ExprState *state, CopyJitBuildStep *step, CodeGen *codeGen, size_t offset, size_t next_offset, struct ExprEvalStep *op, const struct Patch *patch)
{
	intptr_t target = get_patch_target(state, step, codeGen, next_offset, op, patch);

	apply_patch_with_target(codeGen, offset, target, patch);
}

static size_t apply_stencil (struct Stencil *stencil, ExprState *state, CopyJitBuildStep *step, CodeGen *codeGen, size_t offset, size_t next_offset, struct ExprEvalStep *op)
{
	memcpy(codeGen->code.as_void + offset, stencil->code, stencil->code_size);
	for (int p = 0 ; p < stencil->patch_size ; p++) {
		const struct Patch *patch = &stencil->patches[p];
		apply_patch(state, step, codeGen, offset, next_offset, op, patch);
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

	// We include some margin for the number of build steps because it's cheap...
	codeGen.build_steps = malloc(sizeof(CopyJitBuildStep) * 3 * state->steps_len);
	codeGen.allocated_build_steps = 3 * state->steps_len;
	codeGen.count_build_steps = 0;
	CurrentState registerState;
	registerState.lost_variable_count = 0;
	for (int r = 0 ; r <REG_COUNT ; r++) {
		registerState.registers[r].isnull = NULL;
		registerState.registers[r].value = NULL;
	}

	for (int opno = 0; opno < state->steps_len; opno++)
	{
		struct ExprEvalStep *op = &state->steps[opno];
		ExprEvalOp opcode = op->opcode;
		if (DEBUG_GEN)
			elog(WARNING, "Need to build an %s - %i opcode at %p", opcodeName(opcode), opcode, op);

		codeGen.offsets[opno] = neededsize;

		if (stencils[opcode].code_size == -1) {
			elog(WARNING, "UNSUPPORTED OPCODE %s", opcodeName(opcode));
			canbuild = false;
		} else {
			opcode = dispatch_opcode(op);
			if (opcode != op->opcode) {
				if (DEBUG_GEN)
					elog(INFO, "Dispatching opcode %i (%s) to opcode %i (EEOP_LAST+%i) instead", op->opcode, opcodeName(op->opcode), opcode, opcode-EEOP_LAST);
				// XXX TODO FIXME XXX we are modifying the opcode here, but what if we can't build in the end???
				op->opcode = opcode;
			}

			// Now check the contract to add the required preliminary steps
			registerState.current_build_step_id = codeGen.count_build_steps;
			for (int r = 0 ; r < REG_COUNT ; r++) {
				registerState.fill_requests[r].isnull = NULL;
				registerState.fill_requests[r].value = NULL;
				registerState.spill_requests[r].isnull = NULL;
				registerState.spill_requests[r].value = NULL;
			}
			// We must make sure here that registers contracts are going to be ok
			// because this implies inserting spill, values or swap calls in-between, modifying the next build step (esp. offsets)
			if (stencils[opcode].registers_contract) {
				if (DEBUG_GEN)
					elog(INFO, "Applying contract for opcode %i", opcode);
				stencils[opcode].registers_contract(state, op, &registerState);
			}

			// Inject the required step for filling/spilling registers, if needed
			for (int r = 0 ; r < REG_COUNT ; r++) {
				if (registerState.spill_requests[r].isnull && registerState.spill_requests[r].value) {
					if (DEBUG_GEN)
						elog(INFO, "Build a spill register from reg %i for %p/%p", r, registerState.spill_requests[r].isnull, registerState.spill_requests[r].value);

					if (r == 0)
						codeGen.build_steps[codeGen.count_build_steps].build_opcode = extra_spill_reg0__opcode;
					else if (r == 1)
						codeGen.build_steps[codeGen.count_build_steps].build_opcode = extra_spill_reg1__opcode;

					if (DEBUG_GEN)
						elog(INFO, "Selected opcode %i aka EEOP_LAST+%i", codeGen.build_steps[codeGen.count_build_steps].build_opcode, codeGen.build_steps[codeGen.count_build_steps].build_opcode - EEOP_LAST);
					codeGen.build_steps[codeGen.count_build_steps].source_step = opno;

					codeGen.build_steps[codeGen.count_build_steps].register_value = (intptr_t) (registerState.spill_requests[r].value);
					codeGen.build_steps[codeGen.count_build_steps].register_isnull = (intptr_t) (registerState.spill_requests[r].isnull);

					neededsize += stencils[codeGen.build_steps[codeGen.count_build_steps].build_opcode].code_size;

					codeGen.count_build_steps++;
					if (codeGen.count_build_steps >= codeGen.allocated_build_steps) {
						codeGen.build_steps = realloc(codeGen.build_steps, sizeof(CopyJitBuildStep) * 2 * codeGen.allocated_build_steps);
						codeGen.allocated_build_steps *= 2;
					}

				}
				if (registerState.fill_requests[r].isnull && registerState.fill_requests[r].value) {
					if (DEBUG_GEN)
						elog(INFO, "Build a fill register for reg %i from %p/%p", r, registerState.fill_requests[r].isnull, registerState.fill_requests[r].value);


					// TODO this is duplicated, bad bad

					if (r == 0 && *((bool *)registerState.fill_requests[r].isnull))
						codeGen.build_steps[codeGen.count_build_steps].build_opcode = extra_set_reg0_null__opcode;
					else if (r == 0)
						codeGen.build_steps[codeGen.count_build_steps].build_opcode = extra_set_reg0_const__opcode;
					else if (r == 1 && *((bool *)registerState.fill_requests[r].isnull))
						codeGen.build_steps[codeGen.count_build_steps].build_opcode = extra_set_reg1_null__opcode;
					else if (r == 1)
						codeGen.build_steps[codeGen.count_build_steps].build_opcode = extra_set_reg1_const__opcode;
					if (DEBUG_GEN)
						elog(INFO, "Selected opcode %i aka EEOP_LAST+%i", codeGen.build_steps[codeGen.count_build_steps].build_opcode, codeGen.build_steps[codeGen.count_build_steps].build_opcode - EEOP_LAST);
					codeGen.build_steps[codeGen.count_build_steps].source_step = opno;

					codeGen.build_steps[codeGen.count_build_steps].register_value = *((intptr_t*)registerState.fill_requests[r].value);
					codeGen.build_steps[codeGen.count_build_steps].register_isnull = *((intptr_t*)registerState.fill_requests[r].isnull);
					neededsize += stencils[codeGen.build_steps[codeGen.count_build_steps].build_opcode].code_size;

					codeGen.count_build_steps++;
					if (codeGen.count_build_steps >= codeGen.allocated_build_steps) {
						codeGen.build_steps = realloc(codeGen.build_steps, sizeof(CopyJitBuildStep) * 2 * codeGen.allocated_build_steps);
						codeGen.allocated_build_steps *= 2;
					}
				}
			}

			// Ok, now we can build

			codeGen.build_steps[codeGen.count_build_steps].build_opcode = opcode;
			codeGen.build_steps[codeGen.count_build_steps].source_step = opno;
			codeGen.count_build_steps++;
			if (codeGen.count_build_steps >= codeGen.allocated_build_steps) {
				codeGen.build_steps = realloc(codeGen.build_steps, sizeof(CopyJitBuildStep) * 2 * codeGen.allocated_build_steps);
				codeGen.allocated_build_steps *= 2;
			}
			neededsize += stencils[opcode].code_size;
			if (TRAMPOLINE_SIZE) {
				// Check for patches that require trampolines to be built
				for (int p = 0 ; p < stencils[opcode].patch_size ; p++) {
					if (stencils[opcode].patches[p].relkind == RELKIND_R_AARCH64_CALL26) {
						required_trampolines++;
					}
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
			codeGen.trampoline_targets = malloc(sizeof(void*) * required_trampolines);
			memset(codeGen.trampoline_targets, 0, sizeof(void*) * required_trampolines);
		}
		context->code = codeGen.code.as_void;
		context->code_size = neededsize + required_trampolines * TRAMPOLINE_SIZE;

		for (int buildStep = 0 ; buildStep < codeGen.count_build_steps ; buildStep++)
		{
			int opno = codeGen.build_steps[buildStep].source_step;
			struct ExprEvalStep *op = &state->steps[opno];
			size_t next_offset = codeGen.offsets[opno+1];
			if (DEBUG_GEN)
				elog(WARNING, "Adding stencil for %s, op address is %p", opcodeName(codeGen.build_steps[buildStep].build_opcode), op);
			offset += apply_stencil(&stencils[codeGen.build_steps[buildStep].build_opcode], state, &codeGen.build_steps[buildStep], &codeGen, offset, next_offset, op);
		}

		if (DEBUG_GEN) {
			elog(WARNING, "Code generated is located at %p for %i bytes (with %i trampolines)", codeGen.code.as_void, codeGen.code_size, required_trampolines);
			int fd = open("/tmp/code.jit.bin", O_CREAT | O_WRONLY, 0666);
			int written = write(fd, codeGen.code.as_void, codeGen.code_size);
			if (written != codeGen.code_size)
				elog(WARNING, "Failed to dump code - %i", written);
			close(fd);
		}

		mprotect_res = mprotect(codeGen.code.as_void, neededsize, PROT_EXEC);
		if (DEBUG_GEN)
			elog(WARNING, "Result of mprotect is %i", mprotect_res);
		state->evalfunc_private = codeGen.code.as_void;
		state->evalfunc = (ExprStateEvalFunc) codeGen.code.as_void; // We jump through ExecRunCompiledExpr so we can breakpoint, if needed...
//		state->evalfunc = ExecRunCompiledExpr;
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

<<<<<<< HEAD
#if 0


/*
 * Create a function that deforms a tuple of type desc up to natts columns.
 */
LLVMValueRef
slot_compile_deform(LLVMJitContext *context, TupleDesc desc,
					const TupleTableSlotOps *ops, int natts)
{
	char	   *funcname;

	LLVMModuleRef mod;
	LLVMContextRef lc;
	LLVMBuilderRef b;

	LLVMTypeRef deform_sig;
	LLVMValueRef v_deform_fn;

	LLVMBasicBlockRef b_entry;
	LLVMBasicBlockRef b_adjust_unavail_cols;
	LLVMBasicBlockRef b_find_start;

	LLVMBasicBlockRef b_out;
	LLVMBasicBlockRef b_dead;
	LLVMBasicBlockRef *attcheckattnoblocks;
	LLVMBasicBlockRef *attstartblocks;
	LLVMBasicBlockRef *attisnullblocks;
	LLVMBasicBlockRef *attcheckalignblocks;
	LLVMBasicBlockRef *attalignblocks;
	LLVMBasicBlockRef *attstoreblocks;

	LLVMValueRef v_offp;

	LLVMValueRef v_tupdata_base;
	LLVMValueRef v_tts_values;
	LLVMValueRef v_tts_nulls;
	LLVMValueRef v_slotoffp;
	LLVMValueRef v_flagsp;
	LLVMValueRef v_nvalidp;
	LLVMValueRef v_nvalid;
	LLVMValueRef v_maxatt;

	LLVMValueRef v_slot;

	LLVMValueRef v_tupleheaderp;
	LLVMValueRef v_tuplep;
	LLVMValueRef v_infomask1;
	LLVMValueRef v_infomask2;
	LLVMValueRef v_bits;

	LLVMValueRef v_hoff;

	LLVMValueRef v_hasnulls;

	/* last column (0 indexed) guaranteed to exist */
	int			guaranteed_column_number = -1;

	/* current known alignment */
	int			known_alignment = 0;

	/* if true, known_alignment describes definite offset of column */
	bool		attguaranteedalign = true;

	int			attnum;

	/* virtual tuples never need deforming, so don't generate code */
	if (ops == &TTSOpsVirtual)
		return NULL;

	/* decline to JIT for slot types we don't know to handle */
	if (ops != &TTSOpsHeapTuple && ops != &TTSOpsBufferHeapTuple &&
		ops != &TTSOpsMinimalTuple)
		return NULL;

	mod = llvm_mutable_module(context);
	lc = LLVMGetModuleContext(mod);

	funcname = llvm_expand_funcname(context, "deform");

	/*
	 * Check which columns have to exist, so we don't have to check the row's
	 * natts unnecessarily.
	 */
	for (attnum = 0; attnum < desc->natts; attnum++)
	{
		CompactAttribute *att = TupleDescCompactAttr(desc, attnum);

		/*
		 * If the column is declared NOT NULL then it must be present in every
		 * tuple, unless there's a "missing" entry that could provide a
		 * non-NULL value for it. That in turn guarantees that the NULL bitmap
		 * - if there are any NULLable columns - is at least long enough to
		 * cover columns up to attnum.
		 *
		 * Be paranoid and also check !attisdropped, even though the
		 * combination of attisdropped && attnotnull combination shouldn't
		 * exist.
		 */
		if (att->attnullability == ATTNULLABLE_VALID &&
			!att->atthasmissing &&
			!att->attisdropped)
			guaranteed_column_number = attnum;
	}

	/* Create the signature and function */
	{
		LLVMTypeRef param_types[1];

		param_types[0] = l_ptr(StructTupleTableSlot);

		deform_sig = LLVMFunctionType(LLVMVoidTypeInContext(lc),
									  param_types, lengthof(param_types), 0);
	}
	v_deform_fn = LLVMAddFunction(mod, funcname, deform_sig);
	LLVMSetLinkage(v_deform_fn, LLVMInternalLinkage);
	LLVMSetParamAlignment(LLVMGetParam(v_deform_fn, 0), MAXIMUM_ALIGNOF);
	llvm_copy_attributes(AttributeTemplate, v_deform_fn);

	b_entry =
	LLVMAppendBasicBlockInContext(lc, v_deform_fn, "entry");
	b_adjust_unavail_cols =
	LLVMAppendBasicBlockInContext(lc, v_deform_fn, "adjust_unavail_cols");
	b_find_start =
	LLVMAppendBasicBlockInContext(lc, v_deform_fn, "find_startblock");
	b_out =
	LLVMAppendBasicBlockInContext(lc, v_deform_fn, "outblock");
	b_dead =
	LLVMAppendBasicBlockInContext(lc, v_deform_fn, "deadblock");

	b = LLVMCreateBuilderInContext(lc);

	attcheckattnoblocks = palloc(sizeof(LLVMBasicBlockRef) * natts);
	attstartblocks = palloc(sizeof(LLVMBasicBlockRef) * natts);
	attisnullblocks = palloc(sizeof(LLVMBasicBlockRef) * natts);
	attcheckalignblocks = palloc(sizeof(LLVMBasicBlockRef) * natts);
	attalignblocks = palloc(sizeof(LLVMBasicBlockRef) * natts);
	attstoreblocks = palloc(sizeof(LLVMBasicBlockRef) * natts);

	known_alignment = 0;

	LLVMPositionBuilderAtEnd(b, b_entry);

	/* perform allocas first, llvm only converts those to registers */
	v_offp = LLVMBuildAlloca(b, TypeSizeT, "v_offp");

	v_slot = LLVMGetParam(v_deform_fn, 0);

	v_tts_values =
	l_load_struct_gep(b, StructTupleTableSlot, v_slot, FIELDNO_TUPLETABLESLOT_VALUES,
					  "tts_values");
	v_tts_nulls =
	l_load_struct_gep(b, StructTupleTableSlot, v_slot, FIELDNO_TUPLETABLESLOT_ISNULL,
					  "tts_ISNULL");
	v_flagsp = l_struct_gep(b, StructTupleTableSlot, v_slot, FIELDNO_TUPLETABLESLOT_FLAGS, "");
	v_nvalidp = l_struct_gep(b, StructTupleTableSlot, v_slot, FIELDNO_TUPLETABLESLOT_NVALID, "");

	if (ops == &TTSOpsHeapTuple || ops == &TTSOpsBufferHeapTuple)
	{
		LLVMValueRef v_heapslot;

		v_heapslot =
		LLVMBuildBitCast(b,
						 v_slot,
				   l_ptr(StructHeapTupleTableSlot),
						 "heapslot");
		v_slotoffp = l_struct_gep(b, StructHeapTupleTableSlot, v_heapslot, FIELDNO_HEAPTUPLETABLESLOT_OFF, "");
		v_tupleheaderp =
		l_load_struct_gep(b, StructHeapTupleTableSlot, v_heapslot, FIELDNO_HEAPTUPLETABLESLOT_TUPLE,
						  "tupleheader");
	}
	else if (ops == &TTSOpsMinimalTuple)
	{
		LLVMValueRef v_minimalslot;

		v_minimalslot =
		LLVMBuildBitCast(b,
						 v_slot,
				   l_ptr(StructMinimalTupleTableSlot),
						 "minimalslot");
		v_slotoffp = l_struct_gep(b,
								  StructMinimalTupleTableSlot,
							v_minimalslot,
							FIELDNO_MINIMALTUPLETABLESLOT_OFF, "");
		v_tupleheaderp =
		l_load_struct_gep(b,
						  StructMinimalTupleTableSlot,
					v_minimalslot,
					FIELDNO_MINIMALTUPLETABLESLOT_TUPLE,
					"tupleheader");
	}
	else
	{
		/* should've returned at the start of the function */
		pg_unreachable();
	}

	v_tuplep =
	l_load_struct_gep(b,
					  StructHeapTupleData,
					  v_tupleheaderp,
					  FIELDNO_HEAPTUPLEDATA_DATA,
					  "tuple");
	v_bits =
	LLVMBuildBitCast(b,
					 l_struct_gep(b,
								  StructHeapTupleHeaderData,
								  v_tuplep,
								  FIELDNO_HEAPTUPLEHEADERDATA_BITS,
								  ""),
								  l_ptr(LLVMInt8TypeInContext(lc)),
					 "t_bits");
	v_infomask1 =
	l_load_struct_gep(b,
					  StructHeapTupleHeaderData,
					  v_tuplep,
					  FIELDNO_HEAPTUPLEHEADERDATA_INFOMASK,
					  "infomask1");
	v_infomask2 =
	l_load_struct_gep(b,
					  StructHeapTupleHeaderData,
					  v_tuplep, FIELDNO_HEAPTUPLEHEADERDATA_INFOMASK2,
					  "infomask2");

	/* t_infomask & HEAP_HASNULL */
	v_hasnulls =
	LLVMBuildICmp(b, LLVMIntNE,
				  LLVMBuildAnd(b,
							   l_int16_const(lc, HEAP_HASNULL),
							   v_infomask1, ""),
							   l_int16_const(lc, 0),
				  "hasnulls");

	/* t_infomask2 & HEAP_NATTS_MASK */
	v_maxatt = LLVMBuildAnd(b,
							l_int16_const(lc, HEAP_NATTS_MASK),
							v_infomask2,
							"maxatt");

	/*
	 * Need to zext, as getelementptr otherwise treats hoff as a signed 8bit
	 * integer, which'd yield a negative offset for t_hoff > 127.
	 */
	v_hoff =
	LLVMBuildZExt(b,
				  l_load_struct_gep(b,
									StructHeapTupleHeaderData,
									v_tuplep,
									FIELDNO_HEAPTUPLEHEADERDATA_HOFF,
									""),
									LLVMInt32TypeInContext(lc), "t_hoff");

	v_tupdata_base = l_gep(b,
						   LLVMInt8TypeInContext(lc),
						   LLVMBuildBitCast(b,
											v_tuplep,
											l_ptr(LLVMInt8TypeInContext(lc)),
											""),
											&v_hoff, 1,
											"v_tupdata_base");

	/*
	 * Load tuple start offset from slot. Will be reset below in case there's
	 * no existing deformed columns in slot.
	 */
	{
		LLVMValueRef v_off_start;

		v_off_start = l_load(b, LLVMInt32TypeInContext(lc), v_slotoffp, "v_slot_off");
		v_off_start = LLVMBuildZExt(b, v_off_start, TypeSizeT, "");
		LLVMBuildStore(b, v_off_start, v_offp);
	}

	/* build the basic block for each attribute, need them as jump target */
	for (attnum = 0; attnum < natts; attnum++)
	{
		attcheckattnoblocks[attnum] =
		l_bb_append_v(v_deform_fn, "block.attr.%d.attcheckattno", attnum);
		attstartblocks[attnum] =
		l_bb_append_v(v_deform_fn, "block.attr.%d.start", attnum);
		attisnullblocks[attnum] =
		l_bb_append_v(v_deform_fn, "block.attr.%d.attisnull", attnum);
		attcheckalignblocks[attnum] =
		l_bb_append_v(v_deform_fn, "block.attr.%d.attcheckalign", attnum);
		attalignblocks[attnum] =
		l_bb_append_v(v_deform_fn, "block.attr.%d.align", attnum);
		attstoreblocks[attnum] =
		l_bb_append_v(v_deform_fn, "block.attr.%d.store", attnum);
	}

	/*
	 * Check if it is guaranteed that all the desired attributes are available
	 * in the tuple (but still possibly NULL), by dint of either the last
	 * to-be-deformed column being NOT NULL, or subsequent ones not accessed
	 * here being NOT NULL.  If that's not guaranteed the tuple headers natt's
	 * has to be checked, and missing attributes potentially have to be
	 * fetched (using slot_getmissingattrs().
	 */
	if ((natts - 1) <= guaranteed_column_number)
	{
		/* just skip through unnecessary blocks */
		LLVMBuildBr(b, b_adjust_unavail_cols);
		LLVMPositionBuilderAtEnd(b, b_adjust_unavail_cols);
		LLVMBuildBr(b, b_find_start);
	}
	else
	{
		LLVMValueRef v_params[3];
		LLVMValueRef f;

		/* branch if not all columns available */
		LLVMBuildCondBr(b,
						LLVMBuildICmp(b, LLVMIntULT,
									  v_maxatt,
					l_int16_const(lc, natts),
									  ""),
				  b_adjust_unavail_cols,
				  b_find_start);

		/* if not, memset tts_isnull of relevant cols to true */
		LLVMPositionBuilderAtEnd(b, b_adjust_unavail_cols);

		v_params[0] = v_slot;
		v_params[1] = LLVMBuildZExt(b, v_maxatt, LLVMInt32TypeInContext(lc), "");
		v_params[2] = l_int32_const(lc, natts);
		f = llvm_pg_func(mod, "slot_getmissingattrs");
		l_call(b,
			   LLVMGetFunctionType(f), f,
			   v_params, lengthof(v_params), "");
		LLVMBuildBr(b, b_find_start);
	}

	LLVMPositionBuilderAtEnd(b, b_find_start);

	v_nvalid = l_load(b, LLVMInt16TypeInContext(lc), v_nvalidp, "");

	/*
	 * Build switch to go from nvalid to the right startblock.  Callers
	 * currently don't have the knowledge, but it'd be good for performance to
	 * avoid this check when it's known that the slot is empty (e.g. in scan
	 * nodes).
	 */
	if (true)
	{
		LLVMValueRef v_switch = LLVMBuildSwitch(b, v_nvalid,
												b_dead, natts);

		for (attnum = 0; attnum < natts; attnum++)
		{
			LLVMValueRef v_attno = l_int16_const(lc, attnum);

			LLVMAddCase(v_switch, v_attno, attcheckattnoblocks[attnum]);
		}
	}
	else
	{
		/* jump from entry block to first block */
		LLVMBuildBr(b, attcheckattnoblocks[0]);
	}

	LLVMPositionBuilderAtEnd(b, b_dead);
	LLVMBuildUnreachable(b);

	/*
	 * Iterate over each attribute that needs to be deformed, build code to
	 * deform it.
	 */
	for (attnum = 0; attnum < natts; attnum++)
	{
		CompactAttribute *att = TupleDescCompactAttr(desc, attnum);
		LLVMValueRef v_incby;
		int			alignto = att->attalignby;
		LLVMValueRef l_attno = l_int16_const(lc, attnum);
		LLVMValueRef v_attdatap;
		LLVMValueRef v_resultp;

		/* build block checking whether we did all the necessary attributes */
		LLVMPositionBuilderAtEnd(b, attcheckattnoblocks[attnum]);

		/*
		 * If this is the first attribute, slot->tts_nvalid was 0. Therefore
		 * also reset offset to 0, it may be from a previous execution.
		 */
		if (attnum == 0)
		{
			LLVMBuildStore(b, l_sizet_const(0), v_offp);
		}

		/*
		 * Build check whether column is available (i.e. whether the tuple has
		 * that many columns stored). We can avoid the branch if we know
		 * there's a subsequent NOT NULL column.
		 */
		if (attnum <= guaranteed_column_number)
		{
			LLVMBuildBr(b, attstartblocks[attnum]);
		}
		else
		{
			LLVMValueRef v_islast;

			v_islast = LLVMBuildICmp(b, LLVMIntUGE,
									 l_attno,
							v_maxatt,
							"heap_natts");
			LLVMBuildCondBr(b, v_islast, b_out, attstartblocks[attnum]);
		}
		LLVMPositionBuilderAtEnd(b, attstartblocks[attnum]);

		/*
		 * Check for nulls if necessary. No need to take missing attributes
		 * into account, because if they're present the heaptuple's natts
		 * would have indicated that a slot_getmissingattrs() is needed.
		 */
		if (att->attnullability != ATTNULLABLE_VALID)
		{
			LLVMBasicBlockRef b_ifnotnull;
			LLVMBasicBlockRef b_ifnull;
			LLVMBasicBlockRef b_next;
			LLVMValueRef v_attisnull;
			LLVMValueRef v_nullbyteno;
			LLVMValueRef v_nullbytemask;
			LLVMValueRef v_nullbyte;
			LLVMValueRef v_nullbit;

			b_ifnotnull = attcheckalignblocks[attnum];
			b_ifnull = attisnullblocks[attnum];

			if (attnum + 1 == natts)
				b_next = b_out;
			else
				b_next = attcheckattnoblocks[attnum + 1];

			v_nullbyteno = l_int32_const(lc, attnum >> 3);
			v_nullbytemask = l_int8_const(lc, 1 << ((attnum) & 0x07));
			v_nullbyte = l_load_gep1(b, LLVMInt8TypeInContext(lc), v_bits, v_nullbyteno, "attnullbyte");

			v_nullbit = LLVMBuildICmp(b,
									  LLVMIntEQ,
							 LLVMBuildAnd(b, v_nullbyte, v_nullbytemask, ""),
									  l_int8_const(lc, 0),
									  "attisnull");

			v_attisnull = LLVMBuildAnd(b, v_hasnulls, v_nullbit, "");

			LLVMBuildCondBr(b, v_attisnull, b_ifnull, b_ifnotnull);

			LLVMPositionBuilderAtEnd(b, b_ifnull);

			/* store null-byte */
			LLVMBuildStore(b,
						   l_int8_const(lc, 1),
						   l_gep(b, LLVMInt8TypeInContext(lc), v_tts_nulls, &l_attno, 1, ""));
			/* store zero datum */
			LLVMBuildStore(b,
						   l_datum_const(0),
						   l_gep(b, TypeDatum, v_tts_values, &l_attno, 1, ""));

			LLVMBuildBr(b, b_next);
			attguaranteedalign = false;
		}
		else
		{
			/* nothing to do */
			LLVMBuildBr(b, attcheckalignblocks[attnum]);
			LLVMPositionBuilderAtEnd(b, attisnullblocks[attnum]);
			LLVMBuildBr(b, attcheckalignblocks[attnum]);
		}
		LLVMPositionBuilderAtEnd(b, attcheckalignblocks[attnum]);

		/* ------
		 * Even if alignment is required, we can skip doing it if provably
		 * unnecessary:
		 * - first column is guaranteed to be aligned
		 * - columns following a NOT NULL fixed width datum have known
		 *   alignment, can skip alignment computation if that known alignment
		 *   is compatible with current column.
		 * ------
		 */
		if (alignto > 1 &&
			(known_alignment < 0 || known_alignment != TYPEALIGN(alignto, known_alignment)))
		{
			/*
			 * When accessing a varlena field, we have to "peek" to see if we
			 * are looking at a pad byte or the first byte of a 1-byte-header
			 * datum.  A zero byte must be either a pad byte, or the first
			 * byte of a correctly aligned 4-byte length word; in either case,
			 * we can align safely.  A non-zero byte must be either a 1-byte
			 * length word, or the first byte of a correctly aligned 4-byte
			 * length word; in either case, we need not align.
			 */
			if (att->attlen == -1)
			{
				LLVMValueRef v_possible_padbyte;
				LLVMValueRef v_ispad;
				LLVMValueRef v_off;

				/* don't know if short varlena or not */
				attguaranteedalign = false;

				v_off = l_load(b, TypeSizeT, v_offp, "");

				v_possible_padbyte =
				l_load_gep1(b, LLVMInt8TypeInContext(lc), v_tupdata_base, v_off, "padbyte");
				v_ispad =
				LLVMBuildICmp(b, LLVMIntEQ,
							  v_possible_padbyte, l_int8_const(lc, 0),
							  "ispadbyte");
				LLVMBuildCondBr(b, v_ispad,
								attalignblocks[attnum],
					attstoreblocks[attnum]);
			}
			else
			{
				LLVMBuildBr(b, attalignblocks[attnum]);
			}

			LLVMPositionBuilderAtEnd(b, attalignblocks[attnum]);

			/* translation of alignment code (cf TYPEALIGN()) */
			{
				LLVMValueRef v_off_aligned;
				LLVMValueRef v_off = l_load(b, TypeSizeT, v_offp, "");

				/* ((ALIGNVAL) - 1) */
				LLVMValueRef v_alignval = l_sizet_const(alignto - 1);

				/* ((uintptr_t) (LEN) + ((ALIGNVAL) - 1)) */
				LLVMValueRef v_lh = LLVMBuildAdd(b, v_off, v_alignval, "");

				/* ~((uintptr_t) ((ALIGNVAL) - 1)) */
				LLVMValueRef v_rh = l_sizet_const(~(alignto - 1));

				v_off_aligned = LLVMBuildAnd(b, v_lh, v_rh, "aligned_offset");

				LLVMBuildStore(b, v_off_aligned, v_offp);
			}

			/*
			 * As alignment either was unnecessary or has been performed, we
			 * now know the current alignment. This is only safe because this
			 * value isn't used for varlena and nullable columns.
			 */
			if (known_alignment >= 0)
			{
				Assert(known_alignment != 0);
				known_alignment = TYPEALIGN(alignto, known_alignment);
			}

			LLVMBuildBr(b, attstoreblocks[attnum]);
			LLVMPositionBuilderAtEnd(b, attstoreblocks[attnum]);
		}
		else
		{
			LLVMPositionBuilderAtEnd(b, attcheckalignblocks[attnum]);
			LLVMBuildBr(b, attalignblocks[attnum]);
			LLVMPositionBuilderAtEnd(b, attalignblocks[attnum]);
			LLVMBuildBr(b, attstoreblocks[attnum]);
		}
		LLVMPositionBuilderAtEnd(b, attstoreblocks[attnum]);

		/*
		 * Store the current offset if known to be constant. That allows LLVM
		 * to generate better code. Without that LLVM can't figure out that
		 * the offset might be constant due to the jumps for previously
		 * decoded columns.
		 */
		if (attguaranteedalign)
		{
			Assert(known_alignment >= 0);
			LLVMBuildStore(b, l_sizet_const(known_alignment), v_offp);
		}

		/* compute what following columns are aligned to */
		if (att->attlen < 0)
		{
			/* can't guarantee any alignment after variable length field */
			known_alignment = -1;
			attguaranteedalign = false;
		}
		else if (att->attnullability == ATTNULLABLE_VALID &&
			attguaranteedalign && known_alignment >= 0)
		{
			/*
			 * If the offset to the column was previously known, a NOT NULL &
			 * fixed-width column guarantees that alignment is just the
			 * previous alignment plus column width.
			 */
			Assert(att->attlen > 0);
			known_alignment += att->attlen;
		}
		else if (att->attnullability == ATTNULLABLE_VALID &&
			(att->attlen % alignto) == 0)
		{
			/*
			 * After a NOT NULL fixed-width column with a length that is a
			 * multiple of its alignment requirement, we know the following
			 * column is aligned to at least the current column's alignment.
			 */
			Assert(att->attlen > 0);
			known_alignment = alignto;
			Assert(known_alignment > 0);
			attguaranteedalign = false;
		}
		else
		{
			known_alignment = -1;
			attguaranteedalign = false;
		}


		/* compute address to load data from */
		{
			LLVMValueRef v_off = l_load(b, TypeSizeT, v_offp, "");

			v_attdatap =
			l_gep(b, LLVMInt8TypeInContext(lc), v_tupdata_base, &v_off, 1, "");
		}

		/* compute address to store value at */
		v_resultp = l_gep(b, TypeDatum, v_tts_values, &l_attno, 1, "");

		/* store null-byte (false) */
		LLVMBuildStore(b, l_int8_const(lc, 0),
					   l_gep(b, TypeStorageBool, v_tts_nulls, &l_attno, 1, ""));

		/*
		 * Store datum. For byval: datums copy the value, extend to Datum's
		 * width, and store. For byref types: store pointer to data.
		 */
		if (att->attbyval)
		{
			LLVMValueRef v_tmp_loaddata;
			LLVMTypeRef vartype = LLVMIntTypeInContext(lc, att->attlen * 8);
			LLVMTypeRef vartypep = LLVMPointerType(vartype, 0);

			v_tmp_loaddata =
			LLVMBuildPointerCast(b, v_attdatap, vartypep, "");
			v_tmp_loaddata = l_load(b, vartype, v_tmp_loaddata, "attr_byval");
			v_tmp_loaddata = LLVMBuildZExt(b, v_tmp_loaddata, TypeDatum, "");

			LLVMBuildStore(b, v_tmp_loaddata, v_resultp);
		}
		else
		{
			LLVMValueRef v_tmp_loaddata;

			/* store pointer */
			v_tmp_loaddata =
			LLVMBuildPtrToInt(b,
							  v_attdatap,
					 TypeDatum,
					 "attr_ptr");
			LLVMBuildStore(b, v_tmp_loaddata, v_resultp);
		}

		/* increment data pointer */
		if (att->attlen > 0)
		{
			v_incby = l_sizet_const(att->attlen);
		}
		else if (att->attlen == -1)
		{
			v_incby = l_call(b,
							 llvm_pg_var_func_type("varsize_any"),
							 llvm_pg_func(mod, "varsize_any"),
							 &v_attdatap, 1,
					"varsize_any");
			l_callsite_ro(v_incby);
			l_callsite_alwaysinline(v_incby);
		}
		else if (att->attlen == -2)
		{
			v_incby = l_call(b,
							 llvm_pg_var_func_type("strlen"),
							 llvm_pg_func(mod, "strlen"),
							 &v_attdatap, 1, "strlen");

			l_callsite_ro(v_incby);

			/* add 1 for NUL byte */
			v_incby = LLVMBuildAdd(b, v_incby, l_sizet_const(1), "");
		}
		else
		{
			Assert(false);
			v_incby = NULL;		/* silence compiler */
		}

		if (attguaranteedalign)
		{
			Assert(known_alignment >= 0);
			LLVMBuildStore(b, l_sizet_const(known_alignment), v_offp);
		}
		else
		{
			LLVMValueRef v_off = l_load(b, TypeSizeT, v_offp, "");

			v_off = LLVMBuildAdd(b, v_off, v_incby, "increment_offset");
			LLVMBuildStore(b, v_off, v_offp);
		}

		/*
		 * jump to next block, unless last possible column, or all desired
		 * (available) attributes have been fetched.
		 */
		if (attnum + 1 == natts)
		{
			/* jump out */
			LLVMBuildBr(b, b_out);
		}
		else
		{
			LLVMBuildBr(b, attcheckattnoblocks[attnum + 1]);
		}
	}


	/* build block that returns */
	LLVMPositionBuilderAtEnd(b, b_out);

	{
		LLVMValueRef v_off = l_load(b, TypeSizeT, v_offp, "");
		LLVMValueRef v_flags;

		LLVMBuildStore(b, l_int16_const(lc, natts), v_nvalidp);
		v_off = LLVMBuildTrunc(b, v_off, LLVMInt32TypeInContext(lc), "");
		LLVMBuildStore(b, v_off, v_slotoffp);
		v_flags = l_load(b, LLVMInt16TypeInContext(lc), v_flagsp, "tts_flags");
		v_flags = LLVMBuildOr(b, v_flags, l_int16_const(lc, TTS_FLAG_SLOW), "");
		LLVMBuildStore(b, v_flags, v_flagsp);
		LLVMBuildRetVoid(b);
	}

	LLVMDisposeBuilder(b);

	return v_deform_fn;
}

#endif
=======

/*
 * ResourceOwner callbacks
 */
static void
ResOwnerReleaseJitContext(Datum res)
{
        CopyJitContext *context = (CopyJitContext *) DatumGetPointer(res);

        context->resowner = NULL;
        jit_release_context(&context->base);
}

>>>>>>> 844fd0c (port to master/pg18)
