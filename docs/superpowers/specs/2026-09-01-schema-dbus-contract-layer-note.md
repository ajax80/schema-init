# schema-dbus contract layer — SP5 seed note

**Status:** note only, not a design. Captured during SP0 so the felt wire (SP5)
is designed from real contracts rather than guessed.

The SP0 corpus records, per exchange, beyond the wire fields:
- **owns**: what well-known name (capability) a sender asserts via RequestName.
- **intent**: property get/set vs method invoke vs signal broadcast vs name lifecycle.
- **pairing**: reply_serial ties a return/error back to its call.

The felt wire (SP5) should be able to express the same *intents* without the
dbus wire encoding: a service asserting a capability, a peer requesting an
action under a contract, a broadcast of state change. SP0 keeps the raw
material; SP5 designs the loss function over it. No protocol is proposed here.
