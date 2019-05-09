/*
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://mozilla.org/.
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * Copyright (C) 2000-2001 Panoptic Computer Network
 * Dossy <dossy@panoptic.com>
 *
 * Alternatively, the contents of this file may be used under the terms
 * of the GNU General Public License (the "GPL"), in which case the
 * provisions of GPL are applicable instead of those above.  If you wish
 * to allow use of your version of this file only under the terms of the
 * GPL and not to allow others to use your version of this file under the
 * License, indicate your decision by deleting the provisions above and
 * replace them with the notice and other provisions required by the GPL.
 * If you do not delete the provisions above, a recipient may use your
 * version of this file under either the License or the GPL.
 *
 */

/*
 * nsdbmysql.c --
 *
 *      Implements the nsdb driver interface for the mysql database.
 *
 */

#include "ns.h"
#include "nsdb.h"

/* MySQL API headers */
#include <mysql.h>
extern void my_thread_end(void);

/* Common system headers */
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#define MAX_ERROR_MSG	1024
#define MAX_IDENTIFIER	1024

static const char *DbType(Ns_DbHandle *handle);
static int         DbServerInit(const char *server, const char *module, const char *driver);
static int         DbOpenDb(Ns_DbHandle *handle);
static int         DbCloseDb(Ns_DbHandle *handle);
static int         DbDML(Ns_DbHandle *handle, char *sql);
static Ns_Set     *DbSelect(Ns_DbHandle *handle, char *sql);
static int         DbGetRow(Ns_DbHandle *handle, Ns_Set *row);
static int         DbGetRowCount(Ns_DbHandle *handle);
static int         DbFlush(Ns_DbHandle *handle);
static int         DbCancel(Ns_DbHandle *handle);
static int         DbExec(Ns_DbHandle *handle, char *sql);
static Ns_Set     *DbBindRow(Ns_DbHandle *handle);

static void        Log(Ns_DbHandle *handle, MYSQL *mysql);
static void        InitThread(void);
static Ns_TlsCleanup CleanupThread;
static Ns_Callback AtExit;
static Ns_TclTraceProc DbInterpInit;

NS_EXPORT NsDb_DriverInitProc Ns_DbDriverInit;

static Ns_Tls tls;                  /* For the thread exit callback. */
static int include_tablenames = 0;  /* Include tablename in resultset. */


static Ns_DbProc mysqlProcs[] = {
    { DbFn_Name,         (ns_funcptr_t) DbType },
    { DbFn_DbType,       (ns_funcptr_t) DbType },
    { DbFn_ServerInit,   (ns_funcptr_t) DbServerInit },
    { DbFn_OpenDb,       (ns_funcptr_t) DbOpenDb },
    { DbFn_CloseDb,      (ns_funcptr_t) DbCloseDb },
    { DbFn_DML,          (ns_funcptr_t) DbDML },
    { DbFn_Select,       (ns_funcptr_t) DbSelect },
    { DbFn_GetRow,       (ns_funcptr_t) DbGetRow },
    { DbFn_GetRowCount,  (ns_funcptr_t) DbGetRowCount },
    { DbFn_Flush,        (ns_funcptr_t) DbFlush },
    { DbFn_Cancel,       (ns_funcptr_t) DbCancel },
    { DbFn_Exec,         (ns_funcptr_t) DbExec },
    { DbFn_BindRow,      (ns_funcptr_t) DbBindRow },
    { 0, NULL }
};


NS_EXPORT int   Ns_ModuleVersion = 1;

NS_EXPORT int
Ns_DbDriverInit(const char *driver, const char *UNUSED(path))
{
    static int once = 0;

    if (driver == NULL) {
        Ns_Log(Bug, "nsdbmysql: Ns_DbDriverInit() called with NULL driver name.");
        return NS_ERROR;
    }

    if (!mysql_thread_safe()) {
        Ns_Log(Error, "nsdbmysql: mysql library not compiled thread safe");
        return NS_ERROR;
    }

    if (!once) {
        once = 1;
        if (mysql_library_init(0, NULL, NULL)) {
            return NS_ERROR;
        }
        Ns_TlsAlloc(&tls, CleanupThread);
        Ns_RegisterAtExit(AtExit, NULL);
        Ns_RegisterProcInfo((ns_funcptr_t)AtExit, "nsdbmysql:cleanshutdown", NULL);
    }

    if (Ns_DbRegisterDriver(driver, &(mysqlProcs[0])) != NS_OK) {
        Ns_Log(Error, "nsdbmysql: Could not register the %s driver.", driver);
        return NS_ERROR;
    }
    return NS_OK;
}

