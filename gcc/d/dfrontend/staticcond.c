
/* Compiler implementation of the D programming language
 * Copyright (c) 1999-2017 by Digital Mars
 * All Rights Reserved
 * written by Walter Bright
 * http://www.digitalmars.com
 * Distributed under the Boost Software License, Version 1.0.
 * http://www.boost.org/LICENSE_1_0.txt
 * https://github.com/D-Programming-Language/dmd/blob/master/src/staticcond.c
 */

#include "expression.h"
#include "mtype.h"
#include "scope.h"

Expression *semantic(Expression *e, Scope *sc);

/********************************************
 * Semantically analyze and then evaluate a static condition at compile time.
 * This is special because short circuit operators &&, || and ?: at the top
 * level are not semantically analyzed if the result of the expression is not
 * necessary.
 * Params:
 *      exp = original expression, for error messages
 * Returns:
 *      true if evaluates to true
 */

bool evalStaticCondition(Scope *sc, Expression *exp, Expression *e, bool &errors)
{
    unsigned nerrors = global.errors;

    sc = sc->startCTFE();
    sc->flags |= SCOPEcondition;

    e = semantic(e, sc);
    e = resolveProperties(sc, e);

    sc = sc->endCTFE();
    e = e->optimize(WANTvalue);

    if (nerrors != global.errors ||
        e->op == TOKerror ||
        e->type->toBasetype() == Type::terror)
    {
        errors = true;
        return false;
    }

    if (!e->type->isBoolean())
    {
        exp->error("expression %s of type %s does not have a boolean value", exp->toChars(), e->type->toChars());
        errors = true;
        return false;
    }

    e = e->ctfeInterpret();

    if (e->isBool(true))
        return true;
    else if (e->isBool(false))
        return false;

    e->error("expression %s is not constant", e->toChars());
    errors = true;
    return false;
}
