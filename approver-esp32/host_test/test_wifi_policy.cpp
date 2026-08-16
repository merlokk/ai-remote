// The Wi-Fi policy of CLAUDE.md §10.9, tested where it costs nothing to test
// it (§10.11, host tier).
//
// This is the half of the Wi-Fi pair that has logic in it — round-robin over
// the remembered networks, the backoff, the fallback access point and the two
// minutes it stays up — and, like the navigator, it is written so that testing
// it needs no board and no fake: it includes `<cstdint>` and nothing else.
// What is *not* here is `wifi::Radio`, which is all ESP-IDF and belongs to the
// device tier.
//
// The rules being pinned, each of them §10.9's:
//
//   * `RETRYING` **never becomes a tight loop** — every path out of a failure
//     goes through a delay, and the delay between rounds grows and is capped;
//   * an **auth failure is sticky and is reported**, and is not the same thing
//     as "no such network";
//   * `NO_CREDENTIALS` is a first-class state, not an error;
//   * only `ONLINE` releases the bus task — asserted here as "`kOnline` is the
//     only state that reports a network it is actually on".
//
// And the one the user's shape adds on top: after N fruitless rounds the
// device stops being a client and becomes findable, for a bounded time, unless
// somebody turns up — in which case the clock stops, because they are the
// reason the window exists.

#include "unity.h"
#include "wifi_policy.h"

using wifimgr::Action;
using wifimgr::Desired;
using wifimgr::Failure;
using wifimgr::Policy;
using wifimgr::Settings;
using wifimgr::State;

namespace {

// Every duration is shrunk by roughly two orders of magnitude. The numbers
// only have to be distinguishable from each other — the two-minute window of
// §10.9 is a setting, and a test that waited it out would be a test nobody
// runs.
Settings TestSettings(uint8_t network_count, uint8_t rounds = 2) {
    Settings settings;
    settings.network_count = network_count;
    settings.rounds_before_ap = rounds;
    settings.connect_timeout_ms = 500;
    settings.attempt_gap_ms = 100;
    settings.round_backoff_ms = 200;
    settings.max_backoff_ms = 600;
    settings.ap_window_ms = 1000;
    return settings;
}

constexpr uint32_t kStep = 5;

// Ticks the policy forward, moving `now` in small steps, until it asks for
// something or `limit_ms` has passed. `now` is left where it stopped, so a
// test can assert *when* the action came and not only that it did.
Action RunUntilAction(Policy &policy, uint32_t &now, uint32_t limit_ms) {
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

// A policy already in client mode with `count` networks, its first attempt
// taken off the queue. Most tests start here.
void StartClient(Policy &policy, uint32_t &now, uint8_t count, uint8_t rounds = 2) {
    policy.Configure(TestSettings(count, rounds), now);
    policy.SetDesired(Desired::kClient, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(policy.Tick(now)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kConnecting),
                          static_cast<int>(policy.GetState()));
}

// --- Off, and asked for -----------------------------------------------------

void test_starts_off_and_asks_for_nothing(void) {
    Policy policy;
    uint32_t now = 1000;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kOff), static_cast<int>(policy.GetState()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kNone), static_cast<int>(RunUntilAction(policy, now, 5000)));
}

void test_switching_off_stops_the_radio_once(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 2);

    policy.SetDesired(Desired::kOff, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStop), static_cast<int>(policy.Tick(now)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kOff), static_cast<int>(policy.GetState()));
    // And then silence — an off radio is not a thing to keep switching off.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kNone),
                          static_cast<int>(RunUntilAction(policy, now, 5000)));
}

// --- The access point somebody asked for ------------------------------------

void test_desired_ap_goes_up_and_stays_up(void) {
    Policy policy;
    uint32_t now = 1000;
    policy.Configure(TestSettings(2), now);
    policy.SetDesired(Desired::kAp, now);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartAp), static_cast<int>(policy.Tick(now)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kAp), static_cast<int>(policy.GetState()));

    // **Not the fallback window**: no timer runs, so no amount of waiting and
    // nobody arriving turns this back into a client. That distinction is the
    // reason `kAp` and `kApWindow` are two states.
    policy.OnApClients(0, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kNone),
                          static_cast<int>(RunUntilAction(policy, now, 10000)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kAp), static_cast<int>(policy.GetState()));
}

void test_asking_for_what_is_already_running_changes_nothing(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 2);
    policy.OnConnected(now);

    // The manager reads the desired mode out of `config.json` on every pass of
    // its loop, so this is the common case rather than an odd one: saying
    // "client" to a policy that is already a connected client must not drop
    // the connection to start again.
    policy.SetDesired(Desired::kClient, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kOnline), static_cast<int>(policy.GetState()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kNone),
                          static_cast<int>(RunUntilAction(policy, now, 5000)));
}

