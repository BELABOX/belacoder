/*
    belacoder - live video encoder with dynamic bitrate control
    Copyright (C) 2020 BELABOX project

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <assert.h>
#include <sys/mman.h>

#include <gst/gst.h>
#include <gst/gstinfo.h>
#include <gst/app/gstappsink.h>
#include <glib-unix.h>

#include <srt.h>

#define SRT_MAX_OHEAD 20 // maximum SRT transmission overhead (when using appsink)

#define MIN_BITRATE (500 * 1000)
#define DEF_BITRATE (6 * 1000 * 1000)
#define BITRATE_UPDATE_INT  20 // buffer size sampling interval (ms)
#define BS_INCR_MIN         12 // maximum buffer size to increase the bitrate when the current bitrate is low
#define BS4M_INCR           40 // maximum buffer size at 4Mbps to increase the bitrate
#define BS_DECR_MIN        100 // base value for the scaled BS_DECR (buffer size threshold for reducing the bitrate)
#define BS4M_DECR           50 // value to add to BS_DECR for 4Mbps (scaled proportionally to the current bitrate)
#define BS_DECR_FAST       500 // min buffer size to reduce the bitrate even when the buffer size is already decreasing
#define BITRATE_INCR_STEP  (100*1000) // the bitrate increment step (bps)
#define BITRATE_INCR_INT   750        // the minimum interval for increasing the bitrate (ms)
#define BITRATE_DECR_SCALE  10        // the bitrate is decreased by cur_bitrate/BITRATE_DECR_SCALE
#define BITRATE_DECR_MIN   (100*1000) // the minimum value to decrease the bitrate by (bps)
#define BITRATE_DECR_INT   500        // the minimum interval for decreasing the bitrate (ms)

#define MAX_SOUND_DELAY 10000

#define min(a, b) ((a < b) ? a : b)
#define max(a, b) ((a > b) ? a : b)
#define min_max(a, l, h) (max(min((a), (h)), (l)))

//#define DEBUG 1
#ifdef DEBUG
  #define debug(...) fprintf (stderr, __VA_ARGS__)
#else
  #define debug(...)
#endif

static GstPipeline *gst_pipeline = NULL;
GstElement *encoder, *overlay;
SRTSOCKET sock;
struct sockaddr_in dest;

int enc_bitrate_div = 1;

int sound_delay = 0;

int max_bitrate;
int cur_bitrate = MIN_BITRATE;

uint64_t getms() {
  struct timespec time = {0, 0};
  assert(clock_gettime(CLOCK_MONOTONIC_RAW, &time) == 0);
  return time.tv_sec * 1000 + time.tv_nsec / 1000 / 1000;
}

void update_overlay(int bitrate) {
  if (GST_IS_ELEMENT(overlay)) {
    char overlay_text[100];
    snprintf(overlay_text, 100, "bitrate: %d", bitrate/1000);
    g_object_set (G_OBJECT(overlay), "text", overlay_text, NULL);
  }
}

void update_bitrate() {
  static double avg_bs = 0;
  static double prev_bs = 0;
  static int max_bs = 0;
  static uint64_t next_bitrate_check = 0;
  static uint64_t next_bitrate_adj = 0;

  uint64_t ctime = getms();
  if (ctime < next_bitrate_check) {
    return;
  }
  next_bitrate_check = ctime + BITRATE_UPDATE_INT;

  int bs = -1;
  int sz = sizeof(bs);
  int ret = srt_getsockflag(sock, SRTO_SNDDATA, &bs, &sz);

  if (ret < 0 || bs < 0) {
    return;
  }

  if (bs > max_bs) {
    max_bs = bs;
  }
  // Rolling average
  avg_bs = avg_bs*0.90 + ((double)bs) * 0.10;

  // Scale the buffer size thresholds depending on the current bitrate
  double scaled_incr_bs = max(BS_INCR_MIN, BS4M_INCR * (double)cur_bitrate/(4*1000*1000));
  int scaled_bs_decr = BS_DECR_MIN + (int)(BS4M_DECR * (double)cur_bitrate/(4*1000*1000));

  int bitrate = cur_bitrate;

  debug("bs: %d max_bs: %d avg_bs: %f, scaled_target: %f, bitrate %d\n",
        bs, max_bs, avg_bs, scaled_incr_bs, cur_bitrate);

  if (max_bs < scaled_incr_bs && ctime > next_bitrate_adj) {
    bitrate += BITRATE_INCR_STEP;
    next_bitrate_adj = ctime + BITRATE_INCR_INT;
  } else if (bs > scaled_bs_decr && ctime > next_bitrate_adj && (avg_bs > prev_bs || bs > BS_DECR_FAST)) {
    bitrate -= max(bitrate/BITRATE_DECR_SCALE, BITRATE_DECR_MIN);
    next_bitrate_adj = ctime + BITRATE_DECR_INT;
    /* reset the max bs so we stop decreasing the bitrate
       as soon as the buffer size goes below the threshold */
    max_bs = 0;
  } else {
    max_bs = max_bs * 95 / 100;
  }
  prev_bs = avg_bs;

  bitrate = min_max(bitrate, MIN_BITRATE, max_bitrate);

  if (bitrate != cur_bitrate) {
    cur_bitrate = bitrate;

    // round the bitrate we set to 100 kbps
    bitrate = bitrate / (100 * 1000) * (100 * 1000);
    g_object_set (G_OBJECT(encoder), "bitrate", bitrate / enc_bitrate_div, NULL);

    update_overlay(bitrate);

    debug("set bitrate to %d, internal value %d\n", bitrate, cur_bitrate);
  }
}

