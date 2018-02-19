#include "cache.h"
#include "config.h"
#include "git-compat-util.h"
#include "pack.h"
#include "packfile.h"
#include "commit.h"
#include "object.h"
#include "revision.h"
#include "sha1-lookup.h"
#include "commit-graph.h"

#define GRAPH_SIGNATURE 0x43475048 /* "CGPH" */
#define GRAPH_CHUNKID_OIDFANOUT 0x4f494446 /* "OIDF" */
#define GRAPH_CHUNKID_OIDLOOKUP 0x4f49444c /* "OIDL" */
#define GRAPH_CHUNKID_DATA 0x43444154 /* "CDAT" */
#define GRAPH_CHUNKID_LARGEEDGES 0x45444745 /* "EDGE" */

#define GRAPH_DATA_WIDTH 36

#define GRAPH_VERSION_1 0x1
#define GRAPH_VERSION GRAPH_VERSION_1

#define GRAPH_OID_VERSION_SHA1 1
#define GRAPH_OID_LEN_SHA1 20
#define GRAPH_OID_VERSION GRAPH_OID_VERSION_SHA1
#define GRAPH_OID_LEN GRAPH_OID_LEN_SHA1

#define GRAPH_LARGE_EDGES_NEEDED 0x80000000
#define GRAPH_PARENT_MISSING 0x7fffffff
#define GRAPH_EDGE_LAST_MASK 0x7fffffff
#define GRAPH_PARENT_NONE 0x70000000

#define GRAPH_LAST_EDGE 0x80000000

#define GRAPH_FANOUT_SIZE (4 * 256)
#define GRAPH_CHUNKLOOKUP_WIDTH 12
#define GRAPH_CHUNKLOOKUP_SIZE (5 * GRAPH_CHUNKLOOKUP_WIDTH)
#define GRAPH_MIN_SIZE (GRAPH_CHUNKLOOKUP_SIZE + GRAPH_FANOUT_SIZE + \
			GRAPH_OID_LEN + 8)

/* global storage */
struct commit_graph *commit_graph = NULL;

char *get_graph_latest_filename(const char *obj_dir)
{
	struct strbuf fname = STRBUF_INIT;
	strbuf_addf(&fname, "%s/info/graph-latest", obj_dir);
	return strbuf_detach(&fname, 0);
}

char *get_graph_latest_contents(const char *obj_dir)
{
	struct strbuf graph_file = STRBUF_INIT;
	char *fname;
	FILE *f;
	char buf[64];

	fname = get_graph_latest_filename(obj_dir);
	f = fopen(fname, "r");
	FREE_AND_NULL(fname);

	if (!f)
		return 0;

	while (!feof(f)) {
		if (fgets(buf, sizeof(buf), f))
			strbuf_addstr(&graph_file, buf);
	}

	fclose(f);
	return strbuf_detach(&graph_file, NULL);
}

static struct commit_graph *alloc_commit_graph(void)
{
	struct commit_graph *g = xmalloc(sizeof(*g));
	memset(g, 0, sizeof(*g));
	g->graph_fd = -1;

	return g;
}

struct commit_graph *load_commit_graph_one(const char *graph_file)
{
	void *graph_map;
	const unsigned char *data, *chunk_lookup;
	size_t graph_size;
	struct stat st;
	uint32_t i;
	struct commit_graph *graph;
	int fd = git_open(graph_file);
	uint64_t last_chunk_offset;
	uint32_t last_chunk_id;
	uint32_t graph_signature;
	unsigned char graph_version, hash_version;

	if (fd < 0)
		return 0;
	if (fstat(fd, &st)) {
		close(fd);
		return 0;
	}
	graph_size = xsize_t(st.st_size);

	if (graph_size < GRAPH_MIN_SIZE) {
		close(fd);
		die("graph file %s is too small", graph_file);
	}
	graph_map = xmmap(NULL, graph_size, PROT_READ, MAP_PRIVATE, fd, 0);
	data = (const unsigned char *)graph_map;

