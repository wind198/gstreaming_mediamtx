# GStreamer on Ubuntu 22.04 LTS (Jammy)

This guide targets **Ubuntu 22.04** (“Jammy”), which ships **GStreamer 1.20.x** from the default archive. Commands use `apt`; adjust if you use another package manager.

References: [Ubuntu `gstreamer1.0-tools` (Jammy)](https://packages.ubuntu.com/jammy/gstreamer1.0-tools), [GStreamer Rust bindings — Linux install](https://gitlab.freedesktop.org/gstreamer/gstreamer-rs/-/blob/main/README.md), [`gst-plugins-rs` build instructions](https://github.com/GStreamer/gst-plugins-rs/blob/main/README.md), [`whipclientsink` element docs](https://gstreamer.freedesktop.org/documentation/rswebrtc/whipclientsink.html).

---

## 1. Core GStreamer (packages)

Install the runtime plugins most apps need, command-line tools, and headers for compiling against GStreamer:

```bash
sudo apt update
sudo apt install -y \
  gstreamer1.0-tools \
  gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly \
  gstreamer1.0-libav \
  libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev \
  libgstreamer-plugins-bad1.0-dev \
  pkg-config
```

Optional but common for desktop playback and cameras:

```bash
sudo apt install -y \
  gstreamer1.0-x \
  gstreamer1.0-alsa \
  gstreamer1.0-pulseaudio \
  gstreamer1.0-gl \
  gstreamer1.0-gtk3
```

That matches the usual **Debian/Ubuntu** set recommended in the **gstreamer-rs** README for building bindings and plugins.

---

## 2. Verify the install

```bash
gst-inspect-1.0 --version
gst-launch-1.0 --version
```

Smoke test (test source to display or fakesink):

```bash
gst-launch-1.0 videotestsrc num-buffers=30 ! videoconvert ! autovideosink
# headless / CI:
gst-launch-1.0 videotestsrc num-buffers=30 ! fakesink
```

You should see plugin introspection working (`gst-inspect-1.0 videotestsrc`, etc.).

---

## 3. WHIP publishing (`whipclientsink`) — Rust plugins (`gst-plugins-rs`)

Stock Ubuntu **does not** ship the Rust plugin that provides **`whipclientsink`**. That element lives in **[gst-plugins-rs](https://github.com/GStreamer/gst-plugins-rs)** (WebRTC / WHIP stack). The **`whip-publish`** binary and **`Dockerfile.rust_gstreamer`** verify it with `gst-inspect-1.0 whipclientsink`.

### 3.1 Build toolchain

Use **rustup** so your Rust version meets the **MSRV** of `gst-plugins-rs`. On the **0.14** release line, the workspace currently declares **Rust 1.83+**; the `rustc` in Ubuntu 22.04’s repositories is too old for that branch.

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
source "$HOME/.cargo/env"
rustup update stable
rustc --version
```

Install helpers used by upstream `gst-plugins-rs`:

```bash
cargo install cargo-c
```

Install typical native dependencies for building WebRTC-related code on Ubuntu (aligns with `pkg-config` requirements from the **`gst-plugin-webrtc`** crate: GStreamer ≥ 1.20, WebRTC stack):

```bash
sudo apt install -y \
  build-essential \
  git \
  clang \
  libssl-dev \
  libglib2.0-dev \
  libgstreamer-plugins-bad1.0-dev \
  libgstreamer-plugins-base1.0-dev \
  libgstreamer1.0-dev
```

### 3.2 Build and install the WebRTC plugin crate

Clone a **release tag** or branch that matches your needs (see [gst-plugins-rs releases](https://github.com/GStreamer/gst-plugins-rs)):

```bash
git clone https://github.com/GStreamer/gst-plugins-rs.git
cd gst-plugins-rs
git checkout 0.14   # example: use a stable release branch; adjust as needed
```

Build the crate that contains the WHIP client sink (package name in the repo: **`gst-plugin-webrtc`**):

```bash
cargo cbuild -p gst-plugin-webrtc --release
```

Install system-wide (multi-arch Ubuntu lib dir shown):

```bash
sudo cargo cinstall -p gst-plugin-webrtc --release \
  --prefix=/usr \
  --libdir=/usr/lib/x86_64-linux-gnu
```

Or install only for your user (no `sudo`): install to `$HOME/.local` and extend **`GST_PLUGIN_PATH`** (see below).

### 3.3 Verify `whipclientsink`

The WebRTC Rust plugin installs as **`libgstrswebrtc.so`** (plugin name **`rswebrtc`**). Check the plugin, then the element:

```bash
gst-inspect-1.0 rswebrtc
gst-inspect-1.0 whipclientsink
```

If nothing is found, ensure the plugin `.so` is under a path GStreamer scans (e.g. `/usr/lib/x86_64-linux-gnu/gstreamer-1.0/`), or point **`GST_PLUGIN_PATH`** at the directory that contains **`libgstrswebrtc.so`** after `cargo cbuild` (layout depends on target triple, often under `target/<triple>/release/`).

---

## 4. Run this repo’s publisher (Rust + GStreamer WHIP by default)

With MediaMTX up (see `docker-compose.mediamtx.yml` and `mediamtx.yml`), the **`whip_video`** service runs **[`whip-publish`](whip-publish/)** (**`/app/whip-publish`**) and publishes **`VIDEO_PATH`** to MediaMTX over **WHIP** (`POST` to **`MEDIAMTX_WHIP_BASE/<STREAM_NAME>/whip`**). The browser still opens **`http://<host>:8889/<STREAM_NAME>`** for **WebRTC read**. See [README-WHIP-STABILITY.md](README-WHIP-STABILITY.md) and [README-PITFALLS.md](README-PITFALLS.md) for VP8 ingest quirks, **`inboundFramesInError`**, and WHEP **`DELETE` 404** behavior.

**Docker Compose** (bind-mounts `./test.mp4` → `/media/video.mp4`):

```bash
docker compose -f docker-compose.mediamtx.yml --profile publish up -d --build whip_video
```

Env on **`whip_video`** (Rust): `VIDEO_PATH`, `STREAM_NAME`, **`MEDIAMTX_WHIP_BASE`**, **`SCALE_WIDTH`**, **`SCALE_HEIGHT`**, **`FPS`**, **`WHIP_VIDEO_CAPS`**, **`WHIP_EXTRA`**.

Local build: `cd whip-publish && cargo build --release` (needs **GStreamer dev** + **`whipclientsink`** / **gst-plugins-rs** as in this README).

**Optional — same pipeline in C++**: image **`whip_cpp_gstreamer:local`** from **[`Dockerfile.cpp_whip`](Dockerfile.cpp_whip)**, binary **`/app/whip-publish-cpp`** in **[`whip-publish-cpp/`](whip-publish-cpp/)**. Compose service **`whip_publish_cpp`** uses profile **`publish-cpp`** only:

```bash
docker compose -f docker-compose.mediamtx.yml --profile publish-cpp up -d --build whip_publish_cpp
```

Convenience: **`make build-cpp`** / **`make publish-cpp-up`**. Do **not** run **`whip_video`** (Rust WHIP, profile **`publish`**) and **`whip_publish_cpp`** together on the same **`STREAM_NAME`** (for example both publishing **`live`**).

---

## 5. Testing with Make

The [Makefile](Makefile) wraps Docker Compose. From the repo root, run **`make`** (or **`make help`**) to list targets. Typical flow:

1. **Start MediaMTX** (once per session):

   ```bash
   make up
   ```

2. **Build the publisher image** (after Dockerfile or `whip-publish/` changes):

   ```bash
   make build
   ```

3. **Smoke-test the HTTP API**:

   ```bash
   make test-api
   ```

4. **Run timed publisher test** (Compose runs a one-off container; exit **124** means the `timeout` stopped a still-running process, which is normal):

   | Target | What it checks |
   |--------|----------------|
   | `make test-video-whip-seek` | Timed **`/app/whip-publish`** (WHIP path **`live`**); requires `./test.mp4` and MediaMTX |

   Override duration: **`make test-video-whip-seek TIMEOUT=90`**.

   Playback stalls / **WHEP DELETE 404**: see [README-WHIP-STABILITY.md](README-WHIP-STABILITY.md).

5. **Watch logs** while testing:

   ```bash
   make logs
   ```

   (Ctrl+C to stop tailing.)

6. **Bring up the publisher** (Rust WHIP on path **`live`**):

   - `make publish-up` or **`make publish-stable-up`** (same Compose service).

   Then open the browser URLs below. **`make publish-down`** stops the compose project.

---

## 6. Watch in a browser (WebRTC / WHEP)

Assume `STREAM_NAME` is `live` and MediaMTX WebRTC listens on **8889** (see `mediamtx.yml`).

| Method | URL |
|--------|-----|
| **Built-in WebRTC page** (easiest in a browser) | `http://<host>:8889/live` |
| **WHEP** (for players/libraries that support WHEP, e.g. GStreamer `whepsrc`, FFmpeg; not “paste in the address bar”) | `http://<host>:8889/live/whep` |

Replace `live` with your path name if you use another `STREAM_NAME`. From the host machine with default compose ports, use `http://127.0.0.1:8889/live`.

---

## 7. Troubleshooting

Long debugging arc (WHIP ingest, VP8 partitions, H.264 B-frames, GLib threading): [README-PITFALLS.md](README-PITFALLS.md).

| Issue | What to try |
|--------|----------------|
| `gst-inspect-1.0` finds no element | Confirm plugin packages are installed; run `gst-inspect-1.0` with no args and scroll for your plugin name. |
| Rust build failures / MSRV errors | Upgrade via `rustup update stable`; use a **tagged** `gst-plugins-rs` release branch matching your GStreamer version. |
| Plugin built but not discovered | Set **`GST_PLUGIN_PATH`** to the directory containing the built `libgstwebrtc*.so` (name may vary slightly by version). |
| WHIP fails at runtime | Firewall / UDP (**8189**), TCP (**8188**), and HTTP WHIP (**8889**) must reach MediaMTX; match `webrtc*` settings in `mediamtx.yml`. |
| MediaMTX: `packets containing single partitions are not supported` / high **`inboundFramesInError`** (VP8 WHIP) | Ensure **`WHIP_VIDEO_CAPS`** uses VP8 with the repo’s **`vp8enc`** branch; see [README-MediaMTX-VP8.md](README-MediaMTX-VP8.md) and [README-WHIP-STABILITY.md](README-WHIP-STABILITY.md). |
| Playback stalls, **`DELETE …/whep/<uuid>` → 404** | **404 on DELETE** is often harmless (session already closed). Publisher lifetime / WHIP churn: [README-WHIP-STABILITY.md](README-WHIP-STABILITY.md). |

---

## 8. Optional: unrelated helper package

Ubuntu also packages **`simple-whip-client`** (a small WHIP-related tool). That is **not** the same as registering **`whipclientsink`** inside GStreamer; you still need **`gst-plugins-rs`** as above for this repo’s pipeline.
