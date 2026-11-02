# GuestWork for ESPHome

`gw_direct` connects a mini-split's ESP8266 Wi-Fi module (an ESPHome board on the indoor head's Midea port) straight
to GuestWork, so the unit shows up in GuestWork with no hub and no Home Assistant.

ESPHome's own `mqtt:` can't use TLS on the ESP8266. This component makes the board's one connection itself: MQTT over
TLS (BearSSL), checked against GuestWork's device certificate authority. It uses the same topics and payloads as
ESPHome's MQTT climate.

You don't write the config by hand. In GuestWork, open **Integrations → Mini-splits on Wi-Fi**, add a unit, and pick
**ESP8266**. GuestWork then gives you the whole config with this component and the unit's own login filled in. Flash it
with `esphome run`.

```yaml
external_components:
  - source: github://mrdreewes/guestwork-esphome@v1.0.0
    components: [gw_direct]

gw_direct:
  broker: …            # filled in by GuestWork
  port: …
  username: …          # the unit's own login; also its client id
  password: "…"
  topic_prefix: gw/…
  climate_id: ac_unit
  beeper_id: beeper_switch
  certificate_authority: |
    -----BEGIN CERTIFICATE-----
    …
```
