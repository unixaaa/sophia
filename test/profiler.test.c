
/*
 * sophia database
 * sphia.org
 *
 * Copyright (c) Dmitry Simonenko
 * BSD License
*/

#include <libsr.h>
#include <libst.h>
#include <sophia.h>

static void
profiler_count(stc *cx srunused)
{
	void *env = sp_env();
	t( env != NULL );
	void *c = sp_ctl(env);
	t( c != NULL );
	t( sp_set(c, "db.test.logdir", cx->suite->logdir) == 0 );
	t( sp_set(c, "db.test.dir", cx->suite->dir) == 0 );
	t( sp_set(c, "db.test.cmp", sr_cmpu32) == 0 );
	t( sp_set(c, "db.test.threads", "0") == 0 );
	t( sp_set(c, "db.test.node_branch_wm", "0") == 0 );
	void *db = sp_get(c, "db.test");
	t( db != NULL );
	t( sp_open(env) == 0 );

	void *o = sp_get(c, "db.test.profiler.total_branch_count");
	t( o != NULL );
	t( strcmp( sp_get(o, "value", NULL), "0") == 0 );
	sp_destroy(o);
	o = sp_get(c, "db.test.profiler.total_node_count");
	t( o != NULL );
	t( strcmp( sp_get(o, "value", NULL), "1") == 0 );
	sp_destroy(o);

	int i = 0;
	while ( i < 100 ) {
		void *o = sp_object(db);
		t( sp_set(o, "key", &i, sizeof(i)) == 0 );
		t( sp_set(db, o) == 0 );
		i++;
	}
	o = sp_get(c, "db.test.profiler.count");
	t( o != NULL );
	t( strcmp( sp_get(o, "value", NULL), "100") == 0 );
	sp_destroy(o);
	t( sp_set(c, "db.test.run_branch") == 0 );

	o = sp_get(c, "db.test.profiler.total_branch_count");
	t( o != NULL );
	t( strcmp( sp_get(o, "value", NULL), "1") == 0 );
	sp_destroy(o);

	o = sp_get(c, "db.test.profiler.count");
	t( o != NULL );
	t( strcmp( sp_get(o, "value", NULL), "100") == 0 );
	sp_destroy(o);

	i = 0;
	while ( i < 10 ) {
		void *o = sp_object(db);
		t( sp_set(o, "key", &i, sizeof(i)) == 0 );
		t( sp_set(db, o) == 0 );
		i++;
	}

	o = sp_get(c, "db.test.profiler.count");
	t( o != NULL );
	t( strcmp( sp_get(o, "value", NULL), "110") == 0 );
	sp_destroy(o);

	t( sp_set(c, "db.test.run_branch") == 0 );
	t( sp_set(c, "db.test.run_merge") == 0 );

	o = sp_get(c, "db.test.profiler.count");
	t( o != NULL );
	t( strcmp( sp_get(o, "value", NULL), "100") == 0 );
	sp_destroy(o);

	t( sp_destroy(env) == 0 );
}

stgroup *profiler_group(void)
{
	stgroup *group = st_group("profiler");
	st_groupadd(group, st_test("count", profiler_count));
	return group;
}
