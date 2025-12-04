
#include "postgres.h"
#include "fmgr.h"

#include "jit/jit.h"

#include "executor/execExpr.h"
#include "executor/tuptable.h"

#include "nodes/execnodes.h"

#include "utils/expandeddatum.h"
#include "utils/memutils.h"
#if PG_VERSION_NUM < 180000
#include "utils/resowner_private.h"
#endif

#define REGISTER_DEFINITION char nullFlags, intptr_t reg0, intptr_t reg1
#define REGISTER_PASS nullFlags, reg0, reg1

#define SET_REGISTER_VALUE(id,value)

#define goto_next __attribute__((musttail)) return NEXT_CALL(expression, econtext, isNull, REGISTER_PASS)

/*
 * Note : using the ghccc ABI implies calling only functions sharing this ABI.
 * It thus can't be used here.
 * musttail should be enough
 */

extern void CONST_ISNULL;
extern intptr_t CONST_VALUE;
extern int RESULTNUM;
extern int ATTNUM;
extern Datum RESULTSLOT_VALUES;
extern bool RESULTSLOT_ISNULL;
extern NullableDatum FUNC_ARG;
extern void REGISTER_ISNULL;
extern intptr_t REGISTER_VALUE;

extern ExprEvalStep op;

extern Datum FUNC_CALL   (FunctionCallInfo fcinfo);

extern Datum FORCE_NEXT_CALL   (struct ExprState *expression, struct ExprContext *econtext, bool *isNull, REGISTER_DEFINITION);
extern Datum NEXT_CALL   (struct ExprState *expression, struct ExprContext *econtext, bool *isNull, REGISTER_DEFINITION);
extern Datum JUMP_DONE   (struct ExprState *expression, struct ExprContext *econtext, bool *isNull, REGISTER_DEFINITION);
extern Datum JUMP_NULL   (struct ExprState *expression, struct ExprContext *econtext, bool *isNull, REGISTER_DEFINITION);

#define GOTO(target) target(expression, econtext, isNull, REGISTER_PASS)
#define STENCIL(opcode) Datum stencil_##opcode (struct ExprState *expression, struct ExprContext *econtext, bool *isNull, REGISTER_DEFINITION)

#define SELECTOR(stencil,criteria) const char *selector_stencil_ ##stencil = #criteria;

#define BEGIN_REGISTER_CONTRACT(stencil) const char *register_contract_ ##stencil = ""
#define EXPECT(register_id,null_src,value_src) "EXPECT in " #register_id " null:" #null_src " value:" #value_src "\n"
#define EXPECT_FCINFO(fcinfo) "EXPECT_FCINFO " #fcinfo "\n"
#define WRITE(register_id,null_src,value_src) "WRITE in " #register_id " null:" #null_src " value:" #value_src "\n"
#define END_REGISTER_CONTRACT "";



//////////////////////////////////////
///           EEOP_DONE            ///
//////////////////////////////////////

STENCIL(EEOP_DONE)
{
	*isNull = expression->resnull;
	return reg0;
}
BEGIN_REGISTER_CONTRACT(EEOP_DONE)
EXPECT(0, &(expression->resnull), &(expression->resvalue))
END_REGISTER_CONTRACT

//////////////////////////////////////
///           EEOP_CONST           ///
//////////////////////////////////////

STENCIL(EEOP_CONST)
{
	*(op.resnull)  = (char) ((intptr_t) &CONST_ISNULL); // op.d.constval.isnull
	*(op.resvalue) = (Datum) &CONST_VALUE; // op.d.constval.value;
	goto_next;
}

STENCIL(EEOP_CONST__null)
{
	*(op.resnull) = 1;
	*(op.resvalue) = (Datum) &CONST_VALUE; // op.d.constval.value;
	goto_next;
}
SELECTOR(EEOP_CONST__null, op->d.constval.isnull)

STENCIL(EEOP_CONST__notnull)
{
	*(op.resnull) = 0;
	*(op.resvalue) = (Datum) &CONST_VALUE; // op.d.constval.value;
	goto_next;
}
SELECTOR(EEOP_CONST__notnull, !op->d.constval.isnull)

//////////////////////////////////////
///        EEOP_ASSIGN_TMP         ///
//////////////////////////////////////

