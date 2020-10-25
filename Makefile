CFLAGS=`pkg-config gstreamer-1.0 gstreamer-app-1.0 srt --cflags` -O2 -Wall
LDFLAGS=`pkg-config gstreamer-1.0 gstreamer-app-1.0 srt --libs`

belacoder: belacoder.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f belacoder
