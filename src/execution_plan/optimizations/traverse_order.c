/*
* Copyright 2018-2020 Redis Labs Ltd. and Contributors
*
* This file is available under the Redis Labs Source Available License Agreement
*/

#include "./traverse_order.h"
#include "../../util/arr.h"
#include "../../util/strcmp.h"
#include "../../util/rmalloc.h"
#include <assert.h>

#define T 1           // Transpose penalty.
#define L 2 * T       // Label score.
#define F 4 * T       // Filter score.
#define B 8 * F       // Bound variable bonus.

// Print arrangement.
static inline void _Arrangement_Print(AlgebraicExpression **arrangement, uint size) {
	printf("Arrangement_Print\n");
	for(uint i = 0; i < size; i++) {
		AlgebraicExpression *exp = arrangement[i];
		printf("%d, src: %s, dest: %s\n", i, AlgebraicExpression_Source(exp),
			   AlgebraicExpression_Destination(exp));
	}
}

static bool _valid_position(AlgebraicExpression **exps, uint exp_count, int pos, QueryGraph *qg) {
	AlgebraicExpression *exp = exps[pos];
	if(pos == 0) {
		// The first operand cannot be a variable-length edge with a labeled source or destination,
		// as a labeled endpoint must be first to get replaced by a LabelScan op.
		QGNode *src = QueryGraph_GetNodeByAlias(qg, AlgebraicExpression_Source(exp));
		QGNode *dest = QueryGraph_GetNodeByAlias(qg, AlgebraicExpression_Destination(exp));
		if((src->label || dest->label) &&
		   AlgebraicExpression_Edge(exp) &&
		   AlgebraicExpression_OperandCount(exp) == 1) return false;
		// Since the first operand has no previous variables to check, it is assumed to be valid.
		return true;
	}

	const char *src = AlgebraicExpression_Source(exp);
	const char *dest = AlgebraicExpression_Destination(exp);
	// Iterate over all previous expressions to make sure at least one of this expression's endpoints are bound.
	for(int i = pos - 1; i >= 0; i --) {
		AlgebraicExpression *prev = exps[i];
		const char *prev_src = AlgebraicExpression_Source(prev);
		const char *prev_dest = AlgebraicExpression_Destination(prev);
		if(!RG_STRCMP(prev_src, src)     ||
		   !RG_STRCMP(prev_dest, src)    ||
		   !RG_STRCMP(prev_src, dest)    ||
		   !RG_STRCMP(prev_dest, dest)) return true;
	}
	return false;
}

// Promote a selected operand to the given position while otherwise maintaining the optimal order.
static void _promote_next_operand(AlgebraicExpression **exps, uint exp_count, uint to, uint from) {
	AlgebraicExpression *tmp = exps[to];
	// Migrate 'from' to 'to'
	exps[to] = exps[from];
	// Move all expressions between 'to' and 'from' 1 to the right.
	for(uint i = to + 1; i < from; i ++) exps[i + 1] = exps[i];
	// Set the replaced expression directly after its original position.
	exps[to + 1] = tmp;
}

static void _order_expressions(AlgebraicExpression **exps, uint exp_count, QueryGraph *qg) {
	int first_operand = 0;
	for(uint i = 0; i < exp_count - 1; i ++) {
		int j = i + 1;
		while(!_valid_position(exps, exp_count, i, qg)) {
			if(j >= exp_count) {
				// Failed to resolve a valid sequence with the first operand,
				// swap the first operand and reset.
				i = 0;
				first_operand++;
				_promote_next_operand(exps, exp_count, 0, first_operand);
				break;
			}
			_promote_next_operand(exps, exp_count, i, j);
			j++;
		}
	}
}

