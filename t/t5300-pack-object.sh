#!/bin/sh
#
# Copyright (c) 2005 Junio C Hamano
#

test_description='git pack-object'

. ./test-lib.sh

test_expect_success 'setup' '
	rm -f .git/index* &&
	test-tool genzeros 4096 | tr "\000" "a" >a &&
	test-tool genzeros 4096 | tr "\000" "b" >b &&
	test-tool genzeros 4096 | tr "\000" "c" >c &&
	test-tool genrandom "seed a" 2097152 >a_big &&
	test-tool genrandom "seed b" 2097152 >b_big &&
	git update-index --add a a_big b b_big c &&
	cat c >d && echo foo >>d && git update-index --add d &&
	tree=$(git write-tree) &&
	commit=$(git commit-tree $tree </dev/null) &&
	{
		echo $tree &&
		echo $commit &&
		git ls-tree $tree | sed -e "s/.* \\([0-9a-f]*\\)	.*/\\1/"
	} >obj-list &&
	{
		git diff-tree --root -p $commit &&
		while read object
		do
			t=$(git cat-file -t $object) &&
			git cat-file $t $object || return 1
		done <obj-list
	} >expect
'

test_expect_success 'pack-object traces bytes written to stdout' '
	test_when_finished "rm -f pack.trace pack.pack" &&
	GIT_TRACE2_EVENT="$PWD/pack.trace" \
		git pack-objects --quiet --revs --stdout >pack.pack <<-EOF &&
	$commit
	EOF
	bytes=$(test_file_size pack.pack) &&
	test_grep "\"key\":\"write_pack_file/wrote_bytes\",\"value\":\"$bytes\"" pack.trace
'

test_expect_success 'pack-object traces bytes written to split pack files' '
	test_when_finished "rm -f split.trace traced-pack-*" &&
	GIT_TRACE2_EVENT="$PWD/split.trace" \
		git -c pack.packSizeLimit=3m pack-objects --quiet traced-pack <obj-list &&
	test 2 = $(ls traced-pack-*.pack | wc -l) &&
	bytes=0 &&
	for pack in traced-pack-*.pack
	do
		pack_size=$(test_file_size "$pack") &&
		bytes=$((bytes + pack_size)) || return 1
	done &&
	test_grep "\"key\":\"write_pack_file/wrote_bytes\",\"value\":\"$bytes\"" split.trace
'

test_expect_success 'setup pack-object <stdin' '
	git init pack-object-stdin &&
	test_commit -C pack-object-stdin one &&
	test_commit -C pack-object-stdin two

'

test_expect_success 'pack-object <stdin parsing: basic [|--revs]' '
	cat >in <<-EOF &&
	$(git -C pack-object-stdin rev-parse one)
	EOF

	git -C pack-object-stdin pack-objects basic-stdin <in &&
	idx=$(echo pack-object-stdin/basic-stdin-*.idx) &&
	git show-index <"$idx" >actual &&
	test_line_count = 1 actual &&

	git -C pack-object-stdin pack-objects --revs basic-stdin-revs <in &&
	idx=$(echo pack-object-stdin/basic-stdin-revs-*.idx) &&
	git show-index <"$idx" >actual &&
	test_line_count = 3 actual
'

test_expect_success 'pack-object <stdin parsing: [|--revs] bad line' '
	cat >in <<-EOF &&
	$(git -C pack-object-stdin rev-parse one)
	garbage
	$(git -C pack-object-stdin rev-parse two)
	EOF

	sed "s/^> //g" >err.expect <<-EOF &&
	fatal: expected object ID, got garbage:
	>  garbage

	EOF
	test_must_fail git -C pack-object-stdin pack-objects bad-line-stdin <in 2>err.actual &&
	test_cmp err.expect err.actual &&

	cat >err.expect <<-EOF &&
	fatal: bad revision '"'"'garbage'"'"'
	EOF
	test_must_fail git -C pack-object-stdin pack-objects --revs bad-line-stdin-revs <in 2>err.actual &&
	test_cmp err.expect err.actual
'

test_expect_success 'pack-object <stdin parsing: [|--revs] empty line' '
	cat >in <<-EOF &&
	$(git -C pack-object-stdin rev-parse one)

	$(git -C pack-object-stdin rev-parse two)
	EOF

	sed -e "s/^> //g" -e "s/Z$//g" >err.expect <<-EOF &&
	fatal: expected object ID, got garbage:
	>  Z

	EOF
	test_must_fail git -C pack-object-stdin pack-objects empty-line-stdin <in 2>err.actual &&
	test_cmp err.expect err.actual &&

	git -C pack-object-stdin pack-objects --revs empty-line-stdin-revs <in &&
	idx=$(echo pack-object-stdin/empty-line-stdin-revs-*.idx) &&
	git show-index <"$idx" >actual &&
	test_line_count = 3 actual
'

test_expect_success 'pack-object <stdin parsing: [|--revs] with --stdin' '
	cat >in <<-EOF &&
	$(git -C pack-object-stdin rev-parse one)
	$(git -C pack-object-stdin rev-parse two)
	EOF

	# There is the "--stdin-packs is incompatible with --revs"
	# test below, but we should make sure that the revision.c
	# --stdin is not picked up
	cat >err.expect <<-EOF &&
	fatal: disallowed abbreviated or ambiguous option '"'"'stdin'"'"'
	EOF
	test_must_fail git -C pack-object-stdin pack-objects stdin-with-stdin-option --stdin <in 2>err.actual &&
	test_cmp err.expect err.actual &&

	test_must_fail git -C pack-object-stdin pack-objects --stdin --revs stdin-with-stdin-option-revs 2>err.actual <in &&
	test_cmp err.expect err.actual
