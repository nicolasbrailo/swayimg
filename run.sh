# Antialiasing makes it slow in some setups
WAYLAND_DISPLAY="wayland-1" DISPLAY="" \
  /home/batman/src/swayimg/build/swayimg \
  --config="general.scale=fit" \
  --config="general.fullscreen=yes" \
  --config="general.antialiasing=no" \
  --config="general.background=000" \
  --config="font.name=monospace" \
  --config="font.size=16" \
  --config="font.color=#A39A92" \
  --config="font.shadow=#77685D" \
  --config="info.mode=full" \
  --config="info.background_color=FFF4EBD9" \
  --config="info.border_color=FF058ED9" \
  --config="info.border_pt=5" \
  --config="info.padding_pt=25" \
  --config="info.full.topleft=name,format,filesize,imagesize,exif,index,scale,frame,status" \
  --config="info.full.topright=name,format,filesize,imagesize,exif,index,scale,frame,status" \
  --config="info.full.bottomleft=name,format,filesize,imagesize,exif,index,scale,frame,status" \
  --config="info.full.bottomright=name,format,filesize,imagesize,exif,index,scale,frame,status" \
  --config="list.source=www" \
  --config="list.www_cache=/home/batman/src/swayimg/build/wwwcache" \
  --config="list.www_url=http://10.0.0.144:5000/get_image" \
  --config="list.www_cache_limit=5" \
  --config="list.www_prefetch_n=2"

