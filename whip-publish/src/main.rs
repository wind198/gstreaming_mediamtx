//! Publish a video file to MediaMTX via WHIP — one pipeline, seek on EOS (EOS dropped before whipclientsink).

use std::ffi::OsStr;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::Arc;

use gstreamer::glib::prelude::*;
use gstreamer::glib::{self as glib};
use gstreamer::prelude::*;
use gstreamer::prelude::Cast;
use gstreamer as gst;
use gstreamer::{
    ClockTime, EventType, MessageView, PadProbeReturn, PadProbeType, SeekFlags,
};

fn print_usage() {
    eprintln!(
        "\
USAGE: whip-publish

Environment:
  VIDEO_PATH           Path to MP4/MKV/etc. (required unless default image path exists)
  STREAM_NAME          MediaMTX path (default: live)
  MEDIAMTX_WHIP_BASE   e.g. http://127.0.0.1:8889 or http://mediamtx:8889
  SCALE_WIDTH          Default 1280
  SCALE_HEIGHT         Default 720
  FPS                  Default 30
  WHIP_VIDEO_CAPS      Negotiated codec (empty → video/x-vp8). If it contains \"vp8\", an explicit\n\
                       vp8enc (multi token-partition) runs before WHIP for MediaMTX compatibility.
  WHIP_EXTRA           Extra whipclientsink properties (whitespace-separated)
"
    );
}

fn inspect_whipclientsink() -> bool {
    Command::new("gst-inspect-1.0")
        .args(["whipclientsink"])
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .map(|s| s.success())
        .unwrap_or(false)
}

fn gst_location_fragment(path: &Path) -> String {
    let s = path.to_string_lossy();
    let escaped = s.replace('\\', "\\\\").replace('"', "\\\"");
    if s.contains(' ') {
        format!("\"{}\"", escaped)
    } else {
        escaped
    }
}