static const char *
DbType(Ns_DbHandle *UNUSED(handle))
{
    return "mysql";
}

static int
DbOpenDb(Ns_DbHandle *handle)
{
    MYSQL           *dbh;
    char            *datasource;
    char            *host = NULL;
    char            *database = NULL;
    char            *port = NULL;
    char            *unix_port = NULL;
    unsigned int    tcp_port = 0u;

    if (handle == NULL || handle->datasource == NULL) {
        Ns_Log(Error, "nsdbmysql: Invalid connection.");
        return NS_ERROR;
    }

    InitThread();

    /* handle->datasource = "host:port:database" */
    datasource = host = ns_strcopy(handle->datasource);
    port = strchr(host, ':');
    if (port != NULL) {
        *port++ = '\0';
        database = strchr(port, ':');
        if (database != NULL) {
            *database++ = '\0';
        }
    }
    if (port == NULL || database == NULL) {
        Ns_Log(Error, "nsdbmysql: %s: invalid datasource %s", handle->driver, handle->datasource);
        ns_free(datasource);
        return NS_ERROR;
    }

    if (port[0] == '/') {
        unix_port = port;
    } else {
        tcp_port = (unsigned int) strtol(port, NULL, 10);
    }

    dbh = mysql_init(NULL);
    if (dbh == NULL) {
        Ns_Log(Error, "nsdbmysql: %s: mysql_init() failed", handle->driver);
        ns_free(datasource);
        return NS_ERROR;
    }

    if (mysql_real_connect(dbh, host, handle->user, handle->password, database, tcp_port, unix_port, 0) == 0) {
        Log(handle, dbh);
        mysql_close(dbh);
        ns_free(datasource);
        return NS_ERROR;
    }

    ns_free(datasource);
    handle->connection = (void *) dbh;
    handle->connected = NS_TRUE;

    return NS_OK;
}

static int
DbCloseDb(Ns_DbHandle *handle)
{
    if (handle == NULL || handle->connection == NULL) {
        Ns_Log(Error, "nsdbmysql: Invalid connection.");
        return NS_ERROR;
    }

    InitThread();

    mysql_close((MYSQL *) handle->connection);
    handle->connected = NS_FALSE;
    return NS_OK;
}

static int
DbDML(Ns_DbHandle *handle, char *sql)
{
    int             rc;

    if (sql == NULL) {
        Ns_Log(Error, "nsdbmysql: no sql.");
        return NS_ERROR;
    }

    if (handle == NULL || handle->connection == NULL) {
        Ns_Log(Error, "nsdbmysql: Invalid connection.");
        return NS_ERROR;
    }

    InitThread();

    rc = mysql_query((MYSQL *) handle->connection, sql);
    Log(handle, (MYSQL *) handle->connection);

    if (rc) {
        return NS_ERROR;
    }

    return NS_OK;
}

static Ns_Set  *
DbSelect(Ns_DbHandle *handle, char *sql)
{
    MYSQL_RES      *result;   
    MYSQL_FIELD    *fields;
    int             rc;
    size_t          i;
    unsigned int    numcols;
    Ns_DString     key;

    if (sql == NULL) {
        Ns_Log(Error, "nsdbmysql: no sql.");
        return NULL;
    }

    if (handle == NULL || handle->connection == NULL) {
        Ns_Log(Error, "nsdbmysql: Invalid connection.");
        return NULL;
    }

    InitThread();

    rc = mysql_query((MYSQL *) handle->connection, sql);
    Log(handle, (MYSQL *) handle->connection);

    if (rc) {
        return NULL;
    }

    result = mysql_store_result((MYSQL *) handle->connection);
    Log(handle, (MYSQL *) handle->connection);

    if (result == NULL) {
        return NULL;
    }

    handle->statement = (void *) result;
    handle->fetchingRows = NS_TRUE;

    numcols = mysql_num_fields((MYSQL_RES *) handle->statement);
    Log(handle, (MYSQL *) handle->connection);

    if (numcols == 0u) {
        Ns_Log(Error, "DbSelect(%s):  Query did not return rows:  %s", handle->datasource, sql);
        mysql_free_result((MYSQL_RES *) handle->statement);
        handle->statement = NULL;
        handle->fetchingRows = NS_FALSE;
        return NULL;
    }

    numcols = mysql_num_fields((MYSQL_RES *) handle->statement);
    fields = mysql_fetch_fields((MYSQL_RES *) handle->statement);

    for (i = 0; i < numcols; i++) {
        Ns_DStringInit(&key);

        if (include_tablenames && strlen(fields[i].table) > 0) {
            Ns_DStringVarAppend(&key, fields[i].table, ".", NULL);
        }

        Ns_DStringAppend(&key, fields[i].name);
        Ns_SetPut((Ns_Set *) handle->row, Ns_DStringValue(&key), NULL);
        Ns_DStringFree(&key);
    }

    return (Ns_Set *) handle->row;
}