#define SRT_PKT_SIZE 1316
GstFlowReturn new_buf_cb(GstAppSink *sink, gpointer user_data) {
  static char pkt[SRT_PKT_SIZE];
  static int pkt_len = 0;

  GstSample *sample = gst_app_sink_pull_sample(sink);

  if (!sample) exit(1);

  // We can only update the bitrate when we have an appsink and a configurable video_enc
  if (GST_IS_ELEMENT(encoder)) {
    update_bitrate();
  }

  GstBuffer *buffer = NULL;
  GstMapInfo map = {0};

  buffer = gst_sample_get_buffer(sample);
  gst_buffer_map(buffer, &map, GST_MAP_READ);

  // We send SRT_PKT_SIZE size packets, splitting and merging samples if needed
  int sample_sz = map.size;
  do {
    int copy_sz = min(SRT_PKT_SIZE - pkt_len, sample_sz);
    memcpy((void *)pkt + pkt_len, map.data, copy_sz);
    pkt_len += copy_sz;

    if (pkt_len == SRT_PKT_SIZE) {
      int nb = srt_send(sock, pkt, SRT_PKT_SIZE);
      assert(nb == SRT_PKT_SIZE);
      pkt_len = 0;
    }

    sample_sz -= copy_sz;
  } while(sample_sz);

  gst_buffer_unmap(buffer, &map);
  gst_sample_unref (sample);

  return GST_FLOW_OK;
}

int parse_ip(struct sockaddr_in *addr, char *ip_str) {
  in_addr_t ip = inet_addr(ip_str);
  if (ip == -1) return -1;

  memset(addr, 0, sizeof(*addr));
  addr->sin_family = AF_INET; 
  addr->sin_addr.s_addr = ip;

  return 0;
}

int parse_ip_port(struct sockaddr_in *addr, char *ip_str, char *port_str) {
  if (parse_ip(addr, ip_str) != 0) return -1;

  int port = strtol(port_str, NULL, 10);
  if (port <= 0 || port > 65535) return -2;
  addr->sin_port = htons(port);

  return 0;
}

void init_srt(char *ip, char *port) {
  srt_startup();

  int ret = parse_ip_port(&dest, ip, port);
  assert(ret == 0);

  sock = srt_create_socket();

  // auto, based on input rate
  int64_t max_bw = 0;
  ret = srt_setsockflag(sock, SRTO_MAXBW, &max_bw, sizeof(max_bw));
  assert(ret == 0);

  // overhead(retransmissions)
  int32_t ohead = SRT_MAX_OHEAD;
  ret = srt_setsockflag(sock, SRTO_OHEADBW, &ohead, sizeof(ohead));
  assert(ret == 0);

  ret = srt_connect(sock, (struct sockaddr*)&dest, sizeof(dest));
  assert(ret == 0);
}

void exit_syntax() {
  fprintf(stderr, "Syntax: belacoder PIPELINE_FILE IP PORT DELAY(ms) [MAX_BITRATE(bps)]\n");
  exit(EXIT_FAILURE);
}

