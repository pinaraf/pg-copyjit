
#include "postgres.h"
#include "fmgr.h"

#include "jit/jit.h"

#include "executor/execExpr.h"
#include "executor/tuptable.h"

#include "nodes/execnodes.h"

#include "utils/expandeddatum.h"
#include "utils/memutils.h"
#include "utils/resowner_private.h"


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
extern int FUNC_NARGS;

extern ExprEvalStep op;

extern Datum NEXT_CALL (struct ExprState *expression, struct ExprContext *econtext, bool *isNull);
extern Datum JUMP_DONE (struct ExprState *expression, struct ExprContext *econtext, bool *isNull);
extern Datum FUNC_CALL (FunctionCallInfo fcinfo);

Datum stencil_EEOP_CONST (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	// Not optimal at all I think, but should be fine
    *op.resnull = (char) ((intptr_t) &CONST_ISNULL); // op.d.constval.isnull;
    *op.resvalue = (Datum) &CONST_VALUE; // op.d.constval.value;

    __attribute__((musttail))
    return NEXT_CALL(expression, econtext, isNull);
}

Datum stencil_EEOP_ASSIGN_TMP (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	RESULTSLOT_VALUES = expression->resvalue;
	RESULTSLOT_ISNULL = expression->resnull;

    __attribute__((musttail))
    return NEXT_CALL(expression, econtext, isNull);
}

Datum stencil_EEOP_ASSIGN_TMP_MAKE_RO (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	RESULTSLOT_ISNULL = expression->resnull;
	if (!expression->resnull)
		RESULTSLOT_VALUES = MakeExpandedObjectReadOnlyInternal(expression->resvalue);
	else
		RESULTSLOT_VALUES = expression->resvalue;

    __attribute__((musttail))
    return NEXT_CALL(expression, econtext, isNull);
}

Datum stencil_EEOP_DONE (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
    *isNull = expression->resnull;
    return expression->resvalue;
}

Datum stencil_EEOP_FUNCEXPR (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	FunctionCallInfo fcinfo = op.d.func.finfo;
	Datum d;

	fcinfo->isnull = false;
	d = FUNC_CALL(fcinfo);
	*op.resvalue = d;
	*op.resnull = fcinfo->isnull;

    __attribute__((musttail))
    return NEXT_CALL(expression, econtext, isNull);
}

Datum stencil_EEOP_FUNCEXPR_STRICT (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	FunctionCallInfo fcinfo = op.d.func.finfo;
	NullableDatum *args = fcinfo->args;
	int			nargs = (int) ((intptr_t) &FUNC_NARGS);
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

    __attribute__((musttail))
    return NEXT_CALL(expression, econtext, isNull);
}

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

    __attribute__((musttail))
    return NEXT_CALL(expression, econtext, isNull);
}

Datum stencil_EEOP_SCAN_VAR (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	TupleTableSlot * scanslot = econtext->ecxt_scantuple;

	/* See EEOP_INNER_VAR comments */

	*op.resvalue = scanslot->tts_values[(intptr_t) &ATTNUM];
	*op.resnull = scanslot->tts_isnull[(intptr_t) &ATTNUM];

	__attribute__((musttail))
    return NEXT_CALL(expression, econtext, isNull);
}
#if 1
Datum stencil_EEOP_SCAN_FETCHSOME (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	TupleTableSlot * scanslot = econtext->ecxt_scantuple;
	// Not implemented, not needed, don't care, don't know ? CheckOpSlotCompatibility(op, scanslot);

	// Note : this is were deforming will need to happen
	slot_getsomeattrs(scanslot, op.d.fetch.last_var);

	__attribute__((musttail))
    return NEXT_CALL(expression, econtext, isNull);
}
#else
Datum stencil_EEOP_ASSIGN_SCAN_VAR (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	TupleTableSlot *scanslot = econtext->ecxt_scantuple;

	/*
	* We do not need CheckVarSlotCompatibility here; that was taken
	* care of at compilation time.  But see EEOP_INNER_VAR comments.
	*/
	RESULTSLOT_VALUES = scanslot->tts_values[(intptr_t) &ATTNUM];
	RESULTSLOT_ISNULL = scanslot->tts_isnull[(intptr_t) &ATTNUM];

	__attribute__((musttail))
    return NEXT_CALL(expression, econtext, isNull);
}


Datum stencil_EEOP_NULLTEST_ISNULL (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	*op.resvalue = BoolGetDatum(*op.resnull);
	*op.resnull = false;

	__attribute__((musttail))
    return NEXT_CALL(expression, econtext, isNull);
}


Datum stencil_EEOP_NULLTEST_ISNOTNULL (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	*op.resvalue = BoolGetDatum(!*op.resnull);
	*op.resnull = false;

	__attribute__((musttail))
    return NEXT_CALL(expression, econtext, isNull);
}

Datum stencil_EEOP_ASSIGN_INNER_VAR (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	TupleTableSlot *innerslot = econtext->ecxt_innertuple;

	RESULTSLOT_VALUES = innerslot->tts_values[(intptr_t) &ATTNUM];
	RESULTSLOT_ISNULL = innerslot->tts_isnull[(intptr_t) &ATTNUM];
}

Datum stencil_EEOP_ASSIGN_OUTER_VAR (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	TupleTableSlot *outerslot = econtext->ecxt_outertuple;

	RESULTSLOT_VALUES = outerslot->tts_values[(intptr_t) &ATTNUM];
	RESULTSLOT_ISNULL = outerslot->tts_isnull[(intptr_t) &ATTNUM];
}
#endif