static int
DbGetRow(Ns_DbHandle *handle, Ns_Set *row)
{
    MYSQL_ROW       my_row;
    size_t          i;
    unsigned int    numcols;

    if (handle->fetchingRows == NS_FALSE) {
        Ns_Log(Error, "DbGetRow(%s):  No rows waiting to fetch.", handle->datasource);
        return NS_ERROR;
    }

    InitThread();

    numcols = mysql_num_fields((MYSQL_RES *) handle->statement);
    Log(handle, (MYSQL *) handle->connection);

    if (numcols == 0) {
        mysql_free_result((MYSQL_RES *) handle->statement);
        handle->statement = NULL;
        handle->fetchingRows = NS_FALSE;
        return NS_ERROR;
    }

    if (numcols != Ns_SetSize(row)) {
        Ns_Log(Error, "DbGetRow: Number of columns in row (%ld)"
                      " not equal to number of columns in row fetched (%d).",
                      Ns_SetSize(row), numcols);
        mysql_free_result((MYSQL_RES *) handle->statement);
        handle->statement = NULL;
        handle->fetchingRows = NS_FALSE;
        return NS_ERROR;
    }

    my_row = mysql_fetch_row((MYSQL_RES *) handle->statement);
    Log(handle, (MYSQL *) handle->connection);

    if (my_row == NULL) {
        mysql_free_result((MYSQL_RES *) handle->statement);
        handle->statement = NULL;
        handle->fetchingRows = NS_FALSE;
        return NS_END_DATA;
    }

    for (i = 0; i < numcols; i++) {
        if (my_row[i] == NULL) {
            Ns_SetPutValue(row, i, "");
        } else {
            Ns_SetPutValue(row, i, my_row[i]);
        }
    }

    return NS_OK;
}

static int
DbGetRowCount(Ns_DbHandle *handle)
{
    if (handle != NULL && handle->connection != NULL) {
        InitThread();

        return (int)mysql_affected_rows((MYSQL *) handle->connection);
    }
    return NS_ERROR;
}

static int
DbFlush(Ns_DbHandle *handle)
{
    return DbCancel(handle);
}

static int
DbCancel(Ns_DbHandle *handle)
{
    if (handle == NULL || handle->connection == NULL) {
        Ns_Log(Error, "nsdbmysql: Invalid connection.");
        return NS_ERROR;
    }

    InitThread();

    if (handle->fetchingRows == NS_TRUE) {
        MYSQL_RES      *result;

        result = (MYSQL_RES *) handle->statement;
        if (result != NULL) {
            mysql_free_result(result);
        }
        handle->statement = NULL;
        handle->fetchingRows = NS_FALSE;
    }

    return NS_OK;
}


static int
DbExec(Ns_DbHandle *handle, char *sql)
{
    MYSQL_RES      *result;
    int             rc;
    unsigned int    numcols, fieldcount;

    if (sql == NULL) {
        Ns_Log(Error, "nsdbmysql: no sql.");
        return NS_ERROR;
    }

    if (handle == NULL || handle->connection == NULL) {
        Ns_Log(Error, "nsdbmysql: Invalid connection.");
        return NS_ERROR;
    }

    InitThread();

    rc = mysql_query((MYSQL *) handle->connection, sql);
    Log(handle, (MYSQL *) handle->connection);

    if (rc) {
        return NS_ERROR;
    }

    result = mysql_store_result((MYSQL *) handle->connection);
    Log(handle, (MYSQL *) handle->connection);

    fieldcount = mysql_field_count((MYSQL *) handle->connection);
    Log(handle, (MYSQL *) handle->connection);

    if (result == NULL) {
    	if (fieldcount == 0) {	
    	    return NS_DML;
    	} else {
    	    Ns_Log(Error, "nsdbmysql: DbExec() has columns but result set is NULL");
    	    return NS_ERROR;
    	}
    }

    numcols = mysql_num_fields(result);
    Log(handle, (MYSQL *) handle->connection);

    if (numcols != 0) {
        handle->statement = (void *) result;
        handle->fetchingRows = NS_TRUE;
        return NS_ROWS;
    } else {
        mysql_free_result(result);
        return NS_DML;
    }

    /* How did we get here? */
    return NS_ERROR;
}

