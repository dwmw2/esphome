#pragma once

namespace esphome::tuya_tcp {

/// No-op whose call from the component anchors the Tuya crypt primitive and
/// tuyaTCP member definitions into the link (the tuyapp library archive is
/// their only user)
void tuyapp_glue_anchor();

/// Same for the BearSSL cipher and hash definitions on RP2040; called from
/// tuyapp_glue_anchor()
void tuyapp_bearssl_anchor();

}  // namespace esphome::tuya_tcp
