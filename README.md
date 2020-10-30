belacoder - live video encoder with dynamic bitrate control and [SRT](https://github.com/Haivision/srt) support
=========

This is a [gstreamer](https://gstreamer.freedesktop.org/)-based encoder with support for [SRT](https://github.com/Haivision/srt) and dynamic bitrate control depending on the network capacity. This means that if needed, the video bitrate is automatically reduced on-the-fly to match the speed of the network connection. The intended application is live video streaming over bonded 4G modems by using it on a single board computer together with a HDMI capture card and [strla](https://github.com/BELABOX/srtla).

belacoder is developed on an NVIDIA Jetson Nano ([Amazon.com](https://amzn.to/3mt2Coz) / [Amazon.co.uk](https://amzn.to/31IOgJ2) / [NVIDIA](https://developer.nvidia.com/embedded/jetson-nano-developer-kit)), and we provide gstreamer pipelines for using its hardware video encoding. However it can also be used on other platforms as long as the correct gstreamer pipeline is provided.


Building
--------

Installing the dependencies on L4T / Ubuntu:

    sudo apt-get install build-essential git libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev # let me know if I forgot anything
    sudo apt-get install libsrt-dev # only available in Ubuntu 20.04

    # alternatively, manually installing SRT on older Ubuntu releases
    git clone https://github.com/BELABOX/srt.git
    cd srt
    ./configure --prefix=/usr/local
    make -j 4
    sudo make install
    sudo ldconfig
    
Building belacoder:

    git clone https://github.com/BELABOX/belacoder.git
    cd belacoder
    make


Usage
-----

    ./belacoder PIPELINE_FILE IP PORT DELAY(ms) [MAX_BITRATE(bps)]

Where:

* `PIPELINE_FILE` is a text file containing the gstreamer pipeline to use. See the `pipeline` directory for ready-made pipelines.
* `IP` is the IP address of the SRT listener to stream to (only applicable when the gstreamer sink is `appsink name=appsink`)
* `PORT` is the port of the SRT listener to stream to (only applicable when the gstreamer sink is `appsink name=appsink`)
* `DELAY` is the delay in milliseconds to add to the audio stream relative to the video (when using the gstreamer pipelines supplied with belacoder)
* `MAX_BITRATE` is an optional argument for setting the maximum **video** bitrate (when using the gstreamer pipelines supplied with belacoder)


Gstreamer pipelines
-------------------

The gstreamer pipelines are available in the `pipeline` directory, organised in machine-specific directories (or `generic` for software encoder / decoder pipelines). The filename format is `CODEC_CAPTUREDEV_[RES[FPS]]`:

* `CODEC` is either `h265` or `h264`
* `CAPTUREDEV` is either `camlink` for Elgato Cam Link 4K ([Amazon.com](https://amzn.to/2Hx3tFM) / [Amazon.co.uk](https://amzn.to/3jp32us)) or other uncompressed YUY2 capture cards or `v4l_mjpeg` for low cost USB2.0 MJPEG capture cards ([Amazon.com](https://amzn.to/31VOTyS) / [Amazon.co.uk](https://amzn.to/3mwlNxU))
* `RES` can be blank - capturing at the highest available resolution, `720p` or `1080p`
* `FPS` can be blank - capturing at the highest available refresh rate, `29.97`, or `30` FPS

Please check the supplied pipelines for examples. Here are a few unorganised tips & pointers:

* belacoder will work with arbitrary gstreamer pipelines as long as they're valid, however for dynamic bitrate control the video encoder **must** have `name=video_enc` and it must have a `bitrate` property changeable in the running state; the sink **must** be `appsink name=appsink`, which will stream to the SRT IP and port specified as command line arguments
* If a `textoverlay` element with `name=overlay` is specificed, then it will be dynamically updated to show the current bitrate
* An `identity name=delay signal-handoffs=TRUE` element can be used to adjust the PTS (presentation timestamp) of a stream by `DELAY` milliseconds. Use it to synchronise the audio and video if needed (e.g. DELAY of around 900 for a Gopro Hero7 with stabilisation enabled)
* The Jetson Nano hardware encoders seem biased towards allocating most of the bitrate budget to I-frames, while heavily compressing P-frames, especially on lower bitrates. This can heavily affect image quality when most of the image is moving and this is why we limit the quantization range in our pipelines using `qp-range`. This range makes a big improvement over the defaults, however in some cases results can probably be further improved with different parameters.
