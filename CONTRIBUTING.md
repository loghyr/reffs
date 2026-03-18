<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Contributing to Reffs

We welcome contributions to Reffs! To ensure a high standard of code quality and protocol compliance, we ask that you follow these guidelines.

## Developer Certificate of Origin (DCO)

All commits MUST be signed off (`git commit -s`) to certify the Developer Certificate of Origin. By doing so, you attest to the following:

```
Developer's Certificate of Origin 1.1

By making a contribution to this project, I certify that:

(a) The contribution was created in whole or in part by me and I
    have the right to submit it under the open source license
    indicated in the file; or

(b) The contribution is based upon previous work that, to the best
    of my knowledge, is covered under an appropriate open source
    license and I have the right under that license to submit that
    work with modifications, whether created in whole or in part
    by me, under the same open source license (unless I am
    permitted to submit under a different license), as indicated
    in the file; or

(c) The contribution was provided directly to me by some other
    person who certified (a), (b) or (c) and I have not modified
    it.

(d) I understand and agree that this project and the contribution
    are public and that a record of the contribution (including all
    personal information I submit with it, including my sign-off) is
    maintained indefinitely and may be redistributed consistent with
    this project or the open source license(s) involved.
```

## Engineering Standards

- **Coding Style:** Strictly follow the project's `clang-format` configuration.
  - Run `make -f Makefile.reffs style` to check compliance.
  - Run `make -f Makefile.reffs fix-style` to automatically format code.
- **Static Analysis:** Regularly use `clang-tidy` and `scan-build` to identify potential issues.
- **Memory Safety:** Always build with sanitizers during development:
  ```bash
  ./configure --enable-asan --enable-tsan --enable-lsan --enable-ubsan
  ```
- **Failing Test First:** Before fixing any POSIX violation or bug, a new unit test MUST be added that reproduces the failure. The fix is only complete when the new test (and all existing tests) pass.

## Workflow

1. **Fork and Branch:** Create a feature branch for your changes.
2. **Implement and Test:** Ensure your changes are well-tested (unit tests and protocol probes).
3. **Style Check:** Run `make -f Makefile.reffs style` before committing.
4. **Sign-off:** Use `git commit -s` for all commits.
5. **Pull Request:** Submit a PR with a clear description of your changes.

## AI-Assisted Development

Portions of this codebase were drafted with the assistance of AI tools (Claude by Anthropic). All such contributions have been reviewed, tested, and signed off by a human author under the DCO above. AI tools hold no copyright interest in the code and are not project contributors. The human author's `Signed-off-by` is the sole attribution of record.

## License

By contributing, you agree that your contributions will be licensed under the AGPL-3.0-or-later.