	graph_signature = ntohl(*(uint32_t*)data);
	if (graph_signature != GRAPH_SIGNATURE) {
		munmap(graph_map, graph_size);
		close(fd);
		die("graph signature %X does not match signature %X",
			graph_signature, GRAPH_SIGNATURE);
	}

	graph_version = *(unsigned char*)(data + 4);
	if (graph_version != GRAPH_VERSION) {
		munmap(graph_map, graph_size);
		close(fd);
		die("graph version %X does not match version %X",
			graph_version, GRAPH_VERSION);
	}

	hash_version = *(unsigned char*)(data + 5);
	if (hash_version != GRAPH_OID_VERSION) {
		munmap(graph_map, graph_size);
		close(fd);
		die("hash version %X does not match version %X",
			hash_version, GRAPH_OID_VERSION);
	}

	graph = alloc_commit_graph();

	graph->hash_len = GRAPH_OID_LEN;
	graph->num_chunks = *(unsigned char*)(data + 6);
	graph->graph_fd = fd;
	graph->data = graph_map;
	graph->data_len = graph_size;

	last_chunk_id = 0;
	last_chunk_offset = 8;
	chunk_lookup = data + 8;
	for (i = 0; i < graph->num_chunks; i++) {
		uint32_t chunk_id = get_be32(chunk_lookup + 0);
		uint64_t chunk_offset1 = get_be32(chunk_lookup + 4);
		uint32_t chunk_offset2 = get_be32(chunk_lookup + 8);
		uint64_t chunk_offset = (chunk_offset1 << 32) | chunk_offset2;

		chunk_lookup += GRAPH_CHUNKLOOKUP_WIDTH;

		if (chunk_offset > graph_size - GIT_MAX_RAWSZ)
			die("improper chunk offset %08x%08x", (uint32_t)(chunk_offset >> 32),
			    (uint32_t)chunk_offset);

		switch (chunk_id) {
			case GRAPH_CHUNKID_OIDFANOUT:
				graph->chunk_oid_fanout = (uint32_t*)(data + chunk_offset);
				break;

			case GRAPH_CHUNKID_OIDLOOKUP:
				graph->chunk_oid_lookup = data + chunk_offset;
				break;

			case GRAPH_CHUNKID_DATA:
				graph->chunk_commit_data = data + chunk_offset;
				break;

			case GRAPH_CHUNKID_LARGEEDGES:
				graph->chunk_large_edges = data + chunk_offset;
				break;
		}

		if (last_chunk_id == GRAPH_CHUNKID_OIDLOOKUP)
		{
			graph->num_commits = (chunk_offset - last_chunk_offset)
					     / graph->hash_len;
		}

		last_chunk_id = chunk_id;
		last_chunk_offset = chunk_offset;
	}

	return graph;
}

static void prepare_commit_graph_one(const char *obj_dir)
{
	struct strbuf graph_file = STRBUF_INIT;
	char *graph_name;

	if (commit_graph)
		return;

	graph_name = get_graph_latest_contents(obj_dir);

	if (!graph_name)
		return;

	strbuf_addf(&graph_file, "%s/info/%s", obj_dir, graph_name);

	commit_graph = load_commit_graph_one(graph_file.buf);

	FREE_AND_NULL(graph_name);
	strbuf_release(&graph_file);
}

static int prepare_commit_graph_run_once = 0;
void prepare_commit_graph(void)
{
	struct alternate_object_database *alt;
	char *obj_dir;

	if (prepare_commit_graph_run_once)
		return;
	prepare_commit_graph_run_once = 1;

	obj_dir = get_object_directory();
	prepare_commit_graph_one(obj_dir);
	prepare_alt_odb();
	for (alt = alt_odb_list; !commit_graph && alt; alt = alt->next)
		prepare_commit_graph_one(alt->path);
}

static void close_commit_graph(void)
{
	if (!commit_graph)
		return;

	if (commit_graph->graph_fd >= 0) {
		munmap((void *)commit_graph->data, commit_graph->data_len);
		commit_graph->data = NULL;
		close(commit_graph->graph_fd);
	}

	FREE_AND_NULL(commit_graph);
}

