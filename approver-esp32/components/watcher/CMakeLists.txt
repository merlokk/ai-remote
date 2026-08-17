# The `status` subscription (CLAUDE.md §9.7, §10.8.3) — the one thing on this
# device that listens and never answers.
#
# **`responder` is not on this line and must not be**, which is §10.8.3's test
# made structural: deleting this component leaves a working responder, and the
# only way to be sure of that is for neither to be able to name the other.
idf_component_register(SRCS "watcher.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES protocol nats screens ui)
