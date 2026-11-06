# Assert actual matcher work, not merely the presence of an index.
# Each key must occur once with a decimal value. Fast-only requires positive
# index queries and explicit zero fallback counts; slow-only requires the
# converse. Hybrid requires all three positive. Empty or appended logs must
# never prove that matching avoided fallback.
test_ignore_trace () {
	local mode="$1" file="$2" key value

	case "$mode" in
	fast-only|slow-only|hybrid)
		;;
	*)
		echo "unknown ignore trace mode: $mode" >&2 &&
		return 1
		;;
	esac &&
	for key in fast-path slow-fallback fallback-patterns-tested
	do
		value=$(sed -n 's/.*"category":"ignore","key":"'"$key"'","value":"\([^"]*\)".*/\1/p' "$file") &&
		case "$value" in
		""|*[!0-9]*)
			echo "missing, duplicate or invalid ignore counter: $key" >&2 &&
			return 1
			;;
		esac &&
		test_trace2_data_singular ignore "$key" "$value" <"$file" &&
		case "$mode:$key" in
		fast-only:fast-path|slow-only:slow-fallback|slow-only:fallback-patterns-tested|hybrid:*)
			test "$value" -gt 0 || return 1
			;;
		*)
			test "$value" -eq 0 || return 1
			;;
		esac || return 1
	done
}

ignore_trace_mode () {
	if test "$nr" -lt 64 || test "${2:-false}" = true
	then
		echo slow-only
	else
		echo "$1"
	fi
}
