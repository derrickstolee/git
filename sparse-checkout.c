#include "cache.h"
#include "config.h"
#include "dir.h"
#include "lockfile.h"
#include "quote.h"
#include "repository.h"
#include "sparse-checkout.h"
#include "strbuf.h"
#include "string-list.h"
#include "unpack-trees.h"
#include "object-store.h"

char *get_sparse_checkout_filename(void)
{
	return git_pathdup("info/sparse-checkout");
}

int load_sparse_checkout_patterns(struct pattern_list *pl)
{
	int result = 0;
	char *sparse_filename = get_sparse_checkout_filename();

	memset(pl, 0, sizeof(*pl));
	pl->use_cone_patterns = core_sparse_checkout_cone;

	if (add_patterns_from_file_to_list(sparse_filename, "", 0,
					   pl, NULL))
		result = 1;

	free(sparse_filename);
	return result;
}

void write_patterns_to_file(FILE *fp, struct pattern_list *pl)
{
	int i;

	for (i = 0; i < pl->nr; i++) {
		struct path_pattern *p = pl->patterns[i];

		if (p->flags & PATTERN_FLAG_NEGATIVE)
			fprintf(fp, "!");

		fprintf(fp, "%s", p->pattern);

		if (p->flags & PATTERN_FLAG_MUSTBEDIR)
			fprintf(fp, "/");

		fprintf(fp, "\n");
	}
}

int update_working_directory(struct pattern_list *pl)
{
	enum update_sparsity_result result;
	struct unpack_trees_options o;
	struct lock_file lock_file = LOCK_INIT;
	struct repository *r = the_repository;

	memset(&o, 0, sizeof(o));
	o.verbose_update = isatty(2);
	o.update = 1;
	o.head_idx = -1;
	o.src_index = r->index;
	o.dst_index = r->index;
	o.skip_sparse_checkout = 0;
	o.pl = pl;

	setup_work_tree();

	repo_hold_locked_index(r, &lock_file, LOCK_DIE_ON_ERROR);

	setup_unpack_trees_porcelain(&o, "sparse-checkout");
	result = update_sparsity(&o);
	clear_unpack_trees_porcelain(&o);

	if (result == UPDATE_SPARSITY_WARNINGS)
		/*
		 * We don't do any special handling of warnings from untracked
		 * files in the way or dirty entries that can't be removed.
		 */
		result = UPDATE_SPARSITY_SUCCESS;
	if (result == UPDATE_SPARSITY_SUCCESS)
		write_locked_index(r->index, &lock_file, COMMIT_LOCK);
	else
		rollback_lock_file(&lock_file);

	return result;
}

static char *escaped_pattern(char *pattern)
{
	char *p = pattern;
	struct strbuf final = STRBUF_INIT;

	while (*p) {
		if (is_glob_special(*p))
			strbuf_addch(&final, '\\');

		strbuf_addch(&final, *p);
		p++;
	}

	return strbuf_detach(&final, NULL);
}

static void write_cone_to_file(FILE *fp, struct pattern_list *pl)
{
	int i;
	struct pattern_entry *pe;
	struct hashmap_iter iter;
	struct string_list sl = STRING_LIST_INIT_DUP;
	struct strbuf parent_pattern = STRBUF_INIT;

	hashmap_for_each_entry(&pl->parent_hashmap, &iter, pe, ent) {
		if (hashmap_get_entry(&pl->recursive_hashmap, pe, ent, NULL))
			continue;

		if (!hashmap_contains_parent(&pl->recursive_hashmap,
					     pe->pattern,
					     &parent_pattern))
			string_list_insert(&sl, pe->pattern);
	}

	string_list_sort(&sl);
	string_list_remove_duplicates(&sl, 0);

	fprintf(fp, "/*\n!/*/\n");

	for (i = 0; i < sl.nr; i++) {
		char *pattern = escaped_pattern(sl.items[i].string);

		if (strlen(pattern))
			fprintf(fp, "%s/\n!%s/*/\n", pattern, pattern);
		free(pattern);
	}

	string_list_clear(&sl, 0);

	hashmap_for_each_entry(&pl->recursive_hashmap, &iter, pe, ent) {
		if (!hashmap_contains_parent(&pl->recursive_hashmap,
					     pe->pattern,
					     &parent_pattern))
			string_list_insert(&sl, pe->pattern);
	}

	strbuf_release(&parent_pattern);

	string_list_sort(&sl);
	string_list_remove_duplicates(&sl, 0);

	for (i = 0; i < sl.nr; i++) {
		char *pattern = escaped_pattern(sl.items[i].string);
		fprintf(fp, "%s/\n", pattern);
		free(pattern);
	}
}

static int write_patterns_to_sparse_checkout(
			struct pattern_list *pl,
			int refresh_workdir)
{
	char *sparse_filename;
	FILE *fp;
	int fd;
	struct lock_file lk = LOCK_INIT;
	int result = 0;

	sparse_filename = get_sparse_checkout_filename();

	if (safe_create_leading_directories(sparse_filename))
		die(_("failed to create directory for sparse-checkout file"));

	fd = hold_lock_file_for_update(&lk, sparse_filename,
				      LOCK_DIE_ON_ERROR);

	if (refresh_workdir)
		result = update_working_directory(pl);
	if (result) {
		rollback_lock_file(&lk);
		free(sparse_filename);
		clear_pattern_list(pl);
		update_working_directory(NULL);
		return result;
	}

	fp = xfdopen(fd, "w");

	if (core_sparse_checkout_cone)
		write_cone_to_file(fp, pl);
	else
		write_patterns_to_file(fp, pl);

	fflush(fp);
	commit_lock_file(&lk);

	free(sparse_filename);
	clear_pattern_list(pl);

	return 0;
}