static int bsearch_graph(struct commit_graph *g, struct object_id *oid, uint32_t *pos)
{
	return bsearch_hash(oid->hash, g->chunk_oid_fanout,
			    g->chunk_oid_lookup, g->hash_len, pos);
}

static struct commit_list **insert_parent_or_die(struct commit_graph *g,
						 uint64_t pos,
						 struct commit_list **pptr)
{
	struct commit *c;
	struct object_id oid;
	hashcpy(oid.hash, g->chunk_oid_lookup + g->hash_len * pos);
	c = lookup_commit(&oid);
	if (!c)
		die("could not find commit %s", oid_to_hex(&oid));
	c->graph_pos = pos;
	return &commit_list_insert(c, pptr)->next;
}

static int fill_commit_in_graph(struct commit *item, struct commit_graph *g, uint32_t pos)
{
	struct object_id oid;
	uint32_t new_parent_pos;
	uint32_t *parent_data_ptr;
	uint64_t date_low, date_high;
	struct commit_list **pptr;
	const unsigned char *commit_data = g->chunk_commit_data + (g->hash_len + 16) * pos;

	item->object.parsed = 1;
	item->graph_pos = pos;

	hashcpy(oid.hash, commit_data);
	item->tree = lookup_tree(&oid);

	date_high = ntohl(*(uint32_t*)(commit_data + g->hash_len + 8)) & 0x3;
	date_low = ntohl(*(uint32_t*)(commit_data + g->hash_len + 12));
	item->date = (timestamp_t)((date_high << 32) | date_low);

	pptr = &item->parents;

	new_parent_pos = ntohl(*(uint32_t*)(commit_data + g->hash_len));
	if (new_parent_pos == GRAPH_PARENT_NONE)
		return 1;
	pptr = insert_parent_or_die(g, new_parent_pos, pptr);

	new_parent_pos = ntohl(*(uint32_t*)(commit_data + g->hash_len + 4));
	if (new_parent_pos == GRAPH_PARENT_NONE)
		return 1;
	if (!(new_parent_pos & GRAPH_LARGE_EDGES_NEEDED)) {
		pptr = insert_parent_or_die(g, new_parent_pos, pptr);
		return 1;
	}

	parent_data_ptr = (uint32_t*)(g->chunk_large_edges +
			  4 * (uint64_t)(new_parent_pos & GRAPH_EDGE_LAST_MASK));
	do {
		new_parent_pos = ntohl(*parent_data_ptr);
		pptr = insert_parent_or_die(g,
					    new_parent_pos & GRAPH_EDGE_LAST_MASK,
					    pptr);
		parent_data_ptr++;
	} while (!(new_parent_pos & GRAPH_LAST_EDGE));

	return 1;
}

int parse_commit_in_graph(struct commit *item)
{
	if (!core_commit_graph)
		return 0;
	if (item->object.parsed)
		return 1;

	prepare_commit_graph();
	if (commit_graph) {
		uint32_t pos;
		int found;
		if (item->graph_pos != COMMIT_NOT_FROM_GRAPH) {
			pos = item->graph_pos;
			found = 1;
		} else {
			found = bsearch_graph(commit_graph, &(item->object.oid), &pos);
		}

		if (found)
			return fill_commit_in_graph(item, commit_graph, pos);
	}

	return 0;
}

static void write_graph_chunk_fanout(struct sha1file *f,
				     struct commit **commits,
				     int nr_commits)
{
	uint32_t i, count = 0;
	struct commit **list = commits;
	struct commit **last = commits + nr_commits;

	/*
	 * Write the first-level table (the list is sorted,
	 * but we use a 256-entry lookup to be able to avoid
	 * having to do eight extra binary search iterations).
	 */
	for (i = 0; i < 256; i++) {
		while (list < last) {
			if ((*list)->object.oid.hash[0] != i)
				break;
			count++;
			list++;
		}

		sha1write_be32(f, count);
	}
}

