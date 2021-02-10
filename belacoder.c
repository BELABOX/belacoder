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
#include <signal.h>
#include <sys/mman.h>

#include <gst/gst.h>
#include <gst/gstinfo.h>
#include <gst/app/gstappsink.h>
#include <glib-unix.h>

#include <srt.h>

#define SRT_MAX_OHEAD 20 // maximum SRT transmission overhead (when using appsink)

#define MIN_BITRATE (500 * 1000)
#define ABS_MAX_BITRATE (30 * 1000 * 1000)
#define DEF_BITRATE (6 * 1000 * 1000)
#define BITRATE_UPDATE_INT  20 // buffer size sampling interval (ms)
#define BS_INCR_MIN         50 // maximum buffer size to increase the bitrate when the current bitrate is low
#define BS4M_INCR           50 // maximum buffer size at 4Mbps to increase the bitrate
#define BS_DECR_MIN        200 // base value for the scaled BS_DECR (buffer size threshold for reducing the bitrate)
#define BS4M_DECR           80 // value to add to BS_DECR for 4Mbps (scaled proportionally to the current bitrate)
#define BS_DECR_FAST       400 // min buffer size to reduce the bitrate even when the buffer size is already decreasing
#define BITRATE_INCR_STEP  (100*1000) // the bitrate increment step (bps)
#define BITRATE_INCR_INT   350        // the minimum interval for increasing the bitrate (ms)
#define BITRATE_DECR_SCALE  10        // the bitrate is decreased by cur_bitrate/BITRATE_DECR_SCALE
#define BITRATE_DECR_MIN   (100*1000) // the minimum value to decrease the bitrate by (bps)
#define BITRATE_DECR_INT   200        // the minimum interval for decreasing the bitrate (ms)

// settings ranges
#define MAX_SOUND_DELAY 10000
#define MIN_SRT_LATENCY 100
#define MAX_SRT_LATENCY 10000
#define DEF_SRT_LATENCY 2000

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
GMainLoop *loop;
GstElement *encoder, *overlay;
SRTSOCKET sock;

int enc_bitrate_div = 1;

int sound_delay = 0;

int min_bitrate = MIN_BITRATE;
int max_bitrate = DEF_BITRATE;
int cur_bitrate = MIN_BITRATE;

char *bitrate_filename = NULL;

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

int parse_bitrate(char *bitrate_string) {
  int bitrate = strtol(bitrate_string, NULL, 10);
  if (bitrate < MIN_BITRATE || bitrate > ABS_MAX_BITRATE) {
    return -1;
  }
  return bitrate;
}

