#include "gw_direct.h"

#include <cmath>
#include <ctime>

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/components/wifi/wifi_component.h"

namespace esphome::gw_direct {

static const char *const TAG = "gw_direct";

// Every few minutes all of it again, so GuestWork never takes a steady unit for a silent one (its cards say
// "not heard from" after 15 minutes).
static const uint32_t FULL_EVERY_MS = 5 * 60 * 1000;
static const uint32_t MAX_BACKOFF_MS = 60 * 1000;

// ESPHome mqtt climate's words, index = the enum's value (components/mqtt/mqtt_climate.cpp).
static const char *const MODES[] = {"off", "heat_cool", "cool", "heat", "fan_only", "dry", "auto"};
static const char *const ACTIONS[] = {"off", "unknown", "cooling", "heating", "idle", "drying", "fan", "defrosting"};
static const char *const FANS[] = {"on", "off", "auto", "low", "medium", "high", "middle", "focus", "diffuse", "quiet"};
static const char *const SWINGS[] = {"off", "both", "vertical", "horizontal"};
static const char *const PRESETS[] = {"none", "home", "away", "boost", "comfort", "eco", "sleep", "activity"};

template<size_t N> static const char *word(const char *const (&table)[N], size_t i) {
  return i < N ? table[i] : "unknown";
}

void GwDirect::setup() {
  this->anchors_ = new BearSSL::X509List(this->ca_pem_);
  // GuestWork's broker agrees to small records (max fragment length), so the board needs ~1.5 KB of buffers
  // instead of the 16 KB a full TLS record takes.
  this->tls_.setBufferSizes(1024, 512);
  this->tls_.setTrustAnchors(this->anchors_);
  this->tls_.setSession(&this->session_);
  this->tls_.setTimeout(8000);
  this->mqtt_.setServer(this->host_, this->port_);
  this->mqtt_.setBufferSize(384);
  this->mqtt_.setKeepAlive(30);
  this->mqtt_.setSocketTimeout(8);
  this->mqtt_.setCallback(
      [this](char *topic, uint8_t *payload, unsigned int length) { this->on_message_(topic, payload, length); });
  // The real time, for checking the broker's certificate (the build time stands in until it answers).
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  this->climate_->add_on_state_callback([this](climate::Climate & /*unused*/) { this->publish_state_(false); });
  if (this->beeper_ != nullptr)
    this->beeper_->add_on_state_callback([this](bool /*state*/) { this->publish_beeper_(false); });
}

void GwDirect::dump_config() {
  ESP_LOGCONFIG(TAG,
                "GuestWork direct:\n"
                "  Broker: %s:%u\n"
                "  Topic prefix: %s",
                this->host_, this->port_, this->prefix_.c_str());
}

void GwDirect::loop() {
  if (this->mqtt_.connected()) {
    this->mqtt_.loop();
    if (millis() - this->last_full_ > FULL_EVERY_MS) {
      this->publish_state_(true);
      this->publish_beeper_(true);
    }
    return;
  }
  if (this->was_connected_) {
    this->was_connected_ = false;
    ESP_LOGW(TAG, "Disconnected from GuestWork (state %d)", this->mqtt_.state());
  }
  if (!wifi::global_wifi_component->is_connected() || (int32_t) (millis() - this->next_attempt_) < 0)
    return;
  this->connect_();
}

void GwDirect::connect_() {
  time_t now = time(nullptr);
  this->tls_.setX509Time(now > (time_t) this->build_time_ ? now : (time_t) this->build_time_);
  std::string will = this->prefix_ + "/status";
  ESP_LOGD(TAG, "Connecting to %s:%u (free heap %u)", this->host_, this->port_, ESP.getFreeHeap());
  // The client id is the username: GuestWork's broker refuses any other.
  bool ok = this->mqtt_.connect(this->username_, this->username_, this->password_, will.c_str(), 0, true, "offline",
                                true);
  if (!ok) {
    char err[96];
    int code = this->tls_.getLastSSLError(err, sizeof(err));
    ESP_LOGW(TAG, "Couldn't connect to GuestWork: mqtt %d, tls %d %s", this->mqtt_.state(), code, code ? err : "");
    this->next_attempt_ = millis() + this->backoff_ms_;
    this->backoff_ms_ = std::min(this->backoff_ms_ * 2, MAX_BACKOFF_MS);
    return;
  }
  this->was_connected_ = true;
  this->backoff_ms_ = 5000;
  ESP_LOGI(TAG, "Connected to GuestWork (free heap %u)", ESP.getFreeHeap());
  // Only its own command topics: GuestWork's broker refuses anything wider.
  static const char *const FIELDS[] = {"mode", "target_temperature", "fan_mode", "swing_mode", "preset"};
  for (const char *f : FIELDS)
    this->mqtt_.subscribe((this->prefix_ + "/climate/ac/" + f + "/command").c_str(), 0);
  if (this->beeper_ != nullptr)
    this->mqtt_.subscribe((this->prefix_ + "/switch/beeper/command").c_str(), 0);
  this->mqtt_.publish(will.c_str(), "online", true);
  this->publish_state_(true);
  this->publish_beeper_(true);
}

bool GwDirect::publish_(const char *suffix, const char *payload, std::string &last, bool all) {
  if (!all && last == payload)
    return true;
  if (!this->mqtt_.connected())
    return false;
  if (!this->mqtt_.publish((this->prefix_ + "/" + suffix).c_str(), payload, true))
    return false;
  last = payload;
  return true;
}

void GwDirect::publish_state_(bool all) {
  if (!this->mqtt_.connected())
    return;
  if (all)
    this->last_full_ = millis();
  climate::Climate *c = this->climate_;
  auto traits = c->get_traits();
  char buf[VALUE_ACCURACY_MAX_LEN];

  this->publish_("climate/ac/mode/state", word(MODES, (size_t) c->mode), this->last_mode_, all);
  value_accuracy_to_buf(buf, c->target_temperature, traits.get_target_temperature_accuracy_decimals());
  this->publish_("climate/ac/target_temperature/state", buf, this->last_target_, all);
  if (!std::isnan(c->current_temperature)) {
    value_accuracy_to_buf(buf, c->current_temperature, traits.get_current_temperature_accuracy_decimals());
    this->publish_("climate/ac/current_temperature/state", buf, this->last_current_, all);
  }
  this->publish_("climate/ac/action/state", word(ACTIONS, (size_t) c->action), this->last_action_, all);

  std::string fan;
  if (c->has_custom_fan_mode()) {
    fan = c->get_custom_fan_mode().c_str();
  } else if (c->fan_mode.has_value()) {
    fan = word(FANS, (size_t) c->fan_mode.value());
  }
  this->publish_("climate/ac/fan_mode/state", fan.c_str(), this->last_fan_, all);
  this->publish_("climate/ac/swing_mode/state", word(SWINGS, (size_t) c->swing_mode), this->last_swing_, all);

  std::string preset;
  if (c->has_custom_preset()) {
    preset = c->get_custom_preset().c_str();
  } else if (c->preset.has_value()) {
    preset = word(PRESETS, (size_t) c->preset.value());
  }
  this->publish_("climate/ac/preset/state", preset.c_str(), this->last_preset_, all);
}

void GwDirect::publish_beeper_(bool all) {
  if (this->beeper_ == nullptr || !this->mqtt_.connected())
    return;
  this->publish_("switch/beeper/state", this->beeper_->state ? "ON" : "OFF", this->last_beeper_, all);
}

void GwDirect::on_message_(const char *topic, const uint8_t *payload, unsigned int length) {
  std::string t(topic);
  std::string p(reinterpret_cast<const char *>(payload), length);
  if (t.rfind(this->prefix_ + "/", 0) != 0)
    return;
  t = t.substr(this->prefix_.size() + 1);
  ESP_LOGD(TAG, "Command %s = %s", t.c_str(), p.c_str());

  if (t == "switch/beeper/command") {
    if (this->beeper_ == nullptr)
      return;
    if (str_equals_case_insensitive(p, "ON")) {
      this->beeper_->turn_on();
    } else if (str_equals_case_insensitive(p, "OFF")) {
      this->beeper_->turn_off();
    }
    return;
  }

  static const std::string HEAD = "climate/ac/";
  static const std::string TAIL = "/command";
  if (t.size() <= HEAD.size() + TAIL.size() || t.rfind(HEAD, 0) != 0 ||
      t.compare(t.size() - TAIL.size(), TAIL.size(), TAIL) != 0)
    return;
  std::string field = t.substr(HEAD.size(), t.size() - HEAD.size() - TAIL.size());

  // The same calls ESPHome's mqtt climate makes for the same topics.
  auto call = this->climate_->make_call();
  if (field == "mode") {
    call.set_mode(p);
  } else if (field == "target_temperature") {
    auto v = parse_number<float>(p);
    if (!v.has_value())
      return;
    call.set_target_temperature(*v);
  } else if (field == "fan_mode") {
    call.set_fan_mode(p);
  } else if (field == "swing_mode") {
    call.set_swing_mode(p);
  } else if (field == "preset") {
    call.set_preset(p);
  } else {
    return;
  }
  call.perform();
}

}  // namespace esphome::gw_direct
