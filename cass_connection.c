/*-------------------------------------------------------------------------
 *
 * cass_connection.c
 *
 *  Created on: Aug 8, 2015
 *      Author: jaimin
 *
 * IDENTIFICATION
 *		  contrib/cassandra2_fdw/cass_connection.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "cassandra2_fdw.h"

#include "access/xact.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

typedef struct ConnCacheKey
{
  Oid serverid; /* OID of foreign server */
  Oid userid; /* OID of local user whose mapping we use */
} ConnCacheKey;

typedef struct ConnCacheEntry
{
  ConnCacheKey key; /* hash key (must be first) */
  CassSession *conn; /* connection to foreign server, or NULL */
  int xact_depth; /* 0 = no xact open, 1 = main xact open, 2 =
								 * one level of subxact open, etc */
  bool have_prep_stmt; /* have we prepared any stmts in this xact? */
  bool have_error; /* have any subxacts aborted in this xact? */
} ConnCacheEntry;

/*
 * Connection cache (initialized on first use)
 */
static HTAB *ConnectionHash = NULL;

/* tracks whether any work is needed in callback functions */
static bool xact_got_connection = false;

/* prototypes of private functions */
static CassSession *connect_cass_server (ForeignServer *server, UserMapping *user);


static CassCluster* cluster;

/*
 * Cass Connection Initialization function
 */
static void pgcass_close (int code, Datum arg);

CassSession *
pgcass_GetConnection (ForeignServer *server, UserMapping *user,
                      bool will_prep_stmt)
{
  bool found;
  ConnCacheEntry *entry;
  ConnCacheKey key;

  /* First time through, initialize connection cache hashtable */
  if (ConnectionHash == NULL)
    {
      HASHCTL ctl;

      MemSet (&ctl, 0, sizeof (ctl));
      ctl.keysize = sizeof (ConnCacheKey);
      ctl.entrysize = sizeof (ConnCacheEntry);
      ctl.hash = tag_hash;
      /* allocate ConnectionHash in the cache context */
      ctl.hcxt = CacheMemoryContext;
      ConnectionHash = hash_create ("cassandra2_fdw connections", 8,
                                    &ctl,
                                    HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);

      /*
       * Register some callback functions that manage connection cleanup.
       * This should be done just once in each backend.
       */
      //		RegisterXactCallback(pgfdw_xact_callback, NULL);
      //		RegisterSubXactCallback(pgfdw_subxact_callback, NULL);
    }

  /* Set flag that we did GetConnection during the current transaction */
  xact_got_connection = true;

  /* Create hash key for the entry.  Assume no pad bytes in key struct */
  key.serverid = server->serverid;
  key.userid = user->userid;

  /*
   * Find or create cached entry for requested connection.
   */
  entry = hash_search (ConnectionHash, &key, HASH_ENTER, &found);
  if (!found)
    {
      /* initialize new hashtable entry (key is already filled in) */
      entry->conn = NULL;
      entry->xact_depth = 0;
      entry->have_prep_stmt = false;
      entry->have_error = false;
    }

  /*
   * We don't check the health of cached connection here, because it would
   * require some overhead.  Broken connection will be detected when the
   * connection is actually used.
   */

  /*
   * If cache entry doesn't have a connection, we have to establish a new
   * connection.  (If connect_pg_server throws an error, the cache entry
   * will be left in a valid empty state.)
   */
  if (entry->conn == NULL)
    {
      entry->xact_depth = 0; /* just to be sure */
      entry->have_prep_stmt = false;
      entry->have_error = false;
      entry->conn = connect_cass_server (server, user);
      elog (DEBUG3, "new cassandra2_fdw connection %p for server \"%s\"",
            entry->conn, server->servername);
    }

  /*
   * do nothing for connection pre-cmd execute till now.
   */

  /* Remember if caller will prepare statements */
  entry->have_prep_stmt |= will_prep_stmt;

  return entry->conn;
}

/*
 * Release connection reference count created by calling GetConnection.
 */
void
pgcass_ReleaseConnection (CassSession *session)
{
  /*
   * Currently, we don't actually track connection references because all
   * cleanup is managed on a transaction or subtransaction basis instead. So
   * there's nothing to do here.
   */
  CassFuture* close_future = NULL;

  return;

  /* Close the session */
  close_future = cass_session_close (session);
  cass_future_wait (close_future);
  cass_future_free (close_future);
}

/*
 * Connect to remote server using specified server and user mapping properties.
 */
static CassSession *
connect_cass_server (ForeignServer *server, UserMapping *user)
{
  ListCell *lc;
  List *list;
  DefElem *def;
  char *dbserver = NULL, *dbuser = NULL, *password = NULL;
  CassFuture* conn_future = NULL;
  CassSession* session = NULL;

  if (!cluster)
    {
      cluster = cass_cluster_new ();
      on_proc_exit (pgcass_close, 0);
    }

  session = cass_session_new ();

  /* TODO Add contact points */
  list = server->options;
  list = list_concat (list, user->options);

  foreach (lc, list)
  {
    def = (DefElem *) lfirst (lc);
    if (strcmp (def->defname, "url") == 0)
      {
        dbserver = ((Value *) (def->arg))->val.str;
      }
    else if (strcmp (def->defname, "username") == 0)
      {
        dbuser = ((Value *) (def->arg))->val.str;
      }
    else if (strcmp (def->defname, "password") == 0)
      {
        password = ((Value *) (def->arg))->val.str;
      }
  }
  /* TODO check if nessesery */
  if (dbuser == NULL)
    {
      dbuser = "cassandra";
    }
  if (password == NULL)
    {
      password = "cassandra";
    }

  ereport (WARNING,
           (errcode (ERRCODE_WARNING),
            errmsg ("Server: %s, user: %s, password: %s", dbserver, dbuser, password)));


  cass_cluster_set_contact_points (cluster, dbserver);
  cass_cluster_set_credentials (cluster, dbuser, password);

  /* Provide the cluster object as configuration to connect the session */
  conn_future = cass_session_connect (session, cluster);
  if (cass_future_error_code (conn_future) != CASS_OK)
    {
      /* Handle error */
      char buf[256];
      const char* message;
      size_t message_length;

      cass_future_error_message (conn_future, &message, &message_length);

      snprintf (buf, 255, "%.*s", (int) message_length, message);
      cass_future_free (conn_future);

      ereport (ERROR,
               (errcode (ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
                errmsg ("could not connect to server \"%s\"",
                        server->servername),
                errdetail_internal ("%s", buf)));
    }

  return session;
}

/*
 * pgcass_close
 * Shuts down the Connection.
 */
static void
pgcass_close (int code, Datum arg)
{
  cass_cluster_free (cluster);
}