STENCIL(EEOP_ASSIGN_TMP)
{
	RESULTSLOT_VALUES = expression->resvalue;
	RESULTSLOT_ISNULL = expression->resnull;

	goto_next;
}


//////////////////////////////////////
///    EEOP_ASSIGN_TMP_MAKE_RO     ///
//////////////////////////////////////

STENCIL(EEOP_ASSIGN_TMP_MAKE_RO)
{
	RESULTSLOT_ISNULL = expression->resnull;
	if (!expression->resnull)
		RESULTSLOT_VALUES = MakeExpandedObjectReadOnlyInternal(expression->resvalue);
	else
		RESULTSLOT_VALUES = expression->resvalue;

	goto_next;
}


//////////////////////////////////////
///        EEOP_FUNCEXPR           ///
//////////////////////////////////////

STENCIL(EEOP_FUNCEXPR)
{
	FunctionCallInfo fcinfo = op.d.func.fcinfo_data;
	Datum d;

	fcinfo->isnull = false;
	d = FUNC_CALL(fcinfo);
	reg0 = d;
	if (fcinfo->isnull)
		nullFlags |= (1 << 0);
	else
		nullFlags &= ~(1 << 0);

	goto_next;
}
BEGIN_REGISTER_CONTRACT(EEOP_FUNCEXPR)
EXPECT_FCINFO(op->d.func.fcinfo_data)
WRITE(0, op->resnull, op->resvalue)
END_REGISTER_CONTRACT


//////////////////////////////////////
///      EEOP_FUNCEXPR_STRICT      ///
//////////////////////////////////////

/// Variant with int4eq inlined
STENCIL(EEOP_FUNCEXPR_STRICT__int4eq)
{
	if (nullFlags & 3) {
		// Make sure reg0 is marked as null
		nullFlags |= (1 << 0);
	} else {
		reg0 = (DatumGetInt32(reg0) == DatumGetInt32(reg1));
	}
	goto_next;
}
SELECTOR(EEOP_FUNCEXPR_STRICT__int4eq, op->d.func.fn_addr == &int4eq)
BEGIN_REGISTER_CONTRACT(EEOP_FUNCEXPR_STRICT__int4eq)
EXPECT(0, &(op->d.func.fcinfo_data->args[0].isnull), &(op->d.func.fcinfo_data->args[0].value))
EXPECT(1, &(op->d.func.fcinfo_data->args[1].isnull), &(op->d.func.fcinfo_data->args[1].value))
WRITE(0, op->resnull, op->resvalue)
END_REGISTER_CONTRACT

/// Variant with int4lt inlined
STENCIL(EEOP_FUNCEXPR_STRICT__int4lt)
{
	if (nullFlags & 3) {
		// Make sure reg0 is marked as null
		nullFlags |= (1 << 0);
	} else {
		reg0 = (DatumGetInt32(reg0) < DatumGetInt32(reg1));
	}
	goto_next;
}
SELECTOR(EEOP_FUNCEXPR_STRICT__int4lt, op->d.func.fn_addr == &int4lt)
BEGIN_REGISTER_CONTRACT(EEOP_FUNCEXPR_STRICT__int4lt)
EXPECT(0, &(op->d.func.fcinfo_data->args[0].isnull), &(op->d.func.fcinfo_data->args[0].value))
EXPECT(1, &(op->d.func.fcinfo_data->args[1].isnull), &(op->d.func.fcinfo_data->args[1].value))
WRITE(0, op->resnull, op->resvalue)
END_REGISTER_CONTRACT

#if 0
Datum extra_EEOP_FUNCEXPR_STRICT_CHECKER (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	if (FUNC_ARG.isnull)
	{
		*op.resnull = true;

		__attribute__((musttail))
		return FORCE_NEXT_CALL(expression, econtext, isNull);
	}
	goto_next;
}
#endif

STENCIL(EEOP_FUNCEXPR_STRICT)
{
	FunctionCallInfo fcinfo = op.d.func.fcinfo_data;
	NullableDatum *args = fcinfo->args;
	int			nargs = op.d.func.nargs;
	Datum		d;

	/* strict function, so check for NULL args */
	for (int argno = 0; argno < nargs; argno++)
	{
		if (args[argno].isnull)
		{
			*op.resnull = true;
			goto strictfail;
		}
	}

	fcinfo->isnull = false;
	d = FUNC_CALL(fcinfo);
	reg0 = d;
	if (fcinfo->isnull)
		nullFlags |= (1 << 0);
	else
		nullFlags &= ~(1 << 0);

strictfail:
	;

	goto_next;
}
BEGIN_REGISTER_CONTRACT(EEOP_FUNCEXPR_STRICT)
EXPECT_FCINFO(op->d.func.fcinfo_data)
WRITE(0, op->resnull, op->resvalue)
END_REGISTER_CONTRACT

