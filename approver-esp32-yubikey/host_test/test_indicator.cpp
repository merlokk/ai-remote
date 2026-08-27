// **The ranking** (CLAUDE.md §10.17): at any moment several things are true
// about this device, and exactly one of them gets the light.
//
// This file is the whole reason `indicator_policy.cpp` is a separate translation
// unit with no ESP-IDF in it. The alternative — four lines inside the responder —
// would put a sixteen-way decision behind a Wi-Fi radio, a NATS server and a
// security key, and the only way to check it would be to unplug things and watch
// a desk.
//
// Three properties are pinned here that no amount of watching would establish:
//
//   * **the order**, including the two places where it is deliberately not
//     "fix the lowest rung first" — a pending request outranks a fault, and a
//     boot outranks everything;
//   * **that the declaration order of `State` *is* that order**, because `led
//     test` on the console walks the enum to show an operator the palette. When
//     the two disagreed, the walk taught a ranking the device does not use — and
//     nothing failed;
//   * **that no two states look alike.** With one emitter, two states sharing a
//     colour *and* a rhythm is two states the operator cannot tell apart, and
//     the five yellows of the pre-bus stack are the one place that is allowed —
//     because they share an action as well as a colour.

#include "indicator_policy.h"
#include "unity.h"

namespace {

// A device with everything working. Each test spoils exactly one thing about it,
// which is what makes the ranking readable as a list of one-line differences.
indicator::Inputs Working() {
    indicator::Inputs in;
    in.booting = false;
    in.restore_window = false;
    in.fault = false;
    in.storage_mounted = true;
    in.can_verify = true;
    in.wifi_link = true;
    in.internet = true;
    in.bus_connected = true;
    in.registered = true;
    in.subscribed = true;
    in.fido_enrolled = true;
    in.fido_present = true;
    in.request_pending = false;
    in.signing = false;
    return in;
}

void test_indicator_everything_working_is_ready(void) {
    TEST_ASSERT_TRUE(indicator::Decide(Working()) == indicator::State::kReady);
}

void test_indicator_ready_is_green_and_breathing(void) {
    // Asked for by name, and the state this device is in almost all of its life.
    const indicator::Look look = indicator::LookOf(indicator::State::kReady);
    TEST_ASSERT_TRUE(look.colour == led::colour::kGreen);
    TEST_ASSERT_TRUE(look.effect == led::Effect::kBreathe);
    TEST_ASSERT_TRUE_MESSAGE(look.idle, "the resting state must use the idle ceiling");
}

void test_indicator_booting_is_solid_red(void) {
    // Asked for by name, and it doubles as proof that the emitter, the UART and
    // the encoding all work before anything else has had a chance to go wrong.
    indicator::Inputs in = Working();
    in.booting = true;
    TEST_ASSERT_TRUE(indicator::Decide(in) == indicator::State::kBooting);

    const indicator::Look look = indicator::LookOf(indicator::State::kBooting);
    TEST_ASSERT_TRUE(look.colour == led::colour::kRed);
    TEST_ASSERT_TRUE(look.effect == led::Effect::kSolid);
    TEST_ASSERT_FALSE_MESSAGE(look.idle, "boot must be visible, not a resting glow");
}

void test_indicator_booting_outranks_everything_including_a_request(void) {
    // Nothing is known yet, so nothing else may be reported. A device that said
    // "no bus" for the two seconds before the bus had been asked to connect
    // would be telling the truth and helping nobody.
    indicator::Inputs in = Working();
    in.booting = true;
    in.request_pending = true;
    in.fault = true;
    in.wifi_link = false;
    TEST_ASSERT_TRUE(indicator::Decide(in) == indicator::State::kBooting);
}

void test_indicator_everything_between_power_and_the_bus_is_one_yellow(void) {
    // **The repository owner's instruction, pinned.** Storage, key, Wi-Fi,
    // internet and bus are a stack with one action behind all of them; five
    // rhythms would be five things to memorise for one thing to do.
    const indicator::State stack[] = {
        indicator::State::kNoStorage,  indicator::State::kNoVerifier,
        indicator::State::kNoWifi,     indicator::State::kNoInternet,
        indicator::State::kNoBus,
    };
    for (const indicator::State state : stack) {
        const indicator::Look look = indicator::LookOf(state);
        TEST_ASSERT_TRUE_MESSAGE(look.colour == led::colour::kYellow,
                                 "a pre-bus state is not yellow");
        TEST_ASSERT_TRUE_MESSAGE(look.effect == led::Effect::kFastBlink,
                                 "a pre-bus state is not blinking fast");
    }
}

void test_indicator_the_stack_is_ranked_bottom_up(void) {
    // Each rung makes the ones after it impossible, so the first one that is
    // false is the one to fix — and saying "not registered" to somebody whose
    // router is off would be a lie by omission.
    indicator::Inputs in = Working();
    in.storage_mounted = false;
    in.can_verify = false;
    in.wifi_link = false;
    in.bus_connected = false;
    in.registered = false;
    TEST_ASSERT_TRUE(indicator::Decide(in) == indicator::State::kNoStorage);

    in.storage_mounted = true;
    TEST_ASSERT_TRUE(indicator::Decide(in) == indicator::State::kNoVerifier);

    in.can_verify = true;
    TEST_ASSERT_TRUE(indicator::Decide(in) == indicator::State::kNoWifi);

    in.wifi_link = true;
    in.internet = false;
    TEST_ASSERT_TRUE(indicator::Decide(in) == indicator::State::kNoInternet);

    in.internet = true;
    TEST_ASSERT_TRUE(indicator::Decide(in) == indicator::State::kNoBus);

    in.bus_connected = true;
    TEST_ASSERT_TRUE(indicator::Decide(in) == indicator::State::kNotRegistered);
}

void test_indicator_a_pending_request_outranks_a_fault(void) {
    // **The one place the "fix it lowest first" rule is deliberately broken.**
    // A request has a deadline; a fault does not, and the console can report it
    // at any time. Showing the fault would be letting the request expire in
    // order to say something that could wait.
    indicator::Inputs in = Working();
    in.request_pending = true;
    in.fault = true;
    TEST_ASSERT_TRUE(indicator::Decide(in) == indicator::State::kPending);
}

void test_indicator_a_pending_request_outranks_a_missing_network(void) {
    indicator::Inputs in = Working();
    in.request_pending = true;
    in.wifi_link = false;
    in.bus_connected = false;
    TEST_ASSERT_TRUE(indicator::Decide(in) == indicator::State::kPending);
}

void test_indicator_pending_is_the_loudest_thing_this_device_does(void) {
    const indicator::Look look = indicator::LookOf(indicator::State::kPending);
    TEST_ASSERT_TRUE(look.effect == led::Effect::kFastBlink);
    TEST_ASSERT_FALSE_MESSAGE(look.idle, "a request must never use the idle ceiling");
    // And it must not be yellow, which every pre-bus state already is.
    TEST_ASSERT_FALSE(look.colour == led::colour::kYellow);
}

void test_indicator_a_chosen_deny_outranks_the_request_it_answers(void) {
    // **The light has to change when BOOT is tapped**, or the operator cannot tell
    // a tap that landed from one that did not — and on this device the next thing
    // they do is touch the key, which without that feedback signs an `allow`
    // (§10.18.5). It happened twice on the desk before this state existed.
    indicator::Inputs in = Working();
    in.request_pending = true;
    in.deny_pending = true;
    TEST_ASSERT_TRUE(indicator::Decide(in) == indicator::State::kDenyPending);
}

void test_indicator_signing_outranks_a_chosen_deny(void) {
    // The touch arrived and the signature is being made: that is the newer fact.
    indicator::Inputs in = Working();
    in.request_pending = true;
    in.deny_pending = true;
    in.signing = true;
    TEST_ASSERT_TRUE(indicator::Decide(in) == indicator::State::kSigning);
}

void test_indicator_a_chosen_deny_is_red_and_not_a_fault(void) {
    // Red because a deny is red on this device, including its verdict flash — but
    // not red-fast, which is `fault` and already means something else.
    const indicator::Look deny = indicator::LookOf(indicator::State::kDenyPending);
    const indicator::Look fault = indicator::LookOf(indicator::State::kFault);
    TEST_ASSERT_TRUE(deny.colour == led::colour::kRed);
    TEST_ASSERT_FALSE_MESSAGE(deny.effect == fault.effect,
                              "a chosen deny is indistinguishable from a fault");
    TEST_ASSERT_FALSE_MESSAGE(deny.idle, "a decision owed a touch must not use the idle ceiling");
}

void test_indicator_signing_outranks_a_pending_request(void) {
    indicator::Inputs in = Working();
    in.request_pending = true;
    in.signing = true;
    TEST_ASSERT_TRUE(indicator::Decide(in) == indicator::State::kSigning);
}

void test_indicator_not_enrolled_is_its_own_state(void) {
    // A device that is registered and not enrolled cannot approve anything and
    // is not on `approvals.*` at all — which is as stuck as it gets without
    // anything being broken, and one console command from being fixed.
    indicator::Inputs in = Working();
    in.fido_enrolled = false;
    TEST_ASSERT_TRUE(indicator::Decide(in) == indicator::State::kNotEnrolled);
}

void test_indicator_enrolment_outranks_registration(void) {
    // **Both missing, and only one of them can be fixed first.** `register`
    // refuses without an enrolment, because what §6 registers is the key derived
    // from it (§10.18.1) — so a light saying `not-registered` to somebody who has
    // enrolled nothing is sending them to mint a one-time token they cannot
    // spend yet. The bottom-up rule is what settles it: the enrolment is the
    // lower rung.
    indicator::Inputs in = Working();
    in.registered = false;
    in.fido_enrolled = false;
    TEST_ASSERT_TRUE(indicator::Decide(in) == indicator::State::kNotEnrolled);
}

void test_indicator_enrolment_outranks_presence(void) {
    // An enrolment is permanent and its absence means *never*; a key in a pocket
    // is a fifteen-second problem the gate waits out. So the permanent one is
    // reported first.
    indicator::Inputs in = Working();
    in.fido_enrolled = false;
    in.fido_present = false;
    TEST_ASSERT_TRUE(indicator::Decide(in) == indicator::State::kNotEnrolled);
}

void test_indicator_a_missing_key_always_matters(void) {
    // **There is no mode in which it does not** (§10.18): the private key lives in
    // the authenticator, so a device with nothing enrolled cannot sign anything at
    // all, and a light that called that state `ready` would be promising an answer
    // this device cannot give. The switch that used to open this gate was deleted
    // with the signing key it belonged to.
    indicator::Inputs in = Working();
    in.fido_present = false;
    in.fido_enrolled = false;
    TEST_ASSERT_TRUE(indicator::Decide(in) == indicator::State::kNotEnrolled);
}

void test_indicator_connected_but_not_subscribed_is_not_ready(void) {
    // Rare and brief — the responder subscribes on its next tick — but it is a
    // state the device can genuinely be in, and calling it ready would be a
    // light that promises an answer nobody is listening to give.
    indicator::Inputs in = Working();
    in.subscribed = false;
    TEST_ASSERT_TRUE(indicator::Decide(in) == indicator::State::kWatching);
}

void test_indicator_the_restore_window_is_the_brightest_thing_there_is(void) {
    indicator::Inputs in = Working();
    in.restore_window = true;
    TEST_ASSERT_TRUE(indicator::Decide(in) == indicator::State::kRestoreWindow);

    const indicator::Look look = indicator::LookOf(indicator::State::kRestoreWindow);
    TEST_ASSERT_TRUE(look.colour == led::colour::kWhite);
    TEST_ASSERT_TRUE(look.effect == led::Effect::kSolid);
    TEST_ASSERT_FALSE(look.idle);
}

void test_indicator_the_enum_order_is_the_order_decide_ranks_them(void) {
    // **`indicator_policy.h` claims the enum is ordered worst first, "which is
    // also the order `Decide` tests them in" — this is that claim.** It is not
    // documentation: `led test` on the console walks `0..kReady` to show an
    // operator every colour, so an enum in a different order is a walk that
    // teaches the wrong ranking, and an enum missing nothing but ordered wrongly
    // fails no other test in this file.
    //
    // It is checked by spoiling the device one condition at a time, from the best
    // state downwards. Each new condition is worse than every condition already
    // spoiled, so `Decide` must return the new one every time — and its
    // enumerator must be smaller than the last, which is what the walk reads.
    struct Rung {
        indicator::State state;
        void (*spoil)(indicator::Inputs *);
    };
    const Rung kRungs[] = {
        {indicator::State::kWatching, [](indicator::Inputs *in) { in->subscribed = false; }},
        {indicator::State::kNoFidoKey, [](indicator::Inputs *in) { in->fido_present = false; }},
        {indicator::State::kNotRegistered, [](indicator::Inputs *in) { in->registered = false; }},
        // The pair that was wrong: `register` refuses without an enrolment
        // (§10.18.1), so the enrolment is the lower rung — and the enum used to
        // list it the other way round.
        {indicator::State::kNotEnrolled, [](indicator::Inputs *in) { in->fido_enrolled = false; }},
        {indicator::State::kNoBus, [](indicator::Inputs *in) { in->bus_connected = false; }},
        {indicator::State::kNoInternet, [](indicator::Inputs *in) { in->internet = false; }},
        {indicator::State::kNoWifi, [](indicator::Inputs *in) { in->wifi_link = false; }},
        {indicator::State::kNoVerifier, [](indicator::Inputs *in) { in->can_verify = false; }},
        {indicator::State::kNoStorage, [](indicator::Inputs *in) { in->storage_mounted = false; }},
        {indicator::State::kFault, [](indicator::Inputs *in) { in->fault = true; }},
        {indicator::State::kPending, [](indicator::Inputs *in) { in->request_pending = true; }},
        {indicator::State::kDenyPending, [](indicator::Inputs *in) { in->deny_pending = true; }},
        {indicator::State::kSigning, [](indicator::Inputs *in) { in->signing = true; }},
        {indicator::State::kRestoreWindow, [](indicator::Inputs *in) { in->restore_window = true; }},
        {indicator::State::kBooting, [](indicator::Inputs *in) { in->booting = true; }},
    };

    indicator::Inputs in = Working();
    TEST_ASSERT_TRUE(indicator::Decide(in) == indicator::State::kReady);
    int previous = static_cast<int>(indicator::State::kReady);
    for (const Rung &rung : kRungs) {
        rung.spoil(&in);
        TEST_ASSERT_TRUE_MESSAGE(indicator::Decide(in) == rung.state,
                                 indicator::StateName(rung.state));
        TEST_ASSERT_TRUE_MESSAGE(static_cast<int>(rung.state) < previous,
                                 indicator::StateName(rung.state));
        previous = static_cast<int>(rung.state);
    }

    // And every state is on that list, so a state added to the enum without a
    // rung here is a state whose place in the ranking nothing pins. `kReady` is
    // the one that is not spoiled into existence — it is what `Working()` is.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(indicator::State::kReady),
                          static_cast<int>(sizeof(kRungs) / sizeof(kRungs[0])));
}

