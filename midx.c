#include "cache.h"
#include "git-compat-util.h"
#include "pack.h"
#include "packfile.h"
#include "midx.h"

#define MIDX_SIGNATURE 0x4d494458 /* "MIDX" */
#define MIDX_CHUNKID_PACKLOOKUP 0x504c4f4f /* "PLOO" */
#define MIDX_CHUNKID_PACKNAMES 0x504e414d /* "PNAM" */
#define MIDX_CHUNKID_OIDFANOUT 0x4f494446 /* "OIDF" */
#define MIDX_CHUNKID_OIDLOOKUP 0x4f49444c /* "OIDL" */
#define MIDX_CHUNKID_OBJECTOFFSETS 0x4f4f4646 /* "OOFF" */
#define MIDX_CHUNKID_LARGEOFFSETS 0x4c4f4646 /* "LOFF" */

#define MIDX_VERSION_1 1
#define MIDX_VERSION MIDX_VERSION_1

#define MIDX_OID_VERSION_SHA1 1
#define MIDX_OID_LEN_SHA1 20
#define MIDX_OID_VERSION MIDX_OID_VERSION_SHA1
#define MIDX_OID_LEN MIDX_OID_LEN_SHA1

#define MIDX_LARGE_OFFSET_NEEDED 0x80000000

/* MIDX-git global storage */
struct midxed_git *midxed_git = 0;

char* get_midx_filename_oid(const char *pack_dir,
			    struct object_id *oid)
{
	struct strbuf head_path = STRBUF_INIT;
	strbuf_addstr(&head_path, pack_dir);
	strbuf_addstr(&head_path, "/midx-");
	strbuf_addstr(&head_path, oid_to_hex(oid));
	strbuf_addstr(&head_path, ".midx");

	return strbuf_detach(&head_path, NULL);
}

char *get_midx_head_filename(const char *pack_dir)
{
	struct strbuf head_filename = STRBUF_INIT;
	strbuf_addstr(&head_filename, pack_dir);
	strbuf_addstr(&head_filename, "/midx-head");
	return strbuf_detach(&head_filename, NULL);
}

struct object_id *get_midx_head_oid(const char *pack_dir,
				    struct object_id *oid)
{
	char oid_hex[GIT_MAX_HEXSZ + 1];
	FILE *f;
	char *head_filename = get_midx_head_filename(pack_dir);

	f = fopen(head_filename, "r");
	FREE_AND_NULL(head_filename);

	if (!f)
		return 0;

	if (!fgets(oid_hex, sizeof(oid_hex), f))
		die("failed to read midx-head");

	fclose(f);

	if (get_oid_hex(oid_hex, oid))
		return 0;
	return oid;
}

static struct midxed_git *alloc_midxed_git(int extra)
{
	struct midxed_git *m = xmalloc(st_add(sizeof(*m), extra));
	memset(m, 0, sizeof(*m));
	m->midx_fd = -1;

	return m;
}

static struct midxed_git *load_midxed_git_one(const char *midx_file, const char *pack_dir)
{
	void *midx_map;
	const unsigned char *data;
	struct pack_midx_header *hdr;
	size_t midx_size, packs_len;
	struct stat st;
	uint32_t i;
	struct midxed_git *midx;
	int fd = git_open(midx_file);

	if (fd < 0)
		return 0;
	if (fstat(fd, &st)) {
		close(fd);
		return 0;
	}
	midx_size = xsize_t(st.st_size);

	if (midx_size < 16 + 8 * 5 + 4 * 256 + GIT_MAX_RAWSZ) {
		close(fd);
		die("midx file %s is too small", midx_file);
	}
	midx_map = xmmap(NULL, midx_size, PROT_READ, MAP_PRIVATE, fd, 0);
	data = (const unsigned char *)midx_map;

