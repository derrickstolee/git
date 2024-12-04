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
#include "hashmap.h"
#include "hex.h"
#include "list-objects.h"
#include "tree.h"
#include "tree-walk.h"
#include "object.h"
#include "object-name.h"
#include "object-store-ll.h"
#include "oid-array.h"
#include "oidset.h"
#include "promisor-remote.h"
#include "strmap.h"
#include "string-list.h"
#include "revision.h"
#include "trace2.h"
#include "progress.h"
#include "packfile.h"
#include "path-walk.h"
#include "pathspec.h"

static const char * const builtin_backfill_usage[] = {
	N_("git backfill [--batch-size=<n>] [--[no-]sparse] [[--] <pathspec>]"),
	NULL
};

struct backfill_context {
	struct repository *repo;
	struct oid_array current_batch;
	size_t min_batch_size;
	int sparse;

	int use_pathspec;
	struct pathspec ps;
	struct string_list matching_paths;
};

static void backfill_context_clear(struct backfill_context *ctx)
{
	oid_array_clear(&ctx->current_batch);
}

static void download_batch(struct backfill_context *ctx)
{
	promisor_remote_get_direct(ctx->repo,
				   ctx->current_batch.oid,
				   ctx->current_batch.nr);
	oid_array_clear(&ctx->current_batch);

	/*
	 * We likely have a new packfile. Add it to the packed list to
	 * avoid possible duplicate downloads of the same objects.
	 */
	reprepare_packed_git(ctx->repo);
}

static int fill_missing_blobs(const char *path,
			      struct oid_array *list,
			      enum object_type type,
			      void *data)
{
	struct backfill_context *ctx = data;

	if (type != OBJ_BLOB)
		return 0;

	if (ctx->use_pathspec &&
	    !match_pathspec(ctx->repo->index, &ctx->ps, path, strlen(path),
			    0, NULL, 0))
		return 0;

	for (size_t i = 0; i < list->nr; i++) {
		off_t size = 0;
		struct object_info info = OBJECT_INFO_INIT;
		info.disk_sizep = &size;
		if (oid_object_info_extended(ctx->repo,
					     &list->oid[i],
					     &info,
					     OBJECT_INFO_FOR_PREFETCH) ||
		    !size)
			oid_array_append(&ctx->current_batch, &list->oid[i]);
	}

	if (ctx->current_batch.nr >= ctx->min_batch_size)
		download_batch(ctx);

	return 0;
}

static int do_backfill(struct backfill_context *ctx)
{
	struct rev_info revs;
	struct path_walk_info info = PATH_WALK_INFO_INIT;
	int ret;

	if (ctx->sparse) {
		CALLOC_ARRAY(info.pl, 1);
		if (get_sparse_checkout_patterns(info.pl)) {
			path_walk_info_clear(&info);
			return error(_("problem loading sparse-checkout"));
		}
	}

	repo_init_revisions(ctx->repo, &revs, "");
	handle_revision_arg("HEAD", &revs, 0, 0);

	info.blobs = 1;
	info.tags = info.commits = info.trees = 0;

	info.revs = &revs;
	info.path_fn = fill_missing_blobs;
	info.path_fn_data = ctx;

	ret = walk_objects_by_path(&info);

	/* Download the objects that did not fill a batch. */
	if (!ret)
		download_batch(ctx);

	backfill_context_clear(ctx);
	path_walk_info_clear(&info);
	release_revisions(&revs);
	return ret;
}

int cmd_backfill(int argc, const char **argv, const char *prefix, struct repository *repo)
{
	struct backfill_context ctx = {
		.repo = repo,
		.current_batch = OID_ARRAY_INIT,
		.min_batch_size = 50000,
		.sparse = 0,
	};
	struct option options[] = {
		OPT_INTEGER(0, "min-batch-size", &ctx.min_batch_size,
			    N_("Minimum number of objects to request at a time")),
		OPT_BOOL(0, "sparse", &ctx.sparse,
			 N_("Restrict the missing objects to the current sparse-checkout")),
		OPT_END(),
	};

	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage_with_options(builtin_backfill_usage, options);

	argc = parse_options(argc, argv, prefix, options, builtin_backfill_usage,
			     0);

	repo_config(repo, git_default_config, NULL);

	if (argc) {
		parse_pathspec(&ctx.ps, 0, 0, prefix, argv);
		ctx.use_pathspec = 1;
		if (ctx.sparse > 0)
			warning(_("ignoring --sparse option due to presence of pathspec"));
	} else if (ctx.sparse < 0) {
		ctx.sparse = core_apply_sparse_checkout;
	}

	return do_backfill(&ctx);
}
