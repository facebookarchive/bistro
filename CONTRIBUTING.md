# Contributing to Bistro

We want to make contributing to this project as easy and transparent as
possible.

## Our Development Process

Our changes are first made in Facebook's internal repository, which provides
us with additional lint and static analysis support.  Changes are
synchronized periodically to Github, with the goal of maintaining a passing
Travis CI build at all times.

## Pull Requests

We actively welcome your pull requests.
1. Fork the repo and create your branch from `master`.
2. If you've added code that should be tested, add tests.
3. If you've changed APIs, update the documentation in the `gh-pages` branch.
4. Ensure the test suite passes.
5. Make sure your code lints.
6. If you haven't already, complete the Contributor License Agreement ("CLA").

## Contributor License Agreement ("CLA")

In order to accept your pull request, we need you to submit a CLA. You only need
to do this once to work on any of Facebook's open source projects.

Complete your CLA here: <https://code.facebook.com/cla>

## Issues

We use GitHub issues to track public bugs. Please ensure your description is
clear and has sufficient instructions to be able to reproduce the issue.

Facebook has a [bounty program](https://www.facebook.com/whitehat/) for the
safe disclosure of security bugs.  In those cases, please go through the
process outlined on that page and do not file a public issue.

## Coding Style

Adhere to the ambient style. When the style is not 100% consistent, use
these guidelines to disambiguate:

* 2 spaces for indentation, do not use tabs
* 80 character line length
* No `using namespace` in headers; avoid `using namespace` at globle scope
* `UpperCaseFirst` for classes
* `camelCase` for functions and methods
* `underscore_separated` local variables
* `camelTrailingUnderscore_` member variables, no underscore in Thrift structs.
* One class per `.h/.cpp` file, with the filename equal to the class name.
* All control-flow statements have `{}` curly braces, with the opening brace
  on the same line
* Multi-line function signatures: each argument on its own line, indented 4
  spaces. Prefer a blank line after the open brace.
* Function docblocks should use this style: `/**\n * docs\n */`
* Most inline comments use `// comment`

## License

By contributing to Bistro, you agree that your contributions will be
licensed under its [LICENSE](LICENSE).

