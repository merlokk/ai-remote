"""The configuration site's pages, checked without a browser (CLAUDE.md §10.16).

`approver-esp32/host_test/test_web_paths.cpp` asserts what a URL may *reach*.
This asserts something the C++ tier cannot see at all: that the pages in
`approver-esp32/spiffs_image/` are wired to `app.js` the way §10.16 says every
one of them is -- markup, and one `<script src="/app.js">` at the end of it.

It exists because of a page that shipped broken and could not report it.
`wifi.html` carried an inline copy of its own script, left over from before the
pages were consolidated, and that copy was truncated mid-string. HTML has no
notion of an unterminated script: the parser ends the block at the *first*
`</script>` it meets, which was the closing half of the shared script tag on the
next line. So the tag that loads the stylesheet and every page's logic was
swallowed as script text. The page then rendered with none of the CSS and none of
the fetches, with no error anywhere and nothing in the device's log -- its only
symptom was that it did not look like the other five, which is a symptom nobody
sees until they open that particular page on a phone.

Every assertion below is that class of failure: something that leaves a page
loading successfully and doing nothing. None of them needs a board, a network or
a build -- these are the files that get flashed, read straight off the disk.
"""
from __future__ import annotations

import re
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent
SITE = REPO / "approver-esp32" / "spiffs_image"
APP_JS = SITE / "app.js"

SHARED_SCRIPT = '<script src="/app.js"></script>'


def _pages() -> list[Path]:
    return sorted(SITE.glob("*.html"))


def _page_name(page: Path) -> str:
    """What a page calls itself in `data-page`, or `none` if it says nothing --
    which is a page that has opted out rather than one that forgot, and the test
    below is what keeps those two apart."""
    found = re.search(r'<body[^>]*\bdata-page="([a-z]+)"',
                      page.read_text(encoding="utf-8"))
    return found.group(1) if found else "none"


def _handler(app_js: str, name: str) -> str | None:
    """The function `app.js` maps that page name to.

    Read out of the map rather than assumed to be ``name + "Page"``: `devstatus`
    is served by `devStatusPage`, and a test that guessed the name would have
    reported a missing handler for a page that works perfectly well.
    """
    found = re.search(rf"\n    {name}\s*:\s*(\w+)", app_js)
    return found.group(1) if found else None


@pytest.fixture(scope="module")
def app_js() -> str:
    if not APP_JS.exists():
        pytest.skip(f"{APP_JS} is missing")
    return APP_JS.read_text(encoding="utf-8")


def test_there_are_pages_to_check():
    """A glob that matched nothing would make every test below pass vacuously --
    the same trap `test_esp32_vectors.py` guards against with its own count."""
    assert len(_pages()) >= 5


@pytest.mark.parametrize("page", _pages(), ids=lambda p: p.name)
def test_no_page_carries_an_inline_script(page: Path):
    """The one this file was written for.

    A page with its own `<script>` block cannot fail safely on this site: the
    shared tag is what it takes down with it. So the rule is not "do not
    duplicate the logic" -- it is that a page here is markup and one script
    reference, and anything else is a page that can go dark in silence.
    """
    text = page.read_text(encoding="utf-8")
    inline = re.findall(r"<script(?![^>]*\bsrc=)", text)
    assert not inline, (
        f"{page.name} has an inline <script> block. Its logic belongs in app.js, "
        f"behind the page's data-page attribute -- an inline block swallows the "
        f"{SHARED_SCRIPT} tag the moment it is left unterminated."
    )


@pytest.mark.parametrize("page", _pages(), ids=lambda p: p.name)
def test_every_page_loads_the_shared_script_exactly_once(page: Path):
    text = page.read_text(encoding="utf-8")
    assert text.count(SHARED_SCRIPT) == 1, (
        f"{page.name} must carry exactly one {SHARED_SCRIPT} -- it is where the "
        f"stylesheet and this page's own logic both come from."
    )
    assert text.count("<script") == 1, f"{page.name} loads more than one script"


@pytest.mark.parametrize("page", _pages(), ids=lambda p: p.name)
def test_every_page_has_a_handler_in_app_js(page: Path, app_js: str):
    """`data-page` is how one file serves six pages, and a value with no handler
    behind it is a page that loads, styles itself and then does nothing at all --
    no lamp, no numbers, no buttons."""
    name = _page_name(page)
    if name == "none":
        # 404.html is static by design: it says one thing and offers a link home.
        # `none` is how a page says so out loud, so it has to stay a sentinel
        # rather than quietly becoming the name of a handler somebody adds.
        assert _handler(app_js, "none") is None, "`none` is not a page name"
        return
    assert _handler(app_js, name), (
        f'{page.name} says data-page="{name}", and app.js has no handler of that '
        f"name in its pages map"
    )


@pytest.mark.parametrize("page", _pages(), ids=lambda p: p.name)
def test_every_element_its_handler_asks_for_exists_on_the_page(page: Path, app_js: str):
    """`by("ssid")` on a page with no `id="ssid"` throws on the handler's first
    line, and the rest of the page never runs -- which reads as a page that half
    loaded rather than as a bug.

    The parse is deliberately crude: a handler is the text between
    `function xPage() {` and the next top-level `function`. It fails loudly
    rather than quietly -- a handler it cannot find is an assertion, not a skip.
    """
    text = page.read_text(encoding="utf-8")
    function = _handler(app_js, _page_name(page))
    if function is None:
        return

    start = re.search(rf"\n  function {function}\(\) \{{", app_js)
    assert start, f"app.js maps a page to {function}, which it does not define"
    rest = app_js[start.end():]
    end = re.search(r"\n  (?:function \w+\(|var pages =)", rest)
    body = rest[: end.start()] if end else rest

    wanted = set(re.findall(r'\bby\("([A-Za-z0-9_-]+)"\)', body))
    have = set(re.findall(r'\bid="([A-Za-z0-9_-]+)"', text))
    missing = sorted(wanted - have)
    assert not missing, (
        f"{function} asks for {missing} and {page.name} has no such element"
    )


@pytest.mark.parametrize("page", _pages(), ids=lambda p: p.name)
def test_every_link_between_pages_resolves_to_a_file_that_ships(page: Path):
    """A button pointing at a page that is not in the image is a 404 in the
    middle of the site -- and the server cannot tell that from an attempt to read
    `config.json`, because §10.16 makes both the same refusal on purpose. So it
    has to be caught here, where the two are still different things."""
    text = page.read_text(encoding="utf-8")
    for href in re.findall(r'href="/([^"#?]*)"', text):
        target = SITE / (href or "index.html")
        assert target.exists(), f"{page.name} links to /{href}, which does not ship"
