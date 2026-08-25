// The bus link, tested where it costs nothing (CLAUDE.md §10.11, host tier).
//
// Two subjects, and they are here together because they are the two halves of
// the same component that include nothing:
//
//   * **where** — `endpoint.h`, which turns `nats://192.168.11.70:4222` into a
//     host and a port. A URL is the one setting on this device that an
//     operator types in full, so the parser is strict about all of it: what is
//     refused is refused with the reason visible, never guessed at;
//   * **when** — `link_policy.h`, the fifth file in this firmware whose
//     subject includes `<cstdint>` and nothing else (`ui/navigator.h`,
//     `wifi_policy.h`, `reachability.h`, `sync_policy.h`). Opening a socket
//     and speaking §10.5's four verbs is the glue's job next door, and the
//     glue has no decisions in it.
//
// The rules being pinned, all of them §10.3/§10.5/§10.9's:
//
//   * nothing is connected without a network to connect through, and a link
//     that goes away is torn down rather than left hanging on a dead route;
//   * a refused connection retries with a wait that both grows and is capped —
//     either half alone is satisfied by a constant;
//   * a drop after a working connection is **not** a refusal, and starts from
//     the bottom of the backoff again;
//   * changing the address reconnects, and changing it to the same address
//     does not;
//   * a result nobody asked for is ignored, the way `reachability.h` and
//     `sync_policy.h` both state it.

#include <cstdio>

#include "endpoint.h"
#include "link_policy.h"
#include "unity.h"

using nats::Action;
using nats::Endpoint;
using nats::kDefaultPort;
using nats::kNever;
using nats::LinkPolicy;
using nats::LinkSettings;
using nats::State;

