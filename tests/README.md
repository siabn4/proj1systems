
This directory contains the set of tests for minibash.

Auxiliary programs such as `die` and `writetostderr` are 
compiled in this directory, which is added to the PATH while
the shell is being tested.

The current directory is not affected.

Use `--json` to export results to `scores.json`, or choose a filename with
`-o` / `--output`. Export works with the full suite, category selections, and
individual tests:

```sh
python3 minibash_driver.py --shell ./minibash --json -o scores.json
python3 minibash_driver.py --shell ./minibash --basic --valgrind --json
python3 minibash_driver.py --shell ./minibash --test 025-sh-pid-var --json
```

The JSON object contains `summary`, `test_results_by_category`, and `errors`.
Each test includes its functional result and points, input/expected-output paths,
and any Valgrind result and diagnostics. `valgrind_passed: null` means the test
was not checked by Valgrind. Extra tests are always excluded from Valgrind.
Scores remain 55 basic, 30 advanced, 15 Valgrind, plus 20 extra points; full
credit reports 120%. Skipping Valgrind normalizes the remaining base to 100%.

JSON mode suppresses the usual tables; add `--verbose` to also see filenames,
test comments, input filenames, and complete diagnostics. `--list-tests` cannot
be combined with `--json`, because it does not execute tests. A functional or
Valgrind failure sets `summary.overall_success` to false and exits with status 1,
while still writing the report. Valgrind diagnostics include stdout, stderr,
and partial output on timeout; failed or incomplete runs never earn credit.

Run the driver's regression checks with:

```sh
python3 -m unittest -v test_minibash_driver
```
