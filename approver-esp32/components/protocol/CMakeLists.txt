# §7's wire format (CLAUDE.md §10.2) — what a decision is signed over.
#
# No REQUIRES, and like `components/ui` next door that is worth noticing rather
# than filling in: everything here includes `<cstdint>`/`<cstddef>` and nothing
# else, which is what lets §10.11's host tier compile it with a bare C++
# compiler. An entry appearing on this line means something with a dependency in
# it went into the wrong component — and for this component in particular, the
# dependency to watch for is the crypto library. These bytes are assembled here
# and signed somewhere else, deliberately.
idf_component_register(SRCS "signing.cpp"
                    INCLUDE_DIRS ".")
