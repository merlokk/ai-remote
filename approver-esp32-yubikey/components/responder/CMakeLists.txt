# The loop of §7 on this board (CLAUDE.md §10.18). Two tasks: one waits for a
# fingertip, the other signs what that fingertip authorised.
#
# `fido` is the line that says what this device is: a decision comes off a security
# key on the OTG port, and nothing else here can produce one.
#
# `indicator` is here for two calls — a poke when a request arrives and a flash
# when a verdict leaves — and the direction matters: this component tells the
# light what happened, and the light never tells this component anything.
idf_component_register(SRCS "responder.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES protocol crypto registration nats fido ui indicator config
                             boards buttons esp_timer mbedtls)
