#!/bin/sh

test_description='compare behavior among full checkout, sparse-checkout, sparse-index'

GIT_TEST_CHECK_CACHE_TREE=0
GIT_TEST_SPLIT_INDEX=0

. ./test-lib.sh

test_expect_success 'setup' '
	git init initial-repo &&
	(
		cd initial-repo &&
		echo "initial" >a &&
		mkdir folder1 folder2 deep &&
		mkdir deep/deeper1 deep/deeper2 &&
		mkdir deep/deeper1/deepest &&
		cp a folder1 &&
		cp a folder2 &&
		cp a deep &&
		cp a deep/deeper1 &&
		cp a deep/deeper2 &&
		cp a deep/deeper1/deepest &&
		git add . &&
		git commit -m "initial commit" &&
		git checkout -b base &&
		for dir in folder1 folder2 deep
		do
			git checkout -b update-$dir &&
			echo "updated $dir" >$dir/a &&
			git commit -a -m "update $dir" || return 1
		done &&

		git checkout -b rename-base base &&
		echo >folder1/larger-content <<-\EOF &&
		matching
		lines
		help
		inexact
		renames
		EOF
		cp folder1/larger-content folder2/ &&
		cp folder1/larger-content deep/deeper1/ &&
		git add . &&
		git commit -m "add interesting rename content" &&

		git checkout -b rename-out-to-out rename-base &&
		mv folder1/a folder2/b &&
		mv folder1/larger-content folder2/edited-content &&
		echo >>folder2/edited-content &&
		git add . &&
		git commit -m "rename folder1/... to folder2/..." &&

		git checkout -b rename-out-to-in rename-base &&
		mv folder1/a deep/deeper1/b &&
		mv folder1/larger-content deep/deeper1/edited-content &&
		echo >>deep/deeper1/edited-content &&
		git add . &&
		git commit -m "rename folder1/... to deep/deeper1/..." &&

		git checkout -b rename-in-to-out rename-base &&
		mv deep/deeper1/a folder1/b &&
		mv deep/deeper1/larger-content folder1/edited-content &&
		echo >>folder1/edited-content &&
		git add . &&
		git commit -m "rename deep/deeper1/... to folder1/..." &&

		git checkout -b deepest base &&
		echo "updated deepest" >deep/deeper1/deepest/a &&
		git commit -a -m "update deepest" &&

		git checkout -f base &&
		git reset --hard
	)
'

init_repos () {
	rm -rf full-checkout sparse-checkout sparse-index &&

	# create repos in initial state
	cp -r initial-repo full-checkout &&
	git -C full-checkout reset --hard &&

	cp -r initial-repo sparse-checkout &&
	git -C sparse-checkout reset --hard &&
	git -C sparse-checkout sparse-checkout init --cone &&

	cp -r initial-repo sparse-index &&
	git -C sparse-index reset --hard &&
	GIT_TEST_SPARSE_INDEX=1 git -C sparse-index sparse-checkout init --cone &&

	# initialize sparse-checkout definitions
	git -C sparse-checkout sparse-checkout set deep &&
	GIT_TEST_SPARSE_INDEX=1 git -C sparse-index sparse-checkout set deep 2>sparse-index-set-err
}

run_on_sparse () {
	(
		cd sparse-checkout &&
		$* >../sparse-checkout-out 2>../sparse-checkout-err
	) &&
	(
		cd sparse-index &&
		GIT_TEST_SPARSE_INDEX=1 $* >../sparse-index-out 2>../sparse-index-err
	)
}

run_on_all () {
	(
		cd full-checkout &&
		$* >../full-checkout-out 2>../full-checkout-err
	) &&
	run_on_sparse $*
}

test_all_match () {
	run_on_all $* &&
	test_cmp full-checkout-out sparse-checkout-out &&
	test_cmp full-checkout-out sparse-index-out &&
	test_cmp full-checkout-err sparse-checkout-err &&
	test_cmp full-checkout-err sparse-index-err
}

test_sparse_match () {
	run_on_sparse $* &&
	test_cmp sparse-checkout-out sparse-index-out &&
	test_cmp sparse-checkout-err sparse-index-err
}

test_expect_success 'status with options' '
	init_repos &&
	test_sparse_match ls &&
	test_all_match git status --porcelain=v2 &&
	test_all_match git status --porcelain=v2 -z -u &&
	test_all_match git status --porcelain=v2 -uno &&
	run_on_all "touch README.md" &&
	test_all_match git status --porcelain=v2 &&
	test_all_match git status --porcelain=v2 -z -u &&
	test_all_match git status --porcelain=v2 -uno &&
	test_all_match git add README.md &&
	test_all_match git status --porcelain=v2 &&
	test_all_match git status --porcelain=v2 -z -u &&
	test_all_match git status --porcelain=v2 -uno
'

test_expect_success 'add, commit, checkout' '
	init_repos &&

	write_script edit-contents <<-\EOF &&
	echo text >>README.md
	EOF
	run_on_all "../edit-contents" &&

	test_all_match git add README.md &&
	test_all_match git status --porcelain=v2 &&
	test_all_match git commit -m "Add README.md" &&

	test_all_match git checkout HEAD~1 &&
	test_all_match git checkout - &&

	run_on_all "../edit-contents" &&

	export GIT_TRACE2_PERF="$(pwd)/add-trace" &&
	test_all_match git add -A &&
	test_all_match git status --porcelain=v2 &&
	test_all_match git commit -m "Extend README.md" &&

	test_all_match git checkout HEAD~1 &&
	test_all_match git checkout -