'

test_expect_success 'pack-object <stdin parsing: --stdin-packs handles garbage' '
	cat >in <<-EOF &&
	$(git -C pack-object-stdin rev-parse one)
	$(git -C pack-object-stdin rev-parse two)
	EOF

	# That we get "two" and not "one" has to do with OID
	# ordering. It happens to be the same here under SHA-1 and
	# SHA-256. See commentary in pack-objects.c
	cat >err.expect <<-EOF &&
	fatal: could not find pack '"'"'$(git -C pack-object-stdin rev-parse two)'"'"'
	EOF
	test_must_fail git \
		-C pack-object-stdin \
		pack-objects stdin-with-stdin-option --stdin-packs \
		<in 2>err.actual &&
	test_cmp err.expect err.actual
'

# usage: check_deltas <stderr_from_pack_objects> <cmp_op> <nr_deltas>
# e.g.: check_deltas stderr -gt 0
check_deltas() {
	deltas=$(sed -n 's/Total [0-9][0-9]* (delta \([0-9][0-9]*\)).*/\1/p' "$1") &&
	shift &&
	if ! test "$deltas" "$@"
	then
		echo >&2 "unexpected number of deltas (compared $delta $*)"
		return 1
	fi
}

test_expect_success 'pack without delta' '
	packname_1=$(git pack-objects --progress --window=0 test-1 \
			<obj-list 2>stderr) &&
	check_deltas stderr = 0
'

test_expect_success 'negative window clamps to 0' '
	git pack-objects --progress --window=-1 neg-window <obj-list 2>stderr &&
	check_deltas stderr = 0
'

test_expect_success 'pack-objects with bogus arguments' '
	test_must_fail git pack-objects --window=0 test-1 blah blah <obj-list
'

check_unpack () {
	local packname="$1" &&
	local object_list="$2" &&
	local git_config="$3" &&
	test_when_finished "rm -rf git2" &&
	git $git_config init --bare git2 &&
	(
		git $git_config -C git2 unpack-objects -n <"$packname".pack &&
		git $git_config -C git2 unpack-objects <"$packname".pack &&
		git $git_config -C git2 cat-file --batch-check="%(objectname)"
	) <"$object_list" >current &&
	cmp "$object_list" current
}

test_expect_success 'unpack without delta' '
	check_unpack test-1-${packname_1} obj-list
'

BATCH_CONFIGURATION='-c core.fsync=loose-object -c core.fsyncmethod=batch'

test_expect_success 'unpack without delta (core.fsyncmethod=batch)' '
	check_unpack test-1-${packname_1} obj-list "$BATCH_CONFIGURATION"
'

test_expect_success 'pack with REF_DELTA' '
	packname_2=$(git pack-objects --progress test-2 <obj-list 2>stderr) &&
	check_deltas stderr -gt 0
'

test_expect_success 'unpack with REF_DELTA' '
	check_unpack test-2-${packname_2} obj-list
'

test_expect_success 'unpack with REF_DELTA (core.fsyncmethod=batch)' '
       check_unpack test-2-${packname_2} obj-list "$BATCH_CONFIGURATION"
'

test_expect_success 'pack with OFS_DELTA' '
	packname_3=$(git pack-objects --progress --delta-base-offset test-3 \
			<obj-list 2>stderr) &&
	check_deltas stderr -gt 0
'

test_expect_success 'unpack with OFS_DELTA' '
	check_unpack test-3-${packname_3} obj-list
'

test_expect_success 'unpack with OFS_DELTA (core.fsyncmethod=batch)' '
	check_unpack test-3-${packname_3} obj-list "$BATCH_CONFIGURATION"
'

test_expect_success PERL_TEST_HELPERS 'compare delta flavors' '
	perl -e '\''
		defined($_ = -s $_) or die for @ARGV;
		exit 1 if $ARGV[0] <= $ARGV[1];
	'\'' test-2-$packname_2.pack test-3-$packname_3.pack
'

check_use_objects () {
	test_when_finished "rm -rf git2" &&
	git init --bare git2 &&
	cp "$1".pack "$1".idx git2/objects/pack &&
	(
		cd git2 &&
		git diff-tree --root -p $commit &&
		while read object
		do
			t=$(git cat-file -t $object) &&
			git cat-file $t $object || exit 1
		done
	) <obj-list >current &&
	cmp expect current
}

test_expect_success 'use packed objects' '
	check_use_objects test-1-${packname_1}
'

test_expect_success 'use packed deltified (REF_DELTA) objects' '
	check_use_objects test-2-${packname_2}
'

test_expect_success 'use packed deltified (OFS_DELTA) objects' '
	check_use_objects test-3-${packname_3}
'

test_expect_success 'survive missing objects/pack directory' '
	(
		rm -fr missing-pack &&
		mkdir missing-pack &&
		cd missing-pack &&
		git init &&
		GOP=.git/objects/pack &&
		rm -fr $GOP &&
		git index-pack --stdin --keep=test <../test-3-${packname_3}.pack &&
		test -f $GOP/pack-${packname_3}.pack &&
		cmp $GOP/pack-${packname_3}.pack ../test-3-${packname_3}.pack &&
		test -f $GOP/pack-${packname_3}.idx &&
		cmp $GOP/pack-${packname_3}.idx ../test-3-${packname_3}.idx &&
		test -f $GOP/pack-${packname_3}.keep
	)
'

