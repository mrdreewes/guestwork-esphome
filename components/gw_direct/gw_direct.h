#pragma once

#include <string>

#include "esphome/core/component.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/switch/switch.h"

#include <WiFiClientSecureBearSSL.h>
#include <PubSubClient.h>

namespace esphome::gw_direct {

/**
 * The board's one connection to GuestWork (docs/direct-minisplits.md): MQTT over TLS, out to GuestWork's broker.
 * Topics and payloads are ESPHome mqtt climate's own: <prefix>/status, <prefix>/climate/ac/<field>/state|command,
 * <prefix>/switch/beeper/state|command.
 */
class GwDirect : public Component {
 public:
  void set_broker(const char *host, uint16_t port) {
    this->host_ = host;
    this->port_ = port;
  }
  void set_login(const char *username, const char *password) {
    this->username_ = username;
    this->password_ = password;
  }
  void set_topic_prefix(const std::string &prefix) { this->prefix_ = prefix; }
  void set_ca(const char *pem) { this->ca_pem_ = pem; }
  void set_build_time(uint32_t t) { this->build_time_ = t; }
  void set_climate(climate::Climate *c) { this->climate_ = c; }
  void set_beeper(switch_::Switch *s) { this->beeper_ = s; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

 protected:
  void connect_();
  void publish_state_(bool all);
  void publish_beeper_(bool all);
  bool publish_(const char *suffix, const char *payload, std::string &last, bool all);
  void on_message_(const char *topic, const uint8_t *payload, unsigned int length);

  const char *host_{nullptr};
  uint16_t port_{0};
  const char *username_{nullptr};
  const char *password_{nullptr};
  std::string prefix_;
  const char *ca_pem_{nullptr};
  uint32_t build_time_{0};
  climate::Climate *climate_{nullptr};
  switch_::Switch *beeper_{nullptr};

  BearSSL::X509List *anchors_{nullptr};
  BearSSL::Session session_;
  BearSSL::WiFiClientSecure tls_;
  PubSubClient mqtt_{tls_};

  uint32_t next_attempt_{0};
  uint32_t backoff_ms_{5000};
  uint32_t last_full_{0};
  bool was_connected_{false};

  // What each topic last carried, so a report goes only when it changed (and all of them every few minutes).
  std::string last_mode_, last_target_, last_current_, last_action_, last_fan_, last_swing_, last_preset_,
      last_beeper_;
};

}  // namespace esphome::gw_direct