	hdr = midx_map;
	if (ntohl(hdr->midx_signature) != MIDX_SIGNATURE) {
		munmap(midx_map, midx_size);
		close(fd);
		die("MIDX signature %X does not match signature %X",
		    ntohl(hdr->midx_signature), MIDX_SIGNATURE);
	}

	if (ntohl(hdr->midx_version) != MIDX_VERSION) {
		munmap(midx_map, midx_size);
		die("MIDX version %X does not match version %X",
		    ntohl(hdr->midx_version), MIDX_VERSION);
	}

	midx = alloc_midxed_git(strlen(pack_dir) + 1);

	midx->hdr = hdr;
	midx->midx_fd = fd;
	midx->data = midx_map;
	midx->data_len = midx_size;

	for (i = 0; i <= hdr->num_chunks; i++) {
		uint32_t chunk_id = ntohl(*(uint32_t*)(data + sizeof(*hdr) + 12 * i));
		uint64_t chunk_offset1 = ntohl(*(uint32_t*)(data + sizeof(*hdr) + 12 * i + 4));
		uint32_t chunk_offset2 = ntohl(*(uint32_t*)(data + sizeof(*hdr) + 12 * i + 8));
		uint64_t chunk_offset = (chunk_offset1 << 32) | chunk_offset2;

		if (sizeof(data) == 4 && chunk_offset >> 32) {
			munmap(midx_map, midx_size);
			close(fd);
			die(_("unable to memory-map in 32-bit address space"));
		}

		switch (chunk_id) {
			case MIDX_CHUNKID_PACKLOOKUP:
				midx->chunk_pack_lookup = data + chunk_offset;
				break;

			case MIDX_CHUNKID_PACKNAMES:
				midx->chunk_pack_names = data + chunk_offset;
				break;

			case MIDX_CHUNKID_OIDFANOUT:
				midx->chunk_oid_fanout = data + chunk_offset;
				break;

			case MIDX_CHUNKID_OIDLOOKUP:
				midx->chunk_oid_lookup = data + chunk_offset;
				break;

			case MIDX_CHUNKID_OBJECTOFFSETS:
				midx->chunk_object_offsets = data + chunk_offset;
				break;

			case MIDX_CHUNKID_LARGEOFFSETS:
				midx->chunk_large_offsets = data + chunk_offset;
				break;

			case 0:
				break;

			default:
				munmap(midx_map, midx_size);
				close(fd);
				die("unrecognized MIDX chunk id: %08x", chunk_id);
		}
	}

	midx->num_objects = ntohl(*((uint32_t*)(midx->chunk_oid_fanout + 255 * 4)));
	midx->num_packs = ntohl(midx->hdr->num_packs);

	packs_len = st_mult(sizeof(struct packed_git*), midx->num_packs);

	if (packs_len) {
		ALLOC_ARRAY(midx->packs, midx->num_packs);
		ALLOC_ARRAY(midx->pack_names, midx->num_packs);
		memset(midx->packs, 0, packs_len);

		for (i = 0; i < midx->num_packs; i++) {
			uint32_t name_offset = ntohl(*(uint32_t*)(midx->chunk_pack_lookup + 4 * i));
			midx->pack_names[i] = (const char*)(midx->chunk_pack_names + name_offset);
		}
	}

	strcpy(midx->pack_dir, pack_dir);
	return midx;
}

struct midxed_git *get_midxed_git(const char *pack_dir, struct object_id *oid)
{
	struct midxed_git *m;
	char *fname = get_midx_filename_oid(pack_dir, oid);
	m = load_midxed_git_one(fname, pack_dir);
	free(fname);
	return m;
}

static char* get_midx_filename_dir(const char *pack_dir)
{
	struct object_id oid;
	if (!get_midx_head_oid(pack_dir, &oid))
		return 0;

	return get_midx_filename_oid(pack_dir, &oid);
}