STENCIL(EEOP_QUAL)
{
	/* simplified version of BOOL_AND_STEP for use by ExecQual() */

	/* If argument (also result) is false or null ... */
	if ((nullFlags & (1 << 0)) ||
		!DatumGetBool(reg0))
	{
		/* ... bail out early, returning FALSE */
		nullFlags &= ~(1 << 0);
		reg0 = BoolGetDatum(false);

		__attribute__((musttail)) return JUMP_DONE(expression, econtext, isNull, REGISTER_PASS);
	}

	/*
	* Otherwise, leave the TRUE value in place, in case this is the
	* last qual.  Then, TRUE is the correct answer.
	*/

	goto_next;
}
BEGIN_REGISTER_CONTRACT(EEOP_QUAL)
EXPECT(0, op->resnull, op->resvalue)
WRITE(0, op->resnull, op->resvalue)
END_REGISTER_CONTRACT

STENCIL(EEOP_SQLVALUEFUNCTION)
{
	ExecEvalSQLValueFunction(expression, &op);
	goto_next;
}

STENCIL(EEOP_SCAN_SYSVAR)
{
	ExecEvalSysVar(expression, &op, econtext, econtext->ecxt_scantuple);
	goto_next;
}

STENCIL(EEOP_SCAN_VAR)
{
	TupleTableSlot *scanslot = econtext->ecxt_scantuple;

	int attnum = op.d.var.attnum;
	reg0 = scanslot->tts_values[attnum];
	if (scanslot->tts_isnull[attnum])
		nullFlags |= (1 << 0);
	else
		nullFlags &= ~(1 << 0);
	goto_next;
}
BEGIN_REGISTER_CONTRACT(EEOP_SCAN_VAR)
WRITE(0, op->resnull, op->resvalue)
END_REGISTER_CONTRACT

STENCIL(EEOP_SCAN_FETCHSOME)
{
	TupleTableSlot * scanslot = econtext->ecxt_scantuple;

	// Note : this is were deforming will need to happen
	slot_getsomeattrs(scanslot, op.d.fetch.last_var);

	goto_next;
}


// Need a cleaner way to register reg functions
STENCIL(extra_set_reg0_null)
{
	nullFlags |= 1;
	goto_next;
}

STENCIL(extra_set_reg0_const)
{
	reg0 = (Datum) &REGISTER_VALUE;
	nullFlags &= 0xfe;
	goto_next;
}

STENCIL(extra_set_reg0_value)
{
	reg0 = (Datum) &REGISTER_VALUE;
	if (&REGISTER_ISNULL)
		nullFlags |= 1;
	else
		nullFlags &= 0xfe;
	goto_next;
}

STENCIL(extra_set_reg1_null)
{
	nullFlags |= 2;
	goto_next;
}

STENCIL(extra_set_reg1_const)
{
	reg1 = (Datum) &REGISTER_VALUE;
	nullFlags &= 0xfd;
	goto_next;
}

STENCIL(extra_set_reg1_value)
{
	reg1 = (Datum) &REGISTER_VALUE;
	if (&REGISTER_ISNULL)
		nullFlags |= 2;
	else
		nullFlags &= 0xfd;
	goto_next;
}

// Will it be ever used?
STENCIL(extra_swap_reg0_reg1)
{
	bool old_reg0_null = (nullFlags & 1);
	bool old_reg1_null = (nullFlags & 2);
	nullFlags &= 0xfc;
	nullFlags += old_reg1_null + (old_reg0_null * 2);

	Datum old_reg0_value = reg0;
	reg0 = reg1;
	reg1 = old_reg0_value;

	goto_next;
}
#if 0
STENCIL(EEOP_INNER_VAR)
{
	TupleTableSlot *innerslot = econtext->ecxt_innertuple;

	int attnum = op.d.var.attnum;
	*op.resvalue = innerslot->tts_values[attnum];
	*op.resnull = innerslot->tts_isnull[attnum];
	goto_next;
}

