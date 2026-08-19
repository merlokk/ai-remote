// What a URL may reach on the filesystem (CLAUDE.md §10.16), tested where it
// costs nothing (§10.11, host tier).
//
// `web/web_paths.h` includes `<cstddef>` and nothing else, so this suite needs no
// fake — the navigator's shape rather than the drivers'.
//
// **Nearly all of the value is in one place**: the same SPIFFS partition holds
// the pages and `config.json`, so every test below that ends in a refusal is
// about the Wi-Fi passphrase not leaving the device over HTTP. The rest is a
// content-type table.

#include "unity.h"
#include "web_paths.h"
#include "web_policy.h"

#include <cstdio>
#include <cstring>

namespace {

// The answer and the name in one place, so a test reads as the sentence it is
// checking rather than as three lines of buffer handling.
bool Name(const char *uri, char *out, size_t capacity = 64) {
    (void)capacity;
    return web::UriToName(uri, out, 64);
}

}  // namespace

// --- What is served ------------------------------------------------------

void test_the_root_is_the_index(void) {
    char name[64] = {};
    TEST_ASSERT_TRUE(Name("/", name));
    TEST_ASSERT_EQUAL_STRING(web::kIndexName, name);
}

void test_a_page_and_the_things_a_page_is_made_of(void) {
    const char *served[] = {"/index.html", "/style.css", "/app.js",
                            "/logo.png",   "/favicon.ico", "/icon.svg", "/notes.txt"};
    for (const char *uri : served) {
        char name[64] = {};
        TEST_ASSERT_TRUE_MESSAGE(Name(uri, name), uri);
        TEST_ASSERT_EQUAL_STRING(uri + 1, name);
    }
}

void test_a_query_string_is_the_same_page(void) {
    char name[64] = {};
    TEST_ASSERT_TRUE(Name("/index.html?saved=1&x=2", name));
    TEST_ASSERT_EQUAL_STRING("index.html", name);

    TEST_ASSERT_TRUE(Name("/style.css#top", name));
    TEST_ASSERT_EQUAL_STRING("style.css", name);

    // …and a query string on its own is still the index.
    TEST_ASSERT_TRUE(Name("/?saved=1", name));
    TEST_ASSERT_EQUAL_STRING(web::kIndexName, name);
}

// --- What is not, and why ------------------------------------------------

void test_the_config_file_is_not_a_page(void) {
    // **The rule this file exists for.** SPIFFS holds the pages and the Wi-Fi
    // passphrase in the same flat namespace (§10.15), so a server that served
    // whatever was asked for would hand the WPA key and the pinned handler key to
    // anyone who typed the name. `.json` is not on the whitelist, so none of
    // these is reachable — and neither is a secret added to that filesystem next
    // year, which is the whole reason it is a whitelist.
    const char *never[] = {"/config.json", "/config.init.json", "/registration.json",
                           "/keys.json", "/users.json"};
    for (const char *uri : never) {
        char name[64] = {};
        TEST_ASSERT_FALSE_MESSAGE(Name(uri, name), uri);
    }
}

void test_an_extension_nobody_listed_is_refused(void) {
    const char *never[] = {"/firmware.bin", "/splash.bin", "/alert.wav", "/config",
                           "/index", "/index.html.bak", "/.hidden"};
    for (const char *uri : never) {
        char name[64] = {};
        TEST_ASSERT_FALSE_MESSAGE(Name(uri, name), uri);
    }
}

void test_traversal_is_not_defended_against_it_is_unrepresentable(void) {
    // SPIFFS is flat, so a name has no directories in it and a second `/` is
    // refused before anything looks at the dots. Both spellings, and the encoded
    // one — which is refused by the rule that a `%` is never decoded.
    const char *never[] = {"/../config.json", "/..%2fconfig.json", "/a/b.html",
                           "/%2e%2e/config.json", "//config.json", "..",
                           "/subdir/", "/%2econfig.json"};
    for (const char *uri : never) {
        char name[64] = {};
        TEST_ASSERT_FALSE_MESSAGE(Name(uri, name), uri);
    }
}

