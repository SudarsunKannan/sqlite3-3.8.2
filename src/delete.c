/*
** 2001 September 15
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains C code routines that are called by the parser
** in order to generate code for DELETE FROM statements.
*/
#include "sqliteInt.h"

/*
** While a SrcList can in general represent multiple tables and subqueries
** (as in the FROM clause of a SELECT statement) in this case it contains
** the name of a single table, as one might find in an INSERT, DELETE,
** or UPDATE statement.  Look up that table in the symbol table and
** return a pointer.  Set an error message and return NULL if the table 
** name is not found or if any other error occurs.
**
** The following fields are initialized appropriate in pSrc:
**
**    pSrc->a[0].pTab       Pointer to the Table object
**    pSrc->a[0].pIndex     Pointer to the INDEXED BY index, if there is one
**
*/
Table *sqlite3SrcListLookup(Parse *pParse, SrcList *pSrc){
  struct SrcList_item *pItem = pSrc->a;
  Table *pTab;
  assert( pItem && pSrc->nSrc==1 );
  pTab = sqlite3LocateTableItem(pParse, 0, pItem);
  sqlite3DeleteTable(pParse->db, pItem->pTab);
  pItem->pTab = pTab;
  if( pTab ){
    pTab->nRef++;
  }
  if( sqlite3IndexedByLookup(pParse, pItem) ){
    pTab = 0;
  }
  return pTab;
}

/*
** Check to make sure the given table is writable.  If it is not
** writable, generate an error message and return 1.  If it is
** writable return 0;
*/
int sqlite3IsReadOnly(Parse *pParse, Table *pTab, int viewOk){
  /* A table is not writable under the following circumstances:
  **
  **   1) It is a virtual table and no implementation of the xUpdate method
  **      has been provided, or
  **   2) It is a system table (i.e. sqlite_master), this call is not
  **      part of a nested parse and writable_schema pragma has not 
  **      been specified.
  **
  ** In either case leave an error message in pParse and return non-zero.
  */
  if( ( IsVirtual(pTab) 
     && sqlite3GetVTable(pParse->db, pTab)->pMod->pModule->xUpdate==0 )
   || ( (pTab->tabFlags & TF_Readonly)!=0
     && (pParse->db->flags & SQLITE_WriteSchema)==0
     && pParse->nested==0 )
  ){
    sqlite3ErrorMsg(pParse, "table %s may not be modified", pTab->zName);
    return 1;
  }

#ifndef SQLITE_OMIT_VIEW
  if( !viewOk && pTab->pSelect ){
    sqlite3ErrorMsg(pParse,"cannot modify %s because it is a view",pTab->zName);
    return 1;
  }
#endif
  return 0;
}


#if !defined(SQLITE_OMIT_VIEW) && !defined(SQLITE_OMIT_TRIGGER)
/*
** Evaluate a view and store its result in an ephemeral table.  The
** pWhere argument is an optional WHERE clause that restricts the
** set of rows in the view that are to be added to the ephemeral table.
*/
void sqlite3MaterializeView(
  Parse *pParse,       /* Parsing context */
  Table *pView,        /* View definition */
  Expr *pWhere,        /* Optional WHERE clause to be added */
  int iCur             /* Cursor number for ephemerial table */
){
  SelectDest dest;
  Select *pSel;
  SrcList *pFrom;
  sqlite3 *db = pParse->db;
  int iDb = sqlite3SchemaToIndex(db, pView->pSchema);

  pWhere = sqlite3ExprDup(db, pWhere, 0);
  pFrom = sqlite3SrcListAppend(db, 0, 0, 0);

  if( pFrom ){
    assert( pFrom->nSrc==1 );
    pFrom->a[0].zName = sqlite3DbStrDup(db, pView->zName);
    pFrom->a[0].zDatabase = sqlite3DbStrDup(db, db->aDb[iDb].zName);
    assert( pFrom->a[0].pOn==0 );
    assert( pFrom->a[0].pUsing==0 );
  }

  pSel = sqlite3SelectNew(pParse, 0, pFrom, pWhere, 0, 0, 0, 0, 0, 0);
  if( pSel ) pSel->selFlags |= SF_Materialize;

  sqlite3SelectDestInit(&dest, SRT_EphemTab, iCur);
  sqlite3Select(pParse, pSel, &dest);
  sqlite3SelectDelete(db, pSel);
}
#endif /* !defined(SQLITE_OMIT_VIEW) && !defined(SQLITE_OMIT_TRIGGER) */