// --- Round-robin ------------------------------------------------------------

void test_the_first_attempt_is_the_first_network(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 3);
    TEST_ASSERT_EQUAL_UINT8(0, policy.Network());
    TEST_ASSERT_EQUAL_UINT8(0, policy.Round());
}

void test_a_failure_moves_to_the_next_network_after_a_gap(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 3);

    policy.OnFailed(Failure::kNotFound, now);
    // **Through a wait, never straight into the next attempt.** This is the
    // assertion behind §10.9's "never becomes a tight loop": the state between
    // two attempts is a state with a clock in it.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kWaiting), static_cast<int>(policy.GetState()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Failure::kNotFound),
                          static_cast<int>(policy.LastFailure()));

    const uint32_t started = now;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(RunUntilAction(policy, now, 1000)));
    TEST_ASSERT_TRUE(now - started >= 100);
    TEST_ASSERT_EQUAL_UINT8(1, policy.Network());
    TEST_ASSERT_EQUAL_UINT8(0, policy.Round());
}

void test_the_end_of_the_list_wraps_into_a_new_round(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 2, /*rounds=*/3);

    policy.OnFailed(Failure::kNotFound, now);
    RunUntilAction(policy, now, 1000);  // network 1
    TEST_ASSERT_EQUAL_UINT8(1, policy.Network());

    policy.OnFailed(Failure::kNotFound, now);
    const uint32_t started = now;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(RunUntilAction(policy, now, 2000)));
    TEST_ASSERT_EQUAL_UINT8(0, policy.Network());
    TEST_ASSERT_EQUAL_UINT8(1, policy.Round());
    // A round boundary waits longer than a step inside a round does.
    TEST_ASSERT_TRUE(now - started >= 200);
}

void test_the_backoff_grows_with_the_round_and_is_capped(void) {
    Policy policy;
    uint32_t now = 1000;
    // One network and enough rounds that the AP never gets in the way: the
    // list wraps on every failure, so each gap here is a round boundary.
    StartClient(policy, now, 1, /*rounds=*/250);

    for (uint32_t round = 1; round <= 5; ++round) {
        policy.OnFailed(Failure::kNotFound, now);
        const uint32_t started = now;
        TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                              static_cast<int>(RunUntilAction(policy, now, 5000)));
        const uint32_t waited = now - started;
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(round), policy.Round());

        // 200, 400, 600, 600, 600 — growing, and then bounded. **Both halves
        // are asserted**, because either alone is satisfied by a constant: a
        // backoff that never grows hammers a dead AP, and one that never caps
        // means a device that was away for an hour takes an hour to notice it
        // is back.
        const uint32_t expected = round * 200 > 600 ? 600 : round * 200;
        TEST_ASSERT_UINT32_WITHIN(2 * kStep, expected, waited);
    }
}

// --- The fallback access point ---------------------------------------------

void test_the_ap_goes_up_after_the_configured_rounds(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 2, /*rounds=*/2);

    // Two networks, two rounds: four attempts, and the fifth thing that
    // happens is an access point rather than a fifth attempt.
    for (int attempt = 0; attempt < 3; ++attempt) {
        policy.OnFailed(Failure::kNotFound, now);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                              static_cast<int>(RunUntilAction(policy, now, 2000)));
    }
    policy.OnFailed(Failure::kNotFound, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartAp), static_cast<int>(policy.Tick(now)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kApWindow), static_cast<int>(policy.GetState()));
    TEST_ASSERT_EQUAL_UINT8(2, policy.Round());
}

void test_no_rounds_configured_still_tries_every_network_once(void) {
    Policy policy;
    uint32_t now = 1000;
    // A config that said "zero rounds before the AP" would describe a device
    // that never tries the networks it was given, which is not a setting
    // anybody means — so it behaves as one round, and the two networks below
    // are what says so: the AP comes after them, not instead of them.
    StartClient(policy, now, 2, /*rounds=*/0);
    TEST_ASSERT_EQUAL_UINT8(0, policy.Network());

    policy.OnFailed(Failure::kNotFound, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(RunUntilAction(policy, now, 2000)));
    TEST_ASSERT_EQUAL_UINT8(1, policy.Network());

    policy.OnFailed(Failure::kNotFound, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartAp),
                          static_cast<int>(RunUntilAction(policy, now, 2000)));
}

