#pragma once
/* Copy to ignored private/zt_device_private.h and supply the existing fleet
 * settings. Keep the exact same 32-byte key on every badge in the mesh.
 * Gateway bearer token remains in private/zt_gateway_private.h. */
#define ZT_PRIVATE_WIFI_SSID "your-hotspot-name"
#define ZT_PRIVATE_WIFI_PASSWORD "your-hotspot-password"
#define ZT_PRIVATE_MESH_KEY { /* existing 32 key bytes */ }