void test_a_url_that_is_not_a_path_is_refused(void) {
    char name[64] = {};
    TEST_ASSERT_FALSE(Name("", name));
    TEST_ASSERT_FALSE(Name("index.html", name));  // no leading slash
    TEST_ASSERT_FALSE(Name("http://host/index.html", name));
    TEST_ASSERT_FALSE(web::UriToName(nullptr, name, sizeof name));
}

void test_a_name_longer_than_the_filesystem_holds_is_refused(void) {
    // SPIFFS stores 32 bytes of name. A truncated one would open a *different*
    // file, which is the same class of mistake `config::CopyString` refuses to
    // make with an SSID.
    char uri[80] = {};
    char name[64] = {};

    std::snprintf(uri, sizeof uri, "/%.*s.html", static_cast<int>(web::kMaxNameLength - 5),
                  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(web::kMaxNameLength),
                             static_cast<uint32_t>(std::strlen(uri) - 1));
    TEST_ASSERT_TRUE(Name(uri, name));  // exactly at the bound

    std::snprintf(uri, sizeof uri, "/%.*s.html", static_cast<int>(web::kMaxNameLength - 4),
                  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    TEST_ASSERT_FALSE(Name(uri, name));  // and one over it
}

void test_a_buffer_too_small_is_refused_rather_than_filled(void) {
    // The same rule §10.2 keeps about the signing bytes: a refusal writes
    // nothing, because a half-written name is one somebody could open.
    char name[8] = {};
    std::snprintf(name, sizeof name, "%s", "keepme");
    TEST_ASSERT_FALSE(web::UriToName("/index.html", name, sizeof name));
    TEST_ASSERT_EQUAL_STRING("keepme", name);
}

// --- The content types ---------------------------------------------------

void test_every_served_extension_has_a_type(void) {
    TEST_ASSERT_EQUAL_STRING("text/html", web::ContentType("index.html"));
    TEST_ASSERT_EQUAL_STRING("text/css", web::ContentType("style.css"));
    TEST_ASSERT_EQUAL_STRING("application/javascript", web::ContentType("app.js"));
    TEST_ASSERT_EQUAL_STRING("image/png", web::ContentType("logo.png"));
    TEST_ASSERT_EQUAL_STRING("image/x-icon", web::ContentType("favicon.ico"));
    TEST_ASSERT_EQUAL_STRING("image/svg+xml", web::ContentType("icon.svg"));
    TEST_ASSERT_EQUAL_STRING("text/plain", web::ContentType("notes.txt"));
}

void test_a_type_is_never_null(void) {
    // A null would reach `httpd_resp_set_type` and be a crash on a device
    // answering a request somebody made — and this function is only ever called
    // with a name `UriToName` accepted, so the fallback is unreachable today.
    // Kept, one branch, because "unreachable today" is not "unreachable".
    TEST_ASSERT_NOT_NULL(web::ContentType("mystery.zip"));
    TEST_ASSERT_NOT_NULL(web::ContentType(""));
    TEST_ASSERT_NOT_NULL(web::ContentType(nullptr));
}


// --- The pages the server names itself -----------------------------------

void test_the_pages_this_site_is_made_of_are_all_reachable(void) {
    // The site of §10.16: a page of buttons, the dump, the restart, the 404 that
    // catches everything else, and the two files they share. If one of these ever
    // stops passing the whitelist, the symptom on a phone is a blank page.
    const char *site[] = {"/index.html", "/devstatus.html", "/reboot.html",
                          "/404.html",   "/app.css",        "/app.js"};
    for (const char *uri : site) {
        char name[64] = {};
        TEST_ASSERT_TRUE_MESSAGE(Name(uri, name), uri);
        TEST_ASSERT_EQUAL_STRING(uri + 1, name);
    }
}

void test_the_not_found_page_is_a_page_like_any_other(void) {
    // The server opens this one by name rather than from a URL, so nothing forces
    // it through the whitelist. It is asserted to pass anyway: a file the server
    // may open under a rule the rest of it does not follow is exactly the hole
    // this whole file exists to keep shut.
    char name[64] = {};
    char uri[64] = {};
    std::snprintf(uri, sizeof uri, "/%s", web::kNotFoundName);
    TEST_ASSERT_TRUE(Name(uri, name));
    TEST_ASSERT_EQUAL_STRING(web::kNotFoundName, name);
    TEST_ASSERT_EQUAL_STRING("text/html", web::ContentType(web::kNotFoundName));
}

// --- The one thing this server can do (§10.16) ---------------------------

void test_a_reboot_needs_the_word(void) {
    TEST_ASSERT_TRUE(web::ConfirmsReboot("/api/reboot?confirm=reboot"));
    TEST_ASSERT_FALSE(web::ConfirmsReboot("/api/reboot"));
    TEST_ASSERT_FALSE(web::ConfirmsReboot("/api/reboot?"));
    TEST_ASSERT_FALSE(web::ConfirmsReboot("/api/reboot?confirm="));
    TEST_ASSERT_FALSE(web::ConfirmsReboot(nullptr));
}

void test_the_word_is_found_wherever_in_the_query_it_sits(void) {
    TEST_ASSERT_TRUE(web::ConfirmsReboot("/api/reboot?t=1&confirm=reboot"));
    TEST_ASSERT_TRUE(web::ConfirmsReboot("/api/reboot?confirm=reboot&t=1"));
    TEST_ASSERT_TRUE(web::ConfirmsReboot("/api/reboot?a=1&confirm=reboot&b=2"));
    // A fragment never reaches a server, and if one did it would end the query
    // rather than extend a value into it.
    TEST_ASSERT_TRUE(web::ConfirmsReboot("/api/reboot?confirm=reboot#done"));
}

void test_something_that_merely_contains_the_word_is_not_the_word(void) {
    // **Why this is a scan and not a `strstr`.** Both of the first two contain
    // `confirm=reboot` as a substring and neither of them confirmed anything — and
    // the failure they would produce is a device that restarts when somebody's
    // scanner guessed a parameter name.
    TEST_ASSERT_FALSE(web::ConfirmsReboot("/api/reboot?xconfirm=reboot"));
    TEST_ASSERT_FALSE(web::ConfirmsReboot("/api/reboot?confirm=reboots"));
    TEST_ASSERT_FALSE(web::ConfirmsReboot("/api/reboot?confirm=rebo"));
    TEST_ASSERT_FALSE(web::ConfirmsReboot("/api/reboot?confirmed=reboot"));
    // Case-sensitive, like every other comparison in this file: SPIFFS is, and a
    // rule that is relaxed in one place gets relaxed in the next.
    TEST_ASSERT_FALSE(web::ConfirmsReboot("/api/reboot?Confirm=reboot"));
    TEST_ASSERT_FALSE(web::ConfirmsReboot("/api/reboot?confirm=Reboot"));
}

void test_the_confirmation_is_not_a_path(void) {
    // It reads a query, so a *path* that spells the word is not one — otherwise
    // `GET /confirm=reboot.html` would be a restart.
    TEST_ASSERT_FALSE(web::ConfirmsReboot("/confirm=reboot"));
    TEST_ASSERT_FALSE(web::ConfirmsReboot("/api/reboot/confirm=reboot"));
}

// --- When it may be up at all (§10.16) -----------------------------------

void test_no_network_stack_means_no_server_whatever_was_asked(void) {
    // **The rule that replaced a reboot.** `httpd_start` with lwIP down is an
    // assert inside the TCP/IP stack, not an error code — and on this device the
    // stack is brought up lazily (§10.9), so "the radio is off" is the ordinary
    // state of a freshly flashed board rather than an edge case.
    const web::Desired every[] = {web::Desired::kOff, web::Desired::kOn, web::Desired::kAuto};
    for (web::Desired desired : every) {
        TEST_ASSERT_FALSE(web::ShouldRun(desired, true, false, false));
        TEST_ASSERT_FALSE(web::ShouldRun(desired, true, false, true));
    }
}

void test_off_is_off_even_with_a_network(void) {
    TEST_ASSERT_FALSE(web::ShouldRun(web::Desired::kOff, true, true, false));
    TEST_ASSERT_FALSE(web::ShouldRun(web::Desired::kOff, true, true, true));
}

void test_on_is_up_on_any_network(void) {
    // A client link or an access point — either is something to serve on.
    TEST_ASSERT_TRUE(web::ShouldRun(web::Desired::kOn, true, true, false));
    TEST_ASSERT_TRUE(web::ShouldRun(web::Desired::kOn, true, true, true));
}

void test_auto_is_up_only_while_this_device_is_an_access_point(void) {
    // The case the server exists for: nothing would have this device, so it
    // raised its own AP, and an operator with no other way in can reach it. On a
    // working client link `auto` keeps it down and the seven kilobytes free.
    TEST_ASSERT_TRUE(web::ShouldRun(web::Desired::kAuto, true, true, true));
    TEST_ASSERT_FALSE(web::ShouldRun(web::Desired::kAuto, true, true, false));
}

void test_a_radio_on_its_way_out_takes_the_server_with_it(void) {
    // **The rule a reboot bought.** `httpd_stop` closes the listening socket, and
    // closing a socket needs the netif that queued its packets to still exist — so
    // the server has to let go while the radio is still *up* and merely no longer
    // wanted. The manager gives it that window by ticking this before it acts, and
    // this is the comparison that uses it: the stack is up, an access point is
    // even running, and the answer is still no.
    TEST_ASSERT_FALSE(web::ShouldRun(web::Desired::kOn, false, true, false));
    TEST_ASSERT_FALSE(web::ShouldRun(web::Desired::kOn, false, true, true));
    TEST_ASSERT_FALSE(web::ShouldRun(web::Desired::kAuto, false, true, true));
}

void test_every_wish_has_a_word(void) {
    const web::Desired every[] = {web::Desired::kOff, web::Desired::kOn, web::Desired::kAuto};
    for (web::Desired desired : every) {
        TEST_ASSERT_NOT_NULL(web::Name(desired));
        TEST_ASSERT_TRUE(web::Name(desired)[0] != 0);
    }
}

// --- Who is allowed to change it, and when (§10.16) ----------------------

void test_a_held_lifetime_freezes_the_reconciler(void) {
    // **The rule a double free bought, and the reason this function exists at
    // all.** `Maintain()` runs on the Wi-Fi manager's task five times a second
    // and `web cycle` runs on the console's, and both used to call `Stop()`
    // straight out. Two tasks passing the same null check is two `httpd_stop`
    // calls on one handle, which is `httpd_delete` freeing the same four blocks
    // twice -- and the fault surfaces later, inside the allocator, in whichever
    // task happens to call `free` next. §10.16 has the panic.
    //
    // So the diagnostic takes the lifetime and this answers `kNothing` while it
    // is held, over every combination of everything else -- because "the world
    // changed underneath" must not be a reason to touch a server somebody else
    // owns.
    const web::Desired every[] = {web::Desired::kOff, web::Desired::kOn, web::Desired::kAuto};
    for (web::Desired desired : every) {
        for (int bits = 0; bits < 16; ++bits) {
            const bool network_wanted = (bits & 1) != 0;
            const bool stack_up = (bits & 2) != 0;
            const bool ap = (bits & 4) != 0;
            const bool running = (bits & 8) != 0;
            TEST_ASSERT_EQUAL(web::Reconcile::kNothing,
                              web::Next(desired, network_wanted, stack_up, ap, running, true));
        }
    }
}

void test_the_reconciler_is_quiet_when_the_world_already_agrees(void) {
    // Up and wanted, or down and unwanted: a tick that acted on either would be
    // a start on top of a running server (`ESP_ERR_INVALID_STATE`) or a stop of
    // something that is not there, five times a second.
    TEST_ASSERT_EQUAL(web::Reconcile::kNothing,
                      web::Next(web::Desired::kOn, true, true, true, true, false));
    TEST_ASSERT_EQUAL(web::Reconcile::kNothing,
                      web::Next(web::Desired::kOff, true, true, true, false, false));
}

void test_it_starts_what_is_wanted_and_is_not_up(void) {
    TEST_ASSERT_EQUAL(web::Reconcile::kStart,
                      web::Next(web::Desired::kOn, true, true, false, false, false));
    TEST_ASSERT_EQUAL(web::Reconcile::kStart,
                      web::Next(web::Desired::kAuto, true, true, true, false, false));
}

void test_it_stops_what_is_up_and_no_longer_wanted(void) {
    // Each of the three ways the answer can turn to no, because they arrive from
    // different places: the operator, the radio being switched off, and the
    // fallback access point's window closing under `auto`.
    TEST_ASSERT_EQUAL(web::Reconcile::kStop,
                      web::Next(web::Desired::kOff, true, true, true, true, false));
    TEST_ASSERT_EQUAL(web::Reconcile::kStop,
                      web::Next(web::Desired::kOn, false, true, true, true, false));
    TEST_ASSERT_EQUAL(web::Reconcile::kStop,
                      web::Next(web::Desired::kAuto, true, true, false, true, false));
}

void test_the_stop_still_happens_while_the_stack_is_up(void) {
    // The ordering §10.16 records a reboot for: the server has to let go while
    // there is still a netif to close its socket against. `network_wanted` false
    // with `stack_up` true is exactly that window, and the answer is `kStop`
    // rather than `kNothing`.
    TEST_ASSERT_EQUAL(web::Reconcile::kStop,
                      web::Next(web::Desired::kOn, false, true, false, true, false));
}

void RegisterWebPathTests(void) {
    RUN_TEST(test_the_root_is_the_index);
    RUN_TEST(test_a_page_and_the_things_a_page_is_made_of);
    RUN_TEST(test_a_query_string_is_the_same_page);

    RUN_TEST(test_the_config_file_is_not_a_page);
    RUN_TEST(test_an_extension_nobody_listed_is_refused);
    RUN_TEST(test_traversal_is_not_defended_against_it_is_unrepresentable);
    RUN_TEST(test_a_url_that_is_not_a_path_is_refused);
    RUN_TEST(test_a_name_longer_than_the_filesystem_holds_is_refused);
    RUN_TEST(test_a_buffer_too_small_is_refused_rather_than_filled);

    RUN_TEST(test_every_served_extension_has_a_type);
    RUN_TEST(test_a_type_is_never_null);

    RUN_TEST(test_the_pages_this_site_is_made_of_are_all_reachable);
    RUN_TEST(test_the_not_found_page_is_a_page_like_any_other);

    RUN_TEST(test_a_reboot_needs_the_word);
    RUN_TEST(test_the_word_is_found_wherever_in_the_query_it_sits);
    RUN_TEST(test_something_that_merely_contains_the_word_is_not_the_word);
    RUN_TEST(test_the_confirmation_is_not_a_path);

    RUN_TEST(test_no_network_stack_means_no_server_whatever_was_asked);
    RUN_TEST(test_off_is_off_even_with_a_network);
    RUN_TEST(test_on_is_up_on_any_network);
    RUN_TEST(test_auto_is_up_only_while_this_device_is_an_access_point);
    RUN_TEST(test_a_radio_on_its_way_out_takes_the_server_with_it);
    RUN_TEST(test_every_wish_has_a_word);

    RUN_TEST(test_a_held_lifetime_freezes_the_reconciler);
    RUN_TEST(test_the_reconciler_is_quiet_when_the_world_already_agrees);
    RUN_TEST(test_it_starts_what_is_wanted_and_is_not_up);
    RUN_TEST(test_it_stops_what_is_up_and_no_longer_wanted);
    RUN_TEST(test_the_stop_still_happens_while_the_stack_is_up);
}
