#include <postgres.h>
#include <catalog/pg_type.h>
#include <nodes/makefuncs.h>
#include <nodes/nodeFuncs.h>
#include <nodes/plannodes.h>
#include <parser/parsetree.h>
#include <utils/guc.h>
#include <optimizer/planner.h>
#include <optimizer/paths.h>
#include <utils/lsyscache.h>

/* This optimizations allows GROUP BY clauses that transform time in
 * order-preserving ways to use indexes on the time field. It works
 * by transforming sorting clauses from their more complex versions
 * to simplified ones that can use the plain index, if the transform
 * is order preserving.
 *
 * For example, an ordering on date_trunc('minute', time) can be transformed
 * to an ordering on time.
 */

extern void sort_transform_optimization(PlannerInfo *root, RelOptInfo *rel);
static Expr *sort_transform_expr(Expr *orig_expr);

static Expr *
transform_date_trunc(FuncExpr *func)
{
	/*
	 * date_trunc (const, var) => var
	 *
	 * proof: date_trunc(c, time1) > date_trunc(c,time2) iff time1 > time2
	 */
	Expr	   *second;

	if (list_length(func->args) != 2 || !IsA(linitial(func->args), Const))
		return (Expr *) func;

	second = sort_transform_expr(lsecond(func->args));
	if (!IsA(second, Var))
		return (Expr *) func;

	return (Expr *) copyObject(second);
}

static Expr *
transform_time_bucket(FuncExpr *func)
{
	/*
	 * time_bucket(const, var, const) => var
	 *
	 * proof: time_bucket(const1, time1) > time_bucket(const1,time2) iff time1
	 * > time2
	 */
	Expr	   *second;

	if (list_length(func->args) != 2 || !IsA(linitial(func->args), Const))
		return (Expr *) func;

	second = sort_transform_expr(lsecond(func->args));
	if (!IsA(second, Var))
		return (Expr *) func;

	return (Expr *) copyObject(second);
}

static Expr *
transform_timestamp_cast(FuncExpr *func)
{
	/*
	 * transform cast from timestamptz to timestamp
	 *
	 * timestamp(var) => var
	 *
	 * proof: timestamp(time1) > timestamp(time2) iff time1 > time2
	 *
	 */

	Expr	   *first;

	if (list_length(func->args) != 1)
		return (Expr *) func;

	first = sort_transform_expr(linitial(func->args));
	if (!IsA(first, Var))
		return (Expr *) func;

	return (Expr *) copyObject(first);
}

static Expr *
transform_timestamptz_cast(FuncExpr *func)
{
	/*
	 * Transform cast from date to timestamptz, or timestamp to timestamptz,
	 * or abstime to timestamptz Handles only single-argument versions of the
	 * cast to avoid explicit timezone specifiers
	 *
	 *
	 * timestamptz(var) => var
	 *
	 * proof: timestamptz(time1) > timestamptz(time2) iff time1 > time2
	 *
	 */

	Expr	   *first;

	if (list_length(func->args) != 1)
		return (Expr *) func;

	first = sort_transform_expr(linitial(func->args));
	if (!IsA(first, Var))
		return (Expr *) func;

	return (Expr *) copyObject(first);
}


static inline Expr *
transform_time_op_const_interval(OpExpr *op)
{
	/*
	 * optimize timestamp(tz) +/- const interval
	 *
	 * Sort of ts + 1 minute fulfilled by sort of ts
	 */
	if (list_length(op->args) == 2 && IsA(lsecond(op->args), Const))
	{
		Oid			left = exprType((Node *) linitial(op->args));
		Oid			right = exprType((Node *) lsecond(op->args));

		if ((left == TIMESTAMPOID && right == INTERVALOID) ||
			(left == TIMESTAMPTZOID && right == INTERVALOID) ||
			(left == DATEOID && right == INTERVALOID))
		{
			char	   *name = get_opname(op->opno);

			if (strncmp(name, "-", NAMEDATALEN) == 0 ||
				strncmp(name, "+", NAMEDATALEN) == 0)
			{
				Expr	   *first = sort_transform_expr((Expr *) linitial(op->args));

				if (IsA(first, Var))
					return copyObject(first);
			}
		}
	}
	return (Expr *) op;
}

