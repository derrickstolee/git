#include "builtin.h"
#include "config.h"
#include "hex.h"
#include "object-name.h"
#include "object-store.h"
#include "wrapper.h"

static char *create_temp_file(struct object_id *oid)
{
	static char path[50];
	void *buf;
	enum object_type type;
	unsigned long size;
	int fd;

	buf = repo_read_object_file(the_repository, oid, &type, &size);
	if (!buf || type != OBJ_BLOB)
		die("unable to read blob object %s", oid_to_hex(oid));

	xsnprintf(path, sizeof(path), ".merge_file_XXXXXX");
	fd = xmkstemp(path);
	if (write_in_full(fd, buf, size) < 0)
		die_errno("unable to write temp-file");
	close(fd);
	free(buf);
	return path;
}

int cmd_unpack_file(int argc, const char **argv, const char *prefix UNUSED)
{
	struct object_id oid;

	if (argc != 2 || !strcmp(argv[1], "-h"))
		usage("git unpack-file <blob>");
	if (repo_get_oid(the_repository, argv[1], &oid))
		die("Not a valid object name %s", argv[1]);

	prepare_default_config();

	puts(create_temp_file(&oid));
	return 0;
}