int read_bitrate_file() {
  FILE *f = fopen(bitrate_filename, "r");
  if (f == NULL) return -1;

  char *buf = NULL;
  size_t buf_sz = 0;
  int br[2];

  for (int i = 0; i < 2; i++) {
    buf_sz = getline(&buf, &buf_sz, f);
    if (buf_sz < 0) goto ret_err;
    br[i] = parse_bitrate(buf);
    if (br[i] < 0) goto ret_err;
  }

  free(buf);
  min_bitrate = br[0];
  max_bitrate = br[1];
  return 0;

ret_err:
  if (buf) free(buf);
  return -2;
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
  int scaled_bs_incr = BS_INCR_MIN + (int)(BS4M_INCR * (double)cur_bitrate/(4*1000*1000));
  int scaled_bs_decr = BS_DECR_MIN + (int)(BS4M_DECR * (double)cur_bitrate/(4*1000*1000));

  int bitrate = cur_bitrate;

  debug("bs: %d max_bs: %d avg_bs: %f, scaled_incr: %d, scaled_decr: %d, bitrate %d\n",
        bs, max_bs, avg_bs, scaled_bs_incr, scaled_bs_decr, cur_bitrate);

  if (max_bs < scaled_bs_incr && ctime > next_bitrate_adj) {
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

  bitrate = min_max(bitrate, min_bitrate, max_bitrate);

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

int init_srt(char *host, char *port, int srt_latency, char *stream_id) {
  struct addrinfo hints;
  struct addrinfo *addrs;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  int ret = getaddrinfo(host, port, &hints, &addrs);
  if (ret != 0) return -1;

  srt_startup();
  sock = srt_create_socket();
  if (sock == SRT_INVALID_SOCK) return -2;

#if SRT_MAX_OHEAD > 0
  // auto, based on input rate
  int64_t max_bw = 0;
  ret = srt_setsockflag(sock, SRTO_MAXBW, &max_bw, sizeof(max_bw));
  assert(ret == 0);

  // overhead(retransmissions)
  int32_t ohead = SRT_MAX_OHEAD;
  ret = srt_setsockflag(sock, SRTO_OHEADBW, &ohead, sizeof(ohead));
  assert(ret == 0);
#endif

  ret = srt_setsockflag(sock, SRTO_LATENCY, &srt_latency, sizeof(srt_latency));
  assert(ret == 0);

  if (stream_id != NULL) {
    ret = srt_setsockflag(sock, SRTO_STREAMID, stream_id, strlen(stream_id));
    assert(ret == 0);
  }

  int connected = -3;
  for (struct addrinfo *addr = addrs; addr != NULL; addr = addr->ai_next) {
    ret = srt_connect(sock, addr->ai_addr, addr->ai_addrlen);
    if (ret == 0) {
      connected = 0;
      continue;
    }
  }
  freeaddrinfo(addrs);

  return connected;
}

void exit_syntax() {
  fprintf(stderr, "Syntax: belacoder PIPELINE_FILE ADDR PORT [options]\n\n");
  fprintf(stderr, "Options:\n");
  fprintf(stderr, "  -d <delay>          Audio delay in milliseconds\n");
  fprintf(stderr, "  -s <streamid>       SRT stream ID\n");
  fprintf(stderr, "  -l <latency>        SRT latency in milliseconds\n");
  fprintf(stderr, "  -b <bitrate file>   Bitrate settings file, see below\n\n");
  fprintf(stderr, "Bitrate settings file syntax:\n");
  fprintf(stderr, "MIN BITRATE (bps)\n");
  fprintf(stderr, "MAX BITRATE (bps)\n---\n");
  fprintf(stderr, "example for 500 Kbps - 60000 Kbps:\n\n");
  fprintf(stderr, "    printf \"500000\\n6000000\" > bitrate_file\n\n");
  fprintf(stderr, "---\n");
  fprintf(stderr, "Send SIGHUP to reload the bitrate settings while running.\n");
  exit(EXIT_FAILURE);
}

static void cb_delay (GstElement *identity, GstBuffer *buffer, gpointer data) {
  buffer = gst_buffer_make_writable(buffer);
  GST_BUFFER_PTS (buffer) += GST_SECOND * sound_delay / 1000;
}

void cb_pipeline (GstBus *bus, GstMessage *message, gpointer user_data) {
  switch(GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR:
      fprintf(stderr, "gstreamer error\n");
      g_main_loop_quit(loop);
      break;
    case GST_MESSAGE_EOS:
      fprintf(stderr, "gstreamer eos\n");
      g_main_loop_quit(loop);
      break;
    default:
      break;
  }
}

#define FIXED_ARGS 3
int main(int argc, char** argv) {
  int opt;
  char *stream_id = NULL;
  int srt_latency = DEF_SRT_LATENCY;

  while ((opt = getopt(argc, argv, "d:b:s:l:")) != -1) {
    switch (opt) {
      case 'b':
        bitrate_filename = optarg;
        break;
      case 'd':
        sound_delay = strtol(optarg, NULL, 10);
        if (sound_delay < -MAX_SOUND_DELAY || sound_delay > MAX_SOUND_DELAY) {
          fprintf(stderr, "Maximum sound delay +/- %d\n\n", MAX_SOUND_DELAY);
          exit_syntax();
        }
        break;
      case 's':
        stream_id = optarg;
        break;
      case 'l':
        srt_latency = strtol(optarg, NULL, 10);
        if (srt_latency < MIN_SRT_LATENCY || srt_latency > MAX_SRT_LATENCY) {
          fprintf(stderr, "The SRT latency must be between %d and %d ms\n\n",
                  MIN_SRT_LATENCY, MAX_SRT_LATENCY);
          exit_syntax();
        }
        break;
      default:
        exit_syntax();
    }
  }

  if (argc - optind != FIXED_ARGS) {
    exit_syntax();
  }


  // Read the pipeline file
  int pipeline_fd = open(argv[optind], O_RDONLY);
  if (pipeline_fd < 0) {
    fprintf(stderr, "Failed to open the pipeline file %s: ", argv[optind]);
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
  GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(gst_pipeline));
  gst_bus_add_signal_watch(bus);
  g_signal_connect(bus, "message", (GCallback)cb_pipeline, gst_pipeline);


  // Optional SRT streaming via an appsink (needed for dynamic video bitrate)
  GstAppSinkCallbacks callbacks = {NULL, NULL, new_buf_cb};
  GstElement *rtlasink = gst_bin_get_by_name(GST_BIN(gst_pipeline), "appsink");
  if (GST_IS_ELEMENT(rtlasink)) {
    gst_app_sink_set_callbacks (GST_APP_SINK(rtlasink), &callbacks, NULL, NULL);
    int ret = init_srt(argv[optind+1], argv[optind+2], srt_latency, stream_id);
    assert(ret == 0);

    len = sizeof(srt_latency);
    ret = srt_getsockflag(sock, SRTO_PEERLATENCY, &srt_latency, &len);
    assert(ret == 0);
    fprintf(stderr, "Negotiated SRT latency: %d ms\n", srt_latency);
  }

  // Optional dynamic video bitrate
  if (bitrate_filename) {
    int ret;
    if ((ret = read_bitrate_file()) != 0) {
      if (ret == -1) {
        fprintf(stderr, "Failed to read the bitrate settings file %s\n", bitrate_filename);
      } else {
        fprintf(stderr, "Failed to read valid bitrate settings from %s\n", bitrate_filename);
      }
      exit_syntax();
    }
  }
  cur_bitrate = max_bitrate;
  fprintf(stderr, "Max bitrate: %d\n", max_bitrate);
  signal(SIGHUP, (__sighandler_t)read_bitrate_file);

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
  fprintf(stderr, "Sound delay: %d ms\n", sound_delay);
  GstElement *identity_elem = gst_bin_get_by_name(GST_BIN(gst_pipeline), "delay");
  if (GST_IS_ELEMENT(identity_elem)) {
    g_object_set(G_OBJECT(identity_elem), "signal-handoffs", TRUE, NULL);
    g_signal_connect(identity_elem, "handoff", G_CALLBACK(cb_delay), NULL);
  } else {
    fprintf(stderr, "Failed to get a delay element from the pipeline, not applying a delay\n");
  }


  loop = g_main_loop_new (NULL, FALSE);
  /* If the gstreamer pipeline encounters an error, attempt to restart it
     This could happen for example if the capture card is momentary unplugged */
  while(1) {
    // Everything good so far, start the gstreamer pipeline
    gst_element_set_state((GstElement*)gst_pipeline, GST_STATE_PLAYING);
    g_main_loop_run(loop);
    gst_element_set_state((GstElement*)gst_pipeline, GST_STATE_NULL);
  }

  return 0;
}
