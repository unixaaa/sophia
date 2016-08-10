
/*
 * sophia database
 * sphia.org
 *
 * Copyright (c) Dmitry Simonenko
 * BSD License
*/

#include <libss.h>
#include <libsf.h>
#include <libsr.h>
#include <libso.h>
#include <libsv.h>
#include <libsd.h>
#include <libsl.h>
#include <libsi.h>
#include <libsy.h>
#include <libsc.h>

static inline int
sc_rotate(sc *s, scworker *w)
{
	ss_trace(&w->trace, "%s", "log rotation");
	int rc = sl_poolrotate_ready(s->lp);
	if (rc) {
		rc = sl_poolrotate(s->lp);
		if (ssunlikely(rc == -1))
			return -1;
	}
	return 0;
}

static inline int
sc_gc(sc *s, scworker *w)
{
	ss_trace(&w->trace, "%s", "log gc");
	int rc = sl_poolgc(s->lp);
	if (ssunlikely(rc == -1))
		return -1;
	return 0;
}

static inline int
sc_execute(sctask *t, scworker *w, uint64_t vlsn)
{
	si *index = t->db->index;
	si_plannertrace(&t->plan, index->scheme.id, &w->trace);
	return si_execute(index, &w->dc, &t->plan, vlsn);
}

static inline int
sc_plan(sc *s, sctask *task, int id)
{
	scdb *db = sc_current(s);
	uint32_t prio = s->prio[id];
	if (db->workers[id] >= prio)
		return SI_PRETRY;
	return si_plan(db->index, &task->plan);
}

static inline void
sc_taskbegin(sctask *task, scworker *w, uint64_t vlsn)
{
	task->time   = ss_utime();
	task->w      = w;
	task->vlsn   = vlsn;
	task->db     = NULL;
	task->rotate = 0;
	task->gc     = 0;
	task->backup = 0;
	si_planinit(&task->plan);
}

static inline int
sc_taskend(sc *s, sctask *t)
{
	ss_mutexlock(&s->lock);
	scdb *db = t->db;
	switch (t->plan.plan) {
	case SI_BRANCH:
	case SI_CHECKPOINT:
		db->workers[SC_QBRANCH]--;
		t->gc = 1;
		break;
	case SI_COMPACT_INDEX:
		break;
	case SI_BACKUP:
	case SI_BACKUPEND:
		db->workers[SC_QBACKUP]--;
		break;
	case SI_EXPIRE:
		db->workers[SC_QEXPIRE]--;
		t->gc = 1;
		break;
	case SI_GC:
		db->workers[SC_QGC]--;
		t->gc = 1;
		break;
	}
	if (t->rotate == 1)
		s->rotate = 0;
	ss_mutexunlock(&s->lock);
	return 0;
}

