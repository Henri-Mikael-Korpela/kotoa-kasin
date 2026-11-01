[Documentation index](../README.md)

# Continuous integration

GitHub Actions checks every push to `main` and every pull request with the workflow
in `.github/workflows/ci.yml`. It does not deploy; [deployment](deployment.md) is still
`deploy` run by hand.

| Job | What it does |
|---|---|
| Build and test (Release, Debug) | Builds with the strict warning flags of `CMakeLists.txt`, fails on any compiler warning and runs the unit tests with `ctest`. Debug is there because Release compiles the `assert`s out. |
| Static analysis | Runs clang-tidy over `src/` and `tests/`, and ShellCheck over `deploy/bin/deploy` and `scripts/fetch-geoip`. |
| Production image | Builds the `Dockerfile` without pushing it. The image compiles on Alpine (musl) and runs the unit tests there; that is how a clash with a musl macro such as `PAGE_SIZE` shows up before a deployment. |

A run is cancelled when a newer push to the same branch arrives.

## Static analysis

The checks and the reasons for the ones that are off are in `.clang-tidy`. In short:

- the Clang static analyzer (`clang-analyzer-*`): null dereferences, leaks, use after
  free, uninitialised values and similar bugs found by following the code's paths,
- `bugprone-*`: likely mistakes such as repeated branch bodies and implicit widening of
  a multiplication,
- `misc-no-recursion`: the rule of [CLAUDE.md](../CLAUDE.md) against recursion,
- `readability-non-const-parameter`: pointer parameters that could be `const`.

Every finding fails the job. The clang-tidy version is pinned in the workflow, so a new
release cannot break the build by itself; raise the version on purpose and fix what it
finds.

To run it on your machine, with clang-tidy and the SQLite development files installed:

```
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
git ls-files 'src/*.c' 'tests/*.c' | xargs -n 1 clang-tidy -p build --quiet
```

## Changing the checks

Fix a finding in the code when it is real. Turn a check off in `.clang-tidy`, with a
comment saying why, only when it contradicts a rule of this project. Do not add a
`NOLINT` comment to hide a single finding without a reason next to it.
