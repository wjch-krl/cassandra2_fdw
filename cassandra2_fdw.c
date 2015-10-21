/*-------------------------------------------------------------------------
 *
 * Cassandra2 Foreign Data Wrapper for PostgreSQL
 *
 * Copyright (c) 2015 Jaimin Pan
 *
 * This software is released under the PostgreSQL Licence
 *
 * Author: Jaimin Pan <jaimin.pan@gmail.com>
 *
 * IDENTIFICATION
 *		cassandra2_fdw/cassandra2_fdw.c
 *
 *-------------------------------------------------------------------------
 */
//#if (PG_VERSION_NUM < 90200)
//# error The file must compiler when pg version large than 90200
//#endif

#include "postgres.h"

#include <cassandra.h>

#include "cassandra2_fdw.h"

#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/sysattr.h"
#include "executor/spi.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "mb/pg_wchar.h"
#include "optimizer/cost.h"
#include "optimizer/restrictinfo.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_user_mapping.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/vacuum.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "access/reloptions.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_user_mapping.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "commands/vacuum.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "libpq/md5.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pg_list.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/var.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "port.h"
#include "storage/ipc.h"
#include "storage/lock.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/elog.h"
#include "utils/fmgroids.h"
#include "utils/formatting.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/resowner.h"
#include "utils/timestamp.h"
#include "utils/tqual.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"


PG_MODULE_MAGIC;

/* Default CPU cost to start up a foreign query. */
#define DEFAULT_FDW_STARTUP_COST	100.0

/* Default CPU cost to process 1 row (above and beyond cpu_tuple_cost). */
#define DEFAULT_FDW_TUPLE_COST		0.01

/*
 * Describes the valid options for objects that use this wrapper.
 */
struct CassFdwOption
{
  const char *optname;
  Oid optcontext; /* Oid of catalog in which option may appear */
};

/*
 * Valid options for cassandra2_fdw.
 */
static struct CassFdwOption valid_options[] = {
  /* Connection options */
  { "url", ForeignServerRelationId},
  { "querytimeout", ForeignServerRelationId},
  { "portNumber", ForeignServerRelationId},
  { "username", UserMappingRelationId},
  { "password", UserMappingRelationId},
  { "table", ForeignTableRelationId},
  { "queryable_columns", ForeignTableRelationId},
  /* Sentinel */
  { NULL, InvalidOid}
};

/*
 * FDW-specific information for RelOptInfo.fdw_private.
 */
typedef struct CassFdwPlanState
{
  /* Bitmap of attr numbers we need to fetch from the remote server. */
  Bitmapset *attrs_used;

  /* Estimated size and cost for a scan with baserestrictinfo quals. */
  double rows;
  int width;
  Cost startup_cost;
  Cost total_cost;
} CassFdwPlanState;

/*
 * FDW-specific information for ForeignScanState.fdw_state.
 */
typedef struct CassFdwScanState
{
  Relation rel; /* relcache entry for the foreign table */
  AttInMetadata *attinmeta; /* attribute datatype conversion metadata */

  /* extracted fdw_private data */
  char *query; /* text of SELECT command */
  List *retrieved_attrs; /* list of retrieved attribute numbers */

  int NumberOfColumns;

  /* for remote query execution */
  CassSession *cass_conn; /* connection for the scan */
  bool sql_sended;
  CassStatement *statement;

  /* for storing result tuples */
  HeapTuple *tuples; /* array of currently-retrieved tuples */
  int num_tuples; /* # of tuples in array */
  int next_tuple; /* index of next one to return */

  /* batch-level state, for optimizing rewinds and avoiding useless fetch */
  int fetch_ct_2; /* Min(# of fetches done, 2) */
  bool eof_reached; /* true if last fetch reached EOF */

  /* working memory contexts */
  MemoryContext batch_cxt; /* context holding current batch of tuples */
  MemoryContext temp_cxt; /* context for per-tuple temporary data */
} CassFdwScanState;

enum CassFdwScanPrivateIndex
{
  /* SQL statement to execute remotely (as a String node) */
  CassFdwScanPrivateSelectSql,
  /* Integer list of attribute numbers retrieved by the SELECT */
  CassFdwScanPrivateRetrievedAttrs
};


/*
 * SQL functions
 */
