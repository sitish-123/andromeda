# Quick Share wire formats

Vendored, unmodified, for `link/crates/link-quickshare` (compiled by its `build.rs` with prost). Apache-2.0, copyright
Google LLC; each file keeps its license header.

| File | Source |
|---|---|
| `offline_wire_formats.proto` | google/nearby `connections/implementation/proto/`, commit 8b996fa1771d05dd4cbe62146e2489bb9a6794f8 |
| `wire_format.proto` | google/nearby `sharing/proto/`, same commit |
| `enums.proto` | google/nearby `sharing/proto/`, same commit (not compiled; kept with `wire_format.proto`) |
| `proto/sharing_enums.proto` | google/nearby `proto/`, same commit |
| `ukey.proto`, `securegcm.proto`, `securemessage.proto`, `device_to_device_messages.proto` | google/ukey2 `src/main/proto/`, commit 10fc737aa901e873a3367e7e26b88eb01cd55d69 |
