/*
* Copyright 2018-2020 Redis Labs Ltd. and Contributors
*
* This file is available under the Redis Labs Source Available License Agreement
*/

#pragma once

#include "../execution_plan.h"
#include "../../filter_tree/filter_tree.h"
#include "../../arithmetic/algebraic_expression.h"

/* Reorders exps such that exp[i] is the ith expression to evaluate. */
void orderExpressions(
	const QueryGraph *qg,           // QueryGraph containing expression entity data.
	AlgebraicExpression **exps,     // Expressions to order.
	uint exps_count,                // Number of expressions.
	const FT_FilterNode *filters,   // Filters.
	rax *bound_vars                 // Previously-bound variables.
);

