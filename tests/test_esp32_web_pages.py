"""The configuration site's pages, checked without a browser (CLAUDE.md §10.16).

`approver-esp32/host_test/test_web_paths.cpp` asserts what a URL may *reach*.
This asserts something the C++ tier cannot see at all: that the pages in the
firmware's `spiffs_image/` are wired to `app.js` the way §10.16 says every one of
them is -- markup, and one `<script src="/app.js">` at the end of it.

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

**Both boards are checked here, by one copy of these rules.** `approver-esp32/`
and `approver-esp32-yubikey/` serve the same seven pages off the same `app.js`
with the same one-socket-at-a-time shape (§10.16 is the same section number in
either folder, because it is the same design on different hardware). What the two
sites differ in is *content* -- there is no battery gauge on the board with no
PMIC, and no security-key rows on the board with no key -- and none of these tests
is about content. A second copy of this file would be two files that must never
differ and nothing that would notice if they did, which is the argument
`approver-esp32-yubikey/host_test/CMakeLists.txt` already makes about
`parity_vectors.h`.
"""
from __future__ import annotations

import re
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent

#: Every firmware folder that serves a site. A folder with no `app.js` is not one
#: -- which is what keeps this list from silently covering nothing if a board's
#: pages move, rather than passing vacuously.
SITES = [
    REPO / "approver-esp32" / "spiffs_image",
    REPO / "approver-esp32-yubikey" / "spiffs_image",
]

SHARED_SCRIPT = '<script src="/app.js"></script>'


def _pages() -> list[Path]:
    """Every page of every site, as one flat list.

    The site a page belongs to is `page.parent`, so nothing below has to be told
    which board it is looking at -- and the ids carry the folder name, so a
    failure says which of the two devices it came from.
    """
    found: list[Path] = []
    for site in SITES:
        if (site / "app.js").exists():
            found.extend(sorted(site.glob("*.html")))
    return found


def _page_id(page: Path) -> str:
    return f"{page.parent.parent.name}/{page.name}"


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


def _app_js(page: Path) -> str:
    """The `app.js` that ships beside this page. Read per site rather than once,
    because the two boards' copies differ in what their front pages paint."""
    return (page.parent / "app.js").read_text(encoding="utf-8")


def test_there_are_pages_to_check():
    """A glob that matched nothing would make every test below pass vacuously --
    the same trap `test_esp32_vectors.py` guards against with its own count.

    Asserted per site, so a board whose pages stopped shipping is caught here
    rather than by an empty parametrisation nobody reads.
    """
    for site in SITES:
        assert (site / "app.js").exists(), f"{site} has no app.js"
        assert len(sorted(site.glob("*.html"))) >= 5, f"{site} has too few pages"
    assert len(_pages()) >= 10


@pytest.mark.parametrize("page", _pages(), ids=_page_id)
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


@pytest.mark.parametrize("page", _pages(), ids=_page_id)
def test_every_page_loads_the_shared_script_exactly_once(page: Path):
    text = page.read_text(encoding="utf-8")
    assert text.count(SHARED_SCRIPT) == 1, (
        f"{page.name} must carry exactly one {SHARED_SCRIPT} -- it is where the "
        f"stylesheet and this page's own logic both come from."
    )
    assert text.count("<script") == 1, f"{page.name} loads more than one script"


@pytest.mark.parametrize("page", _pages(), ids=_page_id)
def test_every_page_has_a_handler_in_app_js(page: Path):
    """`data-page` is how one file serves six pages, and a value with no handler
    behind it is a page that loads, styles itself and then does nothing at all --
    no lamp, no numbers, no buttons."""
    app_js = _app_js(page)
    name = _page_name(page)
    if name == "none":
        # 404.html and 401.html are static by design: each says one thing and
        # offers a link home. `none` is how a page says so out loud, so it has to
        # stay a sentinel rather than quietly becoming the name of a handler
        # somebody adds.
        assert _handler(app_js, "none") is None, "`none` is not a page name"
        return
    assert _handler(app_js, name), (
        f'{page.name} says data-page="{name}", and app.js has no handler of that '
        f"name in its pages map"
    )


@pytest.mark.parametrize("page", _pages(), ids=_page_id)
def test_every_element_its_handler_asks_for_exists_on_the_page(page: Path):
    """`by("ssid")` on a page with no `id="ssid"` throws on the handler's first
    line, and the rest of the page never runs -- which reads as a page that half
    loaded rather than as a bug.

    **This is the test that catches a port.** The two boards' front pages paint
    different things -- one has a battery gauge, the other has the security key
    and what its one light is saying -- so a handler copied between them without
    its markup fails precisely here.

    The parse is deliberately crude: a handler is the text between
    `function xPage() {` and the next top-level `function`. It fails loudly
    rather than quietly -- a handler it cannot find is an assertion, not a skip.
    """
    app_js = _app_js(page)
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


@pytest.mark.parametrize("page", _pages(), ids=_page_id)
def test_every_link_between_pages_resolves_to_a_file_that_ships(page: Path):
    """A button pointing at a page that is not in the image is a 404 in the
    middle of the site -- and the server cannot tell that from an attempt to read
    `config.json`, because §10.16 makes both the same refusal on purpose. So it
    has to be caught here, where the two are still different things."""
    text = page.read_text(encoding="utf-8")
    for href in re.findall(r'href="/([^"#?]*)"', text):
        target = page.parent / (href or "index.html")
        assert target.exists(), f"{page.name} links to /{href}, which does not ship"
