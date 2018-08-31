#include "builtin.h"
#include "cache.h"
#include "config.h"
#include "dir.h"
#include "git-compat-util.h"
#include "lockfile.h"
#include "packfile.h"
#include "parse-options.h"
#include "midx.h"
#include "object-store.h"

static char const * const builtin_midx_usage[] ={
	N_("git midx [--pack-dir <packdir>]"),
	N_("git midx --write [--pack-dir <packdir>] [--update-head] [--delete-expired]"),
	N_("git midx --read [--midx-id=<oid>]"),
	N_("git midx --clear [--pack-dir <packdir>]"),
	NULL
};

static struct opts_midx {
	const char *pack_dir;
	int write;
	int update_head;
	int delete_expired;
	int read;
	const char *midx_id;
	int clear;
	int has_existing;
	struct object_id old_midx_oid;
} opts;

static int build_midx_from_packs(
	const char *pack_dir,
	const char **pack_names, uint32_t nr_packs,
	const char **midx_id, struct midxed_git *midx)
{
	struct packed_git **packs;
	const char **installed_pack_names;
	uint32_t i, j, nr_installed_packs = 0;
	uint32_t nr_objects = 0;
	struct pack_midx_entry *objects;
	struct pack_midx_entry **obj_ptrs;
	uint32_t nr_total_packs = nr_packs;
	uint32_t pack_offset = 0;
	struct strbuf pack_path = STRBUF_INIT;
	int baselen;

	if (midx)
		nr_total_packs += midx->num_packs;

	ALLOC_ARRAY(packs, nr_total_packs);
	ALLOC_ARRAY(installed_pack_names, nr_total_packs);

	if (midx) {
		for (i = 0; i < midx->num_packs; i++)
			installed_pack_names[nr_installed_packs++] = midx->pack_names[i];
		pack_offset = midx->num_packs;
	}

	strbuf_addstr(&pack_path, pack_dir);
	strbuf_addch(&pack_path, '/');
	baselen = pack_path.len;
	for (i = 0; i < nr_packs; i++) {
		strbuf_setlen(&pack_path, baselen);
		strbuf_addstr(&pack_path, pack_names[i]);

		strbuf_strip_suffix(&pack_path, ".pack");
		strbuf_addstr(&pack_path, ".idx");

		packs[nr_installed_packs] = add_packed_git(pack_path.buf, pack_path.len, 0);

		if (packs[nr_installed_packs] != NULL) {
			if (open_pack_index(packs[nr_installed_packs]))
				continue;

			nr_objects += packs[nr_installed_packs]->num_objects;
			installed_pack_names[nr_installed_packs] = pack_names[i];
			nr_installed_packs++;
		}
	}
	strbuf_release(&pack_path);

	if (!nr_objects || !nr_installed_packs) {
		free(packs);
		free(installed_pack_names);
		return 1;
	}

	if (midx)
		nr_objects += midx->num_objects;

	ALLOC_ARRAY(objects, nr_objects);
	nr_objects = 0;

	for (i = 0; midx && i < midx->num_objects; i++)
		nth_midxed_object_entry(midx, i, &objects[nr_objects++]);

	for (i = pack_offset; i < nr_installed_packs; i++) {
		struct packed_git *p = packs[i];

		for (j = 0; j < p->num_objects; j++) {
			struct pack_midx_entry entry;

			if (!nth_packed_object_oid(&entry.oid, p, j))
				die("unable to get sha1 of object %u in %s",
				i, p->pack_name);

			entry.pack_int_id = i;
			entry.offset = nth_packed_object_offset(p, j);

			objects[nr_objects] = entry;
			nr_objects++;
		}
	}

	ALLOC_ARRAY(obj_ptrs, nr_objects);
	for (i = 0; i < nr_objects; i++)
		obj_ptrs[i] = &objects[i];

	*midx_id = write_midx_file(pack_dir, NULL,
		installed_pack_names, nr_installed_packs,
		obj_ptrs, nr_objects);

	FREE_AND_NULL(installed_pack_names);
	FREE_AND_NULL(obj_ptrs);
	FREE_AND_NULL(objects);

	return 0;
}