void test_the_network_that_worked_is_tried_first_after_the_radio_comes_back(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 3);

    // Get onto the third network and stay there.
    for (int i = 0; i < 2; ++i) {
        policy.OnFailed(Failure::kNotFound, now);
        RunUntilAction(policy, now, 1000);
    }
    TEST_ASSERT_EQUAL_UINT8(2, policy.Network());
    policy.OnConnected(now);

    // `wifi mode off`, then `wifi mode client` again. §10.9's "try
    // last-successful first" is about exactly this: a desk device that moves
    // between a home and an office spends most of its life arriving somewhere
    // it has been before, and starting from the top of the list every time
    // means waiting out the networks that are not here.
    policy.SetDesired(Desired::kOff, now);
    policy.Tick(now);
    policy.SetDesired(Desired::kClient, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(policy.Tick(now)));
    TEST_ASSERT_EQUAL_UINT8(2, policy.Network());
}

void test_the_window_expires_and_the_client_starts_over(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 1, /*rounds=*/1);
    policy.OnFailed(Failure::kNotFound, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartAp),
                          static_cast<int>(RunUntilAction(policy, now, 2000)));

    const uint32_t raised = now;
    TEST_ASSERT_TRUE(policy.ApWindowRemainingMs(now) <= 1000);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(RunUntilAction(policy, now, 3000)));
    TEST_ASSERT_TRUE(now - raised >= 1000);
    TEST_ASSERT_EQUAL_UINT8(0, policy.Round());
    TEST_ASSERT_EQUAL_UINT8(0, policy.Network());
}

void test_a_station_holds_the_window_open(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 1, /*rounds=*/1);
    policy.OnFailed(Failure::kNotFound, now);
    RunUntilAction(policy, now, 2000);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kApWindow), static_cast<int>(policy.GetState()));

    // Somebody is on it. They are the entire reason the window exists, so the
    // clock stops — a device that dropped the operator's phone mid-form to go
    // and retry a network that was not there is a device nobody can configure.
    policy.OnApClients(1, now);
    TEST_ASSERT_EQUAL_UINT32(wifimgr::kHeldOpen, policy.ApWindowRemainingMs(now));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kNone),
                          static_cast<int>(RunUntilAction(policy, now, 10000)));
}

void test_being_told_nobody_is_attached_does_not_hold_the_window_open(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 1, /*rounds=*/1);
    policy.OnFailed(Failure::kNotFound, now);
    RunUntilAction(policy, now, 2000);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kApWindow), static_cast<int>(policy.GetState()));

    // **The manager's actual calling pattern**, and the reason the restart
    // above is on the falling edge rather than on the value: the count is
    // reported on every pass of the loop, so a window that restarted whenever
    // it heard "nobody" would be a fallback AP that never expires and a device
    // that never goes back to looking for its network.
    const uint32_t raised = now;
    Action action = Action::kNone;
    while (action == Action::kNone && now - raised < 3000) {
        policy.OnApClients(0, now);
        action = policy.Tick(now);
        now += kStep;
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient), static_cast<int>(action));
    TEST_ASSERT_TRUE(now - raised >= 1000);
}

void test_the_window_restarts_when_the_last_station_leaves(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 1, /*rounds=*/1);
    policy.OnFailed(Failure::kNotFound, now);
    RunUntilAction(policy, now, 2000);

    policy.OnApClients(1, now);
    now += 5000;
    policy.OnApClients(0, now);

    // From the beginning, not from where it was interrupted: somebody who
    // joined at 1:59 must not leave the next person four seconds.
    TEST_ASSERT_TRUE(policy.ApWindowRemainingMs(now) > 900);
    const uint32_t left = now;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(RunUntilAction(policy, now, 3000)));
    TEST_ASSERT_TRUE(now - left >= 1000);
}

// --- The attempt that is never answered -------------------------------------

void test_an_attempt_that_gets_no_answer_times_out(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 2);

    // The driver says nothing at all — an AP that accepts the association and
    // then never finishes, which on a real board is a captive portal or a
    // marginal link. Without this the device never reaches its second network.
    const uint32_t started = now;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(RunUntilAction(policy, now, 3000)));
    TEST_ASSERT_TRUE(now - started >= 500);
    TEST_ASSERT_EQUAL_UINT8(1, policy.Network());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Failure::kOther),
                          static_cast<int>(policy.LastFailure()));
}

// --- Online -----------------------------------------------------------------

void test_connecting_stops_every_timer(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 2);
    policy.OnConnected(now);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kOnline), static_cast<int>(policy.GetState()));
    TEST_ASSERT_EQUAL_UINT8(0, policy.Network());
    TEST_ASSERT_EQUAL_UINT8(0, policy.Round());
    // In particular the connect timeout, which was running a moment ago.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kNone),
                          static_cast<int>(RunUntilAction(policy, now, 60000)));
}

