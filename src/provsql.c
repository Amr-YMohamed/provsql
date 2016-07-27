#include "postgres.h"
#include "access/sysattr.h"
#include "catalog/pg_aggregate.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/planner.h"
#include "utils/lsyscache.h"

#include "provsql_utils.h"

PG_MODULE_MAGIC;

static const char *PROVSQL_COLUMN_NAME="provsql";

extern void _PG_init(void);
extern void _PG_fini(void);

static planner_hook_type prev_planner = NULL;

static bool process_query(
    Query *q,
    const constants_t *constants,
    bool subquery);
          
static RelabelType *make_provenance_attribute(RangeTblEntry *r, Index relid, AttrNumber attid, const constants_t *constants) {
  RelabelType *re=makeNode(RelabelType);
  Var *v=makeNode(Var);
  v->varno=v->varnoold=relid;
  v->varattno=v->varoattno=attid;
  v->vartype=constants->OID_TYPE_PROVENANCE_TOKEN;
  v->varcollid=InvalidOid;
  v->vartypmod=-1;
  v->location=-1;

  re->arg=(Expr*)v;
  re->resulttype=constants->OID_TYPE_UUID;
  re->resulttypmod=-1;
  re->resultcollid=InvalidOid;
  re->relabelformat=COERCION_EXPLICIT;
  re->location=-1;

  r->selectedCols=bms_add_member(r->selectedCols,attid-FirstLowInvalidHeapAttributeNumber);
  
  return re;
}

static List *get_provenance_attributes(Query *q, const constants_t *constants) {
  List *prov_atts=NIL;
  ListCell *l;
  Index relid=1;

  foreach(l, q->rtable) {
    RangeTblEntry *r = (RangeTblEntry *) lfirst(l);
    ListCell *lc;
    AttrNumber attid=1;

    if(r->rtekind == RTE_RELATION) {
      foreach(lc,r->eref->colnames) {
        Value *v = (Value *) lfirst(lc);

        if(!strcmp(strVal(v),PROVSQL_COLUMN_NAME) &&
            get_atttype(r->relid,attid)==constants->OID_TYPE_PROVENANCE_TOKEN) {
          prov_atts=lappend(prov_atts,make_provenance_attribute(r,relid,attid,constants));
        }

        ++attid;
      }
    } else if(r->rtekind == RTE_SUBQUERY) {
      if(process_query(r->subquery, constants, true)) {
        r->eref->colnames = lappend(r->eref->colnames, makeString("provsql"));
        prov_atts=lappend(prov_atts,make_provenance_attribute(r,relid,r->eref->colnames->length,constants));
      }
    } else {
      ereport(WARNING, (errmsg("FROM clause unsupported by provsql")));
    }

    ++relid;
  }

  return prov_atts;
}

static void remove_provenance_attributes_select(Query *q, const constants_t *constants)
{
  int nbRemoved=0;
  int i=0;

  for(ListCell *cell=list_head(q->targetList), *prev=NULL;
      cell!=NULL
      ;) {
    TargetEntry *rt = (TargetEntry *) lfirst(cell);
    bool removed=false;

    if(rt->expr->type==T_Var) {
      Var *v =(Var *) rt->expr;

      if(rt->resname && !strcmp(rt->resname,PROVSQL_COLUMN_NAME) &&
          v->vartype==constants->OID_TYPE_PROVENANCE_TOKEN) {
        q->targetList=list_delete_cell(q->targetList, cell, prev);

        removed=true;
        ++nbRemoved;
      }
    
      ++i;
    }

    if(removed) {
      if(prev) {
        cell=lnext(prev);
      }
      else {
        cell=list_head(q->targetList);
      }
    } else {
      rt->resno-=nbRemoved;
      prev=cell;
      cell=lnext(cell);
    }
  }
}