test_expect_success 'verify pack' '
	git verify-pack test-1-${packname_1}.idx \
		test-2-${packname_2}.idx \
		test-3-${packname_3}.idx
'

test_expect_success 'verify pack -v' '
	git verify-pack -v test-1-${packname_1}.idx \
		test-2-${packname_2}.idx \
		test-3-${packname_3}.idx
'

test_expect_success 'verify-pack catches mismatched .idx and .pack files' '
	cat test-1-${packname_1}.idx >test-3.idx &&
	cat test-2-${packname_2}.pack >test-3.pack &&
	if git verify-pack test-3.idx
	then false
	else :;
	fi
'

test_expect_success 'verify-pack catches a corrupted pack signature' '
	cat test-1-${packname_1}.pack >test-3.pack &&
	echo | dd of=test-3.pack count=1 bs=1 conv=notrunc seek=2 &&
	if git verify-pack test-3.idx
	then false
	else :;
	fi
'

test_expect_success 'verify-pack catches a corrupted pack version' '
	cat test-1-${packname_1}.pack >test-3.pack &&
	echo | dd of=test-3.pack count=1 bs=1 conv=notrunc seek=7 &&
	if git verify-pack test-3.idx
	then false
	else :;
	fi
'

test_expect_success 'verify-pack catches a corrupted type/size of the 1st packed object data' '
	cat test-1-${packname_1}.pack >test-3.pack &&
	echo | dd of=test-3.pack count=1 bs=1 conv=notrunc seek=12 &&
	if git verify-pack test-3.idx
	then false
	else :;
	fi
'

test_expect_success 'verify-pack catches a corrupted sum of the index file itself' '
	l=$(wc -c <test-3.idx) &&
	l=$(expr $l - 20) &&
	cat test-1-${packname_1}.pack >test-3.pack &&
	printf "%20s" "" | dd of=test-3.idx count=20 bs=1 conv=notrunc seek=$l &&
	if git verify-pack test-3.pack
	then false
	else :;
	fi
'

test_expect_success 'build pack index for an existing pack' '
	cat test-1-${packname_1}.pack >test-3.pack &&
	git index-pack -o tmp.idx test-3.pack &&
	cmp tmp.idx test-1-${packname_1}.idx &&

	git index-pack test-3.pack &&
	cmp test-3.idx test-1-${packname_1}.idx &&

	cat test-2-${packname_2}.pack >test-3.pack &&
	git index-pack -o tmp.idx test-2-${packname_2}.pack &&
	cmp tmp.idx test-2-${packname_2}.idx &&

	git index-pack test-3.pack &&
	cmp test-3.idx test-2-${packname_2}.idx &&

	cat test-3-${packname_3}.pack >test-3.pack &&
	git index-pack -o tmp.idx test-3-${packname_3}.pack &&
	cmp tmp.idx test-3-${packname_3}.idx &&

	git index-pack test-3.pack &&
	cmp test-3.idx test-3-${packname_3}.idx &&

	cat test-1-${packname_1}.pack >test-4.pack &&
	rm -f test-4.keep &&
	git index-pack --keep=why test-4.pack &&
	cmp test-1-${packname_1}.idx test-4.idx &&
	test -f test-4.keep &&

	:
'

test_expect_success 'unpacking with --strict' '

	for j in a b c d e f g
	do
		for i in 0 1 2 3 4 5 6 7 8 9
		do
			o=$(echo $j$i | git hash-object -w --stdin) &&
			echo "100644 $o	0 $j$i" || return 1
		done
	done >LIST &&
	rm -f .git/index &&
	git update-index --index-info <LIST &&
	LIST=$(git write-tree) &&
	rm -f .git/index &&
	head -n 10 LIST | git update-index --index-info &&
	LI=$(git write-tree) &&
	rm -f .git/index &&
	tail -n 10 LIST | git update-index --index-info &&
	ST=$(git write-tree) &&
	git rev-list --objects "$LIST" "$LI" "$ST" >actual &&
	PACK5=$( git pack-objects test-5 <actual ) &&
	PACK6=$( test_write_lines "$LIST" "$LI" "$ST" | git pack-objects test-6 ) &&
	test_create_repo test-5 &&
	(
		cd test-5 &&
		git unpack-objects --strict <../test-5-$PACK5.pack &&
		git ls-tree -r $LIST &&
		git ls-tree -r $LI &&
		git ls-tree -r $ST
	) &&
	test_create_repo test-6 &&
	(
		# tree-only into empty repo -- many unreachables
		cd test-6 &&
		test_must_fail git unpack-objects --strict <../test-6-$PACK6.pack
	) &&
	(
		# already populated -- no unreachables
		cd test-5 &&
		git unpack-objects --strict <../test-6-$PACK6.pack
	)
'

test_expect_success 'index-pack with --strict' '

	for j in a b c d e f g
	do
		for i in 0 1 2 3 4 5 6 7 8 9
		do
			o=$(echo $j$i | git hash-object -w --stdin) &&
			echo "100644 $o	0 $j$i" || return 1
		done
	done >LIST &&
	rm -f .git/index &&
	git update-index --index-info <LIST &&
	LIST=$(git write-tree) &&
	rm -f .git/index &&
	head -n 10 LIST | git update-index --index-info &&
	LI=$(git write-tree) &&
	rm -f .git/index &&
	tail -n 10 LIST | git update-index --index-info &&
	ST=$(git write-tree) &&
	git rev-list --objects "$LIST" "$LI" "$ST" >actual &&
	PACK5=$( git pack-objects test-5 <actual ) &&
	PACK6=$( test_write_lines "$LIST" "$LI" "$ST" | git pack-objects test-6 ) &&
	test_create_repo test-7 &&
	(
		cd test-7 &&
		git index-pack --strict --stdin <../test-5-$PACK5.pack &&
		git ls-tree -r $LIST &&
		git ls-tree -r $LI &&
		git ls-tree -r $ST
	) &&
	test_create_repo test-8 &&
	(
		# tree-only into empty repo -- many unreachables
		cd test-8 &&
		test_must_fail git index-pack --strict --stdin <../test-6-$PACK6.pack
	) &&
	(
		# already populated -- no unreachables
		cd test-7 &&
		git index-pack --strict --stdin <../test-6-$PACK6.pack
	)
