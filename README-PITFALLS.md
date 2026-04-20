# Pitfalls we hit (and what actually fixed them)

This file records problems that looked like “no video”, “random 404s”, or “mysterious Docker logs” while wiring **MediaMTX**, **browser WebRTC** (`http://…:8889/<path>/`), and **GStreamer WHIP** publishers. It is meant as a shortcut for the next debugging session—not a replacement for the main [README.md](README.md).

---

## 1. “No video” with a clean browser Network tab

**Symptom:** WebRTC connects, DevTools shows no obvious HTTP failures, but the picture is black or frozen.

**Pitfall:** The failure is often **media/RTP**, not HTTP. **MediaMTX** logs and the **`/v3/paths/list`** API tell the truth.

**What to check:**

- **`inboundFramesInError`** in the path item (via `curl http://127.0.0.1:8888/v3/paths/list`). If it climbs quickly while “connected”, the server is **dropping ingress video** even though the session exists.
- **`docker logs fleet_mediamtx_dev`** for lines such as:
  - **`WebRTC doesn't support H264 streams with B-frames`** (browser reader closed; WHIP path produced **H.264 with B-frames**).
  - **`packets containing single partitions are not supported`** (VP8 RTP layout **MediaMTX rejects** for WHIP/VP8 from **GStreamer** in our setup).
  - **`received a non-starting fragment`** (often tied to **flush/seek** or packetization churn).

**Historical note:** An earlier default in this repo used **FFmpeg → RTSP** (baseline H.264, no B-frames) so the browser still read **`http://…:8889/<path>/`** over WebRTC while ingest avoided WHIP/VP8 RTP quirks; **`inboundFramesInError`** stayed **0** in those checks. The **current default** is **Rust WHIP** ([`whip-publish`](whip-publish/)); watch **`inboundFramesInError`** and MediaMTX logs for VP8/H.264 issues (see [README-MediaMTX-VP8.md](README-MediaMTX-VP8.md)).

---

## 2. WHIP + “restart everything on every file loop”

**Pitfall:** Spawning a **new process** (or a new **`gst-launch`**) after each **EOS** means a **new WHIP publish** each loop. Viewers see **path churn**, buffer gaps, and odd **WHEP** behavior.

**Mitigation we tried:** A **Rust** pipeline that **seeks to 0** on EOS and **drops EOS** before `whipclientsink` ([`whip-publish`](whip-publish/))—better than respawning, but still not the same as “encoder never loops” for every stack.

**Related:** **`DELETE …/whep/<uuid>` → 404** is often **benign** (session already gone, or ordering during reconnect storms)—see [README-WHIP-STABILITY.md](README-WHIP-STABILITY.md).

---

## 3. Rust + GLib + GStreamer: threading and two “glib” worlds

**Pitfall A:** Calling **`glib::idle_add_local`** from a **GStreamer pad probe** (streaming thread) while the **main loop** owns the default **`MainContext`** → panic: *default main context already acquired by another thread*.

**Fix:** Schedule work with **`glib::MainContext::default().invoke(…)`** (same **`gstreamer::glib`** as the bus watch), not **`idle_add_local`** from the probe.

**Pitfall B:** Mixing **`glib::ControlFlow`** from the standalone **`glib`** crate with **`bus.add_watch_local`** on **`gstreamer::glib`** → type mismatch. Use **`gstreamer::glib`** consistently in the WHIP binary.

---

## 4. Pre-encoded H.264 into `whipclientsink` + `h264parse`

**Pitfall:** **`x264enc ! h264parse ! whipclientsink`** triggered **`Renegotiation is not supported`** inside **`whipclientsink`** when caps evolved (first SPS/PPS / parsed fields). Pipeline failed or path went empty.

**Lesson:** “Encode outside WHIP” is not plug-and-play unless you match **exactly** what **`whipclientsink`** accepts without **caps renegotiation** events.

---

## 5. VP8 token partitions vs “single partitions” in MediaMTX logs

**Pitfall:** Negotiating **VP8** over **WHIP** and even adding **`vp8enc`** with **more token partitions** still left **many** **`packets containing single partitions are not supported`** warnings and non-zero **`inboundFramesInError`**—enough for **no visible video** in the browser.

**Lesson:** VP8 **WHIP → MediaMTX** ingest can be finicky (packetization / **`inboundFramesInError`**). If VP8 WHIP stays noisy, try different **`WHIP_VIDEO_CAPS`** / encoder settings or consult MediaMTX + GStreamer issue trackers for your versions.

---

## 6. Browser URL and HTTP `HEAD`

**Pitfall:** **`HEAD http://127.0.0.1:8889/live/`** can return **404** while **`GET`** returns **200** (redirect + HTML player). Tools that only **`HEAD`** look broken.

**Lesson:** Test with **`curl -L`** or a real browser **GET**, not **`curl -I`** alone.

---

## 7. EOS pad probe + `gst_mini_object_unref` CRITICAL

**Pitfall:** **Dropping EOS** on a pad before **`whipclientsink`** can trigger **`GStreamer-CRITICAL … gst_mini_object_unref`** once per loop. Annoying in logs; separate from “no picture” once ingest was healthy.

**Status:** Logged as known noise; a cleaner EOS strategy would be a follow-up.

---

## 8. Docker Compose hygiene

**Pitfall:** Renaming or removing a **Compose service** leaves **orphan** containers until **`docker compose up --remove-orphans`** (or manual `docker rm`).

**Pitfall:** Enabling **`publish`** (Rust WHIP **`whip_video`**) and **`publish-cpp`** (**`whip_publish_cpp`**) at the same time with the same **`STREAM_NAME`** (for example **`live`**) means **two WHIP publishers** fight for one path — broken ingest, confusing logs, or **`inboundFramesInError`**. Use **one** profile per path, or give one service a different **`STREAM_NAME`**.

---

## 9. Stale WebRTC readers

**Pitfall:** Multiple **`readers`** on the same path (old tabs, retries) make logs noisy and can confuse “who is actually playing”.

**Lesson:** Close extra tabs; use **`/v3/paths/list`** to see **`readers`**.

---

## Where to read next

| Topic | Doc |
|--------|-----|
| Default Rust WHIP publisher, optional C++ WHIP | [README.md](README.md) |
| WHIP stability, WHEP DELETE 404 | [README-WHIP-STABILITY.md](README-WHIP-STABILITY.md) |
| VP8 / H.264 MediaMTX warnings | [README-MediaMTX-VP8.md](README-MediaMTX-VP8.md) |
| WHIP seek-loop (Rust) | [whip-publish/](whip-publish/) |
| WHIP seek-loop (C++) | [whip-publish-cpp/](whip-publish-cpp/) |
