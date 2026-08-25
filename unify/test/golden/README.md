# Golden output files

This directory holds the expected-output files for the Unify golden-output
tests (see `../CMakeLists.txt` and `../run-golden-test.sh`). It starts empty
because no golden files could be generated on the machine this test harness
was written on (no Boost toolchain available there to build `unify-run`).

To (re)generate them: build this module (`vault-unify-core` /
`unify-run`) on a machine with Boost installed, then run
`UNIFY_UPDATE_GOLDEN=1 ctest --output-on-failure` from the build directory
(or let the CI workflow's sample-output-upload step produce a first draft to
copy in) -- this writes `<name>.expected` for every sample program. Review
each diff/file carefully before committing, since it is now the pass/fail
baseline for that program.