'

test_expect_success 'setup for --strict and --fsck-objects downgrading fsck msgs' '
	git init strict &&
	(
		cd strict &&
		test_commit first hello &&
		cat >commit <<-EOF &&
		tree $(git rev-parse HEAD^{tree})
		parent $(git rev-parse HEAD)
		author A U Thor
		committer A U Thor

		commit: this is a commit with bad emails

		EOF
		git hash-object --literally -t commit -w --stdin <commit >commit_list &&
		git pack-objects test <commit_list >pack-name
	)
'

test_with_bad_commit () {
	must_fail_arg="$1" &&
	must_pass_arg="$2" &&
	(
		cd strict &&
		test_must_fail git index-pack "$must_fail_arg" "test-$(cat pack-name).pack" &&
		git index-pack "$must_pass_arg" "test-$(cat pack-name).pack"
	)
}

test_expect_success 'index-pack with --strict downgrading fsck msgs' '
	test_with_bad_commit --strict --strict="missingEmail=ignore"
'

test_expect_success 'index-pack with --fsck-objects downgrading fsck msgs' '
	test_with_bad_commit --fsck-objects --fsck-objects="missingEmail=ignore"
'

test_expect_success 'cleanup for --strict and --fsck-objects downgrading fsck msgs' '
	rm -rf strict
'

test_expect_success 'honor pack.packSizeLimit' '
	git config pack.packSizeLimit 3m &&
	packname_10=$(git pack-objects test-10 <obj-list) &&
	test 2 = $(ls test-10-*.pack | wc -l)
'

test_expect_success 'verify resulting packs' '
	git verify-pack test-10-*.pack
'

test_expect_success 'tolerate packsizelimit smaller than biggest object' '
	git config pack.packSizeLimit 1 &&
	packname_11=$(git pack-objects test-11 <obj-list) &&
	test 5 = $(ls test-11-*.pack | wc -l)
'

test_expect_success 'verify resulting packs' '
	git verify-pack test-11-*.pack
'

test_expect_success 'set up pack for non-repo tests' '
	# make sure we have a pack with no matching index file
	cp test-1-*.pack foo.pack
'

test_expect_success 'index-pack --stdin complains of non-repo' '
	nongit test_must_fail git index-pack --object-format=$(test_oid algo) --stdin <foo.pack &&
	test_path_is_missing non-repo/.git
'

test_expect_success 'index-pack <pack> works in non-repo' '
	nongit git index-pack --object-format=$(test_oid algo) ../foo.pack &&
	test_path_is_file foo.idx
'

test_expect_success 'index-pack --strict <pack> works in non-repo' '
	rm -f foo.idx &&
	nongit git index-pack --strict --object-format=$(test_oid algo) ../foo.pack &&
	test_path_is_file foo.idx
'

test_expect_success DEFAULT_HASH_ALGORITHM 'show-index works OK outside a repository' '
	nongit git show-index <foo.idx
'

for hash in sha1 sha256
do
	test_expect_success 'show-index works OK outside a repository with hash algo passed in via --object-format' '
		test_when_finished "rm -rf explicit-hash-$hash" &&
		git init --object-format=$hash explicit-hash-$hash &&
		test_commit -C explicit-hash-$hash one &&
		git -C explicit-hash-$hash rev-parse one >in &&
		git -C explicit-hash-$hash pack-objects explicit-hash-$hash <in &&
		idx=$(echo explicit-hash-$hash/explicit-hash-$hash*.idx) &&
		nongit git show-index --object-format=$hash <"$idx" >actual &&
		test_line_count = 1 actual
	'
done

test_expect_success !PTHREADS,!FAIL_PREREQS \
	'index-pack --threads=N or pack.threads=N warns when no pthreads' '
	test_must_fail git index-pack --threads=2 2>err &&
	grep ^warning: err >warnings &&
	test_line_count = 1 warnings &&
	test_grep -F "no threads support, ignoring --threads=2" err &&

	test_must_fail git -c pack.threads=2 index-pack 2>err &&
	grep ^warning: err >warnings &&
	test_line_count = 1 warnings &&
	test_grep -F "no threads support, ignoring pack.threads" err &&

	test_must_fail git -c pack.threads=2 index-pack --threads=4 2>err &&
	grep ^warning: err >warnings &&
	test_line_count = 2 warnings &&
	test_grep -F "no threads support, ignoring --threads=4" err &&
	test_grep -F "no threads support, ignoring pack.threads" err
'

