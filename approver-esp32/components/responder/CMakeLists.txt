# The loop, closed (CLAUDE.md §7, §10.2) — `approvals.*` in, a signed decision out.
#
# **The longest REQUIRES line in this firmware, and that is what this component
# is**: it is the one place the key, the registration, the bytes, the card and the
# socket meet. Everything on that line already worked on its own; nothing on it
# knows about any of the others.
#
# What is *not* here is any logic worth testing on the host — the parsing and the
# JSON are `protocol`'s, and what is left is when to subscribe and which task
# signs. §10.11's device tier is where that belongs.
idf_component_register(SRCS "responder.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES protocol crypto registration nats screens ui)
