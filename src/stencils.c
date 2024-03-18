
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
 * Note : abi ghccc implique de n'appeler que des fonctions sur cette même ABI.
 * Donc impossible à utiliser ici.
 * Le musttail devrait être suffisant.
 */

extern void CONST_ISNULL;
extern uintptr_t CONST_VALUE;
extern int RESULTNUM;
extern int ATTNUM;
extern Datum RESULTSLOT_VALUES;
extern bool RESULTSLOT_ISNULL;

extern ExprEvalStep op;

extern Datum NEXT_CALL (struct ExprState *expression, struct ExprContext *econtext, bool *isNull);
extern Datum JUMP_DONE (struct ExprState *expression, struct ExprContext *econtext, bool *isNull);
extern Datum FUNC_CALL (FunctionCallInfo fcinfo);

Datum stencil_EEOP_CONST (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
	// Not optimal at all I think, but should be fine
    *op.resnull = (char) ((uintptr_t) &CONST_ISNULL); // op.d.constval.isnull;
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
	FunctionCallInfo fcinfo = op.d.func.fcinfo_data;
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
	int                     attnum = op.d.var.attnum;

	/* See EEOP_INNER_VAR comments */

	*op.resvalue = scanslot->tts_values[attnum];
	*op.resnull = scanslot->tts_isnull[attnum];

	__attribute__((musttail))
    return NEXT_CALL(expression, econtext, isNull);

}

Datum stencil_EEOP_SCAN_FETCHSOME (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{

	TupleTableSlot * scanslot = econtext->ecxt_scantuple;
	// Not implemented, not needed, don't care, don't know ? CheckOpSlotCompatibility(op, scanslot);

	slot_getsomeattrs(scanslot, op.d.fetch.last_var);

	__attribute__((musttail))
    return NEXT_CALL(expression, econtext, isNull);

}

Datum stencil_EEOP_ASSIGN_SCAN_VAR (struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{

#if 0
	/*
 2b0:   48 8b 47 10             mov    0x10(%rdi),%rax
 2b4:   48 8b 4e 08             mov    0x8(%rsi),%rcx
 2b8:   49 b8 00 00 00 00 00    movabs $0x0,%r8
 2bf:   00 00 00
 2c2:   4d 63 48 18             movslq 0x18(%r8),%r9
 2c6:   4d 63 40 1c             movslq 0x1c(%r8),%r8
 2ca:   4c 8b 51 18             mov    0x18(%rcx),%r10
 2ce:   4f 8b 14 c2             mov    (%r10,%r8,8),%r10
 2d2:   4c 8b 58 18             mov    0x18(%rax),%r11
 2d6:   4f 89 14 cb             mov    %r10,(%r11,%r9,8)
 2da:   48 8b 49 20             mov    0x20(%rcx),%rcx
 2de:   42 0f b6 0c 01          movzbl (%rcx,%r8,1),%ecx
 2e3:   48 8b 40 20             mov    0x20(%rax),%rax
 2e7:   42 88 0c 08             mov    %cl,(%rax,%r9,1)
*/
	TupleTableSlot *resultslot = expression->resultslot;
	TupleTableSlot *scanslot = econtext->ecxt_scantuple;
	int			resultnum = op.d.assign_var.resultnum;
	int			attnum = op.d.assign_var.attnum;

	/*
	* We do not need CheckVarSlotCompatibility here; that was taken
	* care of at compilation time.  But see EEOP_INNER_VAR comments.
	*/
	resultslot->tts_values[resultnum] = scanslot->tts_values[attnum];
	resultslot->tts_isnull[resultnum] = scanslot->tts_isnull[attnum];
#else
	/*
 2d0:   48 8b 46 08             mov    0x8(%rsi),%rax
 2d4:   48 8b 48 18             mov    0x18(%rax),%rcx
 2d8:   49 b8 00 00 00 00 00    movabs $0x0,%r8
 2df:   00 00 00
 2e2:   4d 63 00                movslq (%r8),%r8
 2e5:   4a 8b 0c c1             mov    (%rcx,%r8,8),%rcx
 2e9:   49 b9 00 00 00 00 00    movabs $0x0,%r9
 2f0:   00 00 00
 2f3:   49 89 09                mov    %rcx,(%r9)
 2f6:   48 8b 40 20             mov    0x20(%rax),%rax
 2fa:   42 0f b6 04 00          movzbl (%rax,%r8,1),%eax
 2ff:   48 b9 00 00 00 00 00    movabs $0x0,%rcx
 306:   00 00 00
 309:   88 01                   mov    %al,(%rcx)
*/
	TupleTableSlot *scanslot = econtext->ecxt_scantuple;
	int			attnum = op.d.assign_var.attnum;

	/*
	* We do not need CheckVarSlotCompatibility here; that was taken
	* care of at compilation time.  But see EEOP_INNER_VAR comments.
	*/
	RESULTSLOT_VALUES = scanslot->tts_values[attnum];
	RESULTSLOT_ISNULL = scanslot->tts_isnull[attnum];
#endif

	__attribute__((musttail))
    return NEXT_CALL(expression, econtext, isNull);
}