static Ns_Set  *
DbBindRow(Ns_DbHandle *handle)
{
    MYSQL_FIELD    *fields;
    unsigned int    i;
    unsigned int    numcols;
    Ns_DString     key;

    if (handle == NULL || handle->statement == NULL) {
        Ns_Log(Error, "nsdbmysql: Invalid connection.");
        return NULL;
    }

    InitThread();

    numcols = mysql_num_fields((MYSQL_RES *) handle->statement);

    fields = mysql_fetch_fields((MYSQL_RES *) handle->statement);

    for (i = 0; i < numcols; i++) {
        Ns_DStringInit(&key);

        if (include_tablenames && strlen(fields[i].table) > 0) {
            Ns_DStringVarAppend(&key, fields[i].table, ".", NULL);
        }

        Ns_DStringAppend(&key, fields[i].name);
        Ns_SetPut((Ns_Set *) handle->row, Ns_DStringValue(&key), NULL);
        Ns_DStringFree(&key);
    }

    return (Ns_Set *) handle->row;
}

/* ************************************************************ */

static int 
DbList_Dbs(Tcl_Interp *interp, const char *wild, Ns_DbHandle *handle)
{
    MYSQL_RES      *result;
    MYSQL_ROW       row;
    unsigned int    numcols;
    unsigned int    i;

    if (handle == NULL || handle->connection == NULL) {
        Ns_Log(Error, "nsdbmysql: Invalid connection.");
        return NS_ERROR;
    }

    InitThread();

    result = mysql_list_dbs((MYSQL *) handle->connection, wild);
    Log(handle, (MYSQL *) handle->connection);

    if (result == NULL) {
        Tcl_AppendResult(interp, "mysql_list_dbs failed.", NULL);
        return TCL_ERROR;
    }

    numcols = mysql_num_fields(result);
    Log(handle, (MYSQL *) handle->connection);

    while ((row = mysql_fetch_row(result)) != NULL) {
        for (i = 0; i < numcols; i++) {
            Tcl_AppendElement(interp, row[i]);
        }
    }
    mysql_free_result(result);

    return TCL_OK;
}

static int 
DbList_Tables(Tcl_Interp *interp, const char *wild, Ns_DbHandle *handle)
{
    MYSQL_RES      *result;
    MYSQL_ROW       row;
    unsigned int    numcols;
    unsigned int    i;

    if (handle == NULL || handle->connection == NULL) {
        Ns_Log(Error, "nsdbmysql: Invalid connection.");
        return NS_ERROR;
    }

    InitThread();

    result = mysql_list_tables((MYSQL *) handle->connection, wild);
    Log(handle, (MYSQL *) handle->connection);

    if (result == NULL) {
        Tcl_AppendResult(interp, "mysql_list_tables failed.", NULL);
        return TCL_ERROR;
    }

    numcols = mysql_num_fields(result);
    Log(handle, (MYSQL *) handle->connection);

    while ((row = mysql_fetch_row(result)) != NULL) {
        for (i = 0; i < numcols; i++) {
            Tcl_AppendElement(interp, row[i]);
        }
    }

    mysql_free_result(result);

    return TCL_OK;
}

/*
 * DbCmd - This function implements the "ns_mysql" Tcl command
 * installed into each interpreter of each virtual server.  It provides
 * access to features specific to the MySQL driver.
 */

