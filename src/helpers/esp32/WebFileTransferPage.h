#pragma once

#include "WebFileTransferConfig.h"

#if WADA_WEB_FILE_TRANSFER

static const char WS_HTTP_FILES_DISABLED[] =
  "HTTP/1.1 503 Service Unavailable\r\n"
  "Content-Type: text/plain; charset=utf-8\r\n"
  "Cache-Control: no-store\r\n"
  "Connection: close\r\n\r\n"
  "File Transfer is not enabled on the device. Turn on the Web console, or open File Transfer on the device.\n";

#endif  // WADA_WEB_FILE_TRANSFER