# f1_residual_latency_bound.profile — minimal profile for the Finding-1
# regression probe (scripts/check_f1_residual.sh). Only freq_ghz matters
# here: it is set to 1.0 so consumer_ns_per_block == effective cyc/iter
# exactly, matching sibling_analyze --test's own part-(d) assertion
# (see sibling_analyze.cpp, kCannedMcaLatencyBound self-test). All other
# fields use sibling_analyze's built-in Profile() defaults, which already
# pass validate_profile().
freq_ghz = 1.0