void test_indicator_every_state_has_a_name_and_a_sentence(void) {
    // A state added without either is a state the console cannot report, and
    // §10.7's rule is that the console answers in words.
    for (int i = 0; i <= static_cast<int>(indicator::State::kReady); i++) {
        const indicator::State state = static_cast<indicator::State>(i);
        TEST_ASSERT_NOT_NULL(indicator::StateName(state));
        TEST_ASSERT_NOT_NULL(indicator::StateText(state));
        TEST_ASSERT_TRUE(indicator::StateName(state)[0] != '\0');
        TEST_ASSERT_TRUE(indicator::StateText(state)[0] != '\0');
        TEST_ASSERT_TRUE_MESSAGE(indicator::StateName(state)[0] != '?',
                                 "a state fell through the name switch");
    }
}

void test_indicator_no_two_states_look_alike_outside_the_yellow_stack(void) {
    // **With one emitter, a shared look is a state the operator cannot see.**
    // The five pre-bus states are the one permitted collision, because they
    // share an action as well as an appearance.
    const int last = static_cast<int>(indicator::State::kReady);
    for (int a = 0; a <= last; a++) {
        for (int b = a + 1; b <= last; b++) {
            const indicator::State first = static_cast<indicator::State>(a);
            const indicator::State second = static_cast<indicator::State>(b);
            const indicator::Look one = indicator::LookOf(first);
            const indicator::Look two = indicator::LookOf(second);
            if (!(one.colour == two.colour) || one.effect != two.effect) {
                continue;
            }
            const bool both_yellow = one.colour == led::colour::kYellow;
            TEST_ASSERT_TRUE_MESSAGE(both_yellow, indicator::StateName(second));
        }
    }
}