#if defined(SQLITE_ENABLE_UPDATE_DELETE_LIMIT) && !defined(SQLITE_OMIT_SUBQUERY)
/*
** Generate an expression tree to implement the WHERE, ORDER BY,
** and LIMIT/OFFSET portion of DELETE and UPDATE statements.
**
**     DELETE FROM table_wxyz WHERE a<5 ORDER BY a LIMIT 1;
**                            \__________________________/
**                               pLimitWhere (pInClause)
*/
Expr *sqlite3LimitWhere(
  Parse *pParse,               /* The parser context */
  SrcList *pSrc,               /* the FROM clause -- which tables to scan */
  Expr *pWhere,                /* The WHERE clause.  May be null */
  ExprList *pOrderBy,          /* The ORDER BY clause.  May be null */
  Expr *pLimit,                /* The LIMIT clause.  May be null */
  Expr *pOffset,               /* The OFFSET clause.  May be null */
  char *zStmtType              /* Either DELETE or UPDATE.  For err msgs. */
){
  Expr *pWhereRowid = NULL;    /* WHERE rowid .. */
  Expr *pInClause = NULL;      /* WHERE rowid IN ( select ) */
  Expr *pSelectRowid = NULL;   /* SELECT rowid ... */
  ExprList *pEList = NULL;     /* Expression list contaning only pSelectRowid */
  SrcList *pSelectSrc = NULL;  /* SELECT rowid FROM x ... (dup of pSrc) */
  Select *pSelect = NULL;      /* Complete SELECT tree */

  /* Check that there isn't an ORDER BY without a LIMIT clause.
  */
  if( pOrderBy && (pLimit == 0) ) {
    sqlite3ErrorMsg(pParse, "ORDER BY without LIMIT on %s", zStmtType);
    goto limit_where_cleanup_2;
  }

  /* We only need to generate a select expression if there
  ** is a limit/offset term to enforce.
  */
  if( pLimit == 0 ) {
    /* if pLimit is null, pOffset will always be null as well. */
    assert( pOffset == 0 );
    return pWhere;
  }

  /* Generate a select expression tree to enforce the limit/offset 
  ** term for the DELETE or UPDATE statement.  For example:
  **   DELETE FROM table_a WHERE col1=1 ORDER BY col2 LIMIT 1 OFFSET 1
  ** becomes:
  **   DELETE FROM table_a WHERE rowid IN ( 
  **     SELECT rowid FROM table_a WHERE col1=1 ORDER BY col2 LIMIT 1 OFFSET 1
  **   );
  */

  pSelectRowid = sqlite3PExpr(pParse, TK_ROW, 0, 0, 0);
  if( pSelectRowid == 0 ) goto limit_where_cleanup_2;
  pEList = sqlite3ExprListAppend(pParse, 0, pSelectRowid);
  if( pEList == 0 ) goto limit_where_cleanup_2;

  /* duplicate the FROM clause as it is needed by both the DELETE/UPDATE tree
  ** and the SELECT subtree. */
  pSelectSrc = sqlite3SrcListDup(pParse->db, pSrc, 0);
  if( pSelectSrc == 0 ) {
    sqlite3ExprListDelete(pParse->db, pEList);
    goto limit_where_cleanup_2;
  }

  /* generate the SELECT expression tree. */
  pSelect = sqlite3SelectNew(pParse,pEList,pSelectSrc,pWhere,0,0,
                             pOrderBy,0,pLimit,pOffset);
  if( pSelect == 0 ) return 0;

  /* now generate the new WHERE rowid IN clause for the DELETE/UDPATE */
  pWhereRowid = sqlite3PExpr(pParse, TK_ROW, 0, 0, 0);
  if( pWhereRowid == 0 ) goto limit_where_cleanup_1;
  pInClause = sqlite3PExpr(pParse, TK_IN, pWhereRowid, 0, 0);
  if( pInClause == 0 ) goto limit_where_cleanup_1;

  pInClause->x.pSelect = pSelect;
  pInClause->flags |= EP_xIsSelect;
  sqlite3ExprSetHeight(pParse, pInClause);
  return pInClause;

  /* something went wrong. clean up anything allocated. */
limit_where_cleanup_1:
  sqlite3SelectDelete(pParse->db, pSelect);
  return 0;

limit_where_cleanup_2:
  sqlite3ExprDelete(pParse->db, pWhere);
  sqlite3ExprListDelete(pParse->db, pOrderBy);
  sqlite3ExprDelete(pParse->db, pLimit);
  sqlite3ExprDelete(pParse->db, pOffset);
  return 0;
}
#endif /* defined(SQLITE_ENABLE_UPDATE_DELETE_LIMIT) */
       /*      && !defined(SQLITE_OMIT_SUBQUERY) */