static inline Expr *
transform_int_op_const(OpExpr *op)
{
	/*
	 * Optimize int op const (or const op int), whenever possible. e.g. sort
	 * of  some_int + const fulfilled by sort of some_int same for the
	 * following operator: + - / *
	 *
	 * Note that / is not commutative and const / var does NOT work (namely it
	 * reverses sort order, which we don't handle yet)
	 */
	if (list_length(op->args) == 2 &&
		(IsA(lsecond(op->args), Const) ||IsA(linitial(op->args), Const)))
	{
		Oid			left = exprType((Node *) linitial(op->args));
		Oid			right = exprType((Node *) lsecond(op->args));

		if (
			(left == INT8OID && right == INT8OID) ||
			(left == INT4OID && right == INT4OID) ||
			(left == INT2OID && right == INT2OID)
			)
		{
			char	   *name = get_opname(op->opno);

			if (name[1] == '\0')
			{
				switch (name[0])
				{
					case '-':
					case '+':
					case '*':
						/* commutative cases */
						if (IsA(linitial(op->args), Const))
						{
							Expr	   *nonconst = sort_transform_expr((Expr *) lsecond(op->args));

							if (IsA(nonconst, Var))
								return copyObject(nonconst);
						}
						else
						{
							Expr	   *nonconst = sort_transform_expr((Expr *) linitial(op->args));

							if (IsA(nonconst, Var))
								return copyObject(nonconst);

						}
						break;
					case '/':
						/* only if second arg is const */
						if (IsA(lsecond(op->args), Const))
						{
							Expr	   *nonconst = sort_transform_expr((Expr *) linitial(op->args));

							if (IsA(nonconst, Var))
								return copyObject(nonconst);
						}
						break;
				}

			}
		}
	}
	return (Expr *) op;
}


/* sort_transforms_expr returns a simplified sort expression in a form
 * more common for indexes. Must return same data type & collation too.
 *
 * Sort transforms have the following correctness condition:
 *	Any ordering provided by the returned expression is a valid
 *	ordering under the original expression. The reverse need not
 *	be true.
 *
 * Namely if orig_expr(X) > orig_expr(Y) then
 *			 new_expr(X) > new_expr(Y).
 *
 * Note that if orig_expr(X) = orig_expr(Y) then
 *			 the ordering under new_expr is unconstrained.
 * */
static Expr *
sort_transform_expr(Expr *orig_expr)
{
	if (IsA(orig_expr, FuncExpr))
	{
		FuncExpr   *func = (FuncExpr *) orig_expr;
		char	   *func_name = get_func_name(func->funcid);

		if (strncmp(func_name, "date_trunc", NAMEDATALEN) == 0)
			return transform_date_trunc(func);
		if (strncmp(func_name, "time_bucket", NAMEDATALEN) == 0)
			return transform_time_bucket(func);
		if (strncmp(func_name, "timestamp", NAMEDATALEN) == 0)
			return transform_timestamp_cast(func);
		if (strncmp(func_name, "timestamptz", NAMEDATALEN) == 0)
			return transform_timestamptz_cast(func);
	}
	if (IsA(orig_expr, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) orig_expr;
		Oid			type_first = exprType((Node *) linitial(op->args));

		if (type_first == TIMESTAMPOID ||
			type_first == TIMESTAMPTZOID ||
			type_first == DATEOID)
		{
			return transform_time_op_const_interval(op);
		}
		if (type_first == INT2OID || type_first == INT4OID || type_first == INT8OID)
		{
			return transform_int_op_const(op);
		}
	}
	return orig_expr;
}

/*	sort_transform_ec creates a new EquivalenceClass with transformed
 *	expressions if any of the members of the original EC can be transformed for the sort.
 */