static Expr *addProvenanceToSelect(Query *q, List *prov_atts, const constants_t *constants, bool aggregation_needed)
{
  ArrayExpr *array=makeNode(ArrayExpr);
  FuncExpr *expr=makeNode(FuncExpr);
  TargetEntry *te=makeNode(TargetEntry);

  array->array_typeid=constants->OID_TYPE_UUID_ARRAY;
  array->element_typeid=constants->OID_TYPE_UUID;
  array->elements=prov_atts;
  array->location=-1;

  expr->funcid=constants->OID_FUNCTION_PROVENANCE_AND;
  expr->funcresulttype=constants->OID_TYPE_PROVENANCE_TOKEN;
  expr->funcvariadic=true;
  expr->args=list_make1(array);
  expr->location=-1;

  te->resno=list_length(q->targetList)+1;
  te->resname=(char *)PROVSQL_COLUMN_NAME;

  if(aggregation_needed) {
    Aggref *agg = makeNode(Aggref);
    TargetEntry *te_inner = makeNode(TargetEntry);

    te_inner->resno=1;
    te_inner->expr=(Expr*)expr;

    agg->aggfnoid=constants->OID_FUNCTION_PROVENANCE_AGG;
    agg->aggtype=constants->OID_TYPE_PROVENANCE_TOKEN;
    agg->args=list_make1(te_inner);
    agg->aggkind=AGGKIND_NORMAL;
    agg->location=-1;

    te->expr=(Expr*)agg;
  } else {
    te->expr=(Expr*)expr;
  }
    
  q->targetList=lappend(q->targetList,te);

  return te->expr;
}

typedef struct provenance_mutator_context {
  constants_t *constants;
  Expr *provsql;
} provenance_mutator_context;

static Node *provenance_mutator(Node *node, provenance_mutator_context *context)
{
  if(node == NULL)
    return NULL;

  if(IsA(node, FuncExpr)) {
    FuncExpr *f = (FuncExpr *) node;

    if(f->funcid == context->constants->OID_FUNCTION_PROVENANCE) {
      return copyObject(context->provsql);
    }
  }

  return expression_tree_mutator(node, provenance_mutator, (void *) context);
}

static void replace_provenance_function_by_expression(Query *q, Expr *provsql, const constants_t *constants)
{
  provenance_mutator_context context = {(constants_t *) constants,provsql};

  query_tree_mutator(q, provenance_mutator, &context, QTW_DONT_COPY_QUERY | QTW_IGNORE_RT_SUBQUERIES);
}

static bool process_query(
    Query *q,
    const constants_t *constants,
    bool subquery)
{
  List *prov_atts=get_provenance_attributes(q, constants);
  Expr *provsql;

  if(prov_atts==NIL)
    return false;

//  ereport(NOTICE, (errmsg("Before: %s",nodeToString(q))));

  if(q->hasAggs) {
    ereport(ERROR, (errmsg("Aggregation not supported on tables with provenance")));
  }

  if(!subquery)
    remove_provenance_attributes_select(q, constants);

  if(q->groupClause) {
    q->hasAggs=true;
  }

  provsql = addProvenanceToSelect(q, prov_atts, constants, q->groupClause != NIL);

  replace_provenance_function_by_expression(q, provsql, constants);

//  ereport(NOTICE, (errmsg("After: %s",nodeToString(q))));

  return true;
}

static PlannedStmt *provsql_planner(
    Query *q,
    int cursorOptions,
    ParamListInfo boundParams)
{
  if(q->commandType==CMD_SELECT) {
    constants_t constants;
    if(initialize_constants(&constants))
      process_query(q, &constants, false);
  }

  if(prev_planner)
    return prev_planner(q, cursorOptions, boundParams);
  else
    return standard_planner(q, cursorOptions, boundParams);
}

void _PG_init(void)
{
  prev_planner = planner_hook;
  planner_hook = provsql_planner;
}

void _PG_fini(void)
{
  planner_hook = prev_planner;
}