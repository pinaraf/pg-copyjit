
#include "postgres.h"
#include "fmgr.h"

#include "jit/jit.h"
#include "executor/execExpr.h"
#include "nodes/execnodes.h"

#include "utils/expandeddatum.h"
#include "utils/memutils.h"
#include "utils/resowner_private.h"


/*
 * Note : abi ghccc implique de n'appeler que des fonctions sur cette même ABI.
 * Donc impossible à utiliser ici.
 * Le musttail devrait être suffisant.
 */
/*
extern uintptr_t CONST_ISNULL;
extern uintptr_t CONST_VALUE;

extern int RESULTNUM;*/

extern ExprEvalStep op;

extern Datum NEXT_CALL (struct ExprState *expression, struct ExprContext *econtext, bool *isNull);

Datum stencil_EEOP_CONST (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
    *op.resnull = op.d.constval.isnull; /// TODO (bool*) CONST_ISNULL; // op->d.constval.isnull;
    *op.resvalue = op.d.constval.value; /// TODO (Datum *) CONST_VALUE; // op->d.constval.value;	// Hoo, so unsure

    __attribute__((musttail))
    return NEXT_CALL(expression, econtext, isNull);
}

Datum stencil_EEOP_ASSIGN_TMP (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	int			resultnum = op.d.assign_tmp.resultnum;
	//int			resultnum = (int) &RESULTNUM;
    TupleTableSlot *resultslot = expression->resultslot;

	resultslot->tts_values[resultnum] = expression->resvalue;
	resultslot->tts_isnull[resultnum] = expression->resnull;

    __attribute__((musttail))
    return NEXT_CALL(expression, econtext, isNull);
}

Datum stencil_EEOP_ASSIGN_TMP_MAKE_RO (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	int			resultnum = op.d.assign_tmp.resultnum;
    TupleTableSlot *resultslot = expression->resultslot;

	resultslot->tts_isnull[resultnum] = expression->resnull;
	if (!expression->resnull)
		resultslot->tts_values[resultnum] = MakeExpandedObjectReadOnlyInternal(expression->resvalue);
	else
		resultslot->tts_values[resultnum] = expression->resvalue;

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
	FunctionCallInfo fcinfo = op.d.func.fcinfo_data;
	Datum d;

	fcinfo->isnull = false;
	d = op.d.func.fn_addr(fcinfo);
	*op.resvalue = d;
	*op.resnull = fcinfo->isnull;

    __attribute__((musttail))
    return NEXT_CALL(expression, econtext, isNull);
}

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
	d = op.d.func.fn_addr(fcinfo);
	*op.resvalue = d;
	*op.resnull = fcinfo->isnull;

strictfail:
	;

    __attribute__((musttail))
    return NEXT_CALL(expression, econtext, isNull);
}


extern Datum JUMP_DONE (struct ExprState *expression, struct ExprContext *econtext, bool *isNull);

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
		//EEO_JUMP(op->d.qualexpr.jumpdone);
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


// Needed for SELECT a FROM b :
/*
WARNING:  Need to build an EEOP_SCAN_FETCHSOME - 3 opcode at 94264824831560
WARNING:  UNSUPPORTED OPCODE
WARNING:  Need to build an EEOP_ASSIGN_SCAN_VAR - 13 opcode at 94264824831624
WARNING:  UNSUPPORTED OPCODE
WARNING:  Need to build an EEOP_DONE - 0 opcode at 94264824831688
*/


