#!/bin/bash
rm -rf release
mkdir -p release

cp -rf Lavfi *.{hpp,cpp,txt,json} LICENSE release/

mv release score-addon-lavfi
7z a score-addon-lavfi.zip score-addon-lavfi