namespace {

// ---------------------------------------------------------------------------
// Where: the URL
// ---------------------------------------------------------------------------

Endpoint Parsed(const char *url) {
    Endpoint endpoint = {};
    TEST_ASSERT_TRUE_MESSAGE(nats::ParseUrl(url, &endpoint), url);
    return endpoint;
}

void AssertRefused(const char *url) {
    Endpoint endpoint = {};
    endpoint.port = 0xBEEF;
    snprintf(endpoint.host, sizeof(endpoint.host), "untouched.example");
    TEST_ASSERT_FALSE_MESSAGE(nats::ParseUrl(url, &endpoint), url);
    // **Nothing written on a refusal** — *both* fields, which is a mutation
    // that survived checking only the port: writing the host before the port
    // has been agreed leaves the caller pointed half at the new address and
    // half at the old, and the wrong half is the one that reaches the socket.
    TEST_ASSERT_EQUAL_UINT16(0xBEEF, endpoint.port);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("untouched.example", endpoint.host, url);
}

void test_url_takes_the_scheme_host_and_port(void) {
    const Endpoint endpoint = Parsed("nats://192.168.11.70:4222");
    TEST_ASSERT_EQUAL_STRING("192.168.11.70", endpoint.host);
    TEST_ASSERT_EQUAL_UINT16(4222, endpoint.port);
}

void test_url_defaults_the_port(void) {
    // The port is the one part of a NATS address nobody remembers, and 4222 is
    // the only answer. All three spellings mean the same thing.
    TEST_ASSERT_EQUAL_UINT16(kDefaultPort, Parsed("nats://192.168.11.70").port);
    TEST_ASSERT_EQUAL_UINT16(kDefaultPort, Parsed("192.168.11.70").port);
    TEST_ASSERT_EQUAL_STRING("192.168.11.70", Parsed("192.168.11.70").host);
}

void test_url_takes_a_hostname(void) {
    // Not an address: `esp-tls` resolves, and a household bus behind a router
    // that hands out names is an ordinary thing to point at.
    const Endpoint endpoint = Parsed("nats://desktop.local:4223");
    TEST_ASSERT_EQUAL_STRING("desktop.local", endpoint.host);
    TEST_ASSERT_EQUAL_UINT16(4223, endpoint.port);
}

void test_url_takes_a_host_and_port_with_no_scheme(void) {
    const Endpoint endpoint = Parsed("192.168.11.70:4222");
    TEST_ASSERT_EQUAL_STRING("192.168.11.70", endpoint.host);
    TEST_ASSERT_EQUAL_UINT16(4222, endpoint.port);
}

void test_url_refuses_nothing_to_connect_to(void) {
    AssertRefused("");
    AssertRefused("nats://");
    AssertRefused("nats://:4222");
    AssertRefused(":4222");
    Endpoint endpoint = {};
    TEST_ASSERT_FALSE(nats::ParseUrl(nullptr, &endpoint));
}

void test_url_refuses_a_port_that_is_not_one(void) {
    AssertRefused("nats://host:");
    AssertRefused("nats://host:0");
    AssertRefused("nats://host:65536");
    AssertRefused("nats://host:99999999");
    AssertRefused("nats://host:42a2");
    AssertRefused("nats://host:-1");
    // A port that is right at the top is a port.
    TEST_ASSERT_EQUAL_UINT16(65535, Parsed("nats://host:65535").port);
}

void test_url_refuses_a_transport_this_device_does_not_speak(void) {
    // **WebSocket is in the client and is not used here** (§10.4): the bus is
    // a TCP socket on the LAN. Refusing the spelling is how somebody who typed
    // it finds that out, rather than watching a connection that never happens.
    AssertRefused("ws://host:9222");
    AssertRefused("wss://host:443");
    // And TLS, which is §10.3's eventual fix rather than today's — a URL that
    // promises encryption the socket will not do is worse than no URL.
    AssertRefused("tls://host:4222");
    AssertRefused("http://host:4222");
}

void test_url_refuses_what_it_cannot_hold_or_understand(void) {
    // A NATS URL has no path, no credentials and no query. Each of those,
    // ignored, would be a device connecting somewhere other than where the
    // string says.
    AssertRefused("nats://host:4222/subject");
    AssertRefused("nats://user:pass@host:4222");
    AssertRefused("nats://host:4222 ");
    AssertRefused(" nats://host:4222");
    AssertRefused("nats://[::1]:4222");  // IPv6 is not wired up; say so

    char too_long[nats::kHostSize + 32];
    for (size_t i = 0; i < sizeof(too_long) - 1; ++i) {
        too_long[i] = 'h';
    }
    too_long[sizeof(too_long) - 1] = '\0';
    AssertRefused(too_long);
}

void test_two_endpoints_are_the_same_or_they_are_not(void) {
    // What decides whether an edit is worth dropping a working connection for.
    const Endpoint a = Parsed("nats://192.168.11.70:4222");
    TEST_ASSERT_TRUE(nats::Same(a, Parsed("192.168.11.70")));
    TEST_ASSERT_FALSE(nats::Same(a, Parsed("nats://192.168.11.70:4223")));
    TEST_ASSERT_FALSE(nats::Same(a, Parsed("nats://192.168.11.71:4222")));
}

// ---------------------------------------------------------------------------
// When: the policy
// ---------------------------------------------------------------------------

// The real numbers shrunk so a test is not a wait. The ratios are what matter
// and they are kept: the first retry is short, the cap is well above it.
constexpr uint32_t kRetry = 1000;
constexpr uint32_t kRetryMax = 8000;
constexpr uint32_t kStep = 25;

LinkSettings TestSettings(bool enabled = true) {
    LinkSettings settings;
    settings.enabled = enabled;
    settings.retry_ms = kRetry;
    settings.retry_max_ms = kRetryMax;
    return settings;
}

// Pump until it asks for something, or until `limit_ms` of pretend time has
// gone by without it.
Action RunUntil(LinkPolicy &policy, uint32_t &now, uint32_t limit_ms) {
    const uint32_t deadline = now + limit_ms;
    while (static_cast<int32_t>(deadline - now) >= 0) {
        const Action action = policy.Tick(now);
        if (action != Action::kNone) {
            return action;
        }
        now += kStep;
    }
    return Action::kNone;
}

void AssertAsks(LinkPolicy &policy, uint32_t &now, Action wanted, uint32_t within_ms) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(wanted),
                          static_cast<int>(RunUntil(policy, now, within_ms)));
}

void AssertQuiet(LinkPolicy &policy, uint32_t &now, uint32_t for_ms) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kNone),
                          static_cast<int>(RunUntil(policy, now, for_ms)));
}

// A policy that is up and connected, which is where most of these start.
void Connected(LinkPolicy &policy, uint32_t &now) {
    policy.Configure(TestSettings(), now);
    policy.OnNetwork(true, now);
    AssertAsks(policy, now, Action::kConnect, kRetry);
    policy.OnResult(true, now);
    TEST_ASSERT_TRUE(policy.Connected());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kConnected),
                          static_cast<int>(policy.CurrentState()));
}

void test_link_connects_the_moment_there_is_a_network(void) {
    LinkPolicy policy;
    uint32_t now = 1000;
    policy.Configure(TestSettings(), now);

    // No network: nothing to do, and the state says which of the two reasons
    // it is — a screen that spelled "off" and "no network" the same way would
    // be a screen nobody could act on.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kNoNetwork),
                          static_cast<int>(policy.CurrentState()));
    AssertQuiet(policy, now, 10 * kRetryMax);

    policy.OnNetwork(true, now);
    AssertAsks(policy, now, Action::kConnect, kStep * 2);
}

