#!/bin/sh

test_description='gvfs-helper verb-specific cache-server tests'

. ./test-lib.sh

. "$TEST_DIRECTORY"/lib-gvfs-helper.sh

#################################################################
# Tests for gvfs.<verb>.cache-server config.
#
# These tests verify that verb-specific cache-server overrides work
# correctly. We run two servers on different ports:
#   - Server 0 (base port): configured as gvfs.cache-server (default)
#   - Server 1 (base port + 1): configured as gvfs.<verb>.cache-server
#
# For each verb (prefetch, get, post), we verify that:
#   1. When using the verb-specific override, the request goes to server 1
#   2. When using a different verb, the request goes to server 0
#################################################################

test_expect_success 'verb-specific cache-server: prefetch uses gvfs.prefetch.cache-server' '
	test_when_finished "per_test_cleanup" &&
	test_when_finished "git -C \"$REPO_T1\" config --unset gvfs.prefetch.cache-server" &&
	start_gvfs_protocol_server 0 &&
	start_gvfs_protocol_server 1 &&

	# Configure server 0 as default cache-server and server 1 for prefetch.
	git -C "$REPO_T1" config gvfs.cache-server "$(cache_server_url 0)" &&
	git -C "$REPO_T1" config gvfs.prefetch.cache-server "$(cache_server_url 1)" &&

	# Run prefetch - should go to server 1.
	git -C "$REPO_T1" gvfs-helper \
		--cache-server=trust \
		--remote=origin \
		--no-progress \
		prefetch >OUT.output 2>OUT.stderr &&

	# Verify server 1 was contacted (prefetch-specific).
	verify_server_was_contacted 1 &&

	# Verify server 0 was NOT contacted.
	verify_server_was_not_contacted 0
'

test_expect_success 'verb-specific cache-server: get does NOT use gvfs.prefetch.cache-server' '
	test_when_finished "per_test_cleanup" &&
	test_when_finished "git -C \"$REPO_T1\" config --unset gvfs.prefetch.cache-server" &&
	start_gvfs_protocol_server 0 &&
	start_gvfs_protocol_server 1 &&

	# Configure server 0 as default cache-server and server 1 for prefetch.
	git -C "$REPO_T1" config gvfs.cache-server "$(cache_server_url 0)" &&
	git -C "$REPO_T1" config gvfs.prefetch.cache-server "$(cache_server_url 1)" &&

	# Run get - should go to server 0 (default), not server 1 (prefetch).
	git -C "$REPO_T1" gvfs-helper \
		--cache-server=trust \
		--remote=origin \
		get \
		<"$OID_ONE_BLOB_FILE" >OUT.output 2>OUT.stderr &&

	# Verify server 0 was contacted (default cache-server).
	verify_server_was_contacted 0 &&

	# Verify server 1 was NOT contacted (prefetch-specific).
	verify_server_was_not_contacted 1
'

test_expect_success 'verb-specific cache-server: get uses gvfs.get.cache-server' '
	test_when_finished "per_test_cleanup" &&
	test_when_finished "git -C \"$REPO_T1\" config --unset gvfs.get.cache-server" &&
	start_gvfs_protocol_server 0 &&
	start_gvfs_protocol_server 1 &&

	# Configure server 0 as default cache-server and server 1 for get.
	git -C "$REPO_T1" config gvfs.cache-server "$(cache_server_url 0)" &&
	git -C "$REPO_T1" config gvfs.get.cache-server "$(cache_server_url 1)" &&

	# Run get - should go to server 1.
	git -C "$REPO_T1" gvfs-helper \
		--cache-server=trust \
		--remote=origin \
		get \
		<"$OID_ONE_BLOB_FILE" >OUT.output 2>OUT.stderr &&

	# Verify server 1 was contacted (get-specific).
	verify_server_was_contacted 1 &&

	# Verify server 0 was NOT contacted.
	verify_server_was_not_contacted 0
'

test_expect_success 'verb-specific cache-server: prefetch does NOT use gvfs.get.cache-server' '
	test_when_finished "per_test_cleanup" &&
	test_when_finished "git -C \"$REPO_T1\" config --unset gvfs.get.cache-server" &&
	start_gvfs_protocol_server 0 &&
	start_gvfs_protocol_server 1 &&

	# Configure server 0 as default cache-server and server 1 for get.
	git -C "$REPO_T1" config gvfs.cache-server "$(cache_server_url 0)" &&
	git -C "$REPO_T1" config gvfs.get.cache-server "$(cache_server_url 1)" &&

	# Run prefetch - should go to server 0 (default), not server 1 (get).
	git -C "$REPO_T1" gvfs-helper \
		--cache-server=trust \
		--remote=origin \
		--no-progress \
		prefetch >OUT.output 2>OUT.stderr &&

	# Verify server 0 was contacted (default cache-server).
	verify_server_was_contacted 0 &&

	# Verify server 1 was NOT contacted (get-specific).
	verify_server_was_not_contacted 1
