// The FIX GatewaySession is a header-only, fully-inline class (see
// order_gateway.h). This translation unit exists so build scripts that expect
// an order_gateway.cpp still have something to compile, and as a home for any
// future non-inline / stateful transport code (real socket I/O, TLS, etc.).

#include "order_gateway.h"

namespace hft {

// (Intentionally empty for now — all behaviour is inline in the header.)

}  // namespace hft
