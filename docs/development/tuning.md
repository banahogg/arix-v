# Tuning

`scripts/bench/tune_scalars.py` is an internal experiment, not part of the
normal build. It considers only scalar compile-time overrides from
`src/tuning.h`, uses the hash-partitioned train split for selection, and keeps
the holdout split blind unless invoked with `--final`.

Generate the optional benchmark corpus before running it. Record the source
lockfile revision, compiler and flags, candidate set, selected values, and
holdout result with any tuning change.
