#include <gst/gst.h>
#include <glib.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

void print_usage() {
  std::cerr << R"(USAGE: whip-publish-cpp

Environment:
  VIDEO_PATH           Path to input video
  STREAM_NAME          MediaMTX stream path
  MEDIAMTX_WHIP_BASE   WHIP server base URL
  SCALE_WIDTH          Output width
  SCALE_HEIGHT         Output height
  FPS                  Output frame rate
  WHIP_VIDEO_CAPS      Codec caps (default video/x-vp8; use "none" to omit video-caps)
  WHIP_EXTRA           Extra whipclientsink args (whitespace-separated)
)";
}

bool inspect_whipclientsink() {
  const int r = std::system("gst-inspect-1.0 whipclientsink >/dev/null 2>&1");
  return r == 0;
}

std::string gst_location_fragment(const std::string &path) {
  std::string escaped;
  escaped.reserve(path.size() + 8);
  for (char c : path) {
    if (c == '\\')
      escaped += "\\\\";
    else if (c == '"')
      escaped += "\\\"";
    else
      escaped += c;
  }
  if (path.find(' ') != std::string::npos)
    return "\"" + escaped + "\"";
  return escaped;
}

std::string getenv_or(const char *key, const std::string &def) {
  const char *v = std::getenv(key);
  return v ? std::string(v) : def;
}

std::string trim(const std::string &s) {
  size_t a = 0;
  while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a])))
    ++a;
  size_t b = s.size();
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
    --b;
  return s.substr(a, b - a);
}

std::string to_lower(std::string s) {
  for (char &c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool is_regular_file(const std::string &p) {
  return g_file_test(p.c_str(), G_FILE_TEST_IS_REGULAR) != FALSE;
}

std::string resolve_video_path() {
  std::string raw = getenv_or("VIDEO_PATH", "");
  std::string p = raw.empty() ? "/app/test.mp4" : raw;

  char *resolved = realpath(p.c_str(), nullptr);
  if (resolved) {
    std::string out(resolved);
    free(resolved);
    return out;
  }
  return p;
}

struct AppCtx {
  GstElement *pipeline;
  GMainLoop *loop;
};

static gboolean do_seek_idle(gpointer user_data) {
  auto *app = static_cast<AppCtx *>(user_data);
  if (!gst_element_seek_simple(app->pipeline, GST_FORMAT_TIME,
                               static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH |
                                                         GST_SEEK_FLAG_KEY_UNIT),
                               0)) {
    std::cerr << "seek failed\n";
    g_main_loop_quit(app->loop);
  }
  return G_SOURCE_REMOVE;
}

static GstPadProbeReturn eos_probe_cb(GstPad * /*pad*/, GstPadProbeInfo *info,
                                      gpointer user_data) {
  auto *app = static_cast<AppCtx *>(user_data);

  if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
    GstEvent *event = gst_pad_probe_info_get_event(info);
    if (event && GST_EVENT_TYPE(event) == GST_EVENT_EOS) {
      std::cerr << "EOS detected → seeking to start\n";
      g_main_context_invoke(g_main_context_default(), do_seek_idle, app);
      return GST_PAD_PROBE_DROP;
    }
  }
  return GST_PAD_PROBE_OK;
}

static gboolean bus_call(GstBus * /*bus*/, GstMessage *msg, gpointer user_data) {
  auto *loop = static_cast<GMainLoop *>(user_data);

  switch (GST_MESSAGE_TYPE(msg)) {
  case GST_MESSAGE_ERROR: {
    GError *err = nullptr;
    gchar *dbg = nullptr;
    gst_message_parse_error(msg, &err, &dbg);
    std::cerr << "GStreamer error: " << (err ? err->message : "?") << "\n";
    if (dbg)
      std::cerr << "Debug: " << dbg << "\n";
    if (err)
      g_error_free(err);
    g_free(dbg);
    g_main_loop_quit(loop);
    break;
  }
  default:
    break;
  }
  return TRUE;
}

} // namespace