'

test_expect_success 'verb-specific cache-server: post uses gvfs.post.cache-server' '
	test_when_finished "per_test_cleanup" &&
	test_when_finished "git -C \"$REPO_T1\" config --unset gvfs.post.cache-server" &&
	start_gvfs_protocol_server 0 &&
	start_gvfs_protocol_server 1 &&

	# Configure server 0 as default cache-server and server 1 for post.
	git -C "$REPO_T1" config gvfs.cache-server "$(cache_server_url 0)" &&
	git -C "$REPO_T1" config gvfs.post.cache-server "$(cache_server_url 1)" &&

	# Run post - should go to server 1.
	git -C "$REPO_T1" gvfs-helper \
		--cache-server=trust \
		--remote=origin \
		--no-progress \
		post \
		<"$OIDS_BLOBS_FILE" >OUT.output 2>OUT.stderr &&

	# Verify server 1 was contacted (post-specific).
	verify_server_was_contacted 1 &&

	# Verify server 0 was NOT contacted.
	verify_server_was_not_contacted 0
'

test_expect_success 'verb-specific cache-server: get does NOT use gvfs.post.cache-server' '
	test_when_finished "per_test_cleanup" &&
	test_when_finished "git -C \"$REPO_T1\" config --unset gvfs.post.cache-server" &&
	start_gvfs_protocol_server 0 &&
	start_gvfs_protocol_server 1 &&

	# Configure server 0 as default cache-server and server 1 for post.
	git -C "$REPO_T1" config gvfs.cache-server "$(cache_server_url 0)" &&
	git -C "$REPO_T1" config gvfs.post.cache-server "$(cache_server_url 1)" &&

	# Run get - should go to server 0 (default), not server 1 (post).
	git -C "$REPO_T1" gvfs-helper \
		--cache-server=trust \
		--remote=origin \
		get \
		<"$OID_ONE_BLOB_FILE" >OUT.output 2>OUT.stderr &&

	# Verify server 0 was contacted (default cache-server).
	verify_server_was_contacted 0 &&

	# Verify server 1 was NOT contacted (post-specific).
	verify_server_was_not_contacted 1
'

