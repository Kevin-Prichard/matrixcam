#!/usr/bin/env bash

./build/matrixcam \
    --console-dimensions 80x30 \
    --console-resolution 1920x1080 \
    --camera /dev/video0 \
    --drm-device /dev/dri/card1 \
    --camera-config 640x480x30 \
    --font-file fonts/Noto_Serif_Hentaigana/static/NotoSerifHentaigana-Regular.ttf