int write_patterns_and_update(struct pattern_list *pl)
{
        return write_patterns_to_sparse_checkout(pl, 1);
}

void insert_recursive_pattern(struct pattern_list *pl, struct strbuf *path)
{
	struct pattern_entry *e = xmalloc(sizeof(*e));
	e->patternlen = path->len;
	e->pattern = strbuf_detach(path, NULL);
	hashmap_entry_init(&e->ent,
			   ignore_case ?
			   strihash(e->pattern) :
			   strhash(e->pattern));

	hashmap_add(&pl->recursive_hashmap, &e->ent);

	while (e->patternlen) {
		char *slash = strrchr(e->pattern, '/');
		char *oldpattern = e->pattern;
		size_t newlen;

		if (slash == e->pattern)
			break;

		newlen = slash - e->pattern;
		e = xmalloc(sizeof(struct pattern_entry));
		e->patternlen = newlen;
		e->pattern = xstrndup(oldpattern, newlen);
		hashmap_entry_init(&e->ent,
				   ignore_case ?
				   strihash(e->pattern) :
				   strhash(e->pattern));

		if (!hashmap_get_entry(&pl->parent_hashmap, e, ent, NULL))
			hashmap_add(&pl->parent_hashmap, &e->ent);
	}
}

void strbuf_to_cone_pattern(struct strbuf *line, struct pattern_list *pl)
{
	strbuf_trim(line);

	strbuf_trim_trailing_dir_sep(line);

	if (strbuf_normalize_path(line))
		die(_("could not normalize path %s"), line->buf);

	if (!line->len)
		return;

	if (line->buf[0] != '/')
		strbuf_insertstr(line, 0, "/");

	insert_recursive_pattern(pl, line);
}

#define SPARSE_CHECKOUT_IN_TREE "sparse-checkout.intree"

int load_in_tree_from_config(struct repository *r, struct string_list *sl)
{
	struct string_list_item *item;
	const struct string_list *cl = repo_config_get_value_multi(r, SPARSE_CHECKOUT_IN_TREE);

	for_each_string_list_item(item, cl)
		string_list_insert(sl, item->string);

	return 0;
}

int load_in_tree_pattern_list(struct index_state *istate, struct string_list *sl, struct pattern_list *pl)
{
	struct string_list_item *item;
	struct strbuf path = STRBUF_INIT;

	pl->use_cone_patterns = 1;

	for_each_string_list_item(item, sl) {
		struct object_id *oid;
		enum object_type type;
		char *buf, *cur, *end;
		unsigned long size;
		int pos = index_name_pos(istate, item->string, strlen(item->string));

		if (pos < 0) {
			warning(_("did not find cache entry with name '%s'; not updating sparse-checkout"),
				item->string);
			return 1;
		}

		oid = &istate->cache[pos]->oid;
		type = oid_object_info(the_repository, oid, NULL);

		if (type != OBJ_BLOB) {
			warning(_("expected a file at '%s'; not updating sparse-checkout"),
				oid_to_hex(oid));
			return 1;
		}

		buf = read_object_file(oid, &type, &size);
		end = buf + size;

		for (cur = buf; cur < end; ) {
			char *next;

			next = memchr(cur, '\n', end - cur);

			if (next)
				*next = '\0';

			strbuf_reset(&path);
			strbuf_addch(&path, '/');
			strbuf_addstr(&path, cur);

			insert_recursive_pattern(pl, &path);

			if (next)
				cur = next + 1;
			else
				break;
		}

		free(buf);
	}

	strbuf_release(&path);

	return 0;
}

int set_in_tree_config(struct repository *r, struct string_list *sl)
{
	struct string_list_item *item;
	char *local_config = git_pathdup("config");

	/* clear existing values */
	git_config_set_multivar_in_file_gently(local_config,
					       SPARSE_CHECKOUT_IN_TREE, NULL, NULL, 1);

	for_each_string_list_item(item, sl)
		git_config_set_multivar_in_file_gently(
			local_config, SPARSE_CHECKOUT_IN_TREE,
			item->string, CONFIG_REGEX_NONE, 0);

	free(local_config);
	return 0;
}

int update_in_tree_sparse_checkout(struct repository *r, struct index_state *istate)
{
	struct string_list paths = STRING_LIST_INIT_DUP;
	struct pattern_list temp;

	/* If we do not have this config, skip this step! */
	if (load_in_tree_from_config(r, &paths) ||
	    !paths.nr) {
		return 0;
	}

	/* Check diff for paths over from/to. If any changed, reload */
	/* or for now, reload always! */
	memset(&temp, 0, sizeof(temp));
	hashmap_init(&temp.recursive_hashmap, pl_hashmap_cmp, NULL, 0);
	hashmap_init(&temp.parent_hashmap, pl_hashmap_cmp, NULL, 0);

	if (load_in_tree_pattern_list(istate, &paths, &temp)) {
		warning("load_in_tree_pattern_list failed");
		return 1;
	}
	if (write_patterns_to_sparse_checkout(&temp, 0)) {
		warning("write_patterns_and_update failed");
		return 1;
	}

	return 0;
}
