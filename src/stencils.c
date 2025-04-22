
#include "postgres.h"
#include "fmgr.h"

#include "jit/jit.h"

#include "executor/execExpr.h"
#include "executor/tuptable.h"

#include "nodes/execnodes.h"

#include "utils/expandeddatum.h"
#include "utils/memutils.h"
#include "utils/resowner_private.h"


#define goto_next __attribute__((musttail)) return NEXT_CALL(expression, econtext, isNull)

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

extern ExprEvalStep op;

extern Datum FORCE_NEXT_CALL   (struct ExprState *expression, struct ExprContext *econtext, bool *isNull);
extern Datum NEXT_CALL   (struct ExprState *expression, struct ExprContext *econtext, bool *isNull);
extern Datum JUMP_DONE   (struct ExprState *expression, struct ExprContext *econtext, bool *isNull);
extern Datum JUMP_NULL   (struct ExprState *expression, struct ExprContext *econtext, bool *isNull);
extern Datum FUNC_CALL   (FunctionCallInfo fcinfo);

Datum stencil_EEOP_DONE (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
    *isNull = expression->resnull;
    return expression->resvalue;
}

Datum stencil_EEOP_CONST (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	// Not optimal at all I think, but should be fine
    /**op.resnull = 0;
    if (((intptr_t) &CONST_ISNULL) & 0x01)
	    *op.resnull = 1;*/
    *op.resnull  = (char) ((intptr_t) &CONST_ISNULL); // op.d.constval.isnull
    *op.resvalue = (Datum) &CONST_VALUE; // op.d.constval.value;
//    *op.resvalue = (Datum) 0xAABBCCDDEEFF0011; // op.d.constval.value;

	goto_next;
}

Datum stencil_EEOP_CONST_NULL (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
*op.resnull = 1;
*op.resvalue = (Datum) &CONST_VALUE; // op.d.constval.value;
	goto_next;
}

Datum stencil_EEOP_CONST_NOTNULL (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
*op.resnull = 0;
*op.resvalue = (Datum) &CONST_VALUE; // op.d.constval.value;
	goto_next;
}

Datum stencil_EEOP_ASSIGN_TMP (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	RESULTSLOT_VALUES = expression->resvalue;
	RESULTSLOT_ISNULL = expression->resnull;

	goto_next;
}

Datum stencil_EEOP_ASSIGN_TMP_MAKE_RO (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	RESULTSLOT_ISNULL = expression->resnull;
	if (!expression->resnull)
		RESULTSLOT_VALUES = MakeExpandedObjectReadOnlyInternal(expression->resvalue);
	else
		RESULTSLOT_VALUES = expression->resvalue;

	goto_next;
}

Datum stencil_EEOP_FUNCEXPR (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	FunctionCallInfo fcinfo = op.d.func.fcinfo_data;
	Datum d;

	fcinfo->isnull = false;
	d = FUNC_CALL(fcinfo);
	*op.resvalue = d;
	*op.resnull = fcinfo->isnull;

	goto_next;
}

Datum extra_EEOP_FUNCEXPR_STRICT_int4eq (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	FunctionCallInfo fcinfo = op.d.func.fcinfo_data;
	NullableDatum *args = fcinfo->args;

	if (args[0].isnull || args[1].isnull) {
		*op.resnull = true;
	} else {
		*op.resvalue = (DatumGetInt32(args[0].value) == DatumGetInt32(args[1].value));
		*op.resnull = false;
	}
	goto_next;
}

Datum extra_EEOP_FUNCEXPR_STRICT_int4lt (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	FunctionCallInfo fcinfo = op.d.func.fcinfo_data;
	NullableDatum *args = fcinfo->args;

	if (args[0].isnull || args[1].isnull) {
		*op.resnull = true;
	} else {
		*op.resvalue = (DatumGetInt32(args[0].value) < DatumGetInt32(args[1].value));
		*op.resnull = false;
	}
	goto_next;
}

#if 1
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
#else
Datum stencil_EEOP_FUNCEXPR_STRICT (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
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
	*op.resvalue = d;
	*op.resnull = fcinfo->isnull;

strictfail:
	;

	goto_next;
}
#endif
Datum stencil_EEOP_QUAL (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	/* simplified version of BOOL_AND_STEP for use by ExecQual() */

	/* If argument (also result) is false or null ... */
	if (*op.resnull ||
		!DatumGetBool(*op.resvalue))
	{
		/* ... bail out early, returning FALSE */
		*op.resnull = false;
		*op.resvalue = BoolGetDatum(false);

		__attribute__((musttail))
		return JUMP_DONE(expression, econtext, isNull);
	}

	/*
	* Otherwise, leave the TRUE value in place, in case this is the
	* last qual.  Then, TRUE is the correct answer.
	*/

	goto_next;
}

Datum stencil_EEOP_SQLVALUEFUNCTION (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	ExecEvalSQLValueFunction(expression, &op);
	goto_next;
}

Datum stencil_EEOP_SCAN_SYSVAR (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	ExecEvalSysVar(expression, &op, econtext, econtext->ecxt_scantuple);
	goto_next;
}

Datum stencil_EEOP_SCAN_VAR (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	TupleTableSlot *scanslot = econtext->ecxt_scantuple;

	int attnum = op.d.var.attnum;
	*op.resvalue = scanslot->tts_values[attnum];
	*op.resnull = scanslot->tts_isnull[attnum];
	goto_next;
}

Datum stencil_EEOP_SCAN_FETCHSOME (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	TupleTableSlot * scanslot = econtext->ecxt_scantuple;

	// Note : this is were deforming will need to happen
	slot_getsomeattrs(scanslot, op.d.fetch.last_var);

	goto_next;
}

Datum stencil_EEOP_INNER_VAR (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	TupleTableSlot *innerslot = econtext->ecxt_innertuple;

	int attnum = op.d.var.attnum;
	*op.resvalue = innerslot->tts_values[attnum];
	*op.resnull = innerslot->tts_isnull[attnum];
	goto_next;
}

Datum stencil_EEOP_INNER_FETCHSOME (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	TupleTableSlot * innerslot = econtext->ecxt_innertuple;

	// Note : this is were deforming will need to happen
	slot_getsomeattrs(innerslot, op.d.fetch.last_var);

	goto_next;
}

Datum stencil_EEOP_OUTER_VAR (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	TupleTableSlot *outerslot = econtext->ecxt_outertuple;

	/* See EEOP_INNER_VAR comments */
	int attnum = op.d.var.attnum;
	*op.resvalue = outerslot->tts_values[attnum];
	*op.resnull = outerslot->tts_isnull[attnum];
	goto_next;
}

Datum stencil_EEOP_OUTER_FETCHSOME (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	TupleTableSlot * outerslot = econtext->ecxt_outertuple;

	// Note : this is were deforming will need to happen
	slot_getsomeattrs(outerslot, op.d.fetch.last_var);

	goto_next;
}

Datum stencil_EEOP_ASSIGN_SCAN_VAR (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
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
	int			aggno = op.d.aggref.aggno;

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