test_expect_success 'verb-specific cache-server: all verbs with different servers' '
	test_when_finished "per_test_cleanup" &&
	test_when_finished "git -C \"$REPO_T1\" config --unset gvfs.cache-server" &&
	test_when_finished "git -C \"$REPO_T1\" config --unset gvfs.prefetch.cache-server" &&
	test_when_finished "git -C \"$REPO_T1\" config --unset gvfs.get.cache-server" &&
	test_when_finished "git -C \"$REPO_T1\" config --unset gvfs.post.cache-server" &&
	start_gvfs_protocol_server 0 &&
	start_gvfs_protocol_server 1 &&
	start_gvfs_protocol_server 2 &&
	start_gvfs_protocol_server 3 &&

	# Configure each verb to use a different server:
	# - server 0: default (unused in this test)
	# - server 1: prefetch
	# - server 2: get
	# - server 3: post
	git -C "$REPO_T1" config gvfs.cache-server "$(cache_server_url 0)" &&
	git -C "$REPO_T1" config gvfs.prefetch.cache-server "$(cache_server_url 1)" &&
	git -C "$REPO_T1" config gvfs.get.cache-server "$(cache_server_url 2)" &&
	git -C "$REPO_T1" config gvfs.post.cache-server "$(cache_server_url 3)" &&

	# Run prefetch - should go to server 1.
	git -C "$REPO_T1" gvfs-helper \
		--cache-server=trust \
		--remote=origin \
		--no-progress \
		prefetch >OUT.output 2>OUT.stderr &&
	verify_server_was_contacted 1 &&
	verify_server_was_not_contacted 0 &&
	verify_server_was_not_contacted 2 &&
	verify_server_was_not_contacted 3 &&

	# Clean up shared cache for next verb.
	rm -rf "$SHARED_CACHE_T1"/pack/* &&

	# Run get - should go to server 2.
	git -C "$REPO_T1" gvfs-helper \
		--cache-server=trust \
		--remote=origin \
		get \
		<"$OID_ONE_BLOB_FILE" >OUT.output 2>OUT.stderr &&
	verify_server_was_contacted 2 &&

	# Clean up shared cache for next verb.
	rm -rf "$SHARED_CACHE_T1"/[0-9a-f][0-9a-f]/ &&
	rm -rf "$SHARED_CACHE_T1"/pack/* &&

	# Run post - should go to server 3.
	git -C "$REPO_T1" gvfs-helper \
		--cache-server=trust \
		--remote=origin \
		--no-progress \
		post \
		<"$OIDS_BLOBS_FILE" >OUT.output 2>OUT.stderr &&
	verify_server_was_contacted 3
'

#################################################################
# Tests to verify trace2 data events are emitted when gvfs-helper
# falls back from a verb-specific cache server to the default cache
# server, or from the cache server to the origin.
#################################################################

test_expect_success 'trace: verb-specific-to-default fallback emits trace2 data' '
	test_when_finished "per_test_cleanup" &&
	test_when_finished "rm -f trace-fallback.event" &&
	test_when_finished "git -C \"$REPO_T1\" config --unset gvfs.get.cache-server || true" &&
	start_gvfs_protocol_server 0 &&

	# Configure default cache server to point to the working server.
	git -C "$REPO_T1" config gvfs.cache-server "$(cache_server_url 0)" &&

	# Configure verb-specific cache server to point to a dead port.
	# This will fail immediately with a connection-refused curl error.
	DEAD_PORT=$(($GIT_TEST_GVFS_PROTOCOL_PORT + 999)) &&
	git -C "$REPO_T1" config gvfs.get.cache-server \
		"http://127.0.0.1:$DEAD_PORT/servertype/cache" &&

	# Run get with trace2 event logging. The verb-specific URL will fail,
	# causing a fallback to the default cache server which succeeds.
	GIT_TRACE2_EVENT="$(pwd)/trace-fallback.event" \
	git -C "$REPO_T1" gvfs-helper \
		--cache-server=trust \
		--remote=origin \
		--fallback \
		get \
		--max-retries=0 \
		--connect-timeout-ms=200 \
		<"$OID_ONE_BLOB_FILE" >OUT.output 2>OUT.stderr &&

	stop_gvfs_protocol_server 0 &&

	# Verify the object was fetched successfully via the fallback.
	sed "s/loose //" <OUT.output | sort >OUT.actual &&
	test_cmp "$OID_ONE_BLOB_FILE" OUT.actual &&

	# Verify trace2 emitted verb-specific-to-default fallback events.
	test_grep "\"event\":\"data\".*\"key\":\"cache_server_url_fallback/type\".*\"value\":\"verb-specific-to-default\"" \
		trace-fallback.event &&
	test_grep "\"event\":\"data\".*\"key\":\"cache_server_url_fallback/verb\".*\"value\":\"GET/objects\"" \
		trace-fallback.event
'

test_expect_success 'trace: cache-server-to-origin fallback emits trace2 data' '
	test_when_finished "per_test_cleanup" &&
	test_when_finished "rm -f trace-fallback.event" &&
	start_gvfs_protocol_server_with_mayhem cache_http_503 &&

	# Run get with trace2 event logging. The cache server will return
	# HTTP 503 (via cache_http_503 mayhem), causing a fallback to origin.
	GIT_TRACE2_EVENT="$(pwd)/trace-fallback.event" \
	git -C "$REPO_T1" gvfs-helper \
		--cache-server=trust \
		--remote=origin \
		--fallback \
		get \
		--max-retries=0 \
		<"$OID_ONE_BLOB_FILE" >OUT.output 2>OUT.stderr &&

	stop_gvfs_protocol_server &&

	# Verify the object was fetched successfully via origin fallback.
	sed "s/loose //" <OUT.output | sort >OUT.actual &&
	test_cmp "$OID_ONE_BLOB_FILE" OUT.actual &&

	# Verify trace2 emitted cache-server-to-origin fallback events.
	test_grep "\"event\":\"data\".*\"key\":\"cache_server_url_fallback/type\".*\"value\":\"cache-server-to-origin\"" \
		trace-fallback.event &&
	test_grep "\"event\":\"data\".*\"key\":\"cache_server_url_fallback/verb\".*\"value\":\"GET/objects\"" \
		trace-fallback.event
'

test_done
