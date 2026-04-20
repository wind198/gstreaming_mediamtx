# WHIP playback stability and WHEP `DELETE` `404`

This note explains **`DELETE http://…/live/whep/<uuid>` → 404** in browser DevTools, and what this repository does to avoid **viewer stalls** when looping a file.

---

## Publisher in this repo

[`whip-publish`](whip-publish/) (**Rust**, built into the **`whip_gstreamer`** image) runs **one GStreamer pipeline** per process. On file **EOS**, it **drops EOS** before `whipclientsink`, then **`seek`**s to time **0**, so playback loops **without restarting** `gst-launch` or the process. That avoids the worst **WHIP reconnect churn** from a naive “spawn again on EOS” loop.

**Caveat:** A **flush seek** can still interact with WebRTC signalling such that **MediaMTX** sometimes logs **new WebRTC sessions**; that is looser than a never-ending live encoder, but is usually **smoother** than restarting the whole pipeline every loop.

### Built-in browser at `/live` shows no picture (no HTTP errors)

**Default stack:** [`docker-compose.mediamtx.yml`](docker-compose.mediamtx.yml) runs **`whip_video`** with **[`whip-publish`](whip-publish/)** (**`/app/whip-publish`**, GStreamer **`whipclientsink`**). The browser opens **`http://…:8889/live`** (WebRTC **read**). Check **`curl http://127.0.0.1:8888/v3/paths/list`**: **`inboundFramesInError`** should stay low when WHIP ingest is clean.

**Common WHIP ingest issues:**

- **`closed: WebRTC doesn't support H264 streams with B-frames`** — H.264 WHIP bitstream used **B‑frames**; browser WebRTC read fails even when **Network** looks fine.
- **`packets containing single partitions are not supported`** + rising **`inboundFramesInError`** — **VP8** RTP from WHIP can disagree with MediaMTX’s VP8 ingest; default compose sets **`WHIP_VIDEO_CAPS=video/x-vp8`** so **`whip-publish`** inserts **`vp8enc`** with extra token partitions (see [README-MediaMTX-VP8.md](README-MediaMTX-VP8.md)).

---

## Why stalls happened with “restart on EOS” designs

If a publisher **exits or renegotiates WHIP on every file loop** (e.g. **new process** after each EOS), each cycle performs a **new WHIP publish** (`POST …/<path>/whip`). That tears down the previous publisher session. Viewers may see **buffer gaps**, **freezes**, or **reconnect churn**.

---

## Stall earlier than one file length

If playback freezes **before** the file loops, check:

- **Publisher logs:** pipeline **error** vs normal EOS / seek messages.
- **CPU** on the host (encoding inside WebRTC can be heavy).
- **Browser / ICE** (tab backgrounding, firewall).

---

## `DELETE …/<path>/whep/<session-id>` → **404 Not Found**

The built-in **browser** player uses **WHEP** with a **per-session URL** like:

`http://127.0.0.1:8889/live/whep/eba8830e-24a1-4d22-8cd4-395bf658a56d`

When playback stops it may send **`DELETE`** to end the reader session.

**404** on `DELETE` often means:

1. **Session already closed** — e.g. the publisher dropped WHIP first, MediaMTX removed the session, and the browser’s `DELETE` arrives **late**. Often **harmless** in DevTools.
2. **Race during reconnects** — if WHIP sessions churn, `DELETE` can hit a **stale** id.

Older MediaMTX versions also had bugs around **WHEP/WHIP `DELETE` routing** and **`Location`** headers ([e.g. #3176/#3177](https://github.com/bluenviron/mediamtx/issues)); upgrade if you rely on strict semantics.

**Takeaway:** **404 on DELETE** is usually **diagnostic**, not the sole root cause of video stalls; **publisher lifetime** and **how often WHIP is renegotiated** matter more.

---

## What to use when

| Goal | Use |
|------|-----|
| **Default** — loop MP4 to path **`live`** (WHIP ingest, browser WebRTC read) | Docker **`whip_video`** / **`make publish-up`** — **`/app/whip-publish`** ([`whip-publish`](whip-publish/)), `./test.mp4` bind-mount. |
| **Same pipeline in C++** | Service **`whip_publish_cpp`**, profile **`publish-cpp`** — see [README.md](README.md). |
| Local dev (Rust WHIP) | `cd whip-publish && cargo build --release` — requires **GStreamer dev** + **`whipclientsink`** as in [README.md](README.md). |

---

## See also

- [README-PITFALLS.md](README-PITFALLS.md) — pitfalls we hit (no video with clean Network tab, WHIP ingest, threading, etc.).  
- [README-MediaMTX-VP8.md](README-MediaMTX-VP8.md) — VP8 “single partitions” warnings vs **H.264**.  
- [README.md](README.md) — Make targets, Compose, troubleshooting.