/*
** Generate code for a DELETE FROM statement.
**
**     DELETE FROM table_wxyz WHERE a<5 AND b NOT NULL;
**                 \________/       \________________/
**                  pTabList              pWhere
*/
void sqlite3DeleteFrom(
  Parse *pParse,         /* The parser context */
  SrcList *pTabList,     /* The table from which we should delete things */
  Expr *pWhere           /* The WHERE clause.  May be null */
){
  Vdbe *v;               /* The virtual database engine */
  Table *pTab;           /* The table from which records will be deleted */
  const char *zDb;       /* Name of database holding pTab */
  int i;                 /* Loop counter */
  WhereInfo *pWInfo;     /* Information about the WHERE clause */
  Index *pIdx;           /* For looping over indices of the table */
  int iTabCur;           /* Cursor number for the table */
  int iDataCur;          /* VDBE cursor for the canonical data source */
  int iIdxCur;           /* Cursor number of the first index */
  int nIdx;              /* Number of indices */
  sqlite3 *db;           /* Main database structure */
  AuthContext sContext;  /* Authorization context */
  NameContext sNC;       /* Name context to resolve expressions in */
  int iDb;               /* Database number */
  int memCnt = -1;       /* Memory cell used for change counting */
  int rcauth;            /* Value returned by authorization callback */
  int okOnePass;         /* True for one-pass algorithm without the FIFO */
  int aiCurOnePass[2];   /* The write cursors opened by WHERE_ONEPASS */
  u8 *aToOpen = 0;       /* Open cursor iTabCur+j if aToOpen[j] is true */
  Index *pPk;            /* The PRIMARY KEY index on the table */
  int iPk = 0;           /* First of nPk registers holding PRIMARY KEY value */
  i16 nPk = 1;           /* Number of columns in the PRIMARY KEY */
  int iKey;              /* Memory cell holding key of row to be deleted */
  i16 nKey;              /* Number of memory cells in the row key */
  int iEphCur = 0;       /* Ephemeral table holding all primary key values */
  int iRowSet = 0;       /* Register for rowset of rows to delete */
  int addrBypass = 0;    /* Address of jump over the delete logic */
  int addrLoop = 0;      /* Top of the delete loop */
  int addrDelete = 0;    /* Jump directly to the delete logic */
  int addrEphOpen = 0;   /* Instruction to open the Ephermeral table */
 
#ifndef SQLITE_OMIT_TRIGGER
  int isView;                  /* True if attempting to delete from a view */
  Trigger *pTrigger;           /* List of table triggers, if required */
#endif

  memset(&sContext, 0, sizeof(sContext));
  db = pParse->db;
  if( pParse->nErr || db->mallocFailed ){
    goto delete_from_cleanup;
  }
  assert( pTabList->nSrc==1 );

  /* Locate the table which we want to delete.  This table has to be
  ** put in an SrcList structure because some of the subroutines we
  ** will be calling are designed to work with multiple tables and expect
  ** an SrcList* parameter instead of just a Table* parameter.
  */
  pTab = sqlite3SrcListLookup(pParse, pTabList);
  if( pTab==0 )  goto delete_from_cleanup;

  /* Figure out if we have any triggers and if the table being
  ** deleted from is a view
  */
#ifndef SQLITE_OMIT_TRIGGER
  pTrigger = sqlite3TriggersExist(pParse, pTab, TK_DELETE, 0, 0);
  isView = pTab->pSelect!=0;
#else
# define pTrigger 0
# define isView 0
#endif
#ifdef SQLITE_OMIT_VIEW
# undef isView
# define isView 0
#endif

  /* If pTab is really a view, make sure it has been initialized.
  */
  if( sqlite3ViewGetColumnNames(pParse, pTab) ){
    goto delete_from_cleanup;
  }

  if( sqlite3IsReadOnly(pParse, pTab, (pTrigger?1:0)) ){
    goto delete_from_cleanup;
  }
  iDb = sqlite3SchemaToIndex(db, pTab->pSchema);
  assert( iDb<db->nDb );
  zDb = db->aDb[iDb].zName;
  rcauth = sqlite3AuthCheck(pParse, SQLITE_DELETE, pTab->zName, 0, zDb);
  assert( rcauth==SQLITE_OK || rcauth==SQLITE_DENY || rcauth==SQLITE_IGNORE );
  if( rcauth==SQLITE_DENY ){
    goto delete_from_cleanup;
  }
  assert(!isView || pTrigger);

  /* Assign cursor numbers to the table and all its indices.
  */
  assert( pTabList->nSrc==1 );
  iTabCur = pTabList->a[0].iCursor = pParse->nTab++;
  for(nIdx=0, pIdx=pTab->pIndex; pIdx; pIdx=pIdx->pNext, nIdx++){
    pParse->nTab++;
  }

  /* Start the view context
  */
  if( isView ){
    sqlite3AuthContextPush(pParse, &sContext, pTab->zName);
  }

  /* Begin generating code.
  */
  v = sqlite3GetVdbe(pParse);
  if( v==0 ){
    goto delete_from_cleanup;
  }
  if( pParse->nested==0 ) sqlite3VdbeCountChanges(v);
  sqlite3BeginWriteOperation(pParse, 1, iDb);

  /* If we are trying to delete from a view, realize that view into
  ** a ephemeral table.
  */
#if !defined(SQLITE_OMIT_VIEW) && !defined(SQLITE_OMIT_TRIGGER)
  if( isView ){
    sqlite3MaterializeView(pParse, pTab, pWhere, iTabCur);
    iDataCur = iIdxCur = iTabCur;
  }
#endif

  /* Resolve the column names in the WHERE clause.
  */
  memset(&sNC, 0, sizeof(sNC));
  sNC.pParse = pParse;
  sNC.pSrcList = pTabList;
  if( sqlite3ResolveExprNames(&sNC, pWhere) ){
    goto delete_from_cleanup;
  }

  /* Initialize the counter of the number of rows deleted, if
  ** we are counting rows.
  */
  if( db->flags & SQLITE_CountRows ){
    memCnt = ++pParse->nMem;
    sqlite3VdbeAddOp2(v, OP_Integer, 0, memCnt);
  }

#ifndef SQLITE_OMIT_TRUNCATE_OPTIMIZATION
  /* Special case: A DELETE without a WHERE clause deletes everything.
  ** It is easier just to erase the whole table. Prior to version 3.6.5,
  ** this optimization caused the row change count (the value returned by 
  ** API function sqlite3_count_changes) to be set incorrectly.  */
  if( rcauth==SQLITE_OK && pWhere==0 && !pTrigger && !IsVirtual(pTab) 
   && 0==sqlite3FkRequired(pParse, pTab, 0, 0)
  ){
    assert( !isView );
    sqlite3TableLock(pParse, iDb, pTab->tnum, 1, pTab->zName);
    if( HasRowid(pTab) ){
      sqlite3VdbeAddOp4(v, OP_Clear, pTab->tnum, iDb, memCnt,
                        pTab->zName, P4_STATIC);
    }
    for(pIdx=pTab->pIndex; pIdx; pIdx=pIdx->pNext){
      assert( pIdx->pSchema==pTab->pSchema );
      sqlite3VdbeAddOp2(v, OP_Clear, pIdx->tnum, iDb);
    }
  }else
#endif /* SQLITE_OMIT_TRUNCATE_OPTIMIZATION */
  {
    if( HasRowid(pTab) ){
      /* For a rowid table, initialize the RowSet to an empty set */
      pPk = 0;
      nPk = 1;
      iRowSet = ++pParse->nMem;
      sqlite3VdbeAddOp2(v, OP_Null, 0, iRowSet);
    }else{
      /* For a WITHOUT ROWID table, create an ephermeral table used to
      ** hold all primary keys for rows to be deleted. */
      pPk = sqlite3PrimaryKeyIndex(pTab);
      assert( pPk!=0 );
      nPk = pPk->nKeyCol;
      iPk = pParse->nMem+1;
      pParse->nMem += nPk;
      iEphCur = pParse->nTab++;
      addrEphOpen = sqlite3VdbeAddOp2(v, OP_OpenEphemeral, iEphCur, nPk);
      sqlite3VdbeSetP4KeyInfo(pParse, pPk);
    }
  
    /* Construct a query to find the rowid or primary key for every row
    ** to be deleted, based on the WHERE clause.
    */
    pWInfo = sqlite3WhereBegin(pParse, pTabList, pWhere, 0, 0, 
                               WHERE_ONEPASS_DESIRED|WHERE_DUPLICATES_OK,
                               iTabCur+1);
    if( pWInfo==0 ) goto delete_from_cleanup;
    okOnePass = sqlite3WhereOkOnePass(pWInfo, aiCurOnePass);
  
    /* Keep track of the number of rows to be deleted */
    if( db->flags & SQLITE_CountRows ){
      sqlite3VdbeAddOp2(v, OP_AddImm, memCnt, 1);
    }
  
    /* Extract the rowid or primary key for the current row */
    if( pPk ){
      for(i=0; i<nPk; i++){
        sqlite3ExprCodeGetColumnOfTable(v, pTab, iTabCur,
                                        pPk->aiColumn[i], iPk+i);
      }
      iKey = iPk;
    }else{
      iKey = pParse->nMem + 1;
      iKey = sqlite3ExprCodeGetColumn(pParse, pTab, -1, iTabCur, iKey, 0);
      if( iKey>pParse->nMem ) pParse->nMem = iKey;
    }
  
    if( okOnePass ){
      /* For ONEPASS, no need to store the rowid/primary-key.  There is only
      ** one, so just keep it in its register(s) and fall through to the
      ** delete code.
      */
      nKey = nPk; /* OP_Found will use an unpacked key */
      aToOpen = sqlite3DbMallocRaw(db, nIdx+2);
      if( aToOpen==0 ){
        sqlite3WhereEnd(pWInfo);
        goto delete_from_cleanup;
      }
      memset(aToOpen, 1, nIdx+1);
      aToOpen[nIdx+1] = 0;
      if( aiCurOnePass[0]>=0 ) aToOpen[aiCurOnePass[0]-iTabCur] = 0;
      if( aiCurOnePass[1]>=0 ) aToOpen[aiCurOnePass[1]-iTabCur] = 0;
      if( addrEphOpen ) sqlite3VdbeChangeToNoop(v, addrEphOpen);
      addrDelete = sqlite3VdbeAddOp0(v, OP_Goto); /* Jump to DELETE logic */
    }else if( pPk ){
      /* Construct a composite key for the row to be deleted and remember it */
      iKey = ++pParse->nMem;
      nKey = 0;   /* Zero tells OP_Found to use a composite key */
      sqlite3VdbeAddOp4(v, OP_MakeRecord, iPk, nPk, iKey,
                        sqlite3IndexAffinityStr(v, pPk), P4_TRANSIENT);
      sqlite3VdbeAddOp2(v, OP_IdxInsert, iEphCur, iKey);
    }else{
      /* Get the rowid of the row to be deleted and remember it in the RowSet */
      nKey = 1;  /* OP_Seek always uses a single rowid */
      sqlite3VdbeAddOp2(v, OP_RowSetAdd, iRowSet, iKey);
    }
  
    /* End of the WHERE loop */
    sqlite3WhereEnd(pWInfo);
    if( okOnePass ){
      /* Bypass the delete logic below if the WHERE loop found zero rows */
      addrBypass = sqlite3VdbeMakeLabel(v);
      sqlite3VdbeAddOp2(v, OP_Goto, 0, addrBypass);
      sqlite3VdbeJumpHere(v, addrDelete);
    }
  
    /* Unless this is a view, open cursors for the table we are 
    ** deleting from and all its indices. If this is a view, then the
    ** only effect this statement has is to fire the INSTEAD OF 
    ** triggers.
    */
    if( !isView ){
      sqlite3OpenTableAndIndices(pParse, pTab, OP_OpenWrite, iTabCur, aToOpen,
                                 &iDataCur, &iIdxCur);
      assert( pPk || iDataCur==iTabCur );
      assert( pPk || iIdxCur==iDataCur+1 );
    }
  
    /* Set up a loop over the rowids/primary-keys that were found in the
    ** where-clause loop above.
    */
    if( okOnePass ){
      /* Just one row.  Hence the top-of-loop is a no-op */
      assert( nKey==nPk ); /* OP_Found will use an unpacked key */
      if( aToOpen[iDataCur-iTabCur] ){
        assert( pPk!=0 );
        sqlite3VdbeAddOp4Int(v, OP_NotFound, iDataCur, addrBypass, iKey, nKey);
      }
    }else if( pPk ){
      addrLoop = sqlite3VdbeAddOp1(v, OP_Rewind, iEphCur);
      sqlite3VdbeAddOp2(v, OP_RowKey, iEphCur, iKey);
      assert( nKey==0 );  /* OP_Found will use a composite key */
    }else{
      addrLoop = sqlite3VdbeAddOp3(v, OP_RowSetRead, iRowSet, 0, iKey);
      assert( nKey==1 );
    }  
  
    /* Delete the row */
#ifndef SQLITE_OMIT_VIRTUALTABLE
    if( IsVirtual(pTab) ){
      const char *pVTab = (const char *)sqlite3GetVTable(db, pTab);
      sqlite3VtabMakeWritable(pParse, pTab);
      sqlite3VdbeAddOp4(v, OP_VUpdate, 0, 1, iKey, pVTab, P4_VTAB);
      sqlite3VdbeChangeP5(v, OE_Abort);
      sqlite3MayAbort(pParse);
    }else
#endif
    {
      int count = (pParse->nested==0);    /* True to count changes */
      sqlite3GenerateRowDelete(pParse, pTab, pTrigger, iDataCur, iIdxCur,
                               iKey, nKey, count, OE_Default, okOnePass);
    }
  
    /* End of the loop over all rowids/primary-keys. */
    if( okOnePass ){
      sqlite3VdbeResolveLabel(v, addrBypass);
    }else if( pPk ){
      sqlite3VdbeAddOp2(v, OP_Next, iEphCur, addrLoop+1);
      sqlite3VdbeJumpHere(v, addrLoop);
    }else{
      sqlite3VdbeAddOp2(v, OP_Goto, 0, addrLoop);
      sqlite3VdbeJumpHere(v, addrLoop);
    }     
  
    /* Close the cursors open on the table and its indexes. */
    if( !isView && !IsVirtual(pTab) ){
      if( !pPk ) sqlite3VdbeAddOp1(v, OP_Close, iDataCur);
      for(i=0, pIdx=pTab->pIndex; pIdx; i++, pIdx=pIdx->pNext){
        sqlite3VdbeAddOp1(v, OP_Close, iIdxCur + i);
      }
    }
  } /* End non-truncate path */

  /* Update the sqlite_sequence table by storing the content of the
  ** maximum rowid counter values recorded while inserting into
  ** autoincrement tables.
  */
  if( pParse->nested==0 && pParse->pTriggerTab==0 ){
    sqlite3AutoincrementEnd(pParse);
  }

  /* Return the number of rows that were deleted. If this routine is 
  ** generating code because of a call to sqlite3NestedParse(), do not
  ** invoke the callback function.
  */
  if( (db->flags&SQLITE_CountRows) && !pParse->nested && !pParse->pTriggerTab ){
    sqlite3VdbeAddOp2(v, OP_ResultRow, memCnt, 1);
    sqlite3VdbeSetNumCols(v, 1);
    sqlite3VdbeSetColName(v, 0, COLNAME_NAME, "rows deleted", SQLITE_STATIC);
  }

delete_from_cleanup:
  sqlite3AuthContextPop(&sContext);
  sqlite3SrcListDelete(db, pTabList);
  sqlite3ExprDelete(db, pWhere);
  sqlite3DbFree(db, aToOpen);
  return;
}
/* Make sure "isView" and other macros defined above are undefined. Otherwise
** thely may interfere with compilation of other functions in this file
** (or in another file, if this file becomes part of the amalgamation).  */
#ifdef isView
 #undef isView
