#!/bin/bash
rm -rf release
mkdir -p release

cp -rf Lavfi tests presets *.{hpp,cpp,txt,json,md,sh} LICENSE release/

mv release score-addon-lavfi
7z a score-addon-lavfi.zip score-addon-lavfi
