#pragma once

namespace esphome::tuya_tcp {

/// No-op whose call from the component anchors the Tuya crypt primitive
/// definitions into the link (the tuyapp library archive is their only user)
void tuyapp_crypt_anchor();

}  // namespace esphome::tuya_tcp
