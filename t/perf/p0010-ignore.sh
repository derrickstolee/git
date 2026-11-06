#!/bin/sh

test_description='ignore matching with growing pattern and pathname counts'

. ./perf-lib.sh

test_perf_fresh_repo

test_expect_success 'disable directory caches and external excludes' '
	git config core.untrackedCache false &&
	git config core.fsmonitor false &&
	git config core.excludesFile /dev/null &&
	mkdir .git/ignore-perf &&
	>test_vars
'

make_patterns () {
	awk -v nr="$1" -v kind="$2" '
		BEGIN {
			print "/ignored-sentinel"
			if (kind == "glob-head")
				print "[ab]?*.special"
			for (i = 1; i <= nr; i++) {
				if (kind == "basename")
					printf "absent-%06d.keep\n", i
				else if (kind == "directory")
					printf "/work/absent-%06d/\n", i
				else if (kind == "suffix")
					printf "*.suffix-%06d\n", i
				else
					printf "/absent-%06d.keep\n", i
			}
			if (kind == "glob-tail")
				print "[ab]?*.special"
		}
	' >.gitignore
}

for nr in 1000 2000 4000 8000
do
	test_expect_success "setup $nr paths" '
		mkdir -p work &&
		awk -v nr="$nr" "BEGIN {
			for (i = 1; i <= nr; i++)
				printf \"%06d\\n\", i
		}" >.git/ignore-perf/files &&
		: >.git/ignore-perf/directories &&
		while read name
		do
			>"file-$name.keep" &&
			mkdir -p "work/$name" &&
			>"work/$name/file.keep" &&
			echo "work/$name/" >>.git/ignore-perf/directories ||
			return 1
		done <.git/ignore-perf/files &&
		sed "s|^|file-|; s|$|.keep|" .git/ignore-perf/files >.git/ignore-perf/hit-paths &&
		cp .git/ignore-perf/hit-paths .git/ignore-perf/paths &&
		echo ignored-sentinel >>.git/ignore-perf/paths &&
		echo ignored-sentinel >>.git/ignore-perf/directories &&
		echo ignored-sentinel >.git/ignore-perf/expect &&
		{
			echo .gitignore &&
			echo test_vars &&
			sed "s|^|file-|; s|$|.keep|" .git/ignore-perf/files &&
			sed "s|^|work/|; s|$|/file.keep|" .git/ignore-perf/files
		} >.git/ignore-perf/expect-traversal &&
		sort .git/ignore-perf/expect-traversal >.git/ignore-perf/sorted
	'

	for kind in basename pathname directory suffix glob-head glob-tail
	do
		test_expect_success "setup $kind patterns ($nr)" '
			make_patterns "$nr" "$kind" &&
			if test "$kind" = directory
			then
				input=.git/ignore-perf/directories
			else
				input=.git/ignore-perf/paths
			fi &&
			export input &&
			git check-ignore --no-index --stdin <"$input" >actual &&
			test_cmp .git/ignore-perf/expect actual &&
			rm actual &&
			git ls-files --others --exclude-standard >.git/ignore-perf/actual &&
			test_filter_gitconfig .git/ignore-perf/actual &&
			test_cmp .git/ignore-perf/sorted .git/ignore-perf/actual
		'

		test_perf "check-ignore $kind: $nr patterns, $nr paths" '
			git check-ignore --no-index --stdin <"$input" >/dev/null
		'

		test_perf "ls-files $kind: $nr patterns, $nr root files + $nr directory files" '
			git ls-files --others --exclude-standard >/dev/null
		'
	done
done

for position in early late
do
	test_expect_success "setup $position hit" '
		if test "$position" = early
		then
			echo "/file-*.keep" >.gitignore
		else
			: >.gitignore
		fi &&
		awk "BEGIN {
			for (i = 1; i <= 8000; i++)
				printf \"/absent-%06d\\n\", i
		}" >>.gitignore &&
		if test "$position" = late
		then
			echo "/file-*.keep" >>.gitignore
		fi &&
		git check-ignore --no-index --stdin <.git/ignore-perf/hit-paths >.git/ignore-perf/actual &&
		test_cmp .git/ignore-perf/hit-paths .git/ignore-perf/actual
	'

	test_perf "check-ignore $position hit: 8000 rules, 8000 paths" '
		git check-ignore --no-index --stdin <.git/ignore-perf/hit-paths >/dev/null
	'
done

test_expect_success 'setup directory pruning and retained sibling' '
	mkdir work-retained &&
	>work-retained/file.keep &&
	echo /work/ >.gitignore &&
	{
		echo .gitignore &&
		echo test_vars &&
		sed "s|^|file-|; s|$|.keep|" .git/ignore-perf/files &&
		echo work-retained/file.keep
	} >.git/ignore-perf/expect-pruned &&
	sort .git/ignore-perf/expect-pruned >.git/ignore-perf/sorted &&
	git ls-files --others --exclude-standard >.git/ignore-perf/actual &&
	test_filter_gitconfig .git/ignore-perf/actual &&
	test_cmp .git/ignore-perf/sorted .git/ignore-perf/actual
'

test_perf 'ls-files: prune 8000 directories, retain sibling' '
	git ls-files --others --exclude-standard >/dev/null
'

if test_bool_env GIT_PERF_EXTRA false
then
	for nr in 1000 2000 4000 8000
	do
		test_expect_success "fixed-axis setup ($nr)" '
			make_patterns 4000 pathname &&
			head -n "$nr" .git/ignore-perf/hit-paths >.git/ignore-perf/axis &&
			echo ignored-sentinel >>.git/ignore-perf/axis
		'

		test_perf "check-ignore: 4000 rules, $nr paths" '
			git check-ignore --no-index --stdin <.git/ignore-perf/axis >/dev/null
		'

		test_expect_success "vary patterns ($nr)" '
			make_patterns "$nr" pathname &&
			head -n 4000 .git/ignore-perf/hit-paths >.git/ignore-perf/axis &&
			echo ignored-sentinel >>.git/ignore-perf/axis
		'

		test_perf "check-ignore: $nr rules, 4000 paths" '
			git check-ignore --no-index --stdin <.git/ignore-perf/axis >/dev/null
		'
	done
fi

test_done