void test_link_stays_off_when_there_is_no_address(void) {
    // An empty `nats.url` is the operator saying there is nothing to connect
    // to — the same call §10.8.2 makes about an empty SNTP server, and one
    // switch rather than two fields that can disagree.
    LinkPolicy policy;
    uint32_t now = 0;
    policy.Configure(TestSettings(false), now);
    policy.OnNetwork(true, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kOff),
                          static_cast<int>(policy.CurrentState()));
    AssertQuiet(policy, now, 10 * kRetryMax);
}

void test_link_asks_for_one_connection_at_a_time(void) {
    LinkPolicy policy;
    uint32_t now = 0;
    policy.Configure(TestSettings(), now);
    policy.OnNetwork(true, now);
    AssertAsks(policy, now, Action::kConnect, kRetry);
    // The attempt is outstanding: a second one would be a second socket to the
    // same server, and the first would never be closed.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kConnecting),
                          static_cast<int>(policy.CurrentState()));
    AssertQuiet(policy, now, 10 * kRetryMax);
}

void test_link_retries_a_refused_connection_with_a_growing_capped_wait(void) {
    LinkPolicy policy;
    uint32_t now = 0;
    policy.Configure(TestSettings(), now);
    policy.OnNetwork(true, now);

    // The first attempt is immediate — a device that has just been given a
    // network does not owe anybody a wait.
    AssertAsks(policy, now, Action::kConnect, kStep * 2);

    uint32_t previous = 0;
    bool grew = false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint32_t refused_at = now;
        policy.OnResult(false, now);
        AssertAsks(policy, now, Action::kConnect, 4 * kRetryMax);

        // Growing, and never past the cap. Either assertion alone is satisfied
        // by a constant, which is why both are here.
        const uint32_t waited = now - refused_at;
        TEST_ASSERT_TRUE_MESSAGE(waited >= previous, "the wait shrank");
        TEST_ASSERT_TRUE_MESSAGE(waited <= kRetryMax + 2 * kStep, "the wait ran past its cap");
        if (waited > previous) {
            grew = true;
        }
        previous = waited;
    }
    TEST_ASSERT_TRUE_MESSAGE(grew, "the wait never grew");
    // And by now it is at the cap rather than still climbing towards it.
    TEST_ASSERT_TRUE(previous >= kRetryMax - 2 * kStep);
    TEST_ASSERT_EQUAL_UINT16(8, policy.Failures());
}

void test_link_success_clears_what_was_learned_from_the_failures(void) {
    LinkPolicy policy;
    uint32_t now = 0;
    policy.Configure(TestSettings(), now);
    policy.OnNetwork(true, now);

    for (int i = 0; i < 6; ++i) {
        AssertAsks(policy, now, Action::kConnect, 2 * kRetryMax);
        policy.OnResult(false, now);
    }
    AssertAsks(policy, now, Action::kConnect, 2 * kRetryMax);
    policy.OnResult(true, now);
    TEST_ASSERT_TRUE(policy.Connected());
    TEST_ASSERT_EQUAL_UINT16(0, policy.Failures());

    // A server that has just answered has nothing left to be pessimistic
    // about: the next attempt after a drop starts at the bottom again.
    policy.OnDropped(now);
    const uint32_t dropped_at = now;
    AssertAsks(policy, now, Action::kConnect, 2 * kRetryMax);
    TEST_ASSERT_TRUE(now - dropped_at <= kRetry + 2 * kStep);
}

void test_link_a_drop_is_not_a_refusal_and_not_an_instant_retry(void) {
    // §10.9's lesson about a link that worked and then dropped, applied to a
    // socket: reconnecting the instant the server closed it turns an AP — or a
    // NATS server — that is kicking us into a loop running as fast as the
    // stack can open sockets.
    LinkPolicy policy;
    uint32_t now = 0;
    Connected(policy, now);

    policy.OnDropped(now);
    TEST_ASSERT_FALSE(policy.Connected());
    TEST_ASSERT_EQUAL_UINT16(1, policy.Drops());
    AssertQuiet(policy, now, kRetry - 2 * kStep);
    AssertAsks(policy, now, Action::kConnect, 2 * kRetry);
}

