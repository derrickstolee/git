#!/bin/sh

test_description='multi-pack indexes'
. ./test-lib.sh

test_expect_success 'config' \
    'rm -rf .git &&
     mkdir full &&
     cd full &&
     git init &&
     git config core.midx true &&
     git config pack.threads 1 &&
     packdir=.git/objects/pack'

test_expect_success 'write-midx with no packs' \
    'midx0=$(git midx --write) &&
     test "a$midx0" = "a"'

test_expect_success 'create objects' \
    'for i in $(test_seq 100)
     do
         echo $i >file-1-$i
     done &&
     git add file-* &&
     test_tick &&
     git commit -m "test data 1" &&
     git branch commit1 HEAD'

_midx_read_expect() {
	cat >expect <<- EOF
	header: 4d494458 1 1 20 0 5 $1
	num_objects: $2
	chunks: pack_lookup pack_names oid_fanout oid_lookup object_offsets
	pack_names:
	$(ls $3 | grep pack | grep -v idx | sort)
	pack_dir: $3
	EOF
}

_midx_git_two_modes() {
	git -c core.midx=true $1 >output
	git -c core.midx=false $1 >expect
}

_midx_git_behavior() {
	test_expect_success 'check normal git operations' \
	    '_midx_git_two_modes "log --patch master" &&
	     cmp output expect &&
	     _midx_git_two_modes "rev-list --all --objects" &&
	     cmp output expect'
}

test_expect_success 'write-midx from index version 1' \
    'pack1=$(git rev-list --all --objects | git pack-objects --index-version=1 ${packdir}/test-1) &&
     midx1=$(git midx --write) &&
     test_path_is_file ${packdir}/midx-${midx1}.midx &&
     test_path_is_missing ${packdir}/midx-head &&
     _midx_read_expect \
         "1" "102" \
         "${packdir}" &&
     git midx --read --midx-id=${midx1} >output &&
     cmp output expect'

_midx_git_behavior

test_expect_success 'write-midx from index version 2' \
    'rm "${packdir}/test-1-${pack1}.pack" &&
     pack2=$(git rev-list --all --objects | git pack-objects --index-version=2 ${packdir}/test-2) &&
     midx2=$(git midx --write --update-head) &&
     test_path_is_file ${packdir}/midx-${midx2}.midx &&
     test_path_is_file ${packdir}/midx-head &&
     test $(cat ${packdir}/midx-head) = "$midx2" &&
     _midx_read_expect \
         "1" "102" \
         "${packdir}" &&
     git midx --read> output &&
     cmp output expect'

_midx_git_behavior

test_expect_success 'Create more objects' \
    'for i in $(test_seq 100)
     do
         echo extra-$i >file-2-$i
     done &&
     git add file-* &&
     test_tick &&
     git commit -m "test data 2" &&
     git branch commit2 HEAD'

_midx_git_behavior

test_expect_success 'write-midx with two packs' \
    'pack3=$(git rev-list --objects commit2 ^commit1 | git pack-objects --index-version=2 ${packdir}/test-3) &&
     midx3=$(git midx --write --update-head) &&
     test_path_is_file ${packdir}/midx-${midx3}.midx &&
     test_path_is_file ${packdir}/midx-${midx2}.midx &&
     test_path_is_file ${packdir}/midx-head &&
     test $(cat ${packdir}/midx-head) = "$midx3" &&
     _midx_read_expect \
         "2" "204" \
	 "${packdir}" &&
     git midx --read >output &&
     cmp output expect'

_midx_git_behavior

test_expect_success 'Add more packs' \
    'for i in $(test_seq 10)
     do
         iii=$(printf '%03i' $i)
         test-genrandom "bar" 200 > wide_delta_$iii &&
         test-genrandom "baz $iii" 50 >> wide_delta_$iii &&
         test-genrandom "foo"$i 100 > deep_delta_$iii &&
         test-genrandom "foo"$(expr $i + 1) 100 >> deep_delta_$iii &&
         test-genrandom "foo"$(expr $i + 2) 100 >> deep_delta_$iii &&
         echo $iii >file_$iii &&
         test-genrandom "$iii" 8192 >>file_$iii &&
         git update-index --add file_$iii deep_delta_$iii wide_delta_$iii &&
         { echo 101 && test-genrandom 100 8192; } >file_101 &&
         git update-index --add file_101 &&
         tree=$(git write-tree) &&
         commit=$(git commit-tree $tree -p HEAD</dev/null) && {
         echo $tree &&
         git ls-tree $tree | sed -e "s/.* \\([0-9a-f]*\\)	.*/\\1/"
         } >obj-list &&
         git update-ref HEAD $commit &&
         git pack-objects --index-version=2 ${packdir}/test-pack <obj-list
     done'

_midx_git_behavior

test_expect_success 'write-midx with twelve packs' \
    'midx4=$(git midx --write --update-head --delete-expired) &&
     test_path_is_file ${packdir}/midx-${midx4}.midx &&
     test_path_is_missing ${packdir}/midx-${midx3}.midx &&
     test_path_is_file ${packdir}/midx-${midx2}.midx &&
     test_path_is_file ${packdir}/midx-head &&
     test $(cat ${packdir}/midx-head) = "$midx4" &&
     _midx_read_expect \
         "12" "245" \
         "${packdir}" &&
     git midx --read >output &&
     cmp output expect'

_midx_git_behavior

test_expect_success 'write-midx with no new packs' \
    'midx5=$(git midx --write --update-head --delete-expired) &&
     test_path_is_file ${packdir}/midx-${midx5}.midx &&
     test "a$midx4" = "a$midx5" &&
     test_path_is_file ${packdir}/midx-head &&
     test $(cat ${packdir}/midx-head) = "$midx4"'

_midx_git_behavior

test_expect_success 'create bare repo' \
    'cd .. &&
     git clone --bare full bare &&
     cd bare &&
     git config core.midx true &&
     git config pack.threads 1 &&
     baredir=./objects/pack'

test_expect_success 'write-midx in bare repo' \
    'midxbare=$(git midx --write --update-head --delete-expired) &&
     test_path_is_file ${baredir}/midx-${midxbare}.midx  &&
     test_path_is_file ${baredir}/midx-head &&
     test $(cat ${baredir}/midx-head) = "$midxbare" &&
     _midx_read_expect \
         "12" "245" \
         "${baredir}" &&
     git midx --read >output &&
     cmp output expect'

_midx_git_behavior

test_expect_success 'midx --clear' \
    'git midx --clear &&
     test_path_is_missing "${baredir}/midx-${midx4}.midx" &&
     test_path_is_missing "${baredir}/midx-head" &&
     cd ../full &&
     git midx --clear &&
     test_path_is_missing "${packdir}/midx-${midx4}.midx" &&
     test_path_is_missing "${packdir}/midx-head"'

_midx_git_behavior

test_done
