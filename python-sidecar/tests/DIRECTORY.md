# python-sidecar/tests

Python sidecar unit tests.

## Files

### __init__.py
Package marker for the tests package.

### test_fetch_strips_assets.py
Tests that the /fetch endpoint's _html_to_markdown() conversion strips CSS, JavaScript, navigation elements, and advertising from the fetched HTML before returning markdown; verifies that main body content is preserved.