#endif
#ifdef pTrigger
 #undef pTrigger
#endif

/*
** This routine generates VDBE code that causes a single row of a
** single table to be deleted.  Both the original table entry and
** all indices are removed.
**
** Preconditions:
**
**   1.  iDataCur is an open cursor on the btree that is the canonical data
**       store for the table.  (This will be either the table itself,
**       in the case of a rowid table, or the PRIMARY KEY index in the case
**       of a WITHOUT ROWID table.)
**
**   2.  Read/write cursors for all indices of pTab must be open as
**       cursor number iIdxCur+i for the i-th index.
**
**   3.  The primary key for the row to be deleted must be stored in a
**       sequence of nPk memory cells starting at iPk.  If nPk==0 that means
**       that a search record formed from OP_MakeRecord is contained in the
**       single memory location iPk.
*/
void sqlite3GenerateRowDelete(
  Parse *pParse,     /* Parsing context */
  Table *pTab,       /* Table containing the row to be deleted */
  Trigger *pTrigger, /* List of triggers to (potentially) fire */
  int iDataCur,      /* Cursor from which column data is extracted */
  int iIdxCur,       /* First index cursor */
  int iPk,           /* First memory cell containing the PRIMARY KEY */
  i16 nPk,           /* Number of PRIMARY KEY memory cells */
  u8 count,          /* If non-zero, increment the row change counter */
  u8 onconf,         /* Default ON CONFLICT policy for triggers */
  u8 bNoSeek         /* iDataCur is already pointing to the row to delete */
){
  Vdbe *v = pParse->pVdbe;        /* Vdbe */
  int iOld = 0;                   /* First register in OLD.* array */
  int iLabel;                     /* Label resolved to end of generated code */
  u8 opSeek;                      /* Seek opcode */

  /* Vdbe is guaranteed to have been allocated by this stage. */
  assert( v );
  VdbeModuleComment((v, "BEGIN: GenRowDel(%d,%d,%d,%d)",
                         iDataCur, iIdxCur, iPk, (int)nPk));

  /* Seek cursor iCur to the row to delete. If this row no longer exists 
  ** (this can happen if a trigger program has already deleted it), do
  ** not attempt to delete it or fire any DELETE triggers.  */
  iLabel = sqlite3VdbeMakeLabel(v);
  opSeek = HasRowid(pTab) ? OP_NotExists : OP_NotFound;
  if( !bNoSeek ) sqlite3VdbeAddOp4Int(v, opSeek, iDataCur, iLabel, iPk, nPk);
 
  /* If there are any triggers to fire, allocate a range of registers to
  ** use for the old.* references in the triggers.  */
  if( sqlite3FkRequired(pParse, pTab, 0, 0) || pTrigger ){
    u32 mask;                     /* Mask of OLD.* columns in use */
    int iCol;                     /* Iterator used while populating OLD.* */
    int addrStart;                /* Start of BEFORE trigger programs */

    /* TODO: Could use temporary registers here. Also could attempt to
    ** avoid copying the contents of the rowid register.  */
    mask = sqlite3TriggerColmask(
        pParse, pTrigger, 0, 0, TRIGGER_BEFORE|TRIGGER_AFTER, pTab, onconf
    );
    mask |= sqlite3FkOldmask(pParse, pTab);
    iOld = pParse->nMem+1;
    pParse->nMem += (1 + pTab->nCol);

    /* Populate the OLD.* pseudo-table register array. These values will be 
    ** used by any BEFORE and AFTER triggers that exist.  */
    sqlite3VdbeAddOp2(v, OP_Copy, iPk, iOld);
    for(iCol=0; iCol<pTab->nCol; iCol++){
      if( mask==0xffffffff || mask&(1<<iCol) ){
        sqlite3ExprCodeGetColumnOfTable(v, pTab, iDataCur, iCol, iOld+iCol+1);
      }
    }

    /* Invoke BEFORE DELETE trigger programs. */
    addrStart = sqlite3VdbeCurrentAddr(v);
    sqlite3CodeRowTrigger(pParse, pTrigger, 
        TK_DELETE, 0, TRIGGER_BEFORE, pTab, iOld, onconf, iLabel
    );

    /* If any BEFORE triggers were coded, then seek the cursor to the 
    ** row to be deleted again. It may be that the BEFORE triggers moved
    ** the cursor or of already deleted the row that the cursor was
    ** pointing to.
    */
    if( addrStart<sqlite3VdbeCurrentAddr(v) ){
      sqlite3VdbeAddOp4Int(v, opSeek, iDataCur, iLabel, iPk, nPk);
    }

    /* Do FK processing. This call checks that any FK constraints that
    ** refer to this table (i.e. constraints attached to other tables) 
    ** are not violated by deleting this row.  */
    sqlite3FkCheck(pParse, pTab, iOld, 0, 0, 0);
  }

  /* Delete the index and table entries. Skip this step if pTab is really
  ** a view (in which case the only effect of the DELETE statement is to
  ** fire the INSTEAD OF triggers).  */ 
  if( pTab->pSelect==0 ){
    sqlite3GenerateRowIndexDelete(pParse, pTab, iDataCur, iIdxCur, 0);
    sqlite3VdbeAddOp2(v, OP_Delete, iDataCur, (count?OPFLAG_NCHANGE:0));
    if( count ){
      sqlite3VdbeChangeP4(v, -1, pTab->zName, P4_TRANSIENT);
    }
  }

  /* Do any ON CASCADE, SET NULL or SET DEFAULT operations required to
  ** handle rows (possibly in other tables) that refer via a foreign key
  ** to the row just deleted. */ 
  sqlite3FkActions(pParse, pTab, 0, iOld, 0, 0);

  /* Invoke AFTER DELETE trigger programs. */
  sqlite3CodeRowTrigger(pParse, pTrigger, 
      TK_DELETE, 0, TRIGGER_AFTER, pTab, iOld, onconf, iLabel
  );

  /* Jump here if the row had already been deleted before any BEFORE
  ** trigger programs were invoked. Or if a trigger program throws a 
  ** RAISE(IGNORE) exception.  */
  sqlite3VdbeResolveLabel(v, iLabel);
  VdbeModuleComment((v, "END: GenRowDel()"));
}