test_expect_success !PTHREADS,!FAIL_PREREQS \
	'pack-objects --threads=N or pack.threads=N warns when no pthreads' '
	git pack-objects --threads=2 --stdout --all </dev/null >/dev/null 2>err &&
	grep ^warning: err >warnings &&
	test_line_count = 1 warnings &&
	test_grep -F "no threads support, ignoring --threads" err &&

	git -c pack.threads=2 pack-objects --stdout --all </dev/null >/dev/null 2>err &&
	grep ^warning: err >warnings &&
	test_line_count = 1 warnings &&
	test_grep -F "no threads support, ignoring pack.threads" err &&

	git -c pack.threads=2 pack-objects --threads=4 --stdout --all </dev/null >/dev/null 2>err &&
	grep ^warning: err >warnings &&
	test_line_count = 2 warnings &&
	test_grep -F "no threads support, ignoring --threads" err &&
	test_grep -F "no threads support, ignoring pack.threads" err
'

test_expect_success 'pack-objects in too-many-packs mode' '
	GIT_TEST_FULL_IN_PACK_ARRAY=1 git repack -ad &&
	git fsck
'

test_expect_success 'setup: fake a SHA1 hash collision' '
	git init corrupt &&
	(
		cd corrupt &&
		long_a=$(git hash-object -w ../a | sed -e "s!^..!&/!") &&
		long_b=$(git hash-object -w ../b | sed -e "s!^..!&/!") &&
		test -f	.git/objects/$long_b &&
		cp -f	.git/objects/$long_a \
			.git/objects/$long_b
	)
'

test_expect_success 'make sure index-pack detects the SHA1 collision' '
	(
		cd corrupt &&
		test_must_fail git index-pack -o ../bad.idx ../test-3.pack 2>msg &&
		test_grep "SHA1 COLLISION FOUND" msg
	)
'

test_expect_success 'make sure index-pack detects the SHA1 collision (large blobs)' '
	(
		cd corrupt &&
		test_must_fail git -c core.bigfilethreshold=1 index-pack -o ../bad.idx ../test-3.pack 2>msg &&
		test_grep "SHA1 COLLISION FOUND" msg
	)
'

test_expect_success 'prefetch objects' '
	rm -rf server client &&

	git init server &&
	test_config -C server uploadpack.allowanysha1inwant 1 &&
	test_config -C server uploadpack.allowfilter 1 &&
	test_config -C server protocol.version 2 &&

	echo one >server/one &&
	git -C server add one &&
	git -C server commit -m one &&
	git -C server branch one_branch &&

	echo two_a >server/two_a &&
	echo two_b >server/two_b &&
	git -C server add two_a two_b &&
	git -C server commit -m two &&

	echo three >server/three &&
	git -C server add three &&
	git -C server commit -m three &&
	git -C server branch three_branch &&

	# Clone, fetch "two" with blobs excluded, and re-push it. This requires
	# the client to have the blobs of "two" - verify that these are
	# prefetched in one batch.
	git clone --filter=blob:none --single-branch -b one_branch \
		"file://$(pwd)/server" client &&
	test_config -C client protocol.version 2 &&
	TWO=$(git -C server rev-parse three_branch^) &&
	git -C client fetch --filter=blob:none origin "$TWO" &&
	GIT_TRACE_PACKET=$(pwd)/trace git -C client push origin "$TWO":refs/heads/two_branch &&
	grep "fetch> done" trace >donelines &&
	test_line_count = 1 donelines
'