static void update_head_file(const char *pack_dir, const char *midx_id)
{
	struct strbuf head_path = STRBUF_INIT;
	FILE* f;
	struct lock_file lk = LOCK_INIT;

	strbuf_addstr(&head_path, pack_dir);
	strbuf_addstr(&head_path, "/");
	strbuf_addstr(&head_path, "midx-head");

	hold_lock_file_for_update(&lk, head_path.buf, LOCK_DIE_ON_ERROR);
	strbuf_release(&head_path);

	f = fdopen_lock_file(&lk, "w");
	if (!f)
		die_errno("unable to fdopen midx-head");

	fprintf(f, "%s", midx_id);
	commit_lock_file(&lk);
}

static int cmd_midx_write(void)
{
	const char **pack_names = NULL;
	uint32_t i, nr_packs = 0;
	const char *midx_id;
	DIR *dir;
	struct dirent *de;
	struct midxed_git *m = NULL;

	if (opts.has_existing)
		m = get_midxed_git(opts.pack_dir, &opts.old_midx_oid);

	dir = opendir(opts.pack_dir);
	if (!dir) {
		error_errno("unable to open object pack directory: %s",
			opts.pack_dir);
		return 1;
	}

	nr_packs = 8;
	ALLOC_ARRAY(pack_names, nr_packs);

	i = 0;
	while ((de = readdir(dir)) != NULL) {
		if (is_dot_or_dotdot(de->d_name))
			continue;

		if (ends_with(de->d_name, ".pack")) {
			if (m && contains_pack(m, de->d_name))
				continue;

			ALLOC_GROW(pack_names, i + 1, nr_packs);
			pack_names[i++] = xstrdup(de->d_name);
		}
	}

	nr_packs = i;
	closedir(dir);

	if (!nr_packs && opts.has_existing) {
		printf("%s\n", oid_to_hex(&opts.old_midx_oid));
		goto cleanup;
	}

	if (build_midx_from_packs(opts.pack_dir, pack_names, nr_packs, &midx_id, m))
		die("Failed to build MIDX.");

	printf("%s\n", midx_id);

	if (opts.update_head)
		update_head_file(opts.pack_dir, midx_id);

	if (opts.delete_expired && opts.update_head && opts.has_existing &&
	    strcmp(midx_id, oid_to_hex(&opts.old_midx_oid))) {
		struct strbuf old_path = STRBUF_INIT;
		strbuf_addstr(&old_path, opts.pack_dir);
		strbuf_addstr(&old_path, "/midx-");
		strbuf_addstr(&old_path, oid_to_hex(&opts.old_midx_oid));
		strbuf_addstr(&old_path, ".midx");

		close_midx(m);
		if (remove_path(old_path.buf))
			die("Failed to remove path %s", old_path.buf);

		strbuf_release(&old_path);
	}

cleanup:
	if (pack_names)
		FREE_AND_NULL(pack_names);
	return 0;
}

