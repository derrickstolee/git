#ifndef PATHSPEC_H
#define PATHSPEC_H

struct index_state;

/* Pathspec magic */
#define PATHSPEC_FROMTOP	(1<<0)
#define PATHSPEC_MAXDEPTH	(1<<1)
#define PATHSPEC_LITERAL	(1<<2)
#define PATHSPEC_GLOB		(1<<3)
#define PATHSPEC_ICASE		(1<<4)
#define PATHSPEC_EXCLUDE	(1<<5)
#define PATHSPEC_ATTR		(1<<6)
#define PATHSPEC_ALL_MAGIC	  \
	(PATHSPEC_FROMTOP	| \
	 PATHSPEC_MAXDEPTH	| \
	 PATHSPEC_LITERAL	| \
	 PATHSPEC_GLOB		| \
	 PATHSPEC_ICASE		| \
	 PATHSPEC_EXCLUDE	| \
	 PATHSPEC_ATTR)

#define PATHSPEC_ONESTAR 1	/* the pathspec pattern satisfies GFNM_ONESTAR */

/**
 * See glossary-context.txt for the syntax of pathspec.
 * In memory, a pathspec set is represented by "struct pathspec" and is
 * prepared by parse_pathspec().
 */
struct pathspec {
	int nr;
	unsigned int has_wildcard:1;
	unsigned int recursive:1;
	unsigned int recurse_submodules:1;
	unsigned int can_use_modified_path_bloom_filters:1;
	unsigned magic;
	int max_depth;
	struct pathspec_item {
		char *match;
		char *original;
		unsigned magic;
		int len, prefix;
		int nowildcard_len;
		int flags;
		int attr_match_nr;
		struct attr_match {
			char *value;
			enum attr_match_mode {
				MATCH_SET,
				MATCH_UNSET,
				MATCH_VALUE,
				MATCH_UNSPECIFIED
			} match_mode;
		} *attr_match;
		struct attr_check *attr_check;

		/*
		 * These fields are only relevant during pathspec-limited
		 * revision walks, init_pathspec_item() leaves them
		 * zero-initialized, but copy_pathspec() copies them,
		 * and clear_pathspec() releases the allocated memory.
		 * IOW: they are only valid if 'struct pathspec's
		 * 'can_use_modified_path_bloom_filters' bit is set.
		 */
		uint32_t modified_path_bloom_hashes_nr;
		uint32_t *modified_path_bloom_hashes;
	} *items;
};

#define GUARD_PATHSPEC(ps, mask) \
	do { \
		if ((ps)->magic & ~(mask))	       \
			die("BUG:%s:%d: unsupported magic %x",	\
			    __FILE__, __LINE__, (ps)->magic & ~(mask)); \
	} while (0)

/* parse_pathspec flags */
#define PATHSPEC_PREFER_CWD (1<<0) /* No args means match cwd */
#define PATHSPEC_PREFER_FULL (1<<1) /* No args means match everything */
#define PATHSPEC_MAXDEPTH_VALID (1<<2) /* max_depth field is valid */
/* die if a symlink is part of the given path's directory */
#define PATHSPEC_SYMLINK_LEADING_PATH (1<<3)
#define PATHSPEC_PREFIX_ORIGIN (1<<4)
#define PATHSPEC_KEEP_ORDER (1<<5)
/*
 * For the callers that just need pure paths from somewhere else, not
 * from command line. Global --*-pathspecs options are ignored. No
 * magic is parsed in each pathspec either. If PATHSPEC_LITERAL is
 * allowed, then it will automatically set for every pathspec.
 */
#define PATHSPEC_LITERAL_PATH (1<<6)

/**
 * Given command line arguments and a prefix, convert the input to
 * pathspec. die() if any magic in magic_mask is used.
 *
 * Any arguments used are copied. It is safe for the caller to modify
 * or free 'prefix' and 'args' after calling this function.
 *
 * - magic_mask specifies what features that are NOT supported by the following
 * code. If a user attempts to use such a feature, parse_pathspec() can reject
 * it early.
 *
 * - flags specifies other things that the caller wants parse_pathspec to
 * perform.
 *
 * - prefix and args come from cmd_* functions
 *
 * parse_pathspec() helps catch unsupported features and reject them politely.
 * At a lower level, different pathspec-related functions may not support the
 * same set of features. Such pathspec-sensitive functions are guarded with
 * GUARD_PATHSPEC(), which will die in an unfriendly way when an unsupported
 * feature is requested.
 *
 * The command designers are supposed to make sure that GUARD_PATHSPEC() never
 * dies. They have to make sure all unsupported features are caught by
 * parse_pathspec(), not by GUARD_PATHSPEC. grepping GUARD_PATHSPEC() should
 * give the designers all pathspec-sensitive codepaths and what features they
 * support.
 *
 * A similar process is applied when a new pathspec magic is added. The designer
 * lifts the GUARD_PATHSPEC restriction in the functions that support the new
 * magic. At the same time (s)he has to make sure this new feature will be
 * caught at parse_pathspec() in commands that cannot handle the new magic in
 * some cases. grepping parse_pathspec() should help.
 */
void parse_pathspec(struct pathspec *pathspec,
		    unsigned magic_mask,
		    unsigned flags,
		    const char *prefix,
		    const char **args);
/*
 * Same as parse_pathspec() but uses file as input.
 * When 'file' is exactly "-" it uses 'stdin' instead.
 */
void parse_pathspec_file(struct pathspec *pathspec,
			 unsigned magic_mask,
			 unsigned flags,
			 const char *prefix,
			 const char *file,
			 int nul_term_line);

void copy_pathspec(struct pathspec *dst, const struct pathspec *src);
void clear_pathspec(struct pathspec *);

static inline int ps_strncmp(const struct pathspec_item *item,
			     const char *s1, const char *s2, size_t n)
{
	if (item->magic & PATHSPEC_ICASE)
		return strncasecmp(s1, s2, n);
	else
		return strncmp(s1, s2, n);
}

static inline int ps_strcmp(const struct pathspec_item *item,
			    const char *s1, const char *s2)
{
	if (item->magic & PATHSPEC_ICASE)
		return strcasecmp(s1, s2);
	else
		return strcmp(s1, s2);
}

void add_pathspec_matches_against_index(const struct pathspec *pathspec,
					const struct index_state *istate,
					char *seen);
char *find_pathspecs_matching_against_index(const struct pathspec *pathspec,
					    const struct index_state *istate);
int match_pathspec_attrs(const struct index_state *istate,
			 const char *name, int namelen,
			 const struct pathspec_item *item);

#endif /* PATHSPEC_H */