for hash in sha1 sha256
do
	test_expect_success "verify-pack with $hash packfile" '
		test_when_finished "rm -rf repo" &&
		git init --object-format=$hash repo &&
		test_commit -C repo initial &&
		git -C repo repack -ad &&
		git -C repo verify-pack "$(pwd)"/repo/.git/objects/pack/*.idx &&
		if test $hash = $GIT_TEST_BUILTIN_HASH
		then
			nongit git verify-pack "$(pwd)"/repo/.git/objects/pack/*.idx
		else
			# We have no way to identify the hash used by packfiles
			# or indices, so we always fall back to SHA1.
			nongit test_must_fail git verify-pack "$(pwd)"/repo/.git/objects/pack/*.idx &&
			# But with an explicit object format we should succeed.
			nongit git verify-pack --object-format=$hash "$(pwd)"/repo/.git/objects/pack/*.idx
		fi
	'

	test_expect_success "index-pack outside of a $hash repository" '
		test_when_finished "rm -rf repo" &&
		git init --object-format=$hash repo &&
		test_commit -C repo initial &&
		git -C repo repack -ad &&
		git -C repo index-pack --verify "$(pwd)"/repo/.git/objects/pack/*.pack &&
		if test $hash = $GIT_TEST_BUILTIN_HASH
		then
			nongit git index-pack --verify "$(pwd)"/repo/.git/objects/pack/*.pack
		else
			# We have no way to identify the hash used by packfiles
			# or indices, so we always fall back to SHA1.
			nongit test_must_fail git index-pack --verify "$(pwd)"/repo/.git/objects/pack/*.pack 2>err &&
			# But with an explicit object format we should succeed.
			nongit git index-pack --object-format=$hash --verify "$(pwd)"/repo/.git/objects/pack/*.pack
		fi
	'
done

test_expect_success 'valid and invalid --name-hash-versions' '
	sane_unset GIT_TEST_NAME_HASH_VERSION &&

	# Valid values are hard to verify other than "do not fail".
	# Performance tests will be more valuable to validate these versions.
	# Negative values are converted to version 1.
	for value in -1 1 2
	do
		git pack-objects base --all --name-hash-version=$value || return 1
	done &&

	# Invalid values have clear post-conditions.
	for value in 0 3
	do
		test_must_fail git pack-objects base --all --name-hash-version=$value 2>err &&
		test_grep "invalid --name-hash-version option" err || return 1
	done
'

# The following test is not necessarily a permanent choice, but since we do not
# have a "name hash version" bit in the .bitmap file format, we cannot write the
# hash values into the .bitmap file without risking breakage later.
#
# TODO: Make these compatible in the future and replace this test with the
# expected behavior when both are specified.
test_expect_success '--name-hash-version=2 and --write-bitmap-index are incompatible' '
	git pack-objects base --all --name-hash-version=2 --write-bitmap-index 2>err &&
	test_grep "currently, --write-bitmap-index requires --name-hash-version=1" err &&

	# --stdout option silently removes --write-bitmap-index
	git pack-objects --stdout --all --name-hash-version=2 --write-bitmap-index >out 2>err &&
	test_grep ! "currently, --write-bitmap-index requires --name-hash-version=1" err
'

test_expect_success '--path-walk pack everything' '
	git -C server rev-parse HEAD >in &&
	GIT_PROGRESS_DELAY=0 git -C server pack-objects \
		--stdout --revs --path-walk --progress <in >out.pack 2>err &&
	test_grep "Compressing objects by path" err &&
	git -C server index-pack --stdin <out.pack
'

test_expect_success '--path-walk thin pack' '
	cat >in <<-EOF &&
	$(git -C server rev-parse HEAD)
	^$(git -C server rev-parse HEAD~2)
	EOF
	GIT_PROGRESS_DELAY=0 git -C server pack-objects \
		--thin --stdout --revs --path-walk --progress <in >out.pack 2>err &&
	test_grep "Compressing objects by path" err &&
	git -C server index-pack --fix-thin --stdin <out.pack
'

test_expect_success 'path-walk tries an older predecessor outside the delta window' '
	git init version-pack &&
	(
		cd version-pack &&
		test-tool genrandom predecessor 4096 >file &&
		git add file &&
		git commit -m first &&
		git tag first &&
		git checkout -b side &&
		for i in 1 2 3 4
		do
			test-tool genrandom side-$i 4096 >file &&
			git commit -am side-$i || return 1
		done &&
		git checkout -b main first &&
		git merge -s ours --no-ff side -m merge &&
		echo appended >>file &&
		cp file another &&
		echo more >>another &&
		git add another &&
		git commit -am last &&
		base=$(git rev-parse first:file) &&
		target=$(git rev-parse HEAD:file) &&
		for mode in 1 2
		do
			GIT_TEST_PACK_PATH_WALK_VERSIONS=$mode \
				git pack-objects --all --path-walk --window=2 \
				--depth=3 --threads=2 --no-reuse-delta \
				--no-reuse-object mode-$mode >hash &&
			hash=$(cat hash) &&
			git verify-pack -v mode-$mode-$hash.idx >pack-$mode ||
				return 1
		done &&
		awk -v oid="$target" "\$1 == oid {print NF}" pack-1 >actual &&
		echo 5 >expect &&
		test_cmp expect actual &&
		awk -v oid="$target" "\$1 == oid {print \$7}" pack-2 >actual &&
		echo "$base" >expect &&
		test_cmp expect actual &&
		test-tool path-walk --versions -- --all >versions &&
		awk -F: "
			\$1 == \"version\" && (\$2 == \"tree\" || \$2 == \"blob\") {
				print \$4, \$5
			}
		" versions >orders &&
		awk "
			NR == FNR {order[\$1] = \$2; next}
			NF == 7 && order[\$1] &&
				order[\$7] >= order[\$1] {exit 1}
			NF == 7 && \$6 > 3 {exit 1}
		" orders pack-2
	)
'

test_expect_success 'path-walk rejects a same-ID predecessor first seen at another path' '
	git init shared-versions &&
	(
		cd shared-versions &&
		mkdir z &&
		test-tool genrandom shared 4096 >z/file &&
		git add . &&
		git commit -m first &&
		base=$(git rev-parse HEAD:z/file) &&
		mkdir a &&
		cp z/file a/file &&
		echo extra >a/extra &&
		echo appended >>z/file &&
		git add . &&
		git commit -m second &&
		target=$(git rev-parse HEAD:z/file) &&
		test-tool path-walk --versions -- HEAD >versions &&
		test_grep "version:blob:a/file:$base:2:$ZERO_OID" versions &&
		test_grep "version:blob:z/file:$target:2:$base" versions &&
		GIT_TEST_PACK_PATH_WALK_VERSIONS=2 \
			git pack-objects --all --path-walk --threads=1 \
			--no-reuse-delta --no-reuse-object pack >hash &&
		hash=$(cat hash) &&
		git verify-pack -v pack-$hash.idx >actual &&
		awk -v base="$base" -v target="$target" "
			(\$1 == base || \$1 == target) && NF == 7 {exit 1}
		" actual
	)
'

test_expect_success 'path-walk fallback respects depths of predecessor deltas' '
	git init version-depth &&
	(
		cd version-depth &&
		mkdir a z &&
		test-tool genrandom shared-base 4096 >a/file &&
		git add . &&
		git commit -m first &&
		cp a/file z/file &&
		echo appended >>z/file &&
		git add . &&
		git commit -m second &&
		echo tiny >z/file &&
		git commit -am tiny &&
		cp a/file z/file &&
		echo appended >>z/file &&
		for i in 1 2 3
		do
			test-tool genrandom append-$i 1024 >>z/file &&
			git commit -am append-$i || return 1
		done &&
		GIT_TEST_PACK_PATH_WALK_VERSIONS=2 \
			git pack-objects --all --path-walk --window=2 \
			--depth=2 --threads=2 --no-reuse-delta \
			--no-reuse-object pack >hash &&
		hash=$(cat hash) &&
		git verify-pack -v pack-$hash.idx >pack &&
		awk "NF == 7 && \$6 > max {max = \$6} END {print max + 0}" pack >actual &&
		echo 2 >expect &&
		test_cmp expect actual &&
		awk "NF == 7 && \$6 > 2 {exit 1}" pack
	)
'

test_expect_success 'path-walk version ordering preserves size-based commit deltas' '
	git init commit-versions &&
	(
		cd commit-versions &&
		tree=$(git mktree </dev/null) &&
		test_seq 1 2048 >message &&
		first=$(git commit-tree "$tree" -F message) &&
		echo appended >>message &&
		last=$(git commit-tree "$tree" -p "$first" -F message) &&
		git update-ref refs/heads/main "$last" &&
		for mode in 0 1 2
		do
			GIT_TEST_PACK_PATH_WALK_VERSIONS=$mode \
				git pack-objects --all --path-walk --threads=1 \
				--no-reuse-delta --no-reuse-object mode-$mode >hash &&
			hash=$(cat hash) &&
			git verify-pack -v mode-$mode-$hash.idx >pack-$mode &&
			awk "\$2 == \"commit\" {print \$1, \$7}" pack-$mode |
				sort >commits-$mode || return 1
		done &&
		test_cmp commits-0 commits-1 &&
		test_cmp commits-0 commits-2 &&
		awk -v oid="$first" "\$1 == oid {print \$7}" pack-2 >actual &&
		echo "$last" >expect &&
		test_cmp expect actual
	)
'

test_expect_success 'path-walk predecessor-only search retains size ordering' '
	git init previous-only &&
	(
		cd previous-only &&
		test-tool genrandom previous 8192 >file &&
		git add file &&
		git commit -m first &&
		git tag first &&
		git checkout -b side &&
		for i in 1 2 3 4
		do
			test-tool genrandom side-$i 6144 >file &&
			git commit -am side-$i || return 1
		done &&
		git checkout -b main first &&
		git merge -s ours --no-ff side -m merge &&
		test-tool genrandom previous 4096 >file &&
		git commit -am shorter &&
		base=$(git rev-parse first:file) &&
		target=$(git rev-parse HEAD:file) &&
		for previous in none blob
		do
			GIT_TEST_PACK_PATH_WALK_ORDER=none \
			GIT_TEST_PACK_PATH_WALK_PREVIOUS=$previous \
				git pack-objects --all --path-walk --window=2 \
				--threads=1 --no-reuse-delta --no-reuse-object \
				$previous >hash &&
			hash=$(cat hash) &&
			git verify-pack -v $previous-$hash.idx >pack-$previous ||
				return 1
		done &&
		awk -v oid="$target" "\$1 == oid {print NF}" pack-none >actual &&
		echo 5 >expect &&
		test_cmp expect actual &&
		awk -v oid="$target" "\$1 == oid {print \$7}" pack-blob >actual &&
		echo "$base" >expect &&
		test_cmp expect actual
	)
'

test_expect_success 'path-walk can enable chronological ordering for commits' '
	(
		cd commit-versions &&
		GIT_TEST_PACK_PATH_WALK_ORDER=commit \
		GIT_TEST_PACK_PATH_WALK_PREVIOUS=none \
			git pack-objects --all --path-walk --threads=1 \
			--no-reuse-delta --no-reuse-object ordered >hash &&
		hash=$(cat hash) &&
		git verify-pack -v ordered-$hash.idx >pack &&
		first=$(git rev-parse main^) &&
		last=$(git rev-parse main) &&
		awk -v oid="$last" "\$1 == oid {print \$7}" pack >actual &&
		echo "$first" >expect &&
		test_cmp expect actual
	)
'

test_expect_success 'path-walk supports all per-type order and predecessor combinations' '
	(
		cd version-pack &&
		test-tool path-walk --versions -- --all >versions &&
		awk -F: "\$1 == \"version\" {print \$4, \$5}" versions >orders &&
		for order in none commit tree blob commit,tree commit,blob tree,blob commit,tree,blob
		do
			for previous in none commit tree blob commit,tree commit,blob tree,blob commit,tree,blob
			do
				GIT_TEST_PACK_PATH_WALK_ORDER=$order \
				GIT_TEST_PACK_PATH_WALK_PREVIOUS=$previous \
					git pack-objects --all --path-walk --window=2 \
					--depth=3 --threads=2 --no-reuse-delta \
					--no-reuse-object combination >hash &&
				hash=$(cat hash) &&
				git verify-pack -v combination-$hash.idx >pack &&
				awk -v types="$order" "
					NR == FNR {order[\$1] = \$2; next}
					NF == 7 && order[\$1] &&
						index(\",\" types \",\", \",\" \$2 \",\") &&
						(!order[\$7] || order[\$7] >= order[\$1]) {
						exit 1
					}
				" orders pack || return 1
			done
		done
	)
'

test_expect_success 'path-walk rejects unsupported experimental object types' '
	test_env GIT_TEST_PACK_PATH_WALK_ORDER=tag \
		test_must_fail git pack-objects --all --path-walk invalid 2>err &&
	test_grep "invalid object type.*tag.*GIT_TEST_PACK_PATH_WALK_ORDER" err
'

test_expect_success 'path-walk predecessor-only search handles reused trees' '
	git init self-previous &&
	(
		cd self-previous &&
		mkdir z &&
		test-tool genrandom self-base 4096 >z/file &&
		git add z &&
		git commit -m first &&
		cp -R z a &&
		git add a &&
		git commit -m copied &&
		echo extra >a/extra &&
		git add a/extra &&
		git commit -m changed &&
		blob=$(git rev-parse HEAD:a/file) &&
		test-tool path-walk --versions -- HEAD >versions &&
		test_grep "version:blob:z/file:$blob:1:$ZERO_OID" versions &&
		GIT_TEST_PACK_PATH_WALK_ORDER=none \
		GIT_TEST_PACK_PATH_WALK_PREVIOUS=blob \
			git pack-objects --all --path-walk --threads=1 \
			--no-reuse-delta --no-reuse-object pack >hash 2>err &&
		test_must_be_empty err &&
		hash=$(cat hash) &&
		git verify-pack -v pack-$hash.idx >pack &&
		awk -v oid="$blob" "\$1 == oid {print NF}" pack >actual &&
		echo 5 >expect &&
		test_cmp expect actual
	)
'

test_expect_success 'path-walk skips saturated predecessor delta bases' '
	git init skip-deltas &&
	(
		cd skip-deltas &&
		test-tool genrandom skip-base 4096 >file &&
		git add file &&
		git commit -m first &&
		for i in 1 2 3
		do
			test-tool genrandom skip-$i 256 >>file &&
			git commit -am append-$i || return 1
		done &&
		first=$(git rev-parse HEAD~3:file) &&
		second=$(git rev-parse HEAD~2:file) &&
		target=$(git rev-parse HEAD:file) &&
		for skip in none blob
		do
			GIT_TEST_PACK_PATH_WALK_ORDER=blob \
			GIT_TEST_PACK_PATH_WALK_PREVIOUS=blob \
			GIT_TEST_PACK_PATH_WALK_SKIP=$skip \
				git pack-objects --all --path-walk --window=1 \
				--depth=2 --threads=1 --no-reuse-delta \
				--no-reuse-object $skip >hash &&
			hash=$(cat hash) &&
			git verify-pack -v $skip-$hash.idx >pack-$skip &&
			awk "NF == 7 && \$6 > 2 {exit 1}" pack-$skip ||
				return 1
		done &&
		awk -v oid="$target" "\$1 == oid {print \$7}" pack-none >actual &&
		echo "$first" >expect &&
		test_cmp expect actual &&
		awk -v oid="$target" "\$1 == oid {print \$6, \$7}" pack-blob >actual &&
		echo "2 $second" >expect &&
		test_cmp expect actual
	)
'

test_expect_success 'path-walk can skip size-ordered window bases without predecessors' '
	git init skip-window &&
	(
		cd skip-window &&
		test-tool genrandom window-base 16384 >file &&
		git add file &&
		git commit -m first &&
		for i in 1 2 3 4 5 6 7 8 9 10
		do
			test-tool genrandom window-$i 512 >chunk &&
			dd if=chunk of=file bs=512 seek=$i conv=notrunc &&
			test-tool genrandom append-$i 32 >>file &&
			git commit -am version-$i || return 1
		done &&
		for skip in none blob
		do
			GIT_TEST_PACK_PATH_WALK_ORDER=none \
			GIT_TEST_PACK_PATH_WALK_PREVIOUS=none \
			GIT_TEST_PACK_PATH_WALK_SKIP=$skip \
			GIT_TRACE2_EVENT="$PWD/$skip.trace" \
				git pack-objects --all --path-walk --window=3 \
				--depth=2 --threads=1 --no-reuse-delta \
				--no-reuse-object $skip >hash &&
			hash=$(cat hash) &&
			git verify-pack -v $skip-$hash.idx >pack-$skip &&
			awk "NF == 7 && \$6 > 2 {exit 1}" pack-$skip ||
				return 1
		done &&
		target=$(git rev-parse HEAD~8:file) &&
		without=$(awk -v oid="$target" "\$1 == oid {print \$3}" pack-none) &&
		with=$(awk -v oid="$target" "\$1 == oid {print \$3}" pack-blob) &&
		test "$with" -lt "$without" &&
		test_grep "\"key\":\"version-base/skip-matches\",\"value\":\"[1-9][0-9]*\"" blob.trace
	)
'

test_expect_success 'path-walk fallback groups by name hash before chronological order' '
	git init hash-fallback &&
	(
		cd hash-fallback &&
		mkdir one middle three &&
		test-tool genrandom fallback-shared 8192 >one/long-common-fallback-file.txt &&
		git add one &&
		git commit -m first &&
		test-tool genrandom fallback-unrelated 8192 >middle/unrelated.dat &&
		git add middle &&
		git commit -m middle &&
		cp one/long-common-fallback-file.txt three/long-common-fallback-file.txt &&
		echo appended >>three/long-common-fallback-file.txt &&
		git add three &&
		git commit -m last &&
		base=$(git rev-parse HEAD:one/long-common-fallback-file.txt) &&
		target=$(git rev-parse HEAD:three/long-common-fallback-file.txt) &&
		GIT_TEST_PACK_PATH_WALK_ORDER=blob \
		GIT_TEST_PACK_PATH_WALK_PREVIOUS=blob \
			git pack-objects --all --path-walk --name-hash-version=1 \
			--window=1 --depth=3 --threads=1 --no-reuse-delta \
			--no-reuse-object pack >hash &&
		hash=$(cat hash) &&
		git verify-pack -v pack-$hash.idx >pack &&
		awk -v oid="$target" "\$1 == oid {print \$6, \$7}" pack >actual &&
		echo "1 $base" >expect &&
		test_cmp expect actual
	)
'

test_done
