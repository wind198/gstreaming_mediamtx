# MediaMTX warning: `packets containing single partitions are not supported`

This document explains the log line you may see when publishing to [MediaMTX](https://github.com/bluenviron/mediamtx) with **GStreamer** and **WHIP** (`whipclientsink` from [gst-plugins-rs](https://github.com/GStreamer/gst-plugins-rs)), and what this repository does about it.

---

## What you see

In the **MediaMTX** container logs (e.g. `docker logs fleet_mediamtx_dev`):

```text
WAR [path live] N processing errors, last was: packets containing single partitions are not supported
```

Often at the same time you will see the path online with **VP8**, for example:

```text
INF [path live] stream is available and online, 1 track (VP8)
```

The **GStreamer / `whip_video` container** may look fine: the pipeline is in `PLAYING`, you may only see `Redistribute latency` or normal loop messages. The warning is emitted by **MediaMTX** while it processes the **ingress WebRTC/VP8 RTP**, not because the publisher container “crashed.”

---

## What it means

- **VP8** over RTP can be packetized in different ways. Some encoders (or stacks) produce **RTP packets that use a “single partition” layout** in a way that **MediaMTX’s reader does not support** for that path, so it logs a **processing error** for those packets and counts them (hence `N processing errors` and a high error rate in the log).
- This is a **compatibility** issue between **how VP8 RTP is sent** (here: GStreamer’s WebRTC / `whipclientsink` negotiation) and **how MediaMTX ingests VP8**, not a generic “Docker is broken” failure.
- The stream may still **connect**; **readers** (e.g. browser WebRTC) can be affected (glitches, or the same warnings while reading). Treat the warning as “ingest is not clean for this VP8 packetization.”

Similar issues are discussed in the wild for **VP8 + MediaMTX** and **non-conformant or odd packetization**; the exact text is **MediaMTX** telling you it is **dropping or failing to process** those VP8 RTP packets.

---

## What this repo does by default

The **`whip_video`** Compose service runs **[`whip-publish`](whip-publish/)** (**`/app/whip-publish`**, GStreamer **`whipclientsink`**). Default env uses **`WHIP_VIDEO_CAPS=video/x-vp8`**, which turns on the **`vp8enc token-partitions=3 …`** branch before WHIP to reduce **“single partitions”** style ingest errors (see [`whip-publish/src/main.rs`](whip-publish/src/main.rs)). The browser still uses **`http://…:8889/<path>/`** (WebRTC read).

If you switch caps (for example **H.264** over WHIP), watch for **B-frame** browser failures and **`Renegotiation is not supported`** on `whipclientsink` — see [README-PITFALLS.md](README-PITFALLS.md) and [README-WHIP-STABILITY.md](README-WHIP-STABILITY.md).

---

## How to control the codec

| Goal | What to set |
|------|-------------|
| **Default — Rust WHIP** (`whip-publish`) | Compose already sets **`command: ['/app/whip-publish']`** and **`WHIP_VIDEO_CAPS=video/x-vp8`**. |
| **VP8 over WHIP** | **`WHIP_VIDEO_CAPS=video/x-vp8`** (adds **`vp8enc`** with extra token partitions in `whip-publish`). |
| **H.264 over WHIP** | **`WHIP_VIDEO_CAPS=video/x-h264`** (watch for **B-frame** browser failures). |
| **Omit `video-caps` on `whipclientsink`** | **`WHIP_VIDEO_CAPS=none`** |

In **Docker Compose**, set `environment` on **`whip_video`**, for example:

```yaml
environment:
  SCALE_WIDTH: '1920'
```

Rebuild the image after changing **`Dockerfile.rust_gstreamer`** or **`whip-publish`**; pure `environment` overrides can be applied with `docker compose up -d` (no rebuild) if the image is unchanged.

Use `gst-inspect-1.0 whipclientsink` in the container to see the full **`video-caps`** property and valid values.

---

## Quick checks

1. **`curl http://127.0.0.1:8888/v3/paths/list`**: if **`inboundFramesInError`** climbs quickly on path **`live`**, MediaMTX is dropping VP8 RTP (often **single partitions**); rebuild **`whip_gstreamer:local`** and recreate **`whip_video`**.  
2. **MediaMTX** logs: **`packets containing single partitions are not supported`** should be **rare** after the default **`vp8enc token-partitions=3`** pipeline; if it is still constant, open an issue with **MediaMTX** + **GStreamer** versions.  
3. **Publisher** logs: the printed pipeline should include **`vp8enc`** when using VP8.

---

## See also

- [README-WHIP-STABILITY.md](README-WHIP-STABILITY.md) — stalls, EOS loop vs **seek** loop, **WHEP DELETE 404**.  
- Main [README.md](README.md) (GStreamer setup, Make targets, WHIP, troubleshooting table).  
- [MediaMTX – WebRTC clients / WHEP](https://mediamtx.org/docs/read/webrtc) for playback URLs.
