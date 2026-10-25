//
// seams/order_wide_index.h -- O1c `[net] order_wide_index`: the retail carrier (order_wide_index.cpp).
//
#pragma once

// Splice the four wide-index sites of the scheduled order lane (encode 0x00466242, integrity check
// 0x004661a0, local decode 0x0046643a, remote decode 0x004667b4) when `on`; log one boot line either way.
// Per site: PATCHED, DISPLACED by promotion (libmh's twin carries it) or MISMATCHED (refused).
void install_order_wide_index(bool on);
