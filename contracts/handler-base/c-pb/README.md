# c-pb — pure-C protobuf codecs for handler-base

Replaces **generated** Go `gen/**/*.pb.go` / vtproto for the product path.

| Piece | Role |
|---|---|
| `pb_wire` | Generic protobuf wire (varint/tag/string/map) |
| `pb_msg` | `messages.proto` product structs + encode/decode |
| `c-pb` CLI | line-protocol encode/decode for residual Go dual-run |

## Build / test

```bash
make -C contracts/handler-base/c-pb test
```

## CLI (dual-run)

```bash
printf 'request_id=r1\ntext=hello\n' | ./c-pb encode turn-start > /tmp/t.bin
./c-pb decode turn-start < /tmp/t.bin
```

Kinds: `turn-start`, `turn-event`, `tool-start`, `agent-start`, `stt-lifecycle`, …

## Policy

- Source of field numbers: `proto/messages/v1/messages.proto` (kept as schema docs).
- No protoc, no cgo, no Google protobuf runtime on the product hot path.
- Voice `pb_min` and host-local `*_pb.c` remain; new work should prefer this library.
- Python engine stubs under `gen/python` may remain until workers move off `_pb2`.
