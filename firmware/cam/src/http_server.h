#pragma once

// HTTP service (esp_http_server, each connection runs in its own httpd worker,
// so MJPEG streaming never blocks /capture or /healthz):
//   GET /stream   MJPEG multipart stream, SVGA JPEG, ~12 fps
//   GET /capture  single JPEG frame
//   GET /healthz  JSON status
void httpServerStart();
