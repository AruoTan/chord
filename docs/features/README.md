# Features shared by desktop and Android

Use `docs/features/<feature>.md` for a human-designed feature that involves
both applications. Keep the design small enough to review with its code.
Use one Chord branch and include both implementations and the design in the
same commit when they form a single change.

Describe:

- The user need and expected behavior on each device.
- Desktop files under `desktop/core/`, `desktop/plugins/` or its UI directories, and
  Android files under `android/src/main/java/org/kde/kdeconnect/`.
- Packet types, direction, field names, types, units and optional defaults.
- Capability negotiation and behavior when the peer lacks the feature.
- Error handling, disconnect behavior and any relevant permission checks.
- Automated checks and a short manual pairing and interaction procedure.

Existing protocol entry points are `desktop/core/networkpacket.h` and
`android/src/main/java/org/kde/kdeconnect/NetworkPacket.kt`. Plugin
implementations live in `desktop/plugins/` and
`android/src/main/java/org/kde/kdeconnect/plugins/`. Review both sides of an
existing packet before changing it; match current conventions and preserve
older-peer behavior where intended.

The [development guide](../development.md) provides build and test commands.
Record actual check results and the revision installed on each device, so a
failure can be reproduced with the same desktop and phone builds.