static inline siplannerrc
sc_do(sc *s, sctask *task)
{
	siplannerrc rc;
	scdb *db = task->db;
	sicompaction *c = &db->index->scheme.compaction;

	ss_trace(&task->w->trace, "%s", "schedule");

	/* checkpoint */
	if (db->checkpoint) {
		task->plan.plan = SI_CHECKPOINT;
		task->plan.a = db->checkpoint_lsn;
		rc = si_plan(db->index, &task->plan);
		switch (rc) {
		case SI_PMATCH:
			db->workers[SC_QBRANCH]++;
			return rc;
		case SI_PNONE:
			sc_task_checkpoint_done(db);
			break;
		case SI_PRETRY:
			/* do not start additional compaction
			 * tasks in checkpoint mode */
			si_planinit(&task->plan);
			return rc;
		}
	}

	/* node delayed gc */
	task->plan.plan = SI_NODEGC;
	rc = si_plan(db->index, &task->plan);
	if (rc == SI_PMATCH)
		return rc;

	/* backup */
	if (db->backup)
	{
		/* backup procedure.
		 *
		 * state 0 (start)
		 * -------
		 *
		 * a. disable log gc
		 * b. mark to start backup (state 1)
		 *
		 * state 1 (background, delayed start)
		 * -------
		 *
		 * a. create backup_path/<bsn.incomplete> directory
		 * b. create database directories
		 * c. create log directory
		 * d. state 2
		 *
		 * state 2 (background, copy)
		 * -------
		 *
		 * a. schedule and execute node backup which bsn < backup_bsn
		 * b. state 3
		 *
		 * state 3 (background, completion)
		 * -------
		 *
		 * a. rotate log file
		 * b. copy log files
		 * c. enable log gc, schedule gc
		 * d. rename <bsn.incomplete> into <bsn>
		 * e. set last backup, set COMPLETE
		 *
		*/

		/* state 2 */
		task->plan.plan = SI_BACKUP;
		task->plan.a = s->backup_bsn;
		rc = sc_plan(s, task, SC_QBACKUP);
		switch (rc) {
		case SI_PMATCH:
			db->workers[SC_QBACKUP]++;
			task->db = db;
			return SI_PMATCH;
		case SI_PNONE:
			sc_task_backup_done(task->db);
			assert(s->backup_in_progress > 0);
			s->backup_in_progress--;
			/* state 3 */
			if (s->backup_in_progress == 0)
				task->backup = 1;
			break;
		case SI_PRETRY:
			break;
		}
	}

	/* expire */
	if (db->expire) {
		task->plan.plan = SI_EXPIRE;
		task->plan.a = db->index->scheme.expire;
		rc = sc_plan(s, task, SC_QEXPIRE);
		switch (rc) {
		case SI_PMATCH:
			if (c->mode == 1)
				task->plan.plan = SI_COMPACT_INDEX;
			db->workers[SC_QEXPIRE]++;
			return SI_PMATCH;
		case SI_PNONE:
			sc_task_expire_done(db, task->time);
			break;
		case SI_PRETRY:
			break;
		}
	}

	/* garbage-collection */
	if (db->gc) {
		task->plan.plan = SI_GC;
		task->plan.a = task->vlsn;
		task->plan.b = c->gc_wm;
		rc = sc_plan(s, task, SC_QGC);
		switch (rc) {
		case SI_PMATCH:
			if (c->mode == 1)
				task->plan.plan = SI_COMPACT_INDEX;
			db->workers[SC_QGC]++;
			return SI_PMATCH;
		case SI_PNONE:
			sc_task_gc_done(db, task->time);
			break;
		case SI_PRETRY:
			break;
		}
	}

	/* compact_index (merge directly with in-memory index) */
	if (c->mode == 1) {
		task->plan.plan = SI_COMPACT_INDEX;
		task->plan.a = c->branch_wm;
		rc = si_plan(db->index, &task->plan);
		if (rc == SI_PMATCH)
			return SI_PMATCH;
		si_planinit(&task->plan);
		return SI_PNONE;
	}

	/* branching */
	task->plan.plan = SI_BRANCH;
	task->plan.a = c->branch_wm;
	rc = sc_plan(s, task, SC_QBRANCH);
	if (rc == SI_PMATCH) {
		db->workers[SC_QBRANCH]++;
		return SI_PMATCH;
	}

	/* compaction */
	task->plan.plan = SI_COMPACT;
	task->plan.a = c->compact_wm;
	task->plan.b = c->compact_mode;
	rc = si_plan(db->index, &task->plan);
	if (rc == SI_PMATCH)
		return SI_PMATCH;

	si_planinit(&task->plan);
	return SI_PNONE;
}

static inline void
sc_periodic(sc *s, sctask *task)
{
	/* log rotation */
	if (s->rotate == 0) {
		task->rotate = 1;
		s->rotate = 1;
	}

	scdb *db = task->db;
	sicompaction *c = &db->index->scheme.compaction;

	/* checkpoint.
	 *
	 * start checkpoint process per-database when it reaches
	 * its memory limit.
	 *
	 * do not plan other tasks, when checkpoint is
	 * in-progress.
	*/
	if (db->checkpoint)
		return;
	if (db->index->scheme.memory_limit > 0) {
		uint32_t usage =
			sr_quotaused_percent(db->index->r.quota,
			                     db->index->r.stat);
		if (usage >= c->checkpoint_wm) {
			sc_task_checkpoint(s, db);
			return;
		}
	}
	/* expire */
	if (c->expire_period && db->expire == 0) {
		if ((task->time - db->expire_time) >= c->expire_period_us)
			sc_task_expire(db);
	}
	/* gc */
	if (c->gc_period && db->gc == 0) {
		if ((task->time - db->gc_time) >= c->gc_period_us)
			sc_task_gc(db);
	}
}

static int
sc_schedule(sc *s, sctask *task)
{
	int rc;
	ss_mutexlock(&s->lock);
	task->db = sc_current(s);
	sc_periodic(s, task);
	rc = sc_do(s, task);
	sc_next(s);
	ss_mutexunlock(&s->lock);
	return rc;
}

int sc_step(sc *s, scworker *w, uint64_t vlsn)
{
	sctask task;
	sc_taskbegin(&task, w, vlsn);
	int rc = sc_schedule(s, &task);
	int rc_job = rc;
	/* log rotation */
	if (task.rotate) {
		rc = sc_rotate(s, w);
		if (ssunlikely(rc == -1))
			goto error;
	}
	/* backup completion */
	if (task.backup) {
		rc = sc_backupend(s, w);
		if (ssunlikely(rc == -1))
			sc_backupstop(s);
	}
	if (rc_job == 1) {
		rc = sc_execute(&task, w, vlsn);
		if (ssunlikely(rc == -1)) {
			if (task.plan.plan != SI_BACKUP &&
			    task.plan.plan != SI_BACKUPEND) {
				sr_statusset(s->r->status, SR_MALFUNCTION);
				goto error;
			}
			sc_backupstop(s);
		}
	}
	sc_taskend(s, &task);
	if (task.gc) {
		rc = sc_gc(s, w);
		if (ssunlikely(rc == -1))
			goto error;
	}
	ss_trace(&w->trace, "%s", "sleep");
	return rc_job;
error:
	ss_trace(&w->trace, "%s", "malfunction");
	return -1;
}
