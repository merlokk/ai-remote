#include "web_policy.h"

namespace web {

const char *Name(Desired desired) {
    switch (desired) {
        case Desired::kOff:
            return "off";
        case Desired::kOn:
            return "on";
        case Desired::kAuto:
            return "auto";
    }
    return "off";
}

bool ShouldRun(Desired desired, bool network_wanted, bool stack_up, bool ap) {
    // **The radio being on its way out counts as gone.** This is the one input
    // that is about the *future*, and it is here so that the server closes its
    // socket while there is still a netif to close it against — §10.16 has the
    // panic that taught it.
    if (!network_wanted) {
        return false;
    }
    // **And for every mode**: no network stack, no server. §10.16 has what
    // ignoring this costs too — an assert inside lwIP and a reboot, rather than a
    // failed start somebody could read.
    if (!stack_up) {
        return false;
    }
    switch (desired) {
        case Desired::kOff:
            return false;
        case Desired::kOn:
            return true;
        case Desired::kAuto:
            return ap;
    }
    return false;
}

}  // namespace web