/*
** This routine generates VDBE code that causes the deletion of all
** index entries associated with a single row of a single table, pTab
**
** Preconditions:
**
**   1.  A read/write cursor "iDataCur" must be open on the canonical storage
**       btree for the table pTab.  (This will be either the table itself
**       for rowid tables or to the primary key index for WITHOUT ROWID
**       tables.)
**
**   2.  Read/write cursors for all indices of pTab must be open as
**       cursor number iIdxCur+i for the i-th index.  (The pTab->pIndex
**       index is the 0-th index.)
**
**   3.  The "iDataCur" cursor must be already be positioned on the row
**       that is to be deleted.
*/
void sqlite3GenerateRowIndexDelete(
  Parse *pParse,     /* Parsing and code generating context */
  Table *pTab,       /* Table containing the row to be deleted */
  int iDataCur,      /* Cursor of table holding data. */
  int iIdxCur,       /* First index cursor */
  int *aRegIdx       /* Only delete if aRegIdx!=0 && aRegIdx[i]>0 */
){
  int i;             /* Index loop counter */
  int r1;            /* Register holding an index key */
  int iPartIdxLabel; /* Jump destination for skipping partial index entries */
  Index *pIdx;       /* Current index */
  Vdbe *v;           /* The prepared statement under construction */
  Index *pPk;        /* PRIMARY KEY index, or NULL for rowid tables */

  v = pParse->pVdbe;
  pPk = HasRowid(pTab) ? 0 : sqlite3PrimaryKeyIndex(pTab);
  for(i=0, pIdx=pTab->pIndex; pIdx; i++, pIdx=pIdx->pNext){
    assert( iIdxCur+i!=iDataCur || pPk==pIdx );
    if( aRegIdx!=0 && aRegIdx[i]==0 ) continue;
    if( pIdx==pPk ) continue;
    VdbeModuleComment((v, "GenRowIdxDel for %s", pIdx->zName));
    r1 = sqlite3GenerateIndexKey(pParse, pIdx, iDataCur, 0, 1, &iPartIdxLabel);
    sqlite3VdbeAddOp3(v, OP_IdxDelete, iIdxCur+i, r1,
                      pIdx->uniqNotNull ? pIdx->nKeyCol : pIdx->nColumn);
    sqlite3VdbeResolveLabel(v, iPartIdxLabel);
  }
}