static int
DbCmd(ClientData UNUSED(arg), Tcl_Interp *interp, int objc, Tcl_Obj * const objv[])
{
    Ns_DbHandle    *handle;
    char           *wild;
    int             rc;

    static const char *opts[] = {
        "include_tablenames", "list_dbs", "list_tables",
        "resultrows", "select_db", "insert_id", "version",
        NULL
    };
    enum {
        IIncludeTableNamesIdx, IListDbsIdx, IListTablesIdx,
        IResultRowsIdx, ISelectDbIdx, IInsertIdIdx, IVersionIdx
    } opt;

    if (objc < 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "option handle ?args?");
        return TCL_ERROR;
    }

    if (Tcl_GetIndexFromObj(interp, objv[1], opts, "option", 1, (int *) &opt) != TCL_OK) {
        return TCL_ERROR;
    }

    if (Ns_TclDbGetHandle(interp, Tcl_GetString(objv[2]), &handle) != TCL_OK) {
        return TCL_ERROR;
    }

    if (!STREQ(Ns_DbDriverName(handle), DbType(0))) {
        Tcl_AppendResult(interp, "handle \"", Tcl_GetString(objv[2]),
                "\" is not of type \"", DbType(0), "\"", NULL);
        return TCL_ERROR;
    }

    InitThread();

    switch (opt) {
    case IIncludeTableNamesIdx:
        if (objc != 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "handle 1|0");
            return TCL_ERROR;
        }
        include_tablenames = atoi(Tcl_GetString(objv[3]));
        break;

    case IInsertIdIdx:
        Tcl_SetObjResult(interp, Tcl_NewWideIntObj((Tcl_WideInt)mysql_insert_id((MYSQL *) handle->connection)));

        break;

    case IListDbsIdx:
        if (objc > 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "handle ?wild?");
            return TCL_ERROR;
        }
        if (objc == 3) {
            wild = NULL;
        } else {
            wild = Tcl_GetString(objv[3]);
        }
        return DbList_Dbs(interp, wild, handle);

    case IListTablesIdx:
        if (objc > 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "handle ?wild?");
            return TCL_ERROR;
        }
        if (objc == 3) {
            wild = NULL;
        } else {
            wild = Tcl_GetString(objv[3]);
        }
        return DbList_Tables(interp, wild, handle);

    case IResultRowsIdx:
        Tcl_SetObjResult(interp, Tcl_NewWideIntObj((Tcl_WideInt)mysql_affected_rows((MYSQL *) handle->connection)));
        break;

    case ISelectDbIdx:
        if (objc != 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "handle database");
            return TCL_ERROR;
        }
        rc = mysql_select_db((MYSQL *) handle->connection, Tcl_GetString(objv[3]));
        if (rc) {
            Tcl_AppendResult(interp, "mysql_select_db failed.", NULL);
            return TCL_ERROR;
        }
        break;

    case IVersionIdx:
        Tcl_AppendResult(interp, "mysql", mysql_get_server_info((MYSQL *) handle->connection), NULL);
        return TCL_OK;
    }
    
    return TCL_OK;
}

static int DbInterpInit(Tcl_Interp * interp, const void *UNUSED(ignored))
{
    Tcl_CreateObjCommand(interp, "ns_mysql", DbCmd, NULL, NULL);
    return NS_OK;
}

static int DbServerInit(const char *server, const char *UNUSED(module), const char *UNUSED(driver))
{
    Ns_TclRegisterTrace(server, DbInterpInit, NULL, NS_TCL_TRACE_CREATE);
    return NS_OK;
}

static void
Log(Ns_DbHandle *handle, MYSQL *mysql)
{
    Ns_LogSeverity  severity = Error;
    unsigned int    nErr;
    char            msg[MAX_ERROR_MSG + 1];

    nErr = mysql_errno(mysql);
    if (nErr) {
        strncpy(msg, mysql_error(mysql), MAX_ERROR_MSG);
        Ns_Log(severity, "MySQL log message: (%u) '%s'", nErr, msg);

        if (handle != NULL) {
            sprintf(handle->cExceptionCode, "%u", nErr);
            Ns_DStringFree(&(handle->dsExceptionMsg));
            Ns_DStringAppend(&(handle->dsExceptionMsg), msg);
        }
    }
}

/*
 *----------------------------------------------------------------------
 *
 * InitThread, CleanupThread --
 *
 *      Initialise and cleanup msql thread data for each thread
 *      that calls a mysql_* function.
 *
 *      InitThread is called from Open, Prepare and Exec, the 3 functions
 *      which a thread must call before calling any other dbi functions.
 *
 *      CleanupThread is a Tls callback which gets called only when a
 *      thread exits.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      MySQL memory is freed on thread exit.
 *
 *----------------------------------------------------------------------
 */


static void
InitThread(void)
{
    int initialized;

    initialized = PTR2INT(Ns_TlsGet(&tls));
    if (!initialized) {
        Ns_TlsSet(&tls, (void *) NS_TRUE);
        Ns_Log(Debug, "nsdbmysql: InitThread");
        mysql_thread_init();
    }
}

static void
CleanupThread(void *arg)
{
    int initialized = PTR2INT(arg);

    if (initialized) {
        Ns_Log(Debug, "nsdbmysql: CleanupThread");
        mysql_thread_end();
    }
}


/*
 *----------------------------------------------------------------------
 *
 * AtExit --
 *
 *      Cleanup the mysql library when the server exits. This is
 *      important when running the embedded server.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Embedded mysql may flush data to disk and close tables cleanly.
 *
 *----------------------------------------------------------------------
 */

static void
AtExit(void *UNUSED(arg))
{
    Ns_Log(Debug, "nsdbmysql: AtExit");
    mysql_library_end();
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