extern Datum cassandra2_fdw_handler (PG_FUNCTION_ARGS);
extern Datum cassandra2_fdw_validator (PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1 (cassandra2_fdw_handler);
PG_FUNCTION_INFO_V1 (cassandra2_fdw_validator);


/*
 * FDW callback routines
 */
static void cassGetForeignRelSize (PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid);
static void cassGetForeignPaths (PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid);
static ForeignScan *cassGetForeignPlan (
                                        PlannerInfo *root,
                                        RelOptInfo *baserel,
                                        Oid foreigntableid,
                                        ForeignPath *best_path,
                                        List *tlist,
                                        List *scan_clauses);
static void cassExplainForeignScan (ForeignScanState *node, ExplainState *es);
static void cassBeginForeignScan (ForeignScanState *node, int eflags);
static TupleTableSlot *cassIterateForeignScan (ForeignScanState *node);
static void cassReScanForeignScan (ForeignScanState *node);
static void cassEndForeignScan (ForeignScanState *node);

/*
 * Helper functions
 */
static void estimate_path_cost_size (PlannerInfo *root,
                                     RelOptInfo *baserel,
                                     List *join_conds,
                                     double *p_rows, int *p_width,
                                     Cost *p_startup_cost, Cost *p_total_cost);
static bool cassIsValidOption (const char *option, Oid context);
static void cassGetOptions (Oid foreigntableid,
                            char **url, int *querytimeout,
                            int* portNumber, char **username, char **password,
                            char **query, char **tablename);

static void create_cursor (ForeignScanState *node);
static void fetch_more_data (ForeignScanState *node);
static const char *pgcass_transferValue (char* buf, const CassValue* value);
static HeapTuple make_tuple_from_result_row (const CassRow* row,
                                             int ncolumn,
                                             Relation rel,
                                             AttInMetadata *attinmeta,
                                             List *retrieved_attrs,
                                             MemoryContext temp_context);

void deparseSelectSql (StringInfo buf,
                       PlannerInfo *root,
                       RelOptInfo *baserel,
                       Bitmapset *attrs_used,
                       List **retrieved_attrs);


static void deparseTargetList (StringInfo buf,
                               PlannerInfo *root,
                               Index rtindex,
                               Relation rel,
                               Bitmapset *attrs_used,
                               List **retrieved_attrs);

static void deparseColumnRef (StringInfo buf, int varno, int varattno,
                              PlannerInfo *root);

static char* processWhereClause (Expr *expr, RelOptInfo *baserel, PlannerInfo *root, char ** columns, int columns_count);

static char* datumToString (Datum datum, Oid type);

/*
 * Foreign-data wrapper handler function: return a struct with pointers
 * to my callback routines.
 */
Datum
cassandra2_fdw_handler (PG_FUNCTION_ARGS)
{
  FdwRoutine *fdwroutine = makeNode (FdwRoutine);

  fdwroutine->GetForeignRelSize = cassGetForeignRelSize;
  fdwroutine->GetForeignPaths = cassGetForeignPaths;
  fdwroutine->GetForeignPlan = cassGetForeignPlan;

  fdwroutine->ExplainForeignScan = cassExplainForeignScan;
  fdwroutine->BeginForeignScan = cassBeginForeignScan;
  fdwroutine->IterateForeignScan = cassIterateForeignScan;
  fdwroutine->ReScanForeignScan = cassReScanForeignScan;
  fdwroutine->EndForeignScan = cassEndForeignScan;
  fdwroutine->AnalyzeForeignTable = NULL;

  PG_RETURN_POINTER (fdwroutine);
}

/*
 * Validate the generic options given to a FOREIGN DATA WRAPPER, SERVER,
 * USER MAPPING or FOREIGN TABLE that uses file_fdw.
 *
 * Raise an ERROR if the option or its value is considered invalid.
 */
Datum
cassandra2_fdw_validator (PG_FUNCTION_ARGS)
{
  List *options_list = untransformRelOptions (PG_GETARG_DATUM (0));
  Oid catalog = PG_GETARG_OID (1);
  char *svr_url = NULL;
  char *svr_username = NULL;
  char *svr_password = NULL;
  char *svr_queryableColumns = NULL;
  char *svr_table = NULL;
  int svr_querytimeout = 0;
  int svr_portNumber = 0;
  ListCell *cell;

  /*
   * Check that only options supported by cassandra2_fdw,
   * and allowed for the current object type, are given.
   */
  foreach (cell, options_list)
  {
    DefElem *def = (DefElem *) lfirst (cell);

    if (!cassIsValidOption (def->defname, catalog))
      {
        const struct CassFdwOption *opt;
        StringInfoData buf;

        /*
         * Unknown option specified, complain about it. Provide a hint
         * with list of valid options for the object.
         */
        initStringInfo (&buf);
        for (opt = valid_options; opt->optname; opt++)
          {
            if (catalog == opt->optcontext)
              appendStringInfo (&buf, "%s%s", (buf.len > 0) ? ", " : "",
                                opt->optname);
          }

        ereport (ERROR,
                 (errcode (ERRCODE_FDW_INVALID_OPTION_NAME),
                  errmsg ("invalid option \"%s\"", def->defname),
                  buf.len > 0
                  ? errhint ("Valid options in this context are: %s",
                             buf.data)
                  : errhint ("There are no valid options in this context.")));
      }

    if (strcmp (def->defname, "url") == 0)
      {
        if (svr_url)
          ereport (ERROR,
                   (errcode (ERRCODE_SYNTAX_ERROR),
                    errmsg ("conflicting or redundant options")));
        svr_url = defGetString (def);
      }
    else if (strcmp (def->defname, "querytimeout") == 0)
      {
        if (svr_querytimeout)
          ereport (ERROR,
                   (errcode (ERRCODE_SYNTAX_ERROR),
                    errmsg ("conflicting or redundant options")));
        svr_querytimeout = atoi (defGetString (def));
      }
    else if (strcmp (def->defname, "portNumber") == 0)
      {
        if (svr_portNumber)
          ereport (ERROR,
                   (errcode (ERRCODE_SYNTAX_ERROR),
                    errmsg ("conflicting or redundant options")));
        svr_portNumber = atoi (defGetString (def));
      }
    else if (strcmp (def->defname, "username") == 0)
      {
        if (svr_username)
          ereport (ERROR,
                   (errcode (ERRCODE_SYNTAX_ERROR),
                    errmsg ("conflicting or redundant options")));
        svr_username = defGetString (def);
      }
    else if (strcmp (def->defname, "password") == 0)
      {
        if (svr_password)
          ereport (ERROR,
                   (errcode (ERRCODE_SYNTAX_ERROR),
                    errmsg ("conflicting or redundant options")));
        svr_password = defGetString (def);
      }
    else if (strcmp (def->defname, "queryable_columns") == 0)
      {
        if (svr_queryableColumns)
          ereport (ERROR,
                   (errcode (ERRCODE_SYNTAX_ERROR),
                    errmsg ("conflicting or redundant options")));

        svr_queryableColumns = defGetString (def);
      }

    else if (strcmp (def->defname, "table") == 0)
      {
        if (svr_table)
          ereport (ERROR,
                   (errcode (ERRCODE_SYNTAX_ERROR),
                    errmsg ("conflicting or redundant options")));

        svr_table = defGetString (def);
      }
  }

  if (catalog == ForeignServerRelationId && svr_url == NULL)
    ereport (ERROR,
             (errcode (ERRCODE_SYNTAX_ERROR),
              errmsg ("URL must be specified")));

  if (catalog == ForeignTableRelationId &&
      svr_table == NULL)
    ereport (ERROR,
             (errcode (ERRCODE_SYNTAX_ERROR),
              errmsg ("Table and queryable columns must be specified")));

  PG_RETURN_VOID ();
}

/*
 * Check if the provided option is one of the valid options.
 * context is the Oid of the catalog holding the object the option is for.
 */
static bool
cassIsValidOption (const char *option, Oid context)
{
  const struct CassFdwOption *opt;

  for (opt = valid_options; opt->optname; opt++)
    {
      if (context == opt->optcontext && strcmp (opt->optname, option) == 0)
        return true;
    }
  return false;
}

/*
 * Fetch the options for a fdw foreign table.
 */
static void
cassGetOptions (Oid foreigntableid, char **url, int *querytimeout, int *portNumber,
                char **username, char **password, char **queryableColumns, char **tablename)
{
  ForeignTable *table;
  ForeignServer *server;
  UserMapping *user;
  List *options;
  ListCell *lc;

  /*
   * Extract options from FDW objects.
   */
  table = GetForeignTable (foreigntableid);
  server = GetForeignServer (table->serverid);
  user = GetUserMapping (GetUserId (), server->serverid);

  options = NIL;
  options = list_concat (options, table->options);
  options = list_concat (options, server->options);
  options = list_concat (options, user->options);

  /* Loop through the options, and get the server/port */
  foreach (lc, options)
  {
    DefElem *def = (DefElem *) lfirst (lc);

    if (strcmp (def->defname, "username") == 0)
      {
        *username = defGetString (def);
      }
    else if (strcmp (def->defname, "querytimeout") == 0)
      {
        *querytimeout = atoi (defGetString (def));
      }
    else if (strcmp (def->defname, "portNumber") == 0)
      {
        *portNumber = atoi (defGetString (def));
      }
    else if (strcmp (def->defname, "password") == 0)
      {
        *password = defGetString (def);
      }
    else if (strcmp (def->defname, "queryable_columns") == 0)
      {
        *queryableColumns = defGetString (def);
      }
    else if (strcmp (def->defname, "table") == 0)
      {
        *tablename = defGetString (def);
      }
    else if (strcmp (def->defname, "url") == 0)
      {
        *url = defGetString (def);
      }
  }
}

//#if (PG_VERSION_NUM >= 90200)

/*
 * cassGetForeignRelSize
 *		Obtain relation size estimates for a foreign table
 */
static void
cassGetForeignRelSize (PlannerInfo *root,
                       RelOptInfo *baserel,
                       Oid foreigntableid)
{
  CassFdwPlanState *fpinfo;

  fpinfo = (CassFdwPlanState *) palloc0 (sizeof (CassFdwPlanState));
  baserel->fdw_private = (void *) fpinfo;

  fpinfo->attrs_used = NULL;
  pull_varattnos ((Node *) baserel->reltargetlist, baserel->relid,
                  &fpinfo->attrs_used);

  //TODO
  /* Fetch options  */

  /* Estimate relation size */
  {
    /*
     * If the foreign table has never been ANALYZEd, it will have relpages
     * and reltuples equal to zero, which most likely has nothing to do
     * with reality.  We can't do a whole lot about that if we're not
     * allowed to consult the remote server, but we can use a hack similar
     * to plancat.c's treatment of empty relations: use a minimum size
     * estimate of 10 pages, and divide by the column-datatype-based width
     * estimate to get the corresponding number of tuples.
     */
    if (baserel->pages == 0 && baserel->tuples == 0)
      {
        baserel->pages = 10;
        baserel->tuples =
                (10 * BLCKSZ) / (baserel->width + sizeof (HeapTupleHeaderData));
      }

    /* Estimate baserel size as best we can with local statistics. */
    set_baserel_size_estimates (root, baserel);

    /* Fill in basically-bogus cost estimates for use later. */
    estimate_path_cost_size (root, baserel, NIL,
                             &fpinfo->rows, &fpinfo->width,
                             &fpinfo->startup_cost, &fpinfo->total_cost);
  }
}

static void
estimate_path_cost_size (PlannerInfo *root,
                         RelOptInfo *baserel,
                         List *join_conds,
                         double *p_rows, int *p_width,
                         Cost *p_startup_cost, Cost *p_total_cost)
{
  *p_rows = baserel->rows;
  *p_width = baserel->width;

  *p_startup_cost = DEFAULT_FDW_STARTUP_COST;
  *p_total_cost = DEFAULT_FDW_TUPLE_COST * 100;
}

/*
 * cassGetForeignPaths
 *		(9.2+) Get the foreign paths
 */
static void
cassGetForeignPaths (PlannerInfo *root,
                     RelOptInfo *baserel,
                     Oid foreigntableid)
{
  CassFdwPlanState *fpinfo = (CassFdwPlanState *) baserel->fdw_private;
  ForeignPath *path;

  /*
   * Create simplest ForeignScan path node and add it to baserel.  This path
   * corresponds to SeqScan path of regular tables (though depending on what
   * baserestrict conditions we were able to send to remote, there might
   * actually be an indexscan happening there).  We already did all the work
   * to estimate cost and size of this path.
   */
  path = create_foreignscan_path (root, baserel,
                                  fpinfo->rows + baserel->rows,
                                  fpinfo->startup_cost,
                                  fpinfo->total_cost,
                                  NIL, /* no pathkeys */
                                  NULL, /* no outer rel either */
                                  NIL); /* no fdw_private list */
  add_path (baserel, (Path *) path);

  //TODO
}

/*
 * cassGetForeignPlan
 *		Create ForeignScan plan node which implements selected best path
 */
static ForeignScan *
cassGetForeignPlan (PlannerInfo *root,
                    RelOptInfo *baserel,
                    Oid foreigntableid,
                    ForeignPath *best_path,
                    List *tlist,
                    List *scan_clauses)
{
  CassFdwPlanState *fpinfo = (CassFdwPlanState *) baserel->fdw_private;
  Index scan_relid = baserel->relid;
  List *fdw_private;
  List *local_exprs = NIL;
  StringInfoData sql;
  List *retrieved_attrs;

  local_exprs = extract_actual_clauses (scan_clauses, false);

  /*
   * Build the query string to be sent for execution, and identify
   * expressions to be sent as parameters.
   */
  initStringInfo (&sql);
  deparseSelectSql (&sql, root, baserel, fpinfo->attrs_used,
                    &retrieved_attrs);

  /*
   * Build the fdw_private list that will be available to the executor.
   * Items in the list must match enum FdwScanPrivateIndex, above.
   */
  fdw_private = list_make2 (makeString (sql.data),
                            retrieved_attrs);

  /*
   * Create the ForeignScan node from target list, local filtering
   * expressions, remote parameter expressions, and FDW private information.
   *
   * Note that the remote parameter expressions are stored in the fdw_exprs
   * field of the finished plan node; we can't keep them in private state
   * because then they wouldn't be subject to later planner processing.
   */
  return make_foreignscan (tlist,
                           local_exprs,
                           scan_relid,
                           NIL,
                           fdw_private);
}

/*
 * cassExplainForeignScan
 *		Produce extra output for EXPLAIN
 */
static void
cassExplainForeignScan (ForeignScanState *node, ExplainState *es)
{
  List *fdw_private;
  char *sql;
  if (es->verbose)
    {

      //TODO
      fdw_private = ((ForeignScan *) node->ss.ps.plan)->fdw_private;
      sql = strVal (list_nth (fdw_private, CassFdwScanPrivateSelectSql));
      ExplainPropertyText ("Remote SQL", sql, es);
    }
}

/*
 * cassBeginForeignScan
 *		Initiate access to the database
 */
static void
cassBeginForeignScan (ForeignScanState *node, int eflags)
{
  ForeignScan *fsplan = (ForeignScan *) node->ss.ps.plan;
  EState *estate = node->ss.ps.state;
  CassFdwScanState *fsstate;
  RangeTblEntry *rte;
  Oid userid;
  ForeignTable *table;
  ForeignServer *server;
  UserMapping *user;

  /*
   * Do nothing in EXPLAIN (no ANALYZE) case.  node->fdw_state stays NULL.
   */
  if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
    return;

  /*
   * We'll save private state in node->fdw_state.
   */
  fsstate = (CassFdwScanState *) palloc0 (sizeof (CassFdwScanState));
  node->fdw_state = (void *) fsstate;

  /*
   * Identify which user to do the remote access as.  This should match what
   * ExecCheckRTEPerms() does.
   */
  rte = rt_fetch (fsplan->scan.scanrelid, estate->es_range_table);
  userid = rte->checkAsUser ? rte->checkAsUser : GetUserId ();

  /* Get info about foreign table. */
  fsstate->rel = node->ss.ss_currentRelation;
  table = GetForeignTable (RelationGetRelid (fsstate->rel));
  server = GetForeignServer (table->serverid);
  user = GetUserMapping (userid, server->serverid);

  /*
   * Get connection to the foreign server.  Connection manager will
   * establish new connection if necessary.
   */
  fsstate->cass_conn = pgcass_GetConnection (server, user, false);
  fsstate->sql_sended = false;

  {
    char *query;
    List *fdw_private;

    fdw_private = fsplan->fdw_private;
    /* Get prepared query */
    query = strVal (list_nth (fdw_private, CassFdwScanPrivateSelectSql));

    ereport (WARNING,
             (errcode (ERRCODE_WARNING),
              errmsg ("Begin foreign scan with query: %s", query)));

    elog (LOG, "Cass PUSDOWN Query = '%s'", query);

    fsstate->query = query;
  }

  /* Get private info created by planner functions. */
  //	fsstate->query = strVal(list_nth(fsplan->fdw_private,
  //									 CassFdwScanPrivateSelectSql));
  fsstate->retrieved_attrs = (List *) list_nth (fsplan->fdw_private,
                                                CassFdwScanPrivateRetrievedAttrs);

  /* Create contexts for batches of tuples and per-tuple temp workspace. */
  fsstate->batch_cxt = AllocSetContextCreate (estate->es_query_cxt,
                                              "cassandra2_fdw tuple data",
                                              ALLOCSET_DEFAULT_MINSIZE,
                                              ALLOCSET_DEFAULT_INITSIZE,
                                              ALLOCSET_DEFAULT_MAXSIZE);

  fsstate->temp_cxt = AllocSetContextCreate (estate->es_query_cxt,
                                             "cassandra2_fdw temporary data",
                                             ALLOCSET_SMALL_MINSIZE,
                                             ALLOCSET_SMALL_INITSIZE,
                                             ALLOCSET_SMALL_MAXSIZE);

  /* Get info we'll need for input data conversion. */
  fsstate->attinmeta = TupleDescGetAttInMetadata (RelationGetDescr (fsstate->rel));
}

/*
 * cassIterateForeignScan
 *		Read next record from the data file and store it into the
 *		ScanTupleSlot as a virtual tuple
 */
static TupleTableSlot*
cassIterateForeignScan (ForeignScanState *node)
{
  CassFdwScanState *fsstate = (CassFdwScanState *) node->fdw_state;
  TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;

  /*
   * If this is the first call after Begin or ReScan, we need to create the
   * cursor on the remote side.
   */
  if (!fsstate->sql_sended)
    create_cursor (node);

  /*
   * Get some more tuples, if we've run out.
   */
  if (fsstate->next_tuple >= fsstate->num_tuples)
    {
      /* No point in another fetch if we already detected EOF, though. */
      if (!fsstate->eof_reached)
        fetch_more_data (node);
      /* If we didn't get any tuples, must be end of data. */
      if (fsstate->next_tuple >= fsstate->num_tuples)
        return ExecClearTuple (slot);
    }

  /*
   * Return the next tuple.
   */
  ExecStoreTuple (fsstate->tuples[fsstate->next_tuple++],
                  slot,
                  InvalidBuffer,
                  false);

  return slot;
}

/*
 * cassReScanForeignScan
 *		Rescan table, possibly with new parameters
 */
static void
cassReScanForeignScan (ForeignScanState *node)
{
  CassFdwScanState *fsstate = (CassFdwScanState *) node->fdw_state;

  /* If we haven't created the cursor yet, nothing to do. */
  if (!fsstate->sql_sended)
    return;

  {
    /* Easy: just rescan what we already have in memory, if anything */
    fsstate->next_tuple = 0;
    return;
  }

  /* Now force a fresh FETCH. */
  fsstate->tuples = NULL;
  fsstate->num_tuples = 0;
  fsstate->next_tuple = 0;
  fsstate->fetch_ct_2 = 0;
  fsstate->eof_reached = false;
}

/*
 * cassEndForeignScan
 *		Finish scanning foreign table and dispose objects used for this scan
 */
static void
cassEndForeignScan (ForeignScanState *node)
{
  CassFdwScanState *fsstate = (CassFdwScanState *) node->fdw_state;

  /* if fsstate is NULL, we are in EXPLAIN; nothing to do */
  if (fsstate == NULL)
    return;

  /* Close the cursor if open, to prevent accumulation of cursors */
  if (fsstate->sql_sended)
    {
      if (fsstate->statement)
        cass_statement_free (fsstate->statement);
    }

  if (fsstate->query)
    {
      pfree (fsstate->query);
    }

  /* Release remote connection */
  pgcass_ReleaseConnection (fsstate->cass_conn);
  fsstate->cass_conn = NULL;

  /* MemoryContexts will be deleted automatically. */
}

/*
 * Create cursor for node's query with current parameter values.
 */
static void
create_cursor (ForeignScanState *node)
{
  CassFdwScanState *fsstate = (CassFdwScanState *) node->fdw_state;

  /* Build statement and execute query */
  fsstate->statement = cass_statement_new (fsstate->query, 0);

  /* Mark the cursor as created, and show no tuples have been retrieved */
  fsstate->sql_sended = true;
  fsstate->tuples = NULL;
  fsstate->num_tuples = 0;
  fsstate->next_tuple = 0;
  fsstate->fetch_ct_2 = 0;
  fsstate->eof_reached = false;
}

/*
 * Fetch some more rows from the node's cursor.
 */
static void
fetch_more_data (ForeignScanState *node)
{
  CassFdwScanState *fsstate = (CassFdwScanState *) node->fdw_state;
  //MemoryContext oldcontext;
  /*
   * We'll store the tuples in the batch_cxt.  First, flush the previous
   * batch.
   */
  fsstate->tuples = NULL;
  MemoryContextReset (fsstate->batch_cxt);
  //oldcontext = MemoryContextSwitchTo(fsstate->batch_cxt);
  {
    CassSession *conn = fsstate->cass_conn;

    CassFuture* result_future = cass_session_execute (conn, fsstate->statement);
    if (cass_future_error_code (result_future) == CASS_OK)
      {
        const CassResult* res;
        int numrows;
        CassIterator* rows;
        int k;

        /* Retrieve result set and iterate over the rows */
        res = cass_future_get_result (result_future);

        /* Stash away the state info we have already */
        fsstate->NumberOfColumns = cass_result_column_count (res);

        /* Convert the data into HeapTuples */
        numrows = cass_result_row_count (res);
        fsstate->tuples = (HeapTuple *) palloc0 (numrows * sizeof (HeapTuple));
        fsstate->num_tuples = numrows;
        fsstate->next_tuple = 0;

        rows = cass_iterator_from_result (res);
        k = 0;
        while (cass_iterator_next (rows))
          {
            const CassRow* row = cass_iterator_get_row (rows);

            fsstate->tuples[k] = make_tuple_from_result_row (row,
                                                             fsstate->NumberOfColumns,
                                                             fsstate->rel,
                                                             fsstate->attinmeta,
                                                             fsstate->retrieved_attrs,
                                                             fsstate->temp_cxt);

            Assert (k < numrows);
            k++;
          }

        fsstate->eof_reached = true;

        cass_result_free (res);
        cass_iterator_free (rows);
      }
    else
      {
        /* Handle error */
        const char* message;
        size_t message_length;
        cass_future_error_message (result_future, &message, &message_length);
        elog (LOG, "Unable to run query: '%.*s'\n",
              (int) message_length, message);
        ereport (ERROR,
                 (errcode (ERRCODE_SYNTAX_ERROR),
                  errmsg ("Unable to run query: '%.*s'\n",
                          (int) message_length, message)));
        fsstate->eof_reached = true;
      }

    cass_future_free (result_future);
  }
}

static HeapTuple
make_tuple_from_result_row (const CassRow* row,
                            int ncolumn,
                            Relation rel,
                            AttInMetadata *attinmeta,
                            List *retrieved_attrs,
                            MemoryContext temp_context)
{
  HeapTuple tuple;
  TupleDesc tupdesc = RelationGetDescr (rel);
  Datum *values;
  bool *nulls;
  MemoryContext oldcontext;
  ListCell *lc;
  int j;

  /*
   * Do the following work in a temp context that we reset after each tuple.
   * This cleans up not only the data we have direct access to, but any
   * cruft the I/O functions might leak.
   */
  oldcontext = MemoryContextSwitchTo (temp_context);

  values = (Datum *) palloc0 (tupdesc->natts * sizeof (Datum));
  nulls = (bool *) palloc (tupdesc->natts * sizeof (bool));
  /* Initialize to nulls for any columns not present in result */
  memset (nulls, true, tupdesc->natts * sizeof (bool));

  /*
   * i indexes columns in the relation, j indexes columns in the PGresult.
   */
  j = 0;

  foreach (lc, retrieved_attrs)
  {
    int i = lfirst_int (lc);
    char buf[265];
    const char *valstr;

    const CassValue* cassVal = cass_row_get_column (row, i - 1);
    if (cass_true == cass_value_is_null (cassVal))
      valstr = NULL;
    else
      valstr = pgcass_transferValue (buf, cassVal);

    if (i > 0)
      {
        /* ordinary column */
        Assert (i <= tupdesc->natts);
        nulls[i - 1] = (valstr == NULL);
        /* Apply the input function even to nulls, to support domains */
        values[i - 1] = InputFunctionCall (&attinmeta->attinfuncs[i - 1],
                                           (char *) valstr,
                                           attinmeta->attioparams[i - 1],
                                           attinmeta->atttypmods[i - 1]);
      }

    j++;
  }

  /*
   * Check we got the expected number of columns.  Note: j == 0 and
   * PQnfields == 1 is expected, since deparse emits a NULL if no columns.
   */
  if (j > 0 && j != ncolumn)
    elog (ERROR, "remote query result does not match the foreign table");

  /*
   * Build the result tuple in caller's memory context.
   */
  MemoryContextSwitchTo (oldcontext);

  tuple = heap_form_tuple (tupdesc, values, nulls);

  /* Clean up */
  MemoryContextReset (temp_context);

  return tuple;
}

static const char *
pgcass_transferValue (char* buf, const CassValue* value)
{
  const char* result = NULL;

  CassValueType type = cass_value_type (value);
  switch (type)
    {
    case CASS_VALUE_TYPE_INT:
      {
        cass_int32_t i;
        cass_value_get_int32 (value, &i);
        sprintf (buf, "%d", i);
        result = buf;
        break;
      }
    case CASS_VALUE_TYPE_BIGINT:
      {
        cass_int64_t i;
        cass_value_get_int64 (value, &i);
        sprintf (buf, "%lld ", (long long int) i);
        result = buf;
        break;
      }
    case CASS_VALUE_TYPE_BOOLEAN:
      {
        cass_bool_t b;
        cass_value_get_bool (value, &b);
        result = b ? "true" : "false";
        break;
      }
    case CASS_VALUE_TYPE_DOUBLE:
      {
        cass_double_t d;
        cass_value_get_double (value, &d);
        sprintf (buf, "%f", d);
        result = buf;
        break;
      }

    case CASS_VALUE_TYPE_TEXT:
    case CASS_VALUE_TYPE_ASCII:
    case CASS_VALUE_TYPE_VARCHAR:
      {
        const char* s;
        size_t s_length;
        cass_value_get_string (value, &s, &s_length);
        //		sprintf(buf, "\"%.*s\"", (int)s_length, s);
        result = s;
        break;
      }
    case CASS_VALUE_TYPE_UUID:
      {
        CassUuid u;
        char us[CASS_UUID_STRING_LENGTH];

        cass_value_get_uuid (value, &u);
        cass_uuid_string (u, us);
        result = us;
        break;
      }
    case CASS_VALUE_TYPE_LIST:
    case CASS_VALUE_TYPE_MAP:
    default:
      result = "<unhandled type>";
      break;
    }

  return result;
}

/*
 * Construct a simple SELECT statement that retrieves desired columns
 * of the specified foreign table, and append it to "buf".  The output
 * contains just "SELECT ... FROM tablename".
 *
 * We also create an integer List of the columns being retrieved, which is
 * returned to *retrieved_attrs.
 */
void
deparseSelectSql (StringInfo buf,
                  PlannerInfo *root,
                  RelOptInfo *baserel,
                  Bitmapset *attrs_used,
                  List **retrieved_attrs)
{
  RangeTblEntry *rte = planner_rt_fetch (baserel->relid, root);
  Relation rel;
  char *opername, *leftvalue, *rightvalue;
  Expr* expr, *left, *right;
  OpExpr *oper;
  HeapTuple tuple;
  Oid rightargtype, leftargtype;
  List *conditions;
  ListCell *cell;
  char ** columns;
  int queryable_columns_count;

  bool first_col;
  /*
   * Core code already has some lock on each rel being planned, so we can
   * use NoLock here.
   */
  rel = heap_open (rte->relid, NoLock);

  /*
   * Construct SELECT list
   */
  appendStringInfoString (buf, "SELECT ");
  deparseTargetList (buf, root, baserel->relid, rel, attrs_used,
                     retrieved_attrs);

  /*
   * Construct FROM clause
   */
  appendStringInfoString (buf, " FROM ");

  {
    char *svr_url = NULL;
    char *svr_username = NULL;
    char *svr_password = NULL;
    char *svr_queryableColumns = NULL;
    char *svr_table = NULL;
    int svr_querytimeout = 0;
    int svr_portNumber = 0;
    char *tmp_string;
    const char delim = ',';
    /* Fetch options  */
    //TODO get only table name and querable columns
    cassGetOptions (rte->relid,
                    &svr_url, &svr_querytimeout, &svr_portNumber,
                    &svr_username, &svr_password,
                    &svr_queryableColumns, &svr_table);

    tmp_string = (char*) malloc (sizeof (char) *(strlen (svr_queryableColumns) + 1));
    strcpy (tmp_string, svr_queryableColumns);

    for (queryable_columns_count = 0;
            tmp_string[queryable_columns_count];
            tmp_string[queryable_columns_count] == delim ? queryable_columns_count++ : *tmp_string++);

    columns = (char**) malloc (sizeof (char*)*queryable_columns_count);

    char *token = strtok (svr_queryableColumns, ",");
    int i = 0;
    while (token)
      {

        columns[i++] = token;
        token = strtok (NULL, ",");
      }


    appendStringInfoString (buf, svr_table);
  }

  /*
   * Construct WHERE clause
   */
  conditions = baserel->baserestrictinfo;
  //TODO add where with clustering and or primary key
  first_col = true;

  foreach (cell, conditions)
  {

    expr = ((RestrictInfo *) lfirst (cell))->clause;

    if (expr->type == T_OpExpr)
      {

        oper = (OpExpr *) expr;
        /* get operator name, kind, argument type and schema */
        tuple = SearchSysCache1 (OPEROID, ObjectIdGetDatum (oper->opno));
        if (!HeapTupleIsValid (tuple))
          {
            elog (ERROR, "cache lookup failed for operator %u", oper->opno);
          }
        opername = pstrdup (((Form_pg_operator) GETSTRUCT (tuple))->oprname.data);
        leftargtype = ((Form_pg_operator) GETSTRUCT (tuple))->oprleft;
        rightargtype = ((Form_pg_operator) GETSTRUCT (tuple))->oprright;
        ReleaseSysCache (tuple);
        if (!(leftargtype == INTERVALOID && rightargtype == INTERVALOID))
          {

            if (strcmp (opername, "=") == 0)
              {
                left = (Expr *) linitial (oper->args);
                leftvalue = processWhereClause (left, baserel, root, columns, queryable_columns_count);

                right = (Expr *) lsecond (oper->args);
                rightvalue = processWhereClause (right, baserel, root, columns, queryable_columns_count);

                if (rightvalue != NULL && leftvalue != NULL)
                  {
                    if (first_col)
                      {
                        first_col = false;
                        appendStringInfoString (buf, " WHERE ");

                      }
                    else
                      {

                        appendStringInfoString (buf, " AND ");

                      }
                    appendStringInfoString (buf, leftvalue);
                    appendStringInfoString (buf, " = ");
                    appendStringInfoString (buf, rightvalue);
                    //  (*pushdown_clauses)[++clause_count] = true;

                  }
                //  (*pushdown_clauses)[++clause_count] = false;
              }
          }
      }
  }
  heap_close (rel, NoLock);
}

static char*
processWhereClause (Expr *expr, RelOptInfo *baserel, PlannerInfo *root, char ** columns, int columns_count)
{
  StringInfoData result;
  Var *variable;
  Const *constant;
  char *c;
  if (expr->type == T_Const)
    {
      constant = (Const *) expr;
      if (constant->constisnull)
        {
          initStringInfo (&result);
          appendStringInfo (&result, "NULL");
        }
      else
        {
          /* get a string representation of the value */
          c = datumToString (constant->constvalue, constant->consttype);
          if (c == NULL)
            {
              return NULL;
            }
          else
            {
              initStringInfo (&result);
              appendStringInfo (&result, "%s", c);

            }
        }
    }
  else if (expr->type == T_Var)
    {
      int i;
      variable = (Var *) expr;
      /* System columns not supported */
      if (variable->varattno < 1)
        {
          return NULL;
        }
      initStringInfo (&result);
      deparseColumnRef (&result, baserel->relid, variable->varattno, root);
      for (i = 0; i < columns_count; i++)
        {
          if (strcmp (columns[i], result.data) == 0)
            {

              return result.data;
            }
        }

      return NULL;
    }
  return result.data;
}

/*
 * datumToString
 * 		Convert a Datum to a string by calling the type output function.
 */
static char*
datumToString (Datum datum, Oid type)
{
  regproc typoutput;
  char *str;
  StringInfoData result;
  HeapTuple tuple;

  tuple = SearchSysCache1 (TYPEOID, ObjectIdGetDatum (type));
  if (!HeapTupleIsValid (tuple))
    {
      elog (ERROR, "cache lookup failed for type %u", type);
    }
  typoutput = ((Form_pg_type) GETSTRUCT (tuple))->typoutput;
  ReleaseSysCache (tuple);
  switch (type)
    {

    case TEXTOID:
    case CHAROID:
    case BPCHAROID:
    case VARCHAROID:
    case NAMEOID:
    case TIMESTAMPOID:
    case DATEOID:
      str = DatumGetCString (OidFunctionCall1 (typoutput, datum));
      initStringInfo (&result);
      appendStringInfo (&result, "%s%s%s", "'", str, "'");
      break;
    case INTERVALOID:
    default:

      str = DatumGetCString (OidFunctionCall1 (typoutput, datum));
      initStringInfo (&result);
      appendStringInfo (&result, "%s", str);
    }
  return result.data;

}

/*
 * Emit a target list that retrieves the columns specified in attrs_used.
 * This is used for both SELECT and RETURNING targetlists.
 *
 * The tlist text is appended to buf, and we also create an integer List
 * of the columns being retrieved, which is returned to *retrieved_attrs.
 */
static void
deparseTargetList (StringInfo buf,
                   PlannerInfo *root,
                   Index rtindex,
                   Relation rel,
                   Bitmapset *attrs_used,
                   List **retrieved_attrs)
{
  TupleDesc tupdesc = RelationGetDescr (rel);
  bool have_wholerow;
  bool first;
  int i;

  *retrieved_attrs = NIL;

  /* If there's a whole-row reference, we'll need all the columns. */
  have_wholerow = bms_is_member (0 - FirstLowInvalidHeapAttributeNumber,
                                 attrs_used);

  first = true;
  for (i = 1; i <= tupdesc->natts; i++)
    {
      Form_pg_attribute attr = tupdesc->attrs[i - 1];

      /* Ignore dropped attributes. */
      if (attr->attisdropped)
        continue;

      if (have_wholerow ||
          bms_is_member (i - FirstLowInvalidHeapAttributeNumber,
                         attrs_used))
        {
          if (!first)
            appendStringInfoString (buf, ", ");
          first = false;

          deparseColumnRef (buf, rtindex, i, root);

          *retrieved_attrs = lappend_int (*retrieved_attrs, i);
        }
    }

  /*
   * Add ctid if needed.  We currently don't support retrieving any other
   * system columns.
   */
  if (bms_is_member (SelfItemPointerAttributeNumber - FirstLowInvalidHeapAttributeNumber,
                     attrs_used))
    {
      if (!first)
        appendStringInfoString (buf, ", ");
      first = false;

      appendStringInfoString (buf, "ctid");

      *retrieved_attrs = lappend_int (*retrieved_attrs,
                                      SelfItemPointerAttributeNumber);
    }

  /* Don't generate bad syntax if no undropped columns */
  if (first)
    appendStringInfoString (buf, "NULL");
}

/*
 * Construct name to use for given column, and emit it into buf.
 * If it has a column_name FDW option, use that instead of attribute name.
 */
static void
deparseColumnRef (StringInfo buf, int varno, int varattno, PlannerInfo * root)
{

  RangeTblEntry *rte;
  char *colname = NULL;
  List *options;
  ListCell *lc;

  /* varno must not be any of OUTER_VAR, INNER_VAR and INDEX_VAR. */
  Assert (!IS_SPECIAL_VARNO (varno));

  /* Get RangeTblEntry from array in PlannerInfo. */
  rte = planner_rt_fetch (varno, root);

  /*
   * If it's a column of a foreign table, and it has the column_name FDW
   * option, use that value.
   */
  options = GetForeignColumnOptions (rte->relid, varattno);

  foreach (lc, options)
  {
    DefElem *def = (DefElem *) lfirst (lc);

    if (strcmp (def->defname, "column_name") == 0)
      {
        colname = defGetString (def);
        break;
      }
  }

  /*
   * If it's a column of a regular table or it doesn't have column_name FDW
   * option, use attribute name.
   */
  if (colname == NULL)
    colname = get_relid_attribute_name (rte->relid, varattno);

  appendStringInfoString (buf, quote_identifier (colname));
}
