#!/bin/bash
# Version 1.0
#
# Script converts MTS to mp4, extracts subtitles from MTS file by avchd2srt-core
# and concatenates date of creating video to file name
#
# Copyright (c) 2014 Alexey Alexashin alexashin.a.n@yandex.ru
#

HOME=`dirname "$(which $0)"`
CMD="${HOME}/avchd2srt-core"
DIR="$*"

[ $# -eq 0 ] && echo "Usage: $0 <path to *.MTS files>" && exit 1

exec 2>&1 
exec >/tmp/$(basename $0).log

for file in `find $DIR -type f -name '*.MTS'` ; do
    srtfile="${file%%.MTS}.srt"                         # subtitle filename
    echo $srtfile

    $CMD $file   2>&1 >/dev/null                       # extracting subtitles from MTS
    if [ -e "$srtfile" ]; then
        PRFX=$(awk 'NR==3 {print $2}' $srtfile | tr -d '\n')                 # cut the creating date from subtitles
        newname="`dirname ${file}`/`basename ${file%%.MTS}`_$PRFX"       # new filename without file suffix
        mv "$file" "$newname".MTS                                            # rename with date mark
        HandBrakeCLI -i "$newname".MTS -o "$newname".mp4 -e x264 -q 22 -B 160 -d -8 >/dev/null        # Converting MTS to mp4
        touch -m -d "$PRFX" "$newname".MTS
        touch -m -d "$PRFX" "$newname".mp4
        rm -f $srtfile
    else
        echo "$srtfile was not created"
    fi
done

exit 0 