static void cb_delay (GstElement *identity, GstBuffer *buffer, gpointer data) {
   buffer = gst_buffer_make_writable(buffer);
   GST_BUFFER_PTS (buffer) += GST_SECOND * sound_delay / 1000;
}

int main(int argc, char** argv) {
  if (argc < 5 || argc > 6) {
    exit_syntax();
  }


  // Read the pipeline file
  int pipeline_fd = open(argv[1], O_RDONLY);
  if (pipeline_fd < 0) {
    fprintf(stderr, "Failed to open the pipeline file %s: ", argv[1]);
    perror("");
    exit(EXIT_FAILURE);
  }
  int len = lseek(pipeline_fd, 0, SEEK_END);
  char *launch_string = mmap(0, len, PROT_READ, MAP_PRIVATE, pipeline_fd, 0);
  fprintf(stderr, "Gstreamer pipeline: %s\n", launch_string);

  gst_init (&argc, &argv);
  GError *error = NULL;
  gst_pipeline  = (GstPipeline*) gst_parse_launch(launch_string, &error);
  if (gst_pipeline == NULL) {
    g_print( "Failed to parse launch: %s\n", error->message);
    return -1;
  }
  if (error) g_error_free(error);


  // Optional SRT streaming via an appsink (needed for dynamic video bitrate)
  GstAppSinkCallbacks callbacks = {NULL, NULL, new_buf_cb};
  GstElement *rtlasink = gst_bin_get_by_name(GST_BIN(gst_pipeline), "appsink");
  if (GST_IS_ELEMENT(rtlasink)) {
    gst_app_sink_set_callbacks (GST_APP_SINK(rtlasink), &callbacks, NULL, NULL);
    init_srt(argv[2], argv[3]);
  }


  // Optional dynamic video bitrate
  if (argc == 5) {
    max_bitrate = DEF_BITRATE;
  } else if (argc == 6) {
    max_bitrate = strtol(argv[5], NULL, 10);
    if (max_bitrate < MIN_BITRATE || max_bitrate > 30*1024*1024) {
      fprintf(stderr, "Invalid bitrate %d\n", max_bitrate);
      exit_syntax();
    }
  }
  cur_bitrate = max_bitrate;
  fprintf(stderr, "Max bitrate: %d\n", max_bitrate);

  encoder = gst_bin_get_by_name(GST_BIN(gst_pipeline), "venc_bps");
  if (!GST_IS_ELEMENT(encoder)) {
    encoder = gst_bin_get_by_name(GST_BIN(gst_pipeline), "venc_kbps");
    enc_bitrate_div = 1000;
  }
  if (GST_IS_ELEMENT(encoder)) {
    g_object_set (G_OBJECT(encoder), "bitrate", cur_bitrate / enc_bitrate_div, NULL);
  } else {
    fprintf(stderr, "Failed to get an encoder element from the pipeline, "
                    "no dynamic bitrate control\n");
    encoder = NULL;
  }


  // Optional bitrate overlay
  overlay = gst_bin_get_by_name(GST_BIN(gst_pipeline), "overlay");
  update_overlay(cur_bitrate);


  // Optional sound delay via an identity element
  sound_delay = strtol(argv[4], NULL, 10);
  fprintf(stderr, "Sound delay: %d ms\n", sound_delay);
  if (sound_delay < -MAX_SOUND_DELAY || sound_delay > MAX_SOUND_DELAY) {
    fprintf(stderr, "Maximum sound delay +/- %d\n", MAX_SOUND_DELAY);
    exit_syntax();
  }
  GstElement *identity_elem = gst_bin_get_by_name(GST_BIN(gst_pipeline), "delay");
  if (GST_IS_ELEMENT(identity_elem)) {
    g_object_set(G_OBJECT(identity_elem), "signal-handoffs", TRUE, NULL);
    g_signal_connect(identity_elem, "handoff", G_CALLBACK(cb_delay), NULL);
  } else {
    fprintf(stderr, "Failed to get a delay element from the pipeline, not applying a delay\n");
  }


  // Everything good so far, start the gstreamer pipeline
  gst_element_set_state((GstElement*)gst_pipeline, GST_STATE_PLAYING);


  while(1) {
    sleep(1);
  }

  return 0;
}