fn resolve_video_path() -> PathBuf {
    let raw = std::env::var("VIDEO_PATH").unwrap_or_default();
    let p = PathBuf::from(if raw.is_empty() {
        "/app/test.mp4"
    } else {
        raw.as_str()
    });
    p.canonicalize().unwrap_or_else(|_| p.clone())
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args = std::env::args_os().skip(1).collect::<Vec<_>>();
    if args.iter().any(|a| a == OsStr::new("--help")) || args.iter().any(|a| a == OsStr::new("-h"))
    {
        print_usage();
        return Ok(());
    }

    if !inspect_whipclientsink() {
        eprintln!("GStreamer element \"whipclientsink\" is not available.");
        std::process::exit(1);
    }

    let video_path = resolve_video_path();
    if !video_path.is_file() {
        eprintln!(
            "VIDEO_PATH missing or not found: {}",
            video_path.display()
        );
        std::process::exit(1);
    }

    let stream_name = std::env::var("STREAM_NAME").unwrap_or_else(|_| "live".into());
    let base = std::env::var("MEDIAMTX_WHIP_BASE").unwrap_or_else(|_| "http://127.0.0.1:8889".into());
    let base = base.trim_end_matches('/');
    let whip_endpoint = format!("{}/{}/whip", base, stream_name);

    let fps = std::env::var("FPS").unwrap_or_else(|_| "30".into());
    let sw = std::env::var("SCALE_WIDTH").unwrap_or_else(|_| "1280".into());
    let sh = std::env::var("SCALE_HEIGHT").unwrap_or_else(|_| "720".into());

    let whip_video_caps = match std::env::var("WHIP_VIDEO_CAPS") {
        Ok(s) if !s.trim().is_empty() => s.trim().to_string(),
        _ => "video/x-vp8".into(),
    };

    // MediaMTX rejects VP8 that uses only one token partition ("single partitions are not supported").
    // Map token-partitions to libvpx enum: 0=1, 1=2, 2=4, 3=8 partitions — use 3 (eight) for widest compatibility.
    let vp8_branch = whip_video_caps.to_lowercase().contains("vp8");
    let before_qprewhip = if vp8_branch {
        "vp8enc token-partitions=3 error-resilient=partitions keyframe-max-dist=30 deadline=1 auto-alt-ref=false ! video/x-vp8 ! "
    } else {
        ""
    };

    let whip_extra: Vec<String> = std::env::var("WHIP_EXTRA")
        .unwrap_or_default()
        .split_whitespace()
        .map(String::from)
        .collect();

    let mut whip_parts: Vec<String> = vec!["name=whip".into()];
    if whip_video_caps.to_lowercase() != "none" {
        whip_parts.push(format!("video-caps={}", whip_video_caps));
    }
    for t in whip_extra {
        whip_parts.push(t);
    }
    whip_parts.push(format!("signaller::whip-endpoint={}", whip_endpoint));
    let whip_joined = whip_parts.join(" ");

    let loc = gst_location_fragment(&video_path);

    let pipeline_str = format!(
        "filesrc location={loc} ! decodebin ! \
         queue max-size-buffers=200 max-size-time=5000000000 max-size-bytes=0 ! \
         videoconvert ! videoscale ! \
         video/x-raw,width={sw},height={sh} ! \
         videorate ! video/x-raw,format=I420,framerate={fps}/1 ! \
         {before_qprewhip}\
         queue name=qprewhip max-size-buffers=200 max-size-time=5000000000 max-size-bytes=0 ! \
         whipclientsink {whip_joined}"
    );

    eprintln!("Starting WHIP seek-loop publisher:");
    eprintln!("{}", pipeline_str);

    gst::init()?;

    let pipeline = gst::parse::launch(&pipeline_str)?
        .downcast::<gst::Pipeline>()
        .map_err(|_| "expected gst::Pipeline from parse_launch")?;

    let pipeline = Arc::new(pipeline);
    let main_loop = Arc::new(glib::MainLoop::new(None, false));

    let qpre = pipeline
        .by_name("qprewhip")
        .ok_or("missing element qprewhip")?;
    let srcpad = qpre
        .static_pad("src")
        .ok_or("no src pad on qprewhip")?;

    srcpad.add_probe(PadProbeType::EVENT_DOWNSTREAM, {
        let pipeline = Arc::clone(&pipeline);
        let main_loop = Arc::clone(&main_loop);
        move |_pad, info| {
            if let Some(ev) = info.event() {
                if ev.type_() == EventType::Eos {
                    eprintln!("EOS — seek to 0 (EOS dropped before WHIP sink)");
                    let pipeline = Arc::clone(&pipeline);
                    let main_loop = Arc::clone(&main_loop);
                    // Pad probe runs on the streaming thread; `idle_add_local` tries to acquire
                    // the default main context here and panics while the main loop holds it.
                    glib::MainContext::default().invoke(move || {
                        if pipeline
                            .seek_simple(
                                SeekFlags::FLUSH | SeekFlags::KEY_UNIT,
                                ClockTime::ZERO,
                            )
                            .is_err()
                        {
                            eprintln!("seek_simple failed");
                            main_loop.quit();
                        }
                    });
                    return PadProbeReturn::Drop;
                }
            }
            PadProbeReturn::Ok
        }
    });

    let bus = pipeline.bus().expect("pipeline bus");
    bus.add_watch_local({
        let main_loop = Arc::clone(&main_loop);
        move |_bus, msg| {
            match msg.view() {
                MessageView::Error(err) => {
                    eprintln!("Error: {}: {:?}", err.error(), err.debug());
                    main_loop.quit();
                    glib::ControlFlow::Break
                }
                _ => glib::ControlFlow::Continue,
            }
        }
    })?;

    pipeline.set_state(gst::State::Playing)?;

    main_loop.run();
    pipeline.set_state(gst::State::Null)?;

    Ok(())
}