static int prepare_midxed_git_head(char *pack_dir, int local)
{
	struct midxed_git *m = midxed_git;
	char *midx_head_path = get_midx_filename_dir(pack_dir);

	if (!core_midx)
		return 1;

	if (midx_head_path) {
		midxed_git = load_midxed_git_one(midx_head_path, pack_dir);
		midxed_git->next = m;
		FREE_AND_NULL(midx_head_path);
		return 1;
	}

	return 0;
}

int prepare_midxed_git_objdir(char *obj_dir, int local)
{
	int ret;
	struct strbuf pack_dir = STRBUF_INIT;
	strbuf_addstr(&pack_dir, obj_dir);
	strbuf_add(&pack_dir, "/pack", 5);

	ret = prepare_midxed_git_head(pack_dir.buf, local);
	strbuf_release(&pack_dir);
	return ret;
}

struct pack_midx_details_internal {
	uint32_t pack_int_id;
	uint32_t internal_offset;
};

struct pack_midx_details *nth_midxed_object_details(struct midxed_git *m,
						    uint32_t n,
						    struct pack_midx_details *d)
{
	struct pack_midx_details_internal *d_internal;
	const unsigned char *details = m->chunk_object_offsets;

	if (n >= m->num_objects)
		return NULL;

	d_internal = (struct pack_midx_details_internal*)(details + 8 * n);
	d->pack_int_id = ntohl(d_internal->pack_int_id);
	d->offset = ntohl(d_internal->internal_offset);

	if (m->chunk_large_offsets && d->offset & MIDX_LARGE_OFFSET_NEEDED) {
		uint32_t large_offset = d->offset ^ MIDX_LARGE_OFFSET_NEEDED;
		const unsigned char *large_offsets = m->chunk_large_offsets + 8 * large_offset;

		d->offset =  (((uint64_t)ntohl(*((uint32_t *)(large_offsets + 0)))) << 32) |
					 ntohl(*((uint32_t *)(large_offsets + 4)));
	}

	return d;
}

struct pack_midx_entry *nth_midxed_object_entry(struct midxed_git *m,
						uint32_t n,
						struct pack_midx_entry *e)
{
	struct pack_midx_details details;
	const unsigned char *index = m->chunk_oid_lookup;

	if (!nth_midxed_object_details(m, n, &details))
		return NULL;

	memcpy(e->oid.hash, index + m->hdr->hash_len * n, m->hdr->hash_len);
	e->pack_int_id = details.pack_int_id;
	e->offset = details.offset;

	return e;
}

const struct object_id *nth_midxed_object_oid(struct object_id *oid,
					      struct midxed_git *m,
					      uint32_t n)
{
	struct pack_midx_entry e;

	if (!nth_midxed_object_entry(m, n, &e))
		return 0;

	hashcpy(oid->hash, e.oid.hash);
	return oid;
}

