#define USE_THE_REPOSITORY_VARIABLE

#include "test-tool.h"
#include "dir.h"
#include "repository.h"
#include "setup.h"
#include "strbuf.h"

static void add_padding(struct pattern_list *pl)
{
	struct strbuf pattern = STRBUF_INIT;
	int i;

	for (i = 0; i < 64; i++) {
		strbuf_reset(&pattern);
		strbuf_addf(&pattern, "/unused-%d", i);
		add_pattern(pattern.buf, "", 0, pl, i + 1);
	}
	strbuf_release(&pattern);
}

static int check_target(struct pattern_list *pl, const char *path,
			enum pattern_match_result expected)
{
	const char *basename = strrchr(path, '/');
	int dtype = DT_REG;

	basename = basename ? basename + 1 : path;
	if (path_matches_pattern_list(path, strlen(path), basename, &dtype, pl, NULL) != expected)
		return error("unexpected matching result after index invalidation");
	if (!pl->ignore_index)
		return error("query did not build an index");
	return 0;
}

static int check_lifetime(const char *positive, const char *negative, const char *path)
{
	struct pattern_list pl = { 0 };
	int result = 1;

	add_padding(&pl);
	add_pattern(positive, "", 0, &pl, 65);
	if (check_target(&pl, path, MATCHED))
		goto out;
	add_pattern(negative, "", 0, &pl, 66);
	if (pl.ignore_index || check_target(&pl, path, NOT_MATCHED))
		goto out;
	clear_pattern_list(&pl);
	add_padding(&pl);
	add_pattern(positive, "", 0, &pl, 65);
	if (check_target(&pl, path, MATCHED))
		goto out;
	result = 0;
out:
	clear_pattern_list(&pl);
	return result;
}

int cmd__ignore_patterns(int argc UNUSED, const char **argv UNUSED)
{
	setup_git_directory(the_repository);
	return check_lifetime("/target", "!/target", "target") ||
		check_lifetime("*get", "!*get", "target") ||
		check_lifetime("/parent/**", "!/parent/**", "parent/target");
}