'

test_expect_success 'add, commit, checkout' '
	init_repos &&

	write_script edit-contents <<-\EOF &&
	echo text >>$1
	EOF
	run_on_all "../edit-contents README.md" &&

	test_all_match git add README.md &&
	test_all_match git status --porcelain=v2 &&
	test_all_match git commit -m "Add README.md" &&

	test_all_match git checkout HEAD~1 &&
	test_all_match git checkout - &&

	run_on_all "../edit-contents README.md" &&

	test_all_match git add -A &&
	test_all_match git status --porcelain=v2 &&
	test_all_match git commit -m "Extend README.md" &&

	test_all_match git checkout HEAD~1 &&
	test_all_match git checkout - &&

	run_on_all "../edit-contents deep/newfile" &&

	test_all_match git status --porcelain=v2 -uno &&
	test_all_match git status --porcelain=v2 &&
	test_all_match git add . &&
	test_all_match git status --porcelain=v2 &&
	test_all_match git commit -m "add deep/newfile" &&

	test_all_match git checkout HEAD~1 &&
	test_all_match git checkout -
'

test_expect_success 'checkout and reset --hard' '
	init_repos &&

	test_all_match git checkout update-folder1 &&
	test_sparse_match git status --porcelain=v2 &&

	test_all_match git checkout update-deep &&
	test_sparse_match git status --porcelain=v2 &&

	test_all_match git checkout -b reset-test &&
	test_all_match git reset --hard deepest &&
	test_all_match git reset --hard update-folder1 &&
	test_all_match git reset --hard update-folder2
'

test_expect_success 'diff --staged' '
	init_repos &&

	write_script edit-contents <<-\EOF &&
	echo text >>README.md
	EOF
	run_on_all "../edit-contents" &&

	test_all_match git diff &&
	test_all_match git diff --staged &&
	test_all_match git add README.md &&
	test_all_match git diff &&
	test_all_match git diff --staged
'

# TODO: this fails for sparse-index
test_expect_failure 'diff with renames' '
	init_repos &&

	for branch in rename-out-to-out rename-out-to-in rename-in-to-out
	do
		test_all_match git checkout rename-base &&
		test_all_match git checkout $branch -- .&&
		test_all_match git diff --staged &&
		test_all_match git diff --staged --find-renames || return 1
	done
'

test_expect_success 'log with pathspec outside sparse definition' '
	init_repos &&

	test_all_match git log -- a &&
	test_all_match git log -- folder1/a &&
	test_all_match git log -- folder2/a &&
	test_all_match git log -- deep/a &&
	test_all_match git log -- deep/deeper1/a &&
	test_all_match git log -- deep/deeper1/deepest/a &&

	test_all_match git checkout update-folder1 &&
	test_all_match git log -- folder1/a
'

# TODO: blame currently does not support blaming files outside of the
# sparse definition. It complains that the file doesn't exist locally.
test_expect_success 'blame with pathspec inside sparse definition' '
	init_repos &&

	test_all_match git blame a &&
	test_all_match git blame deep/a &&
	test_all_match git blame deep/deeper1/a &&
	test_all_match git blame deep/deeper1/deepest/a
'

test_expect_success 'checkout and reset (mixed)' '
	init_repos &&

	test_all_match git checkout -b reset-test update-deep &&
	test_sparse_match git reset deepest &&
	test_sparse_match git reset update-folder1 &&
	test_sparse_match git reset update-folder2
'

test_expect_success 'merge' '
	init_repos &&

	test_all_match git checkout -b merge update-deep &&
	test_all_match git merge -m "folder1" update-folder1 &&
	test_all_match git rev-parse HEAD^{tree} &&
	test_all_match git merge -m "folder2" update-folder2 &&
	test_all_match git rev-parse HEAD^{tree}
'

# TODO: failing with sparse-index
test_expect_failure 'merge with outside renames' '
	init_repos &&

	for type in out-to-out out-to-in in-to-out
	do
		test_all_match git checkout -f -b merge-$type update-deep &&
		test_all_match git merge -m "$type" rename-$type &&
		test_all_match git rev-parse HEAD^{tree} || return 1
	done
'

test_expect_success 'clean' '
	init_repos &&

	echo bogus >>.gitignore &&
	run_on_all cp ../.gitignore . &&
	test_all_match git add .gitignore &&
	test_all_match git commit -m ignore-bogus-files &&

	run_on_sparse mkdir folder1 &&
	run_on_all touch folder1/bogus &&

	test_all_match git status --porcelain=v2 &&
	test_all_match git clean -f &&
	test_all_match git status --porcelain=v2 &&
	test_sparse_match ls &&
	test_sparse_match ls folder1 &&

	test_all_match git clean -xf &&
	test_all_match git status --porcelain=v2 &&
	test_sparse_match ls &&
	test_sparse_match ls folder1 &&

	test_all_match git clean -xdf &&
	test_all_match git status --porcelain=v2 &&
	test_sparse_match ls &&

	# perhaps this is unexpected? at least it is consistent
	test_path_is_dir sparse-checkout/folder1 &&
	test_path_is_dir sparse-index/folder1
'

test_done