static void write_graph_chunk_oids(struct sha1file *f, int hash_len,
				   struct commit **commits, int nr_commits)
{
	struct commit **list, **last = commits + nr_commits;
	for (list = commits; list < last; list++)
		sha1write(f, (*list)->object.oid.hash, (int)hash_len);
}

static int commit_pos(struct commit **commits, int nr_commits,
		      const struct object_id *oid, uint32_t *pos)
{
	uint32_t first = 0, last = nr_commits;

	while (first < last) {
		uint32_t mid = first + (last - first) / 2;
		struct object_id *current;
		int cmp;

		current = &(commits[mid]->object.oid);
		cmp = oidcmp(oid, current);
		if (!cmp) {
			*pos = mid;
			return 1;
		}
		if (cmp > 0) {
			first = mid + 1;
			continue;
		}
		last = mid;
	}

	*pos = first;
	return 0;
}

static void write_graph_chunk_data(struct sha1file *f, int hash_len,
				   struct commit **commits, int nr_commits)
{
	struct commit **list = commits;
	struct commit **last = commits + nr_commits;
	uint32_t num_large_edges = 0;

	while (list < last) {
		struct commit_list *parent;
		uint32_t int_id;
		uint32_t packedDate[2];

		parse_commit(*list);
		sha1write(f, (*list)->tree->object.oid.hash, hash_len);

		parent = (*list)->parents;

		if (!parent)
			int_id = GRAPH_PARENT_NONE;
		else if (!commit_pos(commits, nr_commits,
				     &(parent->item->object.oid), &int_id))
			int_id = GRAPH_PARENT_MISSING;

		sha1write_be32(f, int_id);

		if (parent)
			parent = parent->next;

		if (!parent)
			int_id = GRAPH_PARENT_NONE;
		else if (parent->next)
			int_id = GRAPH_LARGE_EDGES_NEEDED | num_large_edges;
		else if (!commit_pos(commits, nr_commits,
				    &(parent->item->object.oid), &int_id))
			int_id = GRAPH_PARENT_MISSING;

		sha1write_be32(f, int_id);

		if (parent && parent->next) {
			do {
				num_large_edges++;
				parent = parent->next;
			} while (parent);
		}

		if (sizeof((*list)->date) > 4)
			packedDate[0] = htonl(((*list)->date >> 32) & 0x3);
		else
			packedDate[0] = 0;

		packedDate[1] = htonl((*list)->date);
		sha1write(f, packedDate, 8);

		list++;
	}
}

static void write_graph_chunk_large_edges(struct sha1file *f,
					  struct commit **commits,
					  int nr_commits)
{
	struct commit **list = commits;
	struct commit **last = commits + nr_commits;
	struct commit_list *parent;

	while (list < last) {
		int num_parents = 0;
		for (parent = (*list)->parents; num_parents < 3 && parent;
		     parent = parent->next)
			num_parents++;

		if (num_parents <= 2) {
			list++;
			continue;
		}

		/* Since num_parents > 2, this initializer is safe. */
		for (parent = (*list)->parents->next; parent; parent = parent->next) {
			uint32_t int_id, swap_int_id;
			uint32_t last_edge = 0;
			if (!parent->next)
				last_edge |= GRAPH_LAST_EDGE;

			if (commit_pos(commits, nr_commits,
				       &(parent->item->object.oid),
				       &int_id))
				swap_int_id = htonl(int_id | last_edge);
			else
				swap_int_id = htonl(GRAPH_PARENT_MISSING | last_edge);

			sha1write(f, &swap_int_id, 4);
		}

		list++;
	}
}

static int commit_compare(const void *_a, const void *_b)
{
	struct object_id *a = (struct object_id *)_a;
	struct object_id *b = (struct object_id *)_b;
	return oidcmp(a, b);
}

struct packed_commit_list {
	struct commit **list;
	int nr;
	int alloc;
};

struct packed_oid_list {
	struct object_id *list;
	int nr;
	int alloc;
};

