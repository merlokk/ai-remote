# **The name is wrong for this board and it is kept on purpose.** There is no
# user interface here — one WS2812 and one button — but `components/protocol` is
# shared with `../approver-esp32` byte for byte, and it names `ui::Request`,
# `ui::Limits` and `ui::Activity`. Renaming the namespace would mean editing the
# one part of this firmware whose whole value is being identical to the sibling's:
# a struct that drifts there is a signature that stops verifying (§10.2).
#
# So what survives here are the three **data** types and the queue over the first
# of them, and the six view classes that made the name honest on the other board
# are gone with the screens they drew.
#
# No REQUIRES, and that is worth noticing rather than filling in: everything in
# here includes `<cstdint>` and nothing else — not ESP-IDF — which is what lets
# the host tests of §10.11 compile it with a bare C++ compiler. An entry appearing
# on this line is a sign that something with a dependency was put in the wrong
# component.
idf_component_register(SRCS "request_card.cpp" "limits_view.cpp" "activity_view.cpp"
                    INCLUDE_DIRS ".")