void test_a_drop_retries_the_network_it_was_on(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 3);

    // Get onto the second network, then lose it.
    policy.OnFailed(Failure::kNotFound, now);
    RunUntilAction(policy, now, 1000);
    TEST_ASSERT_EQUAL_UINT8(1, policy.Network());
    policy.OnConnected(now);
    policy.OnFailed(Failure::kOther, now);

    // **Through the gap, not straight back in.** An AP that accepts an
    // association and drops it again — one that is kicking us, or one at the
    // edge of range — would otherwise be a reconnect loop running as fast as
    // the radio can associate, which is §10.9's tight loop arriving by the one
    // door that is not a failed attempt.
    const uint32_t dropped = now;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kWaiting), static_cast<int>(policy.GetState()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(RunUntilAction(policy, now, 1000)));
    TEST_ASSERT_TRUE(now - dropped >= 100);
    // §10.9's "try last-successful first" — and a fresh cycle, because the
    // rounds that led here were spent finding a network that then worked.
    TEST_ASSERT_EQUAL_UINT8(1, policy.Network());
    TEST_ASSERT_EQUAL_UINT8(0, policy.Round());
}

void test_a_drop_while_online_is_not_an_auth_failure(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 2);
    policy.OnConnected(now);

    // A link that worked and then dropped with a handshake error is a link
    // that lost the AP, not a password that is wrong — and marking it sticky
    // would strike a working network off the list for the rest of the cycle.
    policy.OnFailed(Failure::kAuth, now);
    TEST_ASSERT_FALSE(policy.AuthFailed(0));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(RunUntilAction(policy, now, 1000)));
    TEST_ASSERT_EQUAL_UINT8(0, policy.Network());
}

// --- The password that is wrong ---------------------------------------------

void test_an_auth_failure_is_sticky_and_the_network_is_skipped(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 3, /*rounds=*/250);

    policy.OnFailed(Failure::kAuth, now);
    TEST_ASSERT_TRUE(policy.AuthFailed(0));
    TEST_ASSERT_FALSE(policy.AuthFailed(1));

    RunUntilAction(policy, now, 1000);
    TEST_ASSERT_EQUAL_UINT8(1, policy.Network());
    policy.OnFailed(Failure::kNotFound, now);
    RunUntilAction(policy, now, 1000);
    TEST_ASSERT_EQUAL_UINT8(2, policy.Network());

    // Round two skips the one that refused us. §10.9: reported, not retried
    // forever — the operator gets told, and the radio stops asking.
    policy.OnFailed(Failure::kNotFound, now);
    RunUntilAction(policy, now, 2000);
    TEST_ASSERT_EQUAL_UINT8(1, policy.Round());
    TEST_ASSERT_EQUAL_UINT8(1, policy.Network());
}

void test_every_password_refused_goes_straight_to_the_ap(void) {
    Policy policy;
    uint32_t now = 1000;
    // Ten rounds configured, and not one of them is spent: there is nothing
    // left to try, so waiting out the rounds would be a device sulking in
    // silence when it could be findable.
    StartClient(policy, now, 2, /*rounds=*/10);

    policy.OnFailed(Failure::kAuth, now);
    RunUntilAction(policy, now, 1000);
    TEST_ASSERT_EQUAL_UINT8(1, policy.Network());
    policy.OnFailed(Failure::kAuth, now);

    TEST_ASSERT_TRUE(policy.NoCandidates());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartAp), static_cast<int>(policy.Tick(now)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kApWindow), static_cast<int>(policy.GetState()));
}

void test_the_ap_window_clears_the_auth_failures(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 1, /*rounds=*/10);
    policy.OnFailed(Failure::kAuth, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartAp), static_cast<int>(policy.Tick(now)));

    // Nobody came, so the window ends — and the sticky failure ends with it.
    // That window was the operator's chance to fix the password; a device that
    // then refused to try the fixed one would be unfixable.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(RunUntilAction(policy, now, 3000)));
    TEST_ASSERT_FALSE(policy.AuthFailed(0));
    TEST_ASSERT_EQUAL_UINT8(0, policy.Network());
}

// --- Nothing to connect to --------------------------------------------------

void test_no_networks_is_an_access_point_that_never_expires(void) {
    Policy policy;
    uint32_t now = 1000;
    policy.Configure(TestSettings(0), now);
    policy.SetDesired(Desired::kClient, now);

    // §10.9's NO_CREDENTIALS: a first-class state, not an error. Fresh from
    // the flasher the device asks to be talked to, and it keeps asking —
    // expiring the window would only lead straight back here.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartAp), static_cast<int>(policy.Tick(now)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kApWindow), static_cast<int>(policy.GetState()));
    TEST_ASSERT_TRUE(policy.NoCandidates());
    TEST_ASSERT_EQUAL_UINT32(wifimgr::kHeldOpen, policy.ApWindowRemainingMs(now));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kNone),
                          static_cast<int>(RunUntilAction(policy, now, 20000)));
}