static EquivalenceClass *
sort_transform_ec(PlannerInfo *root, EquivalenceClass *orig)
{
	ListCell   *lc_member;
	EquivalenceClass *newec = NULL;

	/* check all members, adding only tranformable members to new ec */
	foreach(lc_member, orig->ec_members)
	{
		EquivalenceMember *ec_mem = (EquivalenceMember *) lfirst(lc_member);
		Expr	   *transformed_expr = sort_transform_expr(ec_mem->em_expr);

		if (transformed_expr != ec_mem->em_expr)
		{
			EquivalenceMember *em;
			Oid			type = exprType((Node *) transformed_expr);
			List	   *opfamilies = list_copy(orig->ec_opfamilies);

			/*
			 * if the transform already exists for even one member, assume
			 * exists for all
			 */
			EquivalenceClass *exist =
			get_eclass_for_sort_expr(root, transformed_expr, ec_mem->em_nullable_relids,
									 opfamilies, type,
									 orig->ec_collation, orig->ec_sortref,
									 ec_mem->em_relids, false);

			if (exist != NULL)
			{
				return exist;
			}

			em = makeNode(EquivalenceMember);

			em->em_expr = transformed_expr;
			em->em_relids = bms_copy(ec_mem->em_relids);
			em->em_nullable_relids = bms_copy(ec_mem->em_nullable_relids);
			em->em_is_const = ec_mem->em_is_const;
			em->em_is_child = ec_mem->em_is_child;
			em->em_datatype = type;

			if (newec == NULL)
			{
				/* lazy create the ec. */
				newec = makeNode(EquivalenceClass);
				newec->ec_opfamilies = opfamilies;
				newec->ec_collation = orig->ec_collation;
				newec->ec_members = NIL;
				newec->ec_sources = list_copy(orig->ec_sources);
				newec->ec_derives = list_copy(orig->ec_derives);
				newec->ec_relids = bms_copy(orig->ec_relids);
				newec->ec_has_const = orig->ec_has_const;
				newec->ec_has_volatile = orig->ec_has_volatile;
				newec->ec_below_outer_join = orig->ec_below_outer_join;
				newec->ec_broken = orig->ec_broken;
				newec->ec_sortref = orig->ec_sortref;
				newec->ec_merged = orig->ec_merged;
			}

			newec->ec_members = lappend(newec->ec_members, em);
		}
	}
	/* if any transforms were found return new ec */
	if (newec != NULL)
	{
		root->eq_classes = lappend(root->eq_classes, newec);
		return newec;
	}
	return NULL;
}

/*
 *	This optimization transforms between equivalent sort operations to try
 *	to find useful indexes.
 *
 *	For example: an ORDER BY date_trunc('minute', time) can be implemented by
 *	an ordering of time.
 */
void
sort_transform_optimization(PlannerInfo *root, RelOptInfo *rel)
{
	/*
	 * We attack this problem in three steps:
	 *
	 * 1) Create a pathkey for the transformed (simplified) sort.
	 *
	 * 2) Use the transformed pathkey to find new useful index paths.
	 *
	 * 3) Transform the  pathkey of the new paths back into the original form
	 * to make this transparent to upper levels in the planner.
	 *
	 * */
	ListCell   *lc_pathkey;
	List	   *transformed_query_pathkey = NIL;
	bool		was_transformed = false;

	/* build transformed query pathkeys */
	foreach(lc_pathkey, root->query_pathkeys)
	{
		PathKey    *pk = lfirst(lc_pathkey);
		EquivalenceClass *transformed = sort_transform_ec(root, pk->pk_eclass);

		if (transformed != NULL)
		{
			PathKey    *newpk = make_canonical_pathkey(root,
													   transformed, pk->pk_opfamily, pk->pk_strategy, pk->pk_nulls_first);

			was_transformed = true;
			transformed_query_pathkey = lappend(transformed_query_pathkey, newpk);
		}
		else
		{
			transformed_query_pathkey = lappend(transformed_query_pathkey, pk);

		}
	}

	if (was_transformed)
	{
		ListCell   *lc_plan;

		/* search for indexes on transformed pathkeys */
		List	   *orig_query_pathkeys = root->query_pathkeys;

		root->query_pathkeys = transformed_query_pathkey;
		create_index_paths(root, rel);
		root->query_pathkeys = orig_query_pathkeys;

		/*
		 * change returned paths to use original pathkeys. have to go through
		 * all paths since create_index_paths might have modified existing
		 * pathkey. Always safe to do transform since ordering of
		 * transformed_query_pathkey implements ordering of
		 * orig_query_pathkeys.
		 */
		foreach(lc_plan, rel->pathlist)
		{
			Path	   *path = lfirst(lc_plan);

			if (compare_pathkeys(path->pathkeys, transformed_query_pathkey) == PATHKEYS_EQUAL)
			{
				path->pathkeys = orig_query_pathkeys;
			}
		}
	}

}