void test_indicator_nothing_is_dark(void) {
    // A state that shows nothing is a device that looks unplugged, and on this
    // board that is the one thing the light must never say by accident.
    const int last = static_cast<int>(indicator::State::kReady);
    for (int i = 0; i <= last; i++) {
        const indicator::Look look = indicator::LookOf(static_cast<indicator::State>(i));
        TEST_ASSERT_FALSE_MESSAGE(look.colour.Dark(),
                                  indicator::StateName(static_cast<indicator::State>(i)));
    }
}

}  // namespace

void RegisterIndicatorTests(void) {
    RUN_TEST(test_indicator_everything_working_is_ready);
    RUN_TEST(test_indicator_ready_is_green_and_breathing);
    RUN_TEST(test_indicator_booting_is_solid_red);
    RUN_TEST(test_indicator_booting_outranks_everything_including_a_request);
    RUN_TEST(test_indicator_everything_between_power_and_the_bus_is_one_yellow);
    RUN_TEST(test_indicator_the_stack_is_ranked_bottom_up);
    RUN_TEST(test_indicator_a_pending_request_outranks_a_fault);
    RUN_TEST(test_indicator_a_pending_request_outranks_a_missing_network);
    RUN_TEST(test_indicator_pending_is_the_loudest_thing_this_device_does);
    RUN_TEST(test_indicator_signing_outranks_a_pending_request);
    RUN_TEST(test_indicator_a_chosen_deny_outranks_the_request_it_answers);
    RUN_TEST(test_indicator_signing_outranks_a_chosen_deny);
    RUN_TEST(test_indicator_a_chosen_deny_is_red_and_not_a_fault);
    RUN_TEST(test_indicator_not_enrolled_is_its_own_state);
    RUN_TEST(test_indicator_enrolment_outranks_registration);
    RUN_TEST(test_indicator_enrolment_outranks_presence);
    RUN_TEST(test_indicator_a_missing_key_always_matters);
    RUN_TEST(test_indicator_connected_but_not_subscribed_is_not_ready);
    RUN_TEST(test_indicator_the_restore_window_is_the_brightest_thing_there_is);
    RUN_TEST(test_indicator_the_enum_order_is_the_order_decide_ranks_them);
    RUN_TEST(test_indicator_every_state_has_a_name_and_a_sentence);
    RUN_TEST(test_indicator_no_two_states_look_alike_outside_the_yellow_stack);
    RUN_TEST(test_indicator_nothing_is_dark);
}