static int if_packed_commit_add_to_list(const struct object_id *oid,
					struct packed_git *pack,
					uint32_t pos,
					void *data)
{
	struct packed_oid_list *list = (struct packed_oid_list*)data;
	enum object_type type;
	unsigned long size;
	void *inner_data;
	off_t offset = nth_packed_object_offset(pack, pos);
	inner_data = unpack_entry(pack, offset, &type, &size);

	if (inner_data)
		free(inner_data);

	if (type != OBJ_COMMIT)
		return 0;

	ALLOC_GROW(list->list, list->nr + 1, list->alloc);
	oidcpy(&(list->list[list->nr]), oid);
	(list->nr)++;

	return 0;
}

static void close_reachable(struct packed_oid_list *oids)
{
	int i;
	struct rev_info revs;
	struct commit *commit;
	init_revisions(&revs, NULL);
	for (i = 0; i < oids->nr; i++) {
		commit = lookup_commit(&oids->list[i]);
		if (commit && !parse_commit(commit))
			revs.commits = commit_list_insert(commit, &revs.commits);
	}

	if (prepare_revision_walk(&revs))
		die(_("revision walk setup failed"));

	while ((commit = get_revision(&revs)) != NULL) {
		ALLOC_GROW(oids->list, oids->nr + 1, oids->alloc);
		oidcpy(&oids->list[oids->nr], &(commit->object.oid));
		(oids->nr)++;
	}
}

