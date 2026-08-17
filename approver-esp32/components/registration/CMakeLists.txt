# §6's registration exchange, on the device (CLAUDE.md §10.7).
#
# This is the logic layer (§10.14.2), and its dependency line says so: it is the
# first component in this firmware to need the bus, the key and the filesystem at
# once, because registering is exactly the act of telling one about the other two.
#
# `protocol` is where the bytes and the ordering live — including the rule that
# the handler's signature is checked before any field of the reply is read, which
# is enforced there rather than here so that it can be tested without libsodium.
# What is left in this component is a socket, a file and a random number.
idf_component_register(SRCS "registrar.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES protocol crypto nats storage esp_hw_support espressif__cjson)