/*
** Generate code that will assemble an index key and stores it in register
** regOut.  The key with be for index pIdx which is an index on pTab.
** iCur is the index of a cursor open on the pTab table and pointing to
** the entry that needs indexing.  If pTab is a WITHOUT ROWID table, then
** iCur must be the cursor of the PRIMARY KEY index.
**
** Return a register number which is the first in a block of
** registers that holds the elements of the index key.  The
** block of registers has already been deallocated by the time
** this routine returns.
**
** If *piPartIdxLabel is not NULL, fill it in with a label and jump
** to that label if pIdx is a partial index that should be skipped.
** A partial index should be skipped if its WHERE clause evaluates
** to false or null.  If pIdx is not a partial index, *piPartIdxLabel
** will be set to zero which is an empty label that is ignored by
** sqlite3VdbeResolveLabel().
*/
int sqlite3GenerateIndexKey(
  Parse *pParse,       /* Parsing context */
  Index *pIdx,         /* The index for which to generate a key */
  int iDataCur,        /* Cursor number from which to take column data */
  int regOut,          /* Put the new key into this register if not 0 */
  int prefixOnly,      /* Compute only a unique prefix of the key */
  int *piPartIdxLabel  /* OUT: Jump to this label to skip partial index */
){
  Vdbe *v = pParse->pVdbe;
  int j;
  Table *pTab = pIdx->pTable;
  int regBase;
  int nCol;
  Index *pPk;

  if( piPartIdxLabel ){
    if( pIdx->pPartIdxWhere ){
      *piPartIdxLabel = sqlite3VdbeMakeLabel(v);
      pParse->iPartIdxTab = iDataCur;
      sqlite3ExprIfFalse(pParse, pIdx->pPartIdxWhere, *piPartIdxLabel, 
                         SQLITE_JUMPIFNULL);
    }else{
      *piPartIdxLabel = 0;
    }
  }
  nCol = (prefixOnly && pIdx->uniqNotNull) ? pIdx->nKeyCol : pIdx->nColumn;
  regBase = sqlite3GetTempRange(pParse, nCol);
  pPk = HasRowid(pTab) ? 0 : sqlite3PrimaryKeyIndex(pTab);
  for(j=0; j<nCol; j++){
    i16 idx = pIdx->aiColumn[j];
    if( pPk ) idx = sqlite3ColumnOfIndex(pPk, idx);
    if( idx<0 || idx==pTab->iPKey ){
      sqlite3VdbeAddOp2(v, OP_Rowid, iDataCur, regBase+j);
    }else{
      sqlite3VdbeAddOp3(v, OP_Column, iDataCur, idx, regBase+j);
      sqlite3ColumnDefault(v, pTab, pIdx->aiColumn[j], -1);
    }
  }
  if( regOut ){
    const char *zAff;
    if( pTab->pSelect
     || OptimizationDisabled(pParse->db, SQLITE_IdxRealAsInt)
    ){
      zAff = 0;
    }else{
      zAff = sqlite3IndexAffinityStr(v, pIdx);
    }
    sqlite3VdbeAddOp3(v, OP_MakeRecord, regBase, nCol, regOut);
    sqlite3VdbeChangeP4(v, -1, zAff, P4_TRANSIENT);
  }
  sqlite3ReleaseTempRange(pParse, regBase, nCol);
  return regBase;
}