static int _score_arrangement(AlgebraicExpression **arrangement, uint exp_count, QueryGraph *qg,
							  rax *filtered_entities, rax *bound_vars) {
	int score = 0;

	// A bit naive at the moment.
	for(uint i = 0; i < exp_count; i++) {
		AlgebraicExpression *exp = arrangement[i];

		// Reward bound variables such that any expression with a bound variable
		// will be preferred over any expression without.
		if(bound_vars) {
			if(raxFind(bound_vars, (unsigned char *)AlgebraicExpression_Source(exp),
					   strlen(AlgebraicExpression_Source(exp))) != raxNotFound) {
				score += B * (exp_count - i);
			}

			if(raxFind(bound_vars, (unsigned char *)AlgebraicExpression_Destination(exp),
					   strlen(AlgebraicExpression_Destination(exp))) != raxNotFound) {
				score += B * (exp_count - i);
			}
		}

		// Reward filters in expression.
		if(raxFind(filtered_entities, (unsigned char *)AlgebraicExpression_Source(exp),
				   strlen(AlgebraicExpression_Source(exp))) != raxNotFound) {
			score += F * (exp_count - i);
		}
		if(raxFind(filtered_entities, (unsigned char *)AlgebraicExpression_Destination(exp),
				   strlen(AlgebraicExpression_Destination(exp))) != raxNotFound) {
			score += F * (exp_count - i);
		}
		QGNode *src = QueryGraph_GetNodeByAlias(qg,
												AlgebraicExpression_Source(exp)); // TODO unwisely expensive
		if(src->label) score += L * (exp_count - i);
	}

	return score;
}

// Transpose out-of-order expressions so that each expresson's source is resolved
// in the winning sequence.
static void _resolve_winning_sequence(AlgebraicExpression **exps, uint exp_count) {
	for(uint i = 1; i < exp_count; i ++) {
		AlgebraicExpression *exp = exps[i];
		bool src_resolved = false;

		// See if source is already resolved.
		for(int j = i - 1; j >= 0; j--) {
			AlgebraicExpression *prev_exp = exps[j];
			if(!RG_STRCMP(AlgebraicExpression_Source(prev_exp), AlgebraicExpression_Source(exp)) ||
			   !RG_STRCMP(AlgebraicExpression_Destination(prev_exp), AlgebraicExpression_Source(exp))) {
				src_resolved = true;
				break;
			}
		}
		if(!src_resolved) AlgebraicExpression_Transpose(exps + i);
	}
}

/* Having chosen which algebraic expression will be evaluated first, determine whether
 * it is worthwhile to transpose it and thus swap the source and destination.
 * If the source is bounded, we will not transpose, if only the destination is bounded, we will.
 * If neither are bounded, we fall back to label and filter heuristics.
 * We'll choose to transpose if the destination is filtered and the source is not, or
 * if neither is filtered, if the destination is labeled and the source is not. */
static void _select_entry_point(QueryGraph *qg, AlgebraicExpression **ae, rax *filtered_entities,
								rax *bound_vars) {
	if(AlgebraicExpression_OperandCount(*ae) == 1 &&
	   !RG_STRCMP(AlgebraicExpression_Source(*ae), AlgebraicExpression_Destination(*ae))) return;

	// Always start at a bound variable if one is present.
	if(bound_vars) {
		if(raxFind(bound_vars, (unsigned char *)AlgebraicExpression_Source(*ae),
				   strlen(AlgebraicExpression_Source(*ae))) != raxNotFound) return;

		if(raxFind(bound_vars, (unsigned char *)AlgebraicExpression_Destination(*ae),
				   strlen(AlgebraicExpression_Destination(*ae))) != raxNotFound) {
			AlgebraicExpression_Transpose(ae);
			return;
		}
	}

	// See if either source or destination nodes are filtered.
	if(raxFind(filtered_entities, (unsigned char *)AlgebraicExpression_Source(*ae),
			   strlen(AlgebraicExpression_Source(*ae))) != raxNotFound) {
		return; // The source node is filtered, making the current order most appealing.
	}

	if(raxFind(filtered_entities, (unsigned char *)AlgebraicExpression_Destination(*ae),
			   strlen(AlgebraicExpression_Destination(*ae))) != raxNotFound) {
		AlgebraicExpression_Transpose(ae); // The destination is filtered and the source is not, transpose.
		return;
	}

	/* Prefer filter over label
	 * if no filters are applied prefer labeled entity. */
	QGNode *src = QueryGraph_GetNodeByAlias(qg, AlgebraicExpression_Source(*ae));
	QGNode *dest = QueryGraph_GetNodeByAlias(qg, AlgebraicExpression_Destination(*ae));
	bool srcLabeled = src->label != NULL;
	bool destLabeled = dest->label != NULL;

	/* TODO: when additional statistics are available
	 * do not use label scan if for every node N such that
	 * (N)-[relation]->(T) N is of the same type T, and type of
	 * either source or destination node is T. */
	if(srcLabeled) {
		// Neither end is filtered and the source is labeled, making the current order most appealing.
		return;
	} else if(destLabeled) {
		// The destination is labeled and the source is not, transpose.
		AlgebraicExpression_Transpose(ae);
	}
}

