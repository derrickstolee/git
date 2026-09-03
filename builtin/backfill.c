/* We need this macro to access core_apply_sparse_checkout */
#define USE_THE_REPOSITORY_VARIABLE

#include "builtin.h"
#include "git-compat-util.h"
#include "config.h"
#include "parse-options.h"
#include "repository.h"
#include "commit.h"
#include "dir.h"
#include "environment.h"
#include "hex.h"
#include "tree.h"
#include "tree-walk.h"
#include "object.h"
#include "odb.h"
#include "oid-array.h"
#include "oidset.h"
#include "promisor-remote.h"
#include "strmap.h"
#include "string-list.h"
#include "revision.h"
#include "trace.h"
#include "trace2.h"
#include "progress.h"
#include "packfile.h"
#include "path-walk.h"

static const char * const builtin_backfill_usage[] = {
	N_("git backfill [--min-batch-size=<n>] [--timeout=<seconds>]\n"
	   "             [--[no-]sparse]\n"
	   "             [--[no-]include-edges] [--[no-]progress]\n"
	   "             [<revision-range>]"),
	NULL
};

struct backfill_context {
	struct repository *repo;
	struct oid_array current_batch;
	struct progress *progress;
	uint64_t progress_nr;
	size_t min_batch_size;
	uint64_t start_time;
	int timeout;
	int timed_out;
	int sparse;
	int include_edges;
	int show_progress;
	struct rev_info revs;
};

static void backfill_context_clear(struct backfill_context *ctx)
{
	oid_array_clear(&ctx->current_batch);
}

static int download_batch(struct backfill_context *ctx, int check_timeout)
{
	promisor_remote_get_direct(ctx->repo,
				   ctx->current_batch.oid,
				   ctx->current_batch.nr);
	oid_array_clear(&ctx->current_batch);

	/*
	 * We likely have a new packfile. Add it to the packed list to
	 * avoid possible duplicate downloads of the same objects.
	 */
	odb_reprepare(ctx->repo->objects);

	if (check_timeout &&
	    ctx->timeout >= 0 &&
	    getnanotime() - ctx->start_time >=
		    (uint64_t)ctx->timeout * 1000000000) {
		ctx->timed_out = 1;
		return 1;
	}

	return 0;
}

static int fill_missing_blobs(const char *path UNUSED,
			      struct oid_array *list,
			      enum object_type type,
			      void *data)
{
	struct backfill_context *ctx = data;

	if (type != OBJ_BLOB)
		return 0;

	ctx->progress_nr += list->nr;
	display_progress(ctx->progress, ctx->progress_nr);

	for (size_t i = 0; i < list->nr; i++) {
		if (!odb_has_object(ctx->repo->objects, &list->oid[i], 0))
			oid_array_append(&ctx->current_batch, &list->oid[i]);
	}

	if (ctx->current_batch.nr >= ctx->min_batch_size)
		return download_batch(ctx, 1);

	return 0;
}

static void reject_unsupported_rev_list_options(struct rev_info *revs)
{
	if (revs->diffopt.pickaxe)
		die(_("'%s' cannot be used with 'git backfill'"),
		    (revs->diffopt.pickaxe_opts & DIFF_PICKAXE_REGEX) ? "-G" : "-S");
	if (revs->diffopt.filter || revs->diffopt.filter_not)
		die(_("'%s' cannot be used with 'git backfill'"),
		    "--diff-filter");
	if (revs->diffopt.flags.follow_renames)
		die(_("'%s' cannot be used with 'git backfill'"),
		    "--follow");
	if (revs->line_level_traverse)
		die(_("'%s' cannot be used with 'git backfill'"),
		    "-L");
	if (revs->explicit_diff_merges)
		die(_("'%s' cannot be used with 'git backfill'"),
		    "--diff-merges");
	if (!path_walk_filter_compatible(&revs->filter))
		die(_("cannot backfill with these filter options"));
	if (revs->filter.blob_limit_value)
		die(_("cannot backfill with blob size limits"));
}

static int do_backfill(struct backfill_context *ctx)
{
	struct path_walk_info info = PATH_WALK_INFO_INIT;
	int ret;

	if (ctx->sparse) {
		CALLOC_ARRAY(info.pl, 1);
		info.pl_sparse_trees = 1;
		if (get_sparse_checkout_patterns(info.pl)) {
			path_walk_info_clear(&info);
			return error(_("problem loading sparse-checkout"));
		}
	}

	/* Walk from HEAD if otherwise unspecified. */
	if (!ctx->revs.pending.nr)
		add_head_to_pending(&ctx->revs);
	if (ctx->include_edges)
		ctx->revs.edge_hint = 1;

	info.blobs = 1;
	info.tags = info.commits = info.trees = 0;

	info.revs = &ctx->revs;
	info.path_fn = fill_missing_blobs;
	info.path_fn_data = ctx;

	if (ctx->show_progress)
		ctx->progress = start_delayed_progress(ctx->repo,
						       _("Exploring objects"),
						       0);
	ret = walk_objects_by_path(&info);
	stop_progress(&ctx->progress);

	/* Download the objects that did not fill a batch. */
	if (!ret)
		ret = download_batch(ctx, 0);

	if (ctx->timed_out) {
		warning(_("backfill stopped due to timeout before completing"));
		ret = 0;
	}

	path_walk_info_clear(&info);
	return ret;
}

int cmd_backfill(int argc, const char **argv, const char *prefix, struct repository *repo)
{
	int result;
	struct backfill_context ctx = {
		.repo = repo,
		.current_batch = OID_ARRAY_INIT,
		.min_batch_size = 50000,
		.start_time = getnanotime(),
		.timeout = -1,
		.sparse = -1,
		.revs = REV_INFO_INIT,
		.include_edges = 1,
		.show_progress = -1,
	};
	struct option options[] = {
		OPT_UNSIGNED(0, "min-batch-size", &ctx.min_batch_size,
			     N_("Minimum number of objects to request at a time")),
		OPT_INTEGER_F(0, "timeout", &ctx.timeout,
			      N_("Stop after the given number of seconds"),
			      PARSE_OPT_NONEG),
		OPT_BOOL(0, "sparse", &ctx.sparse,
			 N_("Restrict the missing objects to the current sparse-checkout")),
		OPT_BOOL(0, "include-edges", &ctx.include_edges,
			 N_("Include blobs from boundary commits in the backfill")),
		OPT_BOOL(0, "progress", &ctx.show_progress,
			 N_("show progress")),
		OPT_END(),
	};
	struct repo_config_values *cfg = repo_config_values(the_repository);

	show_usage_with_options_if_asked(argc, argv,
					 builtin_backfill_usage, options);

	argc = parse_options(argc, argv, prefix, options, builtin_backfill_usage,
			     PARSE_OPT_KEEP_UNKNOWN_OPT |
			     PARSE_OPT_KEEP_ARGV0 |
			     PARSE_OPT_KEEP_DASHDASH);

	repo_init_revisions(repo, &ctx.revs, prefix);
	argc = setup_revisions(argc, argv, &ctx.revs, NULL);

	if (argc > 1)
		die(_("unrecognized argument: %s"), argv[1]);
	reject_unsupported_rev_list_options(&ctx.revs);

	repo_config(repo, git_default_config, NULL);

	if (ctx.sparse < 0)
		ctx.sparse = cfg->apply_sparse_checkout;
	if (ctx.show_progress < 0)
		ctx.show_progress = isatty(2);

	result = do_backfill(&ctx);
	backfill_context_clear(&ctx);
	release_revisions(&ctx.revs);
	return result;
}