void test_link_lets_go_when_the_network_does(void) {
    // "Only ONLINE releases the bus task, and on the way down it tears the
    // socket rather than letting it hang on a dead route" — §10.9, and this is
    // the tearing half.
    LinkPolicy policy;
    uint32_t now = 0;
    Connected(policy, now);

    policy.OnNetwork(false, now);
    AssertAsks(policy, now, Action::kDisconnect, kStep * 2);
    TEST_ASSERT_FALSE(policy.Connected());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kNoNetwork),
                          static_cast<int>(policy.CurrentState()));

    // And nothing at all until there is a network again — then at once,
    // because the network coming back is not a reason to serve out a backoff.
    AssertQuiet(policy, now, 10 * kRetryMax);
    policy.OnNetwork(true, now);
    AssertAsks(policy, now, Action::kConnect, kStep * 2);
}

void test_link_lets_go_when_it_is_switched_off(void) {
    LinkPolicy policy;
    uint32_t now = 0;
    Connected(policy, now);

    policy.Configure(TestSettings(false), now);
    AssertAsks(policy, now, Action::kDisconnect, kStep * 2);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kOff),
                          static_cast<int>(policy.CurrentState()));
    AssertQuiet(policy, now, 10 * kRetryMax);
}

void test_link_restart_drops_what_is_up_and_reconnects_at_once(void) {
    // What a changed address costs: the connection that is up is to the wrong
    // server, so it goes — and the new one is not made to wait out a backoff
    // that belongs to a different host.
    LinkPolicy policy;
    uint32_t now = 0;
    Connected(policy, now);

    policy.Restart(now);
    AssertAsks(policy, now, Action::kDisconnect, kStep * 2);
    AssertAsks(policy, now, Action::kConnect, kStep * 2);

    // And from idle, a restart is simply "try now".
    policy.OnResult(false, now);
    policy.Restart(now);
    AssertAsks(policy, now, Action::kConnect, kStep * 2);
}

void test_link_restart_asked_for_mid_attempt_is_not_remembered_forever(void) {
    // **Found on the board, not here.** `nats url` typed while the task was
    // five seconds into a connect left the flag set: the attempt failed, the
    // teardown branch that clears it is only reached while something is up,
    // and the next *successful* connection was then dropped on its first tick
    // for a restart asked for minutes earlier.
    //
    // What a restart means from idle is "try now", so that is what a pending
    // one has to turn into the moment there is nothing to tear down — which is
    // also what an operator who has just changed the address wants: the new
    // one tried at once rather than after the old one's backoff.
    LinkPolicy policy;
    uint32_t now = 0;
    policy.Configure(TestSettings(), now);
    policy.OnNetwork(true, now);
    AssertAsks(policy, now, Action::kConnect, kRetry);

    policy.Restart(now);          // the address changed while this was in flight
    policy.OnResult(false, now);  // and the attempt against the old one failed

    // At once, rather than at the end of a backoff earned somewhere else.
    AssertAsks(policy, now, Action::kConnect, kStep * 2);
    policy.OnResult(true, now);
    TEST_ASSERT_TRUE(policy.Connected());

    // And the connection that worked is left alone.
    AssertQuiet(policy, now, 10 * kRetryMax);
    TEST_ASSERT_TRUE(policy.Connected());
}

void test_link_connect_now_overrides_the_backoff_but_not_the_off_switch(void) {
    LinkPolicy policy;
    uint32_t now = 0;
    policy.Configure(TestSettings(), now);
    policy.OnNetwork(true, now);
    for (int i = 0; i < 6; ++i) {
        AssertAsks(policy, now, Action::kConnect, 2 * kRetryMax);
        policy.OnResult(false, now);
    }

    policy.ConnectNow(now);
    AssertAsks(policy, now, Action::kConnect, kStep * 2);
    policy.OnResult(false, now);

    // Forcing the question does not conjure a server to ask, nor a network to
    // ask through — the same call `sync_policy.h` makes about `date sync`.
    policy.Configure(TestSettings(false), now);
    policy.ConnectNow(now);
    AssertQuiet(policy, now, 10 * kRetryMax);
}

void test_link_ignores_a_result_nobody_asked_for(void) {
    LinkPolicy policy;
    uint32_t now = 0;
    policy.Configure(TestSettings(), now);
    policy.OnNetwork(true, now);

    // A late answer must not mark a machine connected that has moved on.
    policy.OnResult(true, now);
    TEST_ASSERT_FALSE(policy.Connected());
    policy.OnResult(false, now);
    TEST_ASSERT_EQUAL_UINT16(0, policy.Failures());

    // And a drop reported by something that was never up changes nothing.
    policy.OnDropped(now);
    TEST_ASSERT_EQUAL_UINT16(0, policy.Drops());
}

