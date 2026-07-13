#!/bin/sh

test_description='gvfs-helper POST with gvfs.postThreads config

Verify that the post verb works correctly in both sequential
(gvfs.postThreads=1) and parallel (gvfs.postThreads=4) modes.
Each test is run under both configurations to ensure identical results
and to exercise both code paths in do__http_post__fetch_oidset().
'

. ./test-lib.sh

. "$TEST_DIRECTORY"/lib-gvfs-helper.sh

# Helper: POST a set of OIDs and verify we get the expected packfiles.
#
do_post_blobs () {
	git -C "$REPO_T1" gvfs-helper \
		--cache-server=disable \
		--remote=origin \
		--no-progress \
		post \
		<"$OIDS_BLOBS_FILE" >OUT.output 2>OUT.stderr &&

	test_must_be_empty OUT.stderr &&
	verify_received_packfile_count 1
}

# Helper: POST blobs with a small block size to force multiple batches.
#
do_post_blobs_small_blocks () {
	git -C "$REPO_T1" gvfs-helper \
		--cache-server=disable \
		--remote=origin \
		--no-progress \
		post \
		--block-size=2 \
		<"$OIDS_BLOBS_FILE" >OUT.output 2>OUT.stderr &&

	test_must_be_empty OUT.stderr
}

# Helper: POST same set twice to test duplicate handling.
#
do_post_duplicate () {
	git -C "$REPO_T1" gvfs-helper \
		--cache-server=disable \
		--remote=origin \
		--no-progress \
		post \
		<"$OIDS_BLOBS_FILE" >OUT.output 2>OUT.stderr &&

	test_must_be_empty OUT.stderr &&
	verify_received_packfile_count 1 &&

	# Second fetch of same objects should still succeed.
	git -C "$REPO_T1" gvfs-helper \
		--cache-server=disable \
		--remote=origin \
		--no-progress \
		post \
		<"$OIDS_BLOBS_FILE" >OUT.output 2>OUT.stderr &&

	test_must_be_empty OUT.stderr
}

for threads in 1 4
do
	if test "$threads" = "1"
	then
		mode="sequential"
	else
		mode="parallel"
	fi

	test_expect_success "post blobs ($mode, threads=$threads)" '
		test_when_finished "per_test_cleanup" &&
		start_gvfs_protocol_server &&
		git -C "$REPO_T1" config gvfs.postThreads '$threads' &&

		GIT_TRACE2_EVENT="$(pwd)/trace-$test_count.txt" &&
		export GIT_TRACE2_EVENT &&

		do_post_blobs &&

		stop_gvfs_protocol_server
	'

	test_expect_success "post small blocks ($mode, threads=$threads)" '
		test_when_finished "per_test_cleanup" &&
		start_gvfs_protocol_server &&
		git -C "$REPO_T1" config gvfs.postThreads '$threads' &&

		GIT_TRACE2_EVENT="$(pwd)/trace-$test_count.txt" &&
		export GIT_TRACE2_EVENT &&

		do_post_blobs_small_blocks &&

		stop_gvfs_protocol_server
	'

	test_expect_success "post duplicate ($mode, threads=$threads)" '
		test_when_finished "per_test_cleanup" &&
		start_gvfs_protocol_server &&
		git -C "$REPO_T1" config gvfs.postThreads '$threads' &&

		do_post_duplicate &&

		stop_gvfs_protocol_server
	'
done

test_done