// Insertion sort to order exps arrays by score descending.
static void _sort_exp_array(AlgebraicExpression **exps, int *exp_scores, int exp_count) {
	for(int i = 1; i < exp_count; i ++) {
		int current_score = exp_scores[i];
		AlgebraicExpression *current_exp = exps[i];
		int j = i - 1;

		while(j >= 0 && exp_scores[j] < current_score) {
			exp_scores[j + 1] = exp_scores[j];
			exps[j + 1] = exps[j];
			j --;
		}
		exp_scores[j + 1] = current_score;
		exps[j + 1] = current_exp;
	}
}

// Score each individual expression and reorder the array in order of score.
static void _sort_exps_by_score(AlgebraicExpression **exps, uint exp_count, QueryGraph *qg,
								rax *filtered_entities, rax *bound_vars) {
	int exp_scores[exp_count];
	// Score each individual expression.
	for(uint i = 0; i < exp_count; i ++) {
		exp_scores[i] = _score_arrangement(&exps[i], 1, qg, filtered_entities, bound_vars);
	}

	// Sort expressions array in order of descending score.
	_sort_exp_array(exps, exp_scores, exp_count);
}

/* Given a set of algebraic expressions representing a graph traversal
 * we pick the order in which the expressions will be evaluated
 * taking into account filters and transposes.
 * exps will reordered. */
void orderExpressions(QueryGraph *qg, AlgebraicExpression **exps, uint exp_count,
					  const FT_FilterNode *filters, rax *bound_vars) {
	assert(exps && exp_count > 0);

	/* Return early if we only have one expression that represents a scan rather than a traversal.
	 * e.g. MATCH (n:L) RETURN n */
	if(exp_count == 1 && AlgebraicExpression_OperandCount(exps[0]) == 1 &&
	   !RG_STRCMP(AlgebraicExpression_Source(exps[0]), AlgebraicExpression_Destination(exps[0]))) return;

	// Collect all filtered aliases.
	rax *filtered_entities = FilterTree_CollectModified(filters);

	/* If we only have one arrangement, we still want to select the optimal entry point
	 * but have no other work to do. */
	if(exp_count == 1) goto select_entry_point;

	// Reorder the expressions by descending score.
	_sort_exps_by_score(exps, exp_count, qg, filtered_entities, bound_vars);

	// Reorder the array until it forms a valid arrangement with an optimal score.
	_order_expressions(exps, exp_count, qg);

	// Transpose expressions as necessary so that the traversals will work in the selected order.
	_resolve_winning_sequence(exps, exp_count);

select_entry_point:
	// Transpose the winning expression if the destination node is a more efficient starting place.
	_select_entry_point(qg, exps + 0, filtered_entities, bound_vars);

	raxFree(filtered_entities);
}