char *write_commit_graph(const char *obj_dir,
			 const char **pack_indexes,
			 int nr_packs,
			 const char **commit_hex,
			 int nr_commits)
{
	struct packed_oid_list oids;
	struct packed_commit_list commits;
	struct sha1file *f;
	int i, count_distinct = 0;
	DIR *info_dir;
	struct strbuf tmp_file = STRBUF_INIT;
	struct strbuf graph_file = STRBUF_INIT;
	unsigned char final_hash[GIT_MAX_RAWSZ];
	char *graph_name;
	int fd;
	uint32_t chunk_ids[5];
	uint64_t chunk_offsets[5];
	int num_chunks;
	int num_long_edges;
	struct commit_list *parent;

	oids.nr = 0;
	oids.alloc = (int)(0.15 * approximate_object_count());

	if (oids.alloc < 1024)
		oids.alloc = 1024;
	ALLOC_ARRAY(oids.list, oids.alloc);

	if (pack_indexes) {
		struct strbuf packname = STRBUF_INIT;
		int dirlen;
		strbuf_addf(&packname, "%s/pack/", obj_dir);
		dirlen = packname.len;
		for (i = 0; i < nr_packs; i++) {
			struct packed_git *p;
			strbuf_setlen(&packname, dirlen);
			strbuf_addstr(&packname, pack_indexes[i]);
			p = add_packed_git(packname.buf, packname.len, 1);
			if (!p)
				die("error adding pack %s", packname.buf);
			if (open_pack_index(p))
				die("error opening index for %s", packname.buf);
			for_each_object_in_pack(p, if_packed_commit_add_to_list, &oids);
			close_pack(p);
		}
	}

	if (commit_hex) {
		for (i = 0; i < nr_commits; i++) {
			const char *end;
			struct object_id oid;
			struct commit *result;

			if (commit_hex[i] && parse_oid_hex(commit_hex[i], &oid, &end))
				continue;

			result = lookup_commit_reference_gently(&oid, 1);

			if (result) {
				ALLOC_GROW(oids.list, oids.nr + 1, oids.alloc);
				oidcpy(&oids.list[oids.nr], &(result->object.oid));
				oids.nr++;
			}
		}
	}

	if (!pack_indexes && !commit_hex)
		for_each_packed_object(if_packed_commit_add_to_list, &oids, 0);

	close_reachable(&oids);

	QSORT(oids.list, oids.nr, commit_compare);

	count_distinct = 1;
	for (i = 1; i < oids.nr; i++) {
		if (oidcmp(&oids.list[i-1], &oids.list[i]))
			count_distinct++;
	}

	commits.nr = 0;
	commits.alloc = count_distinct;
	ALLOC_ARRAY(commits.list, commits.alloc);

	num_long_edges = 0;
	for (i = 0; i < oids.nr; i++) {
		int num_parents = 0;
		if (i > 0 && !oidcmp(&oids.list[i-1], &oids.list[i]))
			continue;

		commits.list[commits.nr] = lookup_commit(&oids.list[i]);
		parse_commit(commits.list[commits.nr]);

		for (parent = commits.list[commits.nr]->parents;
		     parent; parent = parent->next)
			num_parents++;

		if (num_parents > 2)
			num_long_edges += num_parents - 1;

		commits.nr++;
	}
	num_chunks = num_long_edges ? 4 : 3;

	strbuf_addf(&tmp_file, "%s/info", obj_dir);
	info_dir = opendir(tmp_file.buf);

	if (!info_dir && mkdir(tmp_file.buf, 0777) < 0)
		die_errno(_("cannot mkdir %s"), tmp_file.buf);
	if (info_dir)
		closedir(info_dir);

	strbuf_addstr(&tmp_file, "/tmp_graph_XXXXXX");

	fd = git_mkstemp_mode(tmp_file.buf, 0444);
	if (fd < 0)
		die_errno("unable to create '%s'", tmp_file.buf);

	f = sha1fd(fd, tmp_file.buf);

	sha1write_be32(f, GRAPH_SIGNATURE);

	sha1write_u8(f, GRAPH_VERSION);
	sha1write_u8(f, GRAPH_OID_VERSION);
	sha1write_u8(f, num_chunks);
	sha1write_u8(f, 0); /* unused padding byte */

	chunk_ids[0] = GRAPH_CHUNKID_OIDFANOUT;
	chunk_ids[1] = GRAPH_CHUNKID_OIDLOOKUP;
	chunk_ids[2] = GRAPH_CHUNKID_DATA;
	if (num_long_edges)
		chunk_ids[3] = GRAPH_CHUNKID_LARGEEDGES;
	else
		chunk_ids[3] = 0;
	chunk_ids[4] = 0;

	chunk_offsets[0] = 8 + GRAPH_CHUNKLOOKUP_SIZE;
	chunk_offsets[1] = chunk_offsets[0] + GRAPH_FANOUT_SIZE;
	chunk_offsets[2] = chunk_offsets[1] + GRAPH_OID_LEN * commits.nr;
	chunk_offsets[3] = chunk_offsets[2] + (GRAPH_OID_LEN + 16) * commits.nr;
	chunk_offsets[4] = chunk_offsets[3] + 4 * num_long_edges;

	for (i = 0; i <= num_chunks; i++) {
		uint32_t chunk_write[3];

		chunk_write[0] = htonl(chunk_ids[i]);
		chunk_write[1] = htonl(chunk_offsets[i] >> 32);
		chunk_write[2] = htonl(chunk_offsets[i] & 0xffffffff);
		sha1write(f, chunk_write, 12);
	}

	write_graph_chunk_fanout(f, commits.list, commits.nr);
	write_graph_chunk_oids(f, GRAPH_OID_LEN, commits.list, commits.nr);
	write_graph_chunk_data(f, GRAPH_OID_LEN, commits.list, commits.nr);
	write_graph_chunk_large_edges(f, commits.list, commits.nr);

	sha1close(f, final_hash, CSUM_CLOSE | CSUM_FSYNC);

	strbuf_addf(&graph_file, "graph-%s.graph", sha1_to_hex(final_hash));
	graph_name = strbuf_detach(&graph_file, NULL);
	strbuf_addf(&graph_file, "%s/info/%s", obj_dir, graph_name);

	close_commit_graph();
	if (rename(tmp_file.buf, graph_file.buf))
		die("failed to rename %s to %s", tmp_file.buf, graph_file.buf);

	strbuf_release(&tmp_file);
	strbuf_release(&graph_file);
	free(oids.list);
	oids.alloc = 0;
	oids.nr = 0;

	return graph_name;
}