// --- Being told to do something else ---------------------------------------

void test_the_desired_mode_takes_effect_mid_attempt(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 2);

    policy.SetDesired(Desired::kAp, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartAp), static_cast<int>(policy.Tick(now)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kAp), static_cast<int>(policy.GetState()));

    // And an answer to the attempt that was in flight when the mode changed
    // does not drag it back — the reply is about a question nobody is asking
    // any more.
    policy.OnFailed(Failure::kNotFound, now);
    policy.OnConnected(now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kAp), static_cast<int>(policy.GetState()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kNone),
                          static_cast<int>(RunUntilAction(policy, now, 5000)));
}

void test_reconfiguring_restarts_and_forgets_the_refusals(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 2, /*rounds=*/10);
    policy.OnFailed(Failure::kAuth, now);
    TEST_ASSERT_TRUE(policy.AuthFailed(0));

    // Somebody edited the network list. The likeliest reason is the password
    // that was just refused, and the attempt in flight is against an index
    // that may now mean a different network.
    policy.Configure(TestSettings(3, 10), now);
    TEST_ASSERT_FALSE(policy.AuthFailed(0));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient), static_cast<int>(policy.Tick(now)));
    TEST_ASSERT_EQUAL_UINT8(0, policy.Network());
    TEST_ASSERT_EQUAL_UINT8(0, policy.Round());
}

// --- The clock --------------------------------------------------------------

void test_the_millisecond_counter_wrapping_is_just_a_subtraction(void) {
    Policy policy;
    // Forty-nine days of uptime, and the wrap lands inside the gap between two
    // attempts. `buttons.h` makes the same promise about the same counter;
    // this is that promise for a delay that spans the discontinuity.
    uint32_t now = 0xFFFFFFC0u;
    StartClient(policy, now, 2);
    policy.OnFailed(Failure::kNotFound, now);

    const uint32_t started = now;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(RunUntilAction(policy, now, 1000)));
    TEST_ASSERT_TRUE(now - started >= 100);
    TEST_ASSERT_EQUAL_UINT8(1, policy.Network());
}

}  // namespace

void RegisterWifiPolicyTests(void) {
    RUN_TEST(test_starts_off_and_asks_for_nothing);
    RUN_TEST(test_switching_off_stops_the_radio_once);
    RUN_TEST(test_desired_ap_goes_up_and_stays_up);
    RUN_TEST(test_asking_for_what_is_already_running_changes_nothing);
    RUN_TEST(test_the_first_attempt_is_the_first_network);
    RUN_TEST(test_a_failure_moves_to_the_next_network_after_a_gap);
    RUN_TEST(test_the_end_of_the_list_wraps_into_a_new_round);
    RUN_TEST(test_the_backoff_grows_with_the_round_and_is_capped);
    RUN_TEST(test_the_ap_goes_up_after_the_configured_rounds);
    RUN_TEST(test_no_rounds_configured_still_tries_every_network_once);
    RUN_TEST(test_the_network_that_worked_is_tried_first_after_the_radio_comes_back);
    RUN_TEST(test_the_window_expires_and_the_client_starts_over);
    RUN_TEST(test_a_station_holds_the_window_open);
    RUN_TEST(test_being_told_nobody_is_attached_does_not_hold_the_window_open);
    RUN_TEST(test_the_window_restarts_when_the_last_station_leaves);
    RUN_TEST(test_an_attempt_that_gets_no_answer_times_out);
    RUN_TEST(test_connecting_stops_every_timer);
    RUN_TEST(test_a_drop_retries_the_network_it_was_on);
    RUN_TEST(test_a_drop_while_online_is_not_an_auth_failure);
    RUN_TEST(test_an_auth_failure_is_sticky_and_the_network_is_skipped);
    RUN_TEST(test_every_password_refused_goes_straight_to_the_ap);
    RUN_TEST(test_the_ap_window_clears_the_auth_failures);
    RUN_TEST(test_no_networks_is_an_access_point_that_never_expires);
    RUN_TEST(test_the_desired_mode_takes_effect_mid_attempt);
    RUN_TEST(test_reconfiguring_restarts_and_forgets_the_refusals);
    RUN_TEST(test_the_millisecond_counter_wrapping_is_just_a_subtraction);
}