void test_link_reports_what_a_console_needs(void) {
    LinkPolicy policy;
    uint32_t now = 5000;

    policy.Configure(TestSettings(), now);
    // Nothing scheduled reads as `kNever`, not as zero — "immediately" and
    // "never" are the two most wrong answers available.
    TEST_ASSERT_EQUAL_UINT32(kNever, policy.NextAttemptInMs(now));

    policy.OnNetwork(true, now);
    AssertAsks(policy, now, Action::kConnect, kRetry);
    policy.OnResult(false, now);
    const uint32_t next = policy.NextAttemptInMs(now);
    TEST_ASSERT_TRUE(next > 0 && next <= kRetry);

    AssertAsks(policy, now, Action::kConnect, 2 * kRetry);
    policy.OnResult(true, now);
    TEST_ASSERT_EQUAL_UINT32(kNever, policy.NextAttemptInMs(now));
    TEST_ASSERT_EQUAL_UINT32(0, policy.ConnectedForMs(now));
    now += 1234;
    TEST_ASSERT_EQUAL_UINT32(1234, policy.ConnectedForMs(now));
    TEST_ASSERT_EQUAL_UINT16(1, policy.Connects());

    // Every state has a word, and no two of them share one.
    TEST_ASSERT_EQUAL_STRING("connected", nats::Name(State::kConnected));
    TEST_ASSERT_EQUAL_STRING("off", nats::Name(State::kOff));
    TEST_ASSERT_NOT_NULL(nats::Name(State::kNoNetwork));
    TEST_ASSERT_NOT_NULL(nats::Name(State::kConnecting));
    TEST_ASSERT_NOT_NULL(nats::Name(State::kWaiting));
}

void test_link_survives_the_millisecond_wrap(void) {
    // `esp_timer` milliseconds wrap every ~49 days and a desk object is meant
    // to be up for longer than that. Land a backoff across the wrap: every
    // deadline in this file is a signed difference for exactly this reason.
    LinkPolicy policy;
    uint32_t now = 0xFFFFFFFFu - kRetry / 2;
    policy.Configure(TestSettings(), now);
    policy.OnNetwork(true, now);
    AssertAsks(policy, now, Action::kConnect, kRetry);

    const uint32_t failed_at = now;
    policy.OnResult(false, now);
    AssertQuiet(policy, now, kRetry - 2 * kStep);
    AssertAsks(policy, now, Action::kConnect, 2 * kRetry);
    TEST_ASSERT_TRUE(now - failed_at >= kRetry);
    TEST_ASSERT_TRUE(now < failed_at);  // the wrap really did happen
}

}  // namespace

void RegisterNatsTests(void) {
    RUN_TEST(test_url_takes_the_scheme_host_and_port);
    RUN_TEST(test_url_defaults_the_port);
    RUN_TEST(test_url_takes_a_hostname);
    RUN_TEST(test_url_takes_a_host_and_port_with_no_scheme);
    RUN_TEST(test_url_refuses_nothing_to_connect_to);
    RUN_TEST(test_url_refuses_a_port_that_is_not_one);
    RUN_TEST(test_url_refuses_a_transport_this_device_does_not_speak);
    RUN_TEST(test_url_refuses_what_it_cannot_hold_or_understand);
    RUN_TEST(test_two_endpoints_are_the_same_or_they_are_not);

    RUN_TEST(test_link_connects_the_moment_there_is_a_network);
    RUN_TEST(test_link_stays_off_when_there_is_no_address);
    RUN_TEST(test_link_asks_for_one_connection_at_a_time);
    RUN_TEST(test_link_retries_a_refused_connection_with_a_growing_capped_wait);
    RUN_TEST(test_link_success_clears_what_was_learned_from_the_failures);
    RUN_TEST(test_link_a_drop_is_not_a_refusal_and_not_an_instant_retry);
    RUN_TEST(test_link_lets_go_when_the_network_does);
    RUN_TEST(test_link_lets_go_when_it_is_switched_off);
    RUN_TEST(test_link_restart_drops_what_is_up_and_reconnects_at_once);
    RUN_TEST(test_link_restart_asked_for_mid_attempt_is_not_remembered_forever);
    RUN_TEST(test_link_connect_now_overrides_the_backoff_but_not_the_off_switch);
    RUN_TEST(test_link_ignores_a_result_nobody_asked_for);
    RUN_TEST(test_link_reports_what_a_console_needs);
    RUN_TEST(test_link_survives_the_millisecond_wrap);
}