int bsearch_midx(struct midxed_git *m, const unsigned char *sha1, uint32_t *pos)
{
	uint32_t last, first = 0;

	if (sha1[0])
		first = ntohl(*(uint32_t*)(m->chunk_oid_fanout + 4 * (sha1[0] - 1)));
	last = ntohl(*(uint32_t*)(m->chunk_oid_fanout + 4 * sha1[0]));

	while (first < last) {
		uint32_t mid = first + (last - first) / 2;
		const unsigned char *current;
		int cmp;

		current = m->chunk_oid_lookup + m->hdr->hash_len * mid;
		cmp = hashcmp(sha1, current);
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

static int prepare_midx_pack(struct midxed_git *m, uint32_t pack_int_id)
{
	struct strbuf pack_name = STRBUF_INIT;

	if (pack_int_id >= m->hdr->num_packs)
		return 1;

	if (m->packs[pack_int_id])
		return 0;

	strbuf_addstr(&pack_name, m->pack_dir);
	strbuf_addstr(&pack_name, "/");
	strbuf_addstr(&pack_name, m->pack_names[pack_int_id]);
	strbuf_strip_suffix(&pack_name, ".pack");
	strbuf_addstr(&pack_name, ".idx");

	m->packs[pack_int_id] = add_packed_git(pack_name.buf, pack_name.len, 1);
	strbuf_release(&pack_name);
	return !m->packs[pack_int_id];
}

static int find_pack_entry_midx(const unsigned char *sha1,
				struct midxed_git *m,
				struct packed_git **p,
				off_t *offset)
{
	uint32_t pos;
	struct pack_midx_details d;

	if (!bsearch_midx(m, sha1, &pos) ||
	    !nth_midxed_object_details(m, pos, &d))
		return 0;

	if (d.pack_int_id >= m->num_packs)
		die(_("bad pack-int-id %d"), d.pack_int_id);

	/* load packfile, if necessary */
	if (prepare_midx_pack(m, d.pack_int_id))
		return 0;

	*p = m->packs[d.pack_int_id];
	*offset = d.offset;

	return 1;
}

int fill_pack_entry_midx(const unsigned char *sha1,
			 struct pack_entry *e)
{
	struct packed_git *p;
	struct midxed_git *m;

	if (!core_midx)
		return 0;

	m = midxed_git;
	while (m)
	{
		off_t offset;
		if (!find_pack_entry_midx(sha1, m, &p, &offset)) {
			m = m->next;
			continue;
		}

		/*
		* We are about to tell the caller where they can locate the
		* requested object.  We better make sure the packfile is
		* still here and can be accessed before supplying that
		* answer, as it may have been deleted since the MIDX was
		* loaded!
		*/
		if (!is_pack_valid(p))
			return 0;

		e->offset = offset;
		e->p = p;
		hashcpy(e->sha1, sha1);

		return 1;
	}

	return 0;
}

int contains_pack(struct midxed_git *m, const char *pack_name)
{
	uint32_t first = 0, last = m->num_packs;

	while (first < last) {
		uint32_t mid = first + (last - first) / 2;
		const char *current;
		int cmp;

		current = m->pack_names[mid];
		cmp = strcmp(pack_name, current);
		if (!cmp)
			return 1;
		if (cmp > 0) {
			first = mid + 1;
			continue;
		}
		last = mid;
	}

	return 0;
}

static int midx_sha1_compare(const void *_a, const void *_b)
{
	struct pack_midx_entry *a = *(struct pack_midx_entry **)_a;
	struct pack_midx_entry *b = *(struct pack_midx_entry **)_b;
	return oidcmp(&a->oid, &b->oid);
}

static void write_midx_chunk_packlookup(
	struct sha1file *f,
	const char **pack_names, uint32_t nr_packs)
{
	uint32_t i, cur_len = 0;

	for (i = 0; i < nr_packs; i++) {
		uint32_t swap_len = htonl(cur_len);
		sha1write(f, &swap_len, 4);
		cur_len += strlen(pack_names[i]) + 1;
	}
}

static void write_midx_chunk_packnames(
	struct sha1file *f,
	const char **pack_names, uint32_t nr_packs)
{
	uint32_t i;
	for (i = 0; i < nr_packs; i++)
		sha1write(f, pack_names[i], strlen(pack_names[i]) + 1);
}

static void write_midx_chunk_oidfanout(
	struct sha1file *f,
	struct pack_midx_entry **objects, uint32_t nr_objects)
{
	struct pack_midx_entry **list = objects;
	struct pack_midx_entry **last = objects + nr_objects;
	uint32_t count_distinct = 0;
	uint32_t i;

	/*
	* Write the first-level table (the list is sorted,
	* but we use a 256-entry lookup to be able to avoid
	* having to do eight extra binary search iterations).
	*/
	for (i = 0; i < 256; i++) {
		struct pack_midx_entry **next = list;
		struct pack_midx_entry *prev = 0;
		uint32_t swap_distinct;

		while (next < last) {
			struct pack_midx_entry *obj = *next;
			if (obj->oid.hash[0] != i)
				break;

			if (!prev || oidcmp(&(prev->oid), &(obj->oid)))
				count_distinct++;

			prev = obj;
			next++;
		}

		swap_distinct = htonl(count_distinct);
		sha1write(f, &swap_distinct, 4);
		list = next;
	}
}

static void write_midx_chunk_oidlookup(
	struct sha1file *f, unsigned char hash_len,
	struct pack_midx_entry **objects, uint32_t nr_objects)
{
	struct pack_midx_entry **list = objects;
	struct object_id *last_oid = 0;
	uint32_t i;

	for (i = 0; i < nr_objects; i++) {
		struct pack_midx_entry *obj = *list++;

		if (last_oid && !oidcmp(last_oid, &obj->oid))
			continue;

		last_oid = &obj->oid;
		sha1write(f, obj->oid.hash, (int)hash_len);
	}
}

static void write_midx_chunk_objectoffsets(
	struct sha1file *f, int large_offset_needed,
	struct pack_midx_entry **objects, uint32_t nr_objects, uint32_t *pack_perm)
{
	struct pack_midx_entry **list = objects;
	struct object_id *last_oid = 0;
	uint32_t i, nr_large_offset = 0;

	for (i = 0; i < nr_objects; i++) {
		struct pack_midx_details_internal details;
		struct pack_midx_entry *obj = *list++;

		if (last_oid && !oidcmp(last_oid, &obj->oid))
			continue;

		last_oid = &obj->oid;

		details.pack_int_id = htonl(pack_perm[obj->pack_int_id]);

		if (large_offset_needed && obj->offset >> 31)
			details.internal_offset = (MIDX_LARGE_OFFSET_NEEDED | nr_large_offset++);
		else
			details.internal_offset = (uint32_t)obj->offset;

		details.internal_offset = htonl(details.internal_offset);
		sha1write(f, &details, 8);
	}
}

static void write_midx_chunk_largeoffsets(
	struct sha1file *f, uint32_t nr_large_offset,
	struct pack_midx_entry **objects, uint32_t nr_objects)
{
	struct pack_midx_entry **list = objects;
	struct object_id *last_oid = 0;

	while (nr_large_offset) {
		struct pack_midx_entry *obj = *list++;
		uint64_t offset = obj->offset;
		uint32_t split[2];

		if (last_oid && !oidcmp(last_oid, &obj->oid))
			continue;

		last_oid = &obj->oid;

		if (!(offset >> 31))
			continue;

		split[0] = htonl(offset >> 32);
		split[1] = htonl(offset & 0xffffffff);

		sha1write(f, split, 8);
		nr_large_offset--;
	}
}

struct pack_pair {
	uint32_t pack_int_id;
	const char *pack_name;
};

static int pack_pair_compare(const void *_a, const void *_b)
{
	struct pack_pair *a = (struct pack_pair *)_a;
	struct pack_pair *b = (struct pack_pair *)_b;
	return strcmp(a->pack_name, b->pack_name);
}

static void sort_packs_by_name(const char **pack_names, uint32_t nr_packs, uint32_t *perm)
{
	uint32_t i;
	struct pack_pair *pairs;

	ALLOC_ARRAY(pairs, nr_packs);

	for (i = 0; i < nr_packs; i++) {
		pairs[i].pack_int_id = i;
		pairs[i].pack_name = pack_names[i];
	}

	QSORT(pairs, nr_packs, pack_pair_compare);

	for (i = 0; i < nr_packs; i++) {
		pack_names[i] = pairs[i].pack_name;
		perm[pairs[i].pack_int_id] = i;
	}
}

const char *write_midx_file(const char *pack_dir,
			    const char *midx_name,
			    const char **pack_names,
			    uint32_t nr_packs,
			    struct pack_midx_entry **objects,
			    uint32_t nr_objects)
{
	struct sha1file *f;
	struct pack_midx_entry **sorted_by_sha;
	int i, chunk, fd;
	struct pack_midx_header hdr;
	uint32_t chunk_ids[7];
	uint64_t chunk_offsets[7];
	unsigned char large_offset_needed = 0;
	unsigned int nr_large_offset = 0;
	unsigned char final_hash[GIT_MAX_RAWSZ];
	const char *final_hex;
	int rename_needed = 0;
	uint32_t count_distinct = 0;
	int total_name_len = 0;
	uint32_t *pack_perm;

	if (!core_midx)
		return 0;

	/* Sort packs */
	if (nr_packs) {
		ALLOC_ARRAY(pack_perm, nr_packs);
		sort_packs_by_name(pack_names, nr_packs, pack_perm);
	} else {
		pack_perm = 0;
	}

	/* Sort objects */
	if (nr_objects) {
		sorted_by_sha = objects;

		QSORT(sorted_by_sha, nr_objects, midx_sha1_compare);

		for (i = 0; i < nr_objects; i++) {
			if (i &&
			    !oidcmp(&sorted_by_sha[i-1]->oid, &sorted_by_sha[i]->oid))
				continue;

			count_distinct++;

			if (sorted_by_sha[i]->offset > 0x7fffffff)
				nr_large_offset++;
			if (sorted_by_sha[i]->offset > 0xffffffff)
				large_offset_needed = 1;
		}
	} else {
		sorted_by_sha = NULL;
	}

	for (i = 0; i < nr_packs; i++)
		total_name_len += strlen(pack_names[i]) + 1;

	/* open temp file, or direct file if given */
	if (!midx_name) {
		struct strbuf tmp_file = STRBUF_INIT;
		strbuf_addstr(&tmp_file, pack_dir);
		strbuf_addstr(&tmp_file, "/tmp_midx_XXXXXX");

		fd = git_mkstemp_mode(tmp_file.buf, 0444);
		if (fd < 0)
			die_errno("unable to create '%s'", tmp_file.buf);

		midx_name = strbuf_detach(&tmp_file, NULL);
		rename_needed = 1;
	} else {
		unlink(midx_name);
		fd = open(midx_name, O_CREAT|O_EXCL|O_WRONLY, 0600);
		if (fd < 0)
			die_errno("unable to create '%s'", midx_name);
	}
	f = sha1fd(fd, midx_name);

	/* fill header info */
	hdr.midx_signature = htonl(MIDX_SIGNATURE);
	hdr.midx_version = htonl(MIDX_VERSION);

	hdr.hash_version = MIDX_OID_VERSION;
	hdr.hash_len = MIDX_OID_LEN;
	hdr.num_base_midx = 0;
	hdr.num_packs = htonl(nr_packs);

	/*
	 * We expect the following chunks, which are required:
	 *
	 * Packfile Name Lookup
	 * Packfile Names
	 * OID Fanout
	 * OID Lookup
	 * Object Offsets
	 */
	hdr.num_chunks = large_offset_needed ? 6 : 5;

	/* write header to file */
	assert(sizeof(hdr) == 16);
	sha1write(f, &hdr, sizeof(hdr));

	/*
	 * Fill initial chunk values using offsets
	 * relative to first chunk.
	 */
	chunk_offsets[0] = sizeof(hdr) + 12 * (hdr.num_chunks + 1);
	chunk_ids[0] = MIDX_CHUNKID_PACKLOOKUP;
	chunk_offsets[1] = chunk_offsets[0] + nr_packs * 4;
	chunk_ids[1] = MIDX_CHUNKID_OIDFANOUT;
	chunk_offsets[2] = chunk_offsets[1] + 256 * 4;
	chunk_ids[2] = MIDX_CHUNKID_OIDLOOKUP;
	chunk_offsets[3] = chunk_offsets[2] + (uint64_t)count_distinct
					    * (uint64_t)hdr.hash_len;
	chunk_ids[3] = MIDX_CHUNKID_OBJECTOFFSETS;
	chunk_offsets[4] = chunk_offsets[3] + 8 * (uint64_t)count_distinct;

	if (large_offset_needed) {
		chunk_ids[4] = MIDX_CHUNKID_LARGEOFFSETS;
		chunk_offsets[5] = chunk_offsets[4] + 8 * (uint64_t)nr_large_offset;
		chunk_ids[5] = MIDX_CHUNKID_PACKNAMES;
		chunk_offsets[6] = chunk_offsets[5] + total_name_len;
		chunk_ids[6] = 0;
	} else {
		chunk_ids[4] = MIDX_CHUNKID_PACKNAMES;
		chunk_offsets[5] = chunk_offsets[4] + total_name_len;
		chunk_ids[5] = 0;
	}

	for (i = 0; i <= hdr.num_chunks; i++) {
		uint32_t chunk_write[3];

		chunk_write[0] = htonl(chunk_ids[i]);
		chunk_write[1] = htonl(chunk_offsets[i] >> 32);
		chunk_write[2] = htonl(chunk_offsets[i] & 0xffffffff);
		sha1write(f, chunk_write, 12);
	}

	for (chunk = 0; chunk < hdr.num_chunks; chunk++) {
		switch (chunk_ids[chunk]) {
		case MIDX_CHUNKID_PACKLOOKUP:
			write_midx_chunk_packlookup(f, pack_names, nr_packs);
			break;

		case MIDX_CHUNKID_PACKNAMES:
			write_midx_chunk_packnames(f, pack_names, nr_packs);
			break;

		case MIDX_CHUNKID_OIDFANOUT:
			write_midx_chunk_oidfanout(f, sorted_by_sha, nr_objects);
			break;

		case MIDX_CHUNKID_OIDLOOKUP:
			write_midx_chunk_oidlookup(f, hdr.hash_len, sorted_by_sha,
						   nr_objects);
			break;

		case MIDX_CHUNKID_OBJECTOFFSETS:
			write_midx_chunk_objectoffsets(f, large_offset_needed,
						       sorted_by_sha, nr_objects,
						       pack_perm);
			break;

		case MIDX_CHUNKID_LARGEOFFSETS:
			write_midx_chunk_largeoffsets(f, nr_large_offset,
						      sorted_by_sha, nr_objects);
			break;

		case 0:
			break;

		default:
			die("unrecognized MIDX chunk id: %08x", chunk_ids[chunk]);
		}
	}

	sha1close(f, final_hash, CSUM_CLOSE | CSUM_FSYNC);

	if (rename_needed)
	{
		struct object_id oid;
		char *fname;

		memcpy(oid.hash, final_hash, GIT_MAX_RAWSZ);
		fname = get_midx_filename_oid(pack_dir, &oid);
		final_hex = sha1_to_hex(final_hash);

		if (rename(midx_name, fname))
			die("failed to rename %s to %s", midx_name, fname);

		free(fname);
	} else {
		final_hex = midx_name;
	}

	return final_hex;
}

int close_midx(struct midxed_git *m)
{
	int i;
	if (m->midx_fd < 0)
		return 0;

	for (i = 0; i < m->num_packs; i++) {
		if (m->packs[i]) {
			close_pack(m->packs[i]);
			free(m->packs[i]);
			m->packs[i] = NULL;
		}
	}

	munmap((void *)m->data, m->data_len);
	m->data = 0;

	close(m->midx_fd);
	m->midx_fd = -1;

	free(m->packs);
	free(m->pack_names);

	return 1;
}

void close_all_midx(void)
{
	struct midxed_git *m = midxed_git;
	struct midxed_git *next;

	while (m) {
		next = m->next;
		close_midx(m);
		free(m);
		m = next;
	}

	midxed_git = 0;
}
