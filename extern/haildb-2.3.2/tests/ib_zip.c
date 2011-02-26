/***********************************************************************
Copyright (c) 2009 Innobase Oy. All rights reserved.
Copyright (c) 2009 Oracle. All rights reserved.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

************************************************************************/

/* Simple single threaded test that does the equivalent of:
 Create a database if not exists
 CREATE TABLE T(c1 VARCHAR(n), c2 INT, PK(c1)) if not exists;
 INSERT INTO T VALUES('x', 1); ...
 
 The test will create all the relevant sub-directories in the current
 working directory. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test0aux.h"

#ifdef UNIV_DEBUG_VALGRIND
#include <valgrind/memcheck.h>
#endif

#define DATABASE	"test"
#define TABLE		"t"

/*********************************************************************
Create an InnoDB database (sub-directory). */
static
ib_err_t
create_database(
/*============*/
	const char*	name)
{
	ib_bool_t	err;

	err = ib_database_create(name);
	assert(err == IB_TRUE);

	return(DB_SUCCESS);
}

/*********************************************************************
CREATE TABLE T(
	first	VARCHAR(n),
	last	VARCHAR(n),
	score	INT,
	PRIMARY KEY(first, last); */
static
ib_err_t
create_table(
/*=========*/
	const char*	dbname,			/*!< in: database name */
	const char*	name,			/*!< in: table name */
	int		page_size)		/*!< in: page size */
{
	ib_trx_t	ib_trx;
	ib_id_t		table_id = 0;
	ib_err_t	err = DB_SUCCESS;
	ib_tbl_sch_t	ib_tbl_sch = NULL;
	ib_idx_sch_t	ib_idx_sch = NULL;
	ib_tbl_fmt_t	tbl_fmt = IB_TBL_COMPACT;
	char		table_name[IB_MAX_TABLE_NAME_LEN];

#ifdef __WIN__
	sprintf(table_name, "%s/%s", dbname, name);
#else
	snprintf(table_name, sizeof(table_name), "%s/%s", dbname, name);
#endif

	if (page_size > 0) {
		tbl_fmt = IB_TBL_COMPRESSED;

		printf("Creating compressed table with page size %d\n",
			page_size);
	}

	/* Pass a table page size of 0, ie., use default page size. */
	err = ib_table_schema_create(
		table_name, &ib_tbl_sch, tbl_fmt, page_size);

	if (err == DB_UNSUPPORTED) {
		return(DB_SUCCESS);
	}

	assert(err == DB_SUCCESS);

	err = ib_table_schema_add_col(
		ib_tbl_sch, "c1", IB_VARCHAR, IB_COL_NONE, 0, 10);

	assert(err == DB_SUCCESS);

	err = ib_table_schema_add_col(
		ib_tbl_sch, "c2", IB_INT, 0, 0, sizeof(ib_i32_t));

	assert(err == DB_SUCCESS);

	err = ib_table_schema_add_index(ib_tbl_sch, "c1", &ib_idx_sch);
	assert(err == DB_SUCCESS);

	/* Set prefix length to 0. */
	err = ib_index_schema_add_col( ib_idx_sch, "c1", 0);
	assert(err == DB_SUCCESS);

	err = ib_index_schema_set_clustered(ib_idx_sch);
	assert(err == DB_SUCCESS);

	/* create table */
	ib_trx = ib_trx_begin(IB_TRX_REPEATABLE_READ);
	err = ib_schema_lock_exclusive(ib_trx);
	assert(err == DB_SUCCESS);

	err = ib_table_create(ib_trx, ib_tbl_sch, &table_id);
	assert(err == DB_SUCCESS || err == DB_TABLE_IS_BEING_USED);

	err = ib_trx_commit(ib_trx);
	assert(err == DB_SUCCESS);

	if (ib_tbl_sch != NULL) {
		ib_table_schema_delete(ib_tbl_sch);
	}

	return(err);
}

/*********************************************************************
Open a table and return a cursor for the table. */
static
ib_err_t
open_table(
/*=======*/
	const char*	dbname,		/*!< in: database name */
	const char*	name,		/*!< in: table name */
	ib_trx_t	ib_trx,		/*!< in: transaction */
	ib_crsr_t*	crsr)		/*!< out: innodb cursor */
{
	ib_err_t	err = DB_SUCCESS;
	char		table_name[IB_MAX_TABLE_NAME_LEN];

#ifdef __WIN__
	sprintf(table_name, "%s/%s", dbname, name);
#else
	snprintf(table_name, sizeof(table_name), "%s/%s", dbname, name);
#endif
	err = ib_cursor_open_table(table_name, ib_trx, crsr);
	assert(err == DB_SUCCESS);

	return(err);
}

/*********************************************************************
INSERT INTO T VALUE('x', 1); */
static
void
insert_row(
/*=======*/
	ib_crsr_t	crsr)		/*!< in, out: cursor to use for write */
{
	ib_tpl_t	tpl = NULL;
	ib_err_t	err = DB_ERROR;

	tpl = ib_clust_read_tuple_create(crsr);
	assert(tpl != NULL);

	err = ib_col_set_value(tpl, 0, "x", 1);
	assert(err == DB_SUCCESS);

	err = ib_tuple_write_i32(tpl, 1, 1);
	assert(err == DB_SUCCESS);

	err = ib_cursor_insert_row(crsr, tpl);
	assert(err == DB_SUCCESS || err == DB_DUPLICATE_KEY);

	ib_tuple_delete(tpl);
}

int main(int argc, char* argv[])
{
	ib_err_t	err;
	ib_crsr_t	crsr;
	ib_trx_t	ib_trx;
	ib_u64_t	version;

	(void)argc;
	(void)argv;

	version = ib_api_version();
	printf("API: %d.%d.%d\n",
		(int) (version >> 32),			/* Current version */
		(int) ((version >> 16)) & 0xffff,	/* Revisiion */
	       	(int) (version & 0xffff));		/* Age */

	err = ib_init();
	assert(err == DB_SUCCESS);

	test_configure();

	err = ib_startup("barracuda");
	assert(err == DB_SUCCESS);

	err = create_database(DATABASE);
	assert(err == DB_SUCCESS);

	err = create_table(DATABASE, TABLE, 4);
	assert(err == DB_SUCCESS);

	ib_trx = ib_trx_begin(IB_TRX_REPEATABLE_READ);
	assert(ib_trx != NULL);

	err = open_table(DATABASE, TABLE, ib_trx, &crsr);
	assert(err == DB_SUCCESS);

	err = ib_cursor_lock(crsr, IB_LOCK_IX);
	assert(err == DB_SUCCESS);

	insert_row(crsr);

	err = ib_cursor_close(crsr);
	assert(err == DB_SUCCESS);
	crsr = NULL;

	err = ib_trx_commit(ib_trx);
	assert(err == DB_SUCCESS);

	err = ib_shutdown(IB_SHUTDOWN_NORMAL);
	assert(err == DB_SUCCESS);

#ifdef UNIV_DEBUG_VALGRIND
	VALGRIND_DO_LEAK_CHECK;
#endif

	return(EXIT_SUCCESS);
}