STENCIL(EEOP_INNER_FETCHSOME)
{
	TupleTableSlot * innerslot = econtext->ecxt_innertuple;

	// Note : this is were deforming will need to happen
	slot_getsomeattrs(innerslot, op.d.fetch.last_var);

	goto_next;
}

STENCIL(EEOP_OUTER_VAR)
{
	TupleTableSlot *outerslot = econtext->ecxt_outertuple;

	/* See EEOP_INNER_VAR comments */
	int attnum = op.d.var.attnum;
	*op.resvalue = outerslot->tts_values[attnum];
	*op.resnull = outerslot->tts_isnull[attnum];
	goto_next;
}

STENCIL(EEOP_OUTER_FETCHSOME)
{
	TupleTableSlot * outerslot = econtext->ecxt_outertuple;

	// Note : this is were deforming will need to happen
	slot_getsomeattrs(outerslot, op.d.fetch.last_var);

	goto_next;
}

STENCIL(EEOP_ASSIGN_SCAN_VAR)
{
	TupleTableSlot *scanslot = econtext->ecxt_scantuple;

	/*
	* We do not need CheckVarSlotCompatibility here; that was taken
	* care of at compilation time.  But see EEOP_INNER_VAR comments.
	*/
	RESULTSLOT_VALUES = scanslot->tts_values[op.d.assign_var.attnum];
	RESULTSLOT_ISNULL = scanslot->tts_isnull[op.d.assign_var.attnum];

	goto_next;
}


Datum stencil_EEOP_NULLTEST_ISNULL (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	*op.resvalue = BoolGetDatum(*op.resnull);
	*op.resnull = false;

	goto_next;
}

Datum stencil_EEOP_NULLTEST_ISNOTNULL (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	*op.resvalue = BoolGetDatum(!*op.resnull);
	*op.resnull = false;

	goto_next;
}

Datum stencil_EEOP_ASSIGN_INNER_VAR (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	TupleTableSlot *innerslot = econtext->ecxt_innertuple;

	RESULTSLOT_VALUES = innerslot->tts_values[op.d.assign_var.attnum];
	RESULTSLOT_ISNULL = innerslot->tts_isnull[op.d.assign_var.attnum];
	goto_next;
}

Datum stencil_EEOP_ASSIGN_OUTER_VAR (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	TupleTableSlot *outerslot = econtext->ecxt_outertuple;

	RESULTSLOT_VALUES = outerslot->tts_values[op.d.assign_var.attnum];
	RESULTSLOT_ISNULL = outerslot->tts_isnull[op.d.assign_var.attnum];
	goto_next;
}

Datum stencil_EEOP_SCALARARRAYOP (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	ExecEvalScalarArrayOp(expression, &op);
	goto_next;
}

Datum stencil_EEOP_CASE_TESTVAL (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	if (op.d.casetest.value)
	{
		*op.resvalue = *op.d.casetest.value;
		*op.resnull = *op.d.casetest.isnull;
	}
	else
	{
		*op.resvalue = econtext->caseValue_datum;
		*op.resnull = econtext->caseValue_isNull;
	}

	goto_next;
}

Datum stencil_EEOP_JUMP_IF_NOT_TRUE (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	if (*op.resnull || !DatumGetBool(*op.resvalue))
		__attribute__((musttail))
		return JUMP_DONE(expression, econtext, isNull);

	goto_next;

}

Datum stencil_EEOP_JUMP (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	goto_next;
}

Datum stencil_EEOP_DISTINCT (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	FunctionCallInfo fcinfo = op.d.func.fcinfo_data;

	/* check function arguments for NULLness */
	if (fcinfo->args[0].isnull && fcinfo->args[1].isnull)
	{
		/* Both NULL? Then is not distinct... */
		*op.resvalue = BoolGetDatum(false);
		*op.resnull = false;
	}
	else if (fcinfo->args[0].isnull || fcinfo->args[1].isnull)
	{
		/* Only one is NULL? Then is distinct... */
		*op.resvalue = BoolGetDatum(true);
		*op.resnull = false;
	}
	else
	{
		/* Neither null, so apply the equality function */
		Datum		eqresult;

		fcinfo->isnull = false;
		eqresult = op.d.func.fn_addr(fcinfo);
		/* Must invert result of "="; safe to do even if null */
		*op.resvalue = BoolGetDatum(!DatumGetBool(eqresult));
		*op.resnull = fcinfo->isnull;
	}

	goto_next;
}

