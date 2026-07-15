// Pragmatic stub for arduino-esp32 3.3.10's ssl_client functions on the T-Display P4.
//
// On the P4 / IDF 5.5.1, arduino's NetworkClientSecure.cpp call signatures don't match
// ssl_client.cpp's definitions (an arduino-internal / toolchain ABI mismatch — e.g. the port
// param mangles `unsigned long` at the call site but `uint32_t` at the definition), so they're
// undefined at link. wadamesh doesn't use HTTPS yet (the companion link is USB/BLE/LoRa and map
// tiles are plain HTTP), so stub the referenced symbols to let the app link. Identical to the
// Tanmatsu's stub (same IDF + arduino), just guarded for this board.
//
// Symbols + signatures are taken verbatim from the linker's undefined-reference list. These are
// STRONG (not weak): a weak def in a .a doesn't get pulled to resolve an undefined ref, and there's
// no conflict because arduino's real defs mangle differently (that mismatch is why they're undefined).
// TODO: revisit for the HTTPS version-check / OTA-over-Wi-Fi (the P4's 32 MB PSRAM CAN do TLS).
#if defined(HAS_TDISPLAY_P4)
#include <IPAddress.h>
struct sslclient_context;
#define WK

WK int  start_ssl_client(sslclient_context*, const IPAddress&, unsigned long, const char*, int,
                         const char*, bool, const char*, const char*, const char*, const char*,
                         bool, const char**) { return -1; }
WK void stop_ssl_socket(sslclient_context*) {}
WK int  data_to_read(sslclient_context*) { return 0; }
WK int  send_net_data(sslclient_context*, const unsigned char*, unsigned int) { return -1; }
WK int  get_net_receive(sslclient_context*, unsigned char*, int) { return -1; }
WK int  peek_net_receive(sslclient_context*, int) { return -1; }
WK int  send_ssl_data(sslclient_context*, const unsigned char*, unsigned int) { return -1; }
WK int  get_ssl_receive(sslclient_context*, unsigned char*, int) { return -1; }
WK void ssl_init(sslclient_context*) {}
WK int  ssl_starttls_handshake(sslclient_context*) { return -1; }
#endif