static int cmd_midx_read(void)
{
	struct object_id midx_oid;
	struct midxed_git *midx;
	uint32_t i;

	if (opts.midx_id && strlen(opts.midx_id) == GIT_MAX_HEXSZ)
		get_oid_hex(opts.midx_id, &midx_oid);
	else if (!get_midx_head_oid(opts.pack_dir, &midx_oid))
		die("No midx-head exists.");

	midx = get_midxed_git(opts.pack_dir, &midx_oid);

	printf("header: %08x %08x %02x %02x %02x %02x %08x\n",
		ntohl(midx->hdr->midx_signature),
		ntohl(midx->hdr->midx_version),
		midx->hdr->hash_version,
		midx->hdr->hash_len,
		midx->hdr->num_base_midx,
		midx->hdr->num_chunks,
		ntohl(midx->hdr->num_packs));
	printf("num_objects: %d\n", midx->num_objects);
	printf("chunks:");

	if (midx->chunk_pack_lookup)
		printf(" pack_lookup");
	if (midx->chunk_pack_names)
		printf(" pack_names");
	if (midx->chunk_oid_fanout)
		printf(" oid_fanout");
	if (midx->chunk_oid_lookup)
		printf(" oid_lookup");
	if (midx->chunk_object_offsets)
		printf(" object_offsets");
	if (midx->chunk_large_offsets)
		printf(" large_offsets");
	printf("\n");

	printf("pack_names:\n");
	for (i = 0; i < midx->num_packs; i++)
		printf("%s\n", midx->pack_names[i]);

	printf("pack_dir: %s\n", midx->pack_dir);
	return 0;
}

static int cmd_midx_clear(void)
{
	struct strbuf old_path = STRBUF_INIT;
	struct strbuf head_path = STRBUF_INIT;

	if (!opts.has_existing)
		return 0;

	strbuf_addstr(&head_path, opts.pack_dir);
	strbuf_addstr(&head_path, "/");
	strbuf_addstr(&head_path, "midx-head");
	if (remove_path(head_path.buf))
		die("Failed to remove path %s", head_path.buf);

	strbuf_addstr(&old_path, opts.pack_dir);
	strbuf_addstr(&old_path, "/midx-");
	strbuf_addstr(&old_path, oid_to_hex(&opts.old_midx_oid));
	strbuf_addstr(&old_path, ".midx");

	if (remove_path(old_path.buf))
		die("Failed to remove path %s", old_path.buf);

	strbuf_release(&old_path);
	strbuf_release(&head_path);
	return 0;
}

int cmd_midx(int argc, const char **argv, const char *prefix)
{
	static struct option builtin_midx_options[] = {
		{ OPTION_STRING, 'p', "pack-dir", &opts.pack_dir,
			N_("dir"),
			N_("The pack directory containing set of packfile and pack-index pairs.") },
		OPT_BOOL('w', "write", &opts.write,
			N_("write midx file")),
		OPT_BOOL('u', "update-head", &opts.update_head,
			N_("update midx-head to written midx file")),
		OPT_BOOL('d', "delete-expired", &opts.delete_expired,
			N_("delete expired head midx file")),
		OPT_BOOL('r', "read", &opts.read,
			N_("read midx file")),
		OPT_BOOL('c', "clear", &opts.clear,
			N_("clear midx file and midx-head")),
		{ OPTION_STRING, 'M', "midx-id", &opts.midx_id,
			N_("oid"),
			N_("An OID for a specific midx file in the pack-dir."),
			PARSE_OPT_OPTARG, NULL, (intptr_t) "" },
		OPT_END(),
	};

	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage_with_options(builtin_midx_usage, builtin_midx_options);

	git_config(git_default_config, NULL);
	if (!core_midx)
		die("git-midx requires core.midx=true.");

	argc = parse_options(argc, argv, prefix,
			     builtin_midx_options,
			     builtin_midx_usage, 0);

	if (opts.write + opts.read + opts.clear > 1)
		usage_with_options(builtin_midx_usage, builtin_midx_options);

	if (!opts.pack_dir) {
		struct strbuf path = STRBUF_INIT;
		strbuf_addstr(&path, get_object_directory());
		strbuf_addstr(&path, "/pack");
		opts.pack_dir = strbuf_detach(&path, NULL);
	}

	opts.has_existing = !!get_midx_head_oid(opts.pack_dir, &opts.old_midx_oid);

	if (opts.write)
		return cmd_midx_write();
	if (opts.read)
		return cmd_midx_read();
	if (opts.clear)
		return cmd_midx_clear();

	return 0;
}