Datum stencil_EEOP_NOT_DISTINCT (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	FunctionCallInfo fcinfo = op.d.func.fcinfo_data;

	/* check function arguments for NULLness */
	if (fcinfo->args[0].isnull && fcinfo->args[1].isnull)
	{
		/* Both NULL? Then is not distinct... */
		*op.resvalue = BoolGetDatum(true);
		*op.resnull = false;
	}
	else if (fcinfo->args[0].isnull || fcinfo->args[1].isnull)
	{
		/* Only one is NULL? Then is distinct... */
		*op.resvalue = BoolGetDatum(false);
		*op.resnull = false;
	}
	else
	{
		/* Neither null, so apply the equality function */
		Datum		eqresult;

		fcinfo->isnull = false;
		eqresult = op.d.func.fn_addr(fcinfo);
		*op.resvalue = eqresult;
		*op.resnull = fcinfo->isnull;
	}

	goto_next;
}

Datum stencil_EEOP_PARAM_EXEC (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	ExecEvalParamExec(expression, &op, econtext);
	goto_next;
}

Datum stencil_EEOP_PARAM_EXTERN (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	ExecEvalParamExtern(expression, &op, econtext);
	goto_next;
}

Datum stencil_EEOP_AGGREF (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
#if PG_VERSION_NUM < 140000
	int			aggno = op.d.aggref.astate->aggno;
#else
	int			aggno = op.d.aggref.aggno;
#endif
	*op.resvalue = econtext->ecxt_aggvalues[aggno];
	*op.resnull = econtext->ecxt_aggnulls[aggno];

	goto_next;
}

static pg_attribute_always_inline void
ExecAggPlainTransByVal(AggState *aggstate, AggStatePerTrans pertrans,
					   AggStatePerGroup pergroup,
					   ExprContext *aggcontext, int setno)
{
	FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
	MemoryContext oldContext;
	Datum		newVal;

	/* cf. select_current_set() */
	aggstate->curaggcontext = aggcontext;
	aggstate->current_set = setno;

	/* set up aggstate->curpertrans for AggGetAggref() */
	aggstate->curpertrans = pertrans;

	/* invoke transition function in per-tuple context */
	oldContext = MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);

	fcinfo->args[0].value = pergroup->transValue;
	fcinfo->args[0].isnull = pergroup->transValueIsNull;
	fcinfo->isnull = false;		/* just in case transfn doesn't set it */

	newVal = FunctionCallInvoke(fcinfo);

	pergroup->transValue = newVal;
	pergroup->transValueIsNull = fcinfo->isnull;

	MemoryContextSwitchTo(oldContext);
}

Datum stencil_EEOP_AGG_PLAIN_TRANS_STRICT_BYVAL (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	AggState   *aggstate = castNode(AggState, expression->parent);
	AggStatePerTrans pertrans = op.d.agg_trans.pertrans;
	AggStatePerGroup pergroup = &aggstate->all_pergroups[op.d.agg_trans.setoff][op.d.agg_trans.transno];

	Assert(pertrans->transtypeByVal);

	if (likely(!pergroup->transValueIsNull))
		ExecAggPlainTransByVal(aggstate, pertrans, pergroup,
								op.d.agg_trans.aggcontext,
								op.d.agg_trans.setno);


	goto_next;
}

Datum stencil_EEOP_AGG_PLAIN_PERGROUP_NULLCHECK (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	AggState   *aggstate = castNode(AggState, expression->parent);
	AggStatePerGroup pergroup_allaggs =
		aggstate->all_pergroups[op.d.agg_plain_pergroup_nullcheck.setoff];

	if (pergroup_allaggs == NULL)
		__attribute__((musttail))
		return JUMP_NULL(expression, econtext, isNull);

	goto_next;
}

Datum stencil_EEOP_AGG_STRICT_INPUT_CHECK_ARGS (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	NullableDatum *args = op.d.agg_strict_input_check.args;
	int			nargs = op.d.agg_strict_input_check.nargs;

	for (int argno = 0; argno < nargs; argno++)
	{
		if (args[argno].isnull)
			__attribute__((musttail))
			return JUMP_NULL(expression, econtext, isNull);
	}
	goto_next;
}

#endif