int main(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
      print_usage();
      return 0;
    }
  }

  if (!inspect_whipclientsink()) {
    std::cerr << "Missing whipclientsink plugin\n";
    return 1;
  }

  const std::string video_path = resolve_video_path();
  if (!is_regular_file(video_path)) {
    std::cerr << "Invalid VIDEO_PATH: " << video_path << "\n";
    return 1;
  }

  const std::string stream_name = getenv_or("STREAM_NAME", "live");
  std::string base = getenv_or("MEDIAMTX_WHIP_BASE", "http://127.0.0.1:8889");
  while (!base.empty() && base.back() == '/')
    base.pop_back();

  const std::string whip_endpoint = base + "/" + stream_name + "/whip";

  const std::string fps = getenv_or("FPS", "30");
  const std::string sw = getenv_or("SCALE_WIDTH", "1280");
  const std::string sh = getenv_or("SCALE_HEIGHT", "720");

  std::string whip_video_caps;
  {
    const std::string raw = getenv_or("WHIP_VIDEO_CAPS", "");
    const std::string t = trim(raw);
    whip_video_caps = t.empty() ? "video/x-vp8" : t;
  }

  const bool vp8_branch = to_lower(whip_video_caps).find("vp8") != std::string::npos;

  const std::string before_qprewhip =
      vp8_branch ? "vp8enc token-partitions=3 error-resilient=partitions "
                     "keyframe-max-dist=30 deadline=1 auto-alt-ref=false ! "
                     "video/x-vp8 ! "
                   : "";

  std::vector<std::string> whip_extra;
  {
    std::istringstream iss(getenv_or("WHIP_EXTRA", ""));
    std::string tok;
    while (iss >> tok)
      whip_extra.push_back(tok);
  }

  std::ostringstream whip_parts;
  whip_parts << "name=whip";
  if (to_lower(whip_video_caps) != "none")
    whip_parts << " video-caps=" << whip_video_caps;
  for (const auto &t : whip_extra)
    whip_parts << " " << t;
  whip_parts << " signaller::whip-endpoint=" << whip_endpoint;
  const std::string whip_joined = whip_parts.str();

  const std::string loc = gst_location_fragment(video_path);

  std::ostringstream pipeline_ss;
  pipeline_ss << "filesrc location=" << loc << " ! decodebin ! "
              << "queue max-size-buffers=200 max-size-time=5000000000 ! "
              << "videoconvert ! videoscale ! "
              << "video/x-raw,width=" << sw << ",height=" << sh << " ! "
              << "videorate ! video/x-raw,format=I420,framerate=" << fps << "/1 ! "
              << before_qprewhip
              << "queue name=qprewhip max-size-buffers=200 max-size-time=5000000000 ! "
              << "whipclientsink " << whip_joined;

  const std::string pipeline_str = pipeline_ss.str();
  std::cerr << "Pipeline:\n" << pipeline_str << "\n";

  gst_init(&argc, &argv);

  GError *err = nullptr;
  GstElement *pipeline = gst_parse_launch(pipeline_str.c_str(), &err);
  if (!pipeline) {
    std::cerr << "gst_parse_launch failed: " << (err ? err->message : "?") << "\n";
    if (err)
      g_error_free(err);
    return 1;
  }
  if (!GST_IS_PIPELINE(pipeline)) {
    std::cerr << "expected GstPipeline\n";
    gst_object_unref(pipeline);
    return 1;
  }

  GstElement *qpre = gst_bin_get_by_name(GST_BIN(pipeline), "qprewhip");
  if (!qpre) {
    std::cerr << "missing qprewhip\n";
    gst_object_unref(pipeline);
    return 1;
  }
  GstPad *srcpad = gst_element_get_static_pad(qpre, "src");
  if (!srcpad) {
    std::cerr << "missing src pad\n";
    gst_object_unref(qpre);
    gst_object_unref(pipeline);
    return 1;
  }

  GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
  AppCtx app{};
  app.pipeline = pipeline;
  app.loop = loop;

  gst_pad_add_probe(srcpad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                    eos_probe_cb, &app, nullptr);

  GstBus *bus = gst_element_get_bus(pipeline);
  gst_bus_add_watch(bus, bus_call, loop);
  gst_object_unref(bus);

  gst_object_unref(srcpad);
  gst_object_unref(qpre);

  GstStateChangeReturn ret =
      gst_element_set_state(pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    std::cerr << "PLAYING failed\n";
    gst_object_unref(pipeline);
    g_main_loop_unref(loop);
    return 1;
  }

  g_main_loop_run(loop);

  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  g_main_loop_unref(loop);

  return 0;
}
