/*

RPN RULES MODULE
Use RPNLib library (https://github.com/xoseperez/rpnlib)
Copyright (C) 2019 by Xose Pérez <xose dot perez at gmail dot com>

*/

#include "rpnrules.h"

#if RPN_RULES_SUPPORT

#include "broker.h"
#include "mqtt.h"
#include "ntp.h"
#include "relay.h"
#include "rpc.h"
#include "sensor.h"
#include "terminal.h"
#include "ws.h"

#include <vector>
// -----------------------------------------------------------------------------
// Custom commands
// -----------------------------------------------------------------------------

rpn_context _rpn_ctxt;
bool _rpn_run = false;
unsigned long _rpn_delay = RPN_DELAY;
unsigned long _rpn_last = 0;

struct RpnRunner {
    enum class Policy {
        OneShot,
        Periodic
    };

    RpnRunner(Policy policy_, uint32_t period_) :
        policy(policy_),
        period(period_),
        last(millis())
    {}

    Policy policy { Policy::Periodic };

    uint32_t period { 0ul };
    uint32_t last { 0ul };

    bool expired { false };
};

std::vector<RpnRunner> _rpn_runners;

rpn_operator_error _rpnRunnerHandler(rpn_context & ctxt, RpnRunner::Policy policy, uint32_t time) {
    for (auto& runner : _rpn_runners) {
        if ((policy == runner.policy) && (time == runner.period)) {
            return runner.expired
                ? rpn_operator_error::Ok
                : rpn_operator_error::CannotContinue;
        }
    }

    _rpn_runners.emplace_back(policy, time);

    return rpn_operator_error::CannotContinue;
}

// -----------------------------------------------------------------------------

bool _rpnWebSocketOnKeyCheck(const char * key, JsonVariant& value) {
    return (strncmp(key, "rpn", 3) == 0);
}

void _rpnWebSocketOnConnected(JsonObject& root) {

    root["rpnSticky"] = getSetting("rpnSticky", 1 == RPN_STICKY);
    root["rpnDelay"] = getSetting("rpnDelay", RPN_DELAY);
    JsonArray& rules = root.createNestedArray("rpnRules");

    unsigned char i = 0;
    String rule = getSetting({"rpnRule", i});
    while (rule.length()) {
        rules.add(rule);
        rule = getSetting({"rpnRule", ++i});
    }

    #if MQTT_SUPPORT
        i=0;
        JsonArray& topics = root.createNestedArray("rpnTopics");
        JsonArray& names = root.createNestedArray("rpnNames");
        String rpn_topic = getSetting({"rpnTopic", i});
        while (rpn_topic.length() > 0) {
            String rpn_name = getSetting({"rpnName", i});
            topics.add(rpn_topic);
            names.add(rpn_name);
            rpn_topic = getSetting({"rpnTopic", ++i});
        }
    #endif

}

#if MQTT_SUPPORT

void _rpnMQTTSubscribe() {
    unsigned char i = 0;
    String rpn_topic = getSetting({"rpnTopic", i});
    while (rpn_topic.length()) {
        mqttSubscribeRaw(rpn_topic.c_str());
        rpn_topic = getSetting({"rpnTopic", ++i});
    }
}

void _rpnMQTTCallback(unsigned int type, const char * topic, const char * payload) {

    if (type == MQTT_CONNECT_EVENT) {
        _rpnMQTTSubscribe();
    }

    if (type == MQTT_MESSAGE_EVENT) {
        unsigned char i = 0;
        String rpn_topic = getSetting({"rpnTopic", i});
        while (rpn_topic.length()) {
            if (rpn_topic.equals(topic)) {
                String rpn_name = getSetting({"rpnName", i});
                if (rpn_name.length()) {
                    rpn_value value { atof(payload) };
                    rpn_variable_set(_rpn_ctxt, rpn_name, value);
                    _rpn_run = true;
                    break;
                }
            }
            rpn_topic = getSetting({"rpnTopic", ++i});
        }
    }

}
#endif // MQTT_SUPPORT

void _rpnConfigure() {
    #if MQTT_SUPPORT
        if (mqttConnected()) _rpnMQTTSubscribe();
    #endif
    _rpn_delay = getSetting("rpnDelay", RPN_DELAY);
}

void _rpnBrokerCallback(const String& topic, unsigned char id, double value, const char*) {

    char name[32] = {0};
    snprintf(name, sizeof(name), "%s%u", topic.c_str(), id);

    if (topic == MQTT_TOPIC_RELAY) {
        rpn_variable_set(_rpn_ctxt, name, rpn_value(static_cast<bool>(value)));
    } else {
        rpn_variable_set(_rpn_ctxt, name, rpn_value(value));
    }

    _rpn_run = true;

}

void _rpnBrokerStatus(const String& topic, unsigned char id, unsigned int value) {
    _rpnBrokerCallback(topic, id, double(value), nullptr);
}

#if NTP_SUPPORT

rpn_error _rpnNtpNow(rpn_context & ctxt) {
    if (!ntpSynced()) return rpn_operator_error::CannotContinue;
    rpn_value ts { static_cast<rpn_int>(now()) };
    rpn_stack_push(ctxt, ts);
    return 0;
}

rpn_error _rpnNtpFunc(rpn_context & ctxt, rpn_int (*func)(time_t)) {
    rpn_value value;
    rpn_stack_pop(ctxt, value);

    value = rpn_value(func(value.toInt()));
    rpn_stack_push(ctxt, value);

    return 0;
}

#endif // NTP_SUPPORT

String _rpnValueToString(const rpn_value& value) {
    String out;
    if (value.isString()) {
        out = value.toString();
    } else if (value.isFloat()) {
        out = String(value.toFloat(), 10);
    } else if (value.isInt()) {
        out = String(value.toInt(), 10);
    } else if (value.isUint()) {
        out = String(value.toUint(), 10);
    } else if (value.isBoolean()) {
        out = String(value.toBoolean() ? "true" : "false");
    } else if (value.isNull()) {
        out = F("(null)");
    }
    return out;
}

char _rpnStackTypeTag(rpn_stack_value::Type type) {
    switch (type) {
    case rpn_stack_value::Type::None:
        return 'N';
    case rpn_stack_value::Type::Variable:
        return '$';
    case rpn_stack_value::Type::Array:
        return 'A';
    case rpn_stack_value::Type::Value:
    default:
        return ' ';
    }
}

#if RELAY_SUPPORT

rpn_error _rpnRelayStatus(rpn_context & ctxt, bool force) {
    rpn_value id;
    rpn_value status;

    rpn_stack_pop(ctxt, id);
    rpn_stack_pop(ctxt, status);

    rpn_uint value = status.toUint();
    if (value == 2) {
        relayToggle(id.toUint());
    } else if (relayStatusTarget(id.toUint()) != (value == 1)) {
        relayStatus(id.toUint(), value == 1);
    }

    return 0;
}

#endif // RELAY_SUPPORT

void _rpnShowStack(Print& print) {
    print.println(F("Stack:"));

    auto index = rpn_stack_size(_rpn_ctxt);
    if (!index) {
        print.println(F("      (empty)"));
        return;
    }

    rpn_stack_foreach(_rpn_ctxt, [&index, &print](rpn_stack_value::Type type, const rpn_value& value) {
        print.printf("%c      %02u: %s\n",
            _rpnStackTypeTag(type), index--,
            _rpnValueToString(value).c_str()
        );
    });
}

void _rpnInit() {

    // Init context
    rpn_init(_rpn_ctxt);

    // Time functions need NTP support
    // TODO: since 1.15.0, timelib+ntpclientlib are no longer used with latest Cores
    //       `now` is always in UTC, `utc_...` functions to be used instead to convert time
    #if NTP_SUPPORT && !NTP_LEGACY_SUPPORT
        rpn_operator_set(_rpn_ctxt, "utc", 0, _rpnNtpNow);
        rpn_operator_set(_rpn_ctxt, "now", 0, _rpnNtpNow);

        rpn_operator_set(_rpn_ctxt, "utc_month", 1, [](rpn_context & ctxt) {
            return _rpnNtpFunc(ctxt, utc_month);
        });
        rpn_operator_set(_rpn_ctxt, "month", 1, [](rpn_context & ctxt) {
            return _rpnNtpFunc(ctxt, month);
        });

        rpn_operator_set(_rpn_ctxt, "utc_day", 1, [](rpn_context & ctxt) {
            return _rpnNtpFunc(ctxt, utc_day);
        });
        rpn_operator_set(_rpn_ctxt, "day", 1, [](rpn_context & ctxt) {
            return _rpnNtpFunc(ctxt, day);
        });

        rpn_operator_set(_rpn_ctxt, "utc_dow", 1, [](rpn_context & ctxt) {
            return _rpnNtpFunc(ctxt, utc_weekday);
        });
        rpn_operator_set(_rpn_ctxt, "dow", 1, [](rpn_context & ctxt) {
            return _rpnNtpFunc(ctxt, weekday);
        });

        rpn_operator_set(_rpn_ctxt, "utc_hour", 1, [](rpn_context & ctxt) {
            return _rpnNtpFunc(ctxt, utc_hour);
        });
        rpn_operator_set(_rpn_ctxt, "hour", 1, [](rpn_context & ctxt) {
            return _rpnNtpFunc(ctxt, hour);
        });

        rpn_operator_set(_rpn_ctxt, "utc_minute", 1, [](rpn_context & ctxt) {
            return _rpnNtpFunc(ctxt, utc_minute);
        });
        rpn_operator_set(_rpn_ctxt, "minute", 1, [](rpn_context & ctxt) {
            return _rpnNtpFunc(ctxt, minute);
        });
    #endif

    // TODO: 1.14.0 weekday(...) conversion seemed to have 0..6 range with Monday as 0
    //       using classic Sunday as first, but instead of 0 it is 1
    //       Implementation above also uses 1 for Sunday, staying compatible with TimeLib
    #if NTP_SUPPORT && NTP_LEGACY_SUPPORT
        rpn_operator_set(_rpn_ctxt, "utc", 0, [](rpn_context & ctxt) -> rpn_error {
            if (!ntpSynced()) return rpn_operator_error::CannotContinue;
            rpn_value ts { static_cast<rpn_int>(ntpLocal2UTC(now())) };
            rpn_stack_push(ctxt, ts);
            return 0;
        });
        rpn_operator_set(_rpn_ctxt, "now", 0, _rpnNtpNow);

        rpn_operator_set(_rpn_ctxt, "month", 1, [](rpn_context & ctxt) {
            return _rpnNtpFunc(ctxt, month);
        });
        rpn_operator_set(_rpn_ctxt, "day", 1, [](rpn_context & ctxt) {
            return _rpnNtpFunc(ctxt, day);
        });
        rpn_operator_set(_rpn_ctxt, "dow", 1, [](rpn_context & ctxt) {
            return _rpnNtpFunc(ctxt, weekday);
        });
        rpn_operator_set(_rpn_ctxt, "hour", 1, [](rpn_context & ctxt) {
            return _rpnNtpFunc(ctxt, hour);
        });
        rpn_operator_set(_rpn_ctxt, "minute", 1, [](rpn_context & ctxt) {
            return _rpnNtpFunc(ctxt, minute);
        });
    #endif

    // Accept relay number and numeric API status value (0, 1 and 2)
    #if RELAY_SUPPORT

        // apply status and reset timers when called
        rpn_operator_set(_rpn_ctxt, "relay_reset", 2, [](rpn_context & ctxt) {
            return _rpnRelayStatus(ctxt, true);
        });

        // only update status when target status differs, keep running timers
        rpn_operator_set(_rpn_ctxt, "relay", 2, [](rpn_context & ctxt) {
            return _rpnRelayStatus(ctxt, false);
        });

    #endif // RELAY_SUPPORT == 1

    // Channel operators
    #if RELAY_PROVIDER == RELAY_PROVIDER_LIGHT

        rpn_operator_set(_rpn_ctxt, "update", 0, [](rpn_context & ctxt) -> rpn_error {
            lightUpdate(true, true);
            return 0;
        });

        rpn_operator_set(_rpn_ctxt, "black", 0, [](rpn_context & ctxt) -> rpn_error {
            lightColor((unsigned long) 0);
            return 0;
        });

        rpn_operator_set(_rpn_ctxt, "channel", 2, [](rpn_context & ctxt) -> rpn_error {
            rpn_value value;
            rpn_value id;
            rpn_stack_pop(ctxt, id);
            rpn_stack_pop(ctxt, value);
            lightChannel(id.toUint(), id.toInt());
            return 0;
        });

    #endif

    #if MQTT_SUPPORT
        rpn_operator_set(_rpn_ctxt, "mqtt_send", 2, [](rpn_context & ctxt) -> rpn_error {
            rpn_value message;
            rpn_stack_pop(ctxt, message);

            rpn_value topic;
            rpn_stack_pop(ctxt, topic);

            return mqttSendRaw(topic.toString().c_str(), message.toString().c_str())
                ? rpn_operator_error::Ok
                : rpn_operator_error::CannotContinue;
        });
    #endif

    // Some debugging. Dump stack contents
    rpn_operator_set(_rpn_ctxt, "showstack", 0, [](rpn_context & ctxt) -> rpn_error {
        _rpnShowStack(terminalDefaultStream());
        return 0;
    });

    // And, simple string logging
    #if DEBUG_SUPPORT
        rpn_operator_set(_rpn_ctxt, "dbgmsg", 1, [](rpn_context & ctxt) -> rpn_error {
            rpn_value message;
            rpn_stack_pop(ctxt, message);

            DEBUG_MSG_P(PSTR("[RPN] %s\n"), message.toString().c_str());

            return 0;
        });
    #endif

    rpn_operator_set(_rpn_ctxt, "millis", 0, [](rpn_context & ctxt) -> rpn_error {
        rpn_stack_push(ctxt, rpn_value(static_cast<uint32_t>(millis())));
        return 0;
    });

    rpn_operator_set(_rpn_ctxt, "oneshot_ms", 1, [](rpn_context & ctxt) -> rpn_error {
        auto every = rpn_stack_pop(ctxt);
        return _rpnRunnerHandler(ctxt, RpnRunner::Policy::OneShot, every.toUint());
    });

    rpn_operator_set(_rpn_ctxt, "every_ms", 1, [](rpn_context & ctxt) -> rpn_error {
        auto every = rpn_stack_pop(ctxt);
        return _rpnRunnerHandler(ctxt, RpnRunner::Policy::Periodic, every.toUint());
    });

}

#if TERMINAL_SUPPORT

void _rpnInitCommands() {

    terminalRegisterCommand(F("RPN.RUNNERS"), [](const terminal::CommandContext& ctx) {
        if (!_rpn_runners.size()) {
            terminalError(ctx, F("No active runners"));
            return;
        }

        for (auto& runner : _rpn_runners) {
            char buffer[128] = {0};
            snprintf_P(buffer, sizeof(buffer), PSTR("%p %s %u ms, last %u ms"),
                &runner, (RpnRunner::Policy::Periodic == runner.policy) ? "every" : "one-shot",
                runner.period, runner.last
            );
            ctx.output.println(buffer);
        }

        terminalOK(ctx);
    });

    terminalRegisterCommand(F("RPN.VARS"), [](const terminal::CommandContext& ctx) {
        rpn_variables_foreach(_rpn_ctxt, [&ctx](const String& name, const rpn_value& value) {
            char buffer[256] = {0};
            snprintf_P(buffer, sizeof(buffer), PSTR("      %s: %s"), name.c_str(), _rpnValueToString(value).c_str());
            ctx.output.println(buffer);
        });
        terminalOK(ctx);
    });

    terminalRegisterCommand(F("RPN.OPS"), [](const terminal::CommandContext& ctx) {
        rpn_operators_foreach(_rpn_ctxt, [&ctx](const String& name, size_t argc, rpn_operator::callback_type) {
            char buffer[128] = {0};
            snprintf_P(buffer, sizeof(buffer), PSTR("      %s (%d)"), name.c_str(), argc);
            ctx.output.println(buffer);
        });
        terminalOK(ctx);
    });

    terminalRegisterCommand(F("RPN.TEST"), [](const terminal::CommandContext& ctx) {
        if (ctx.argc != 2) {
            terminalError(F("Wrong arguments"));
            return;
        }

        ctx.output.print(F("Running RPN expression: "));
        ctx.output.println(ctx.argv[1].c_str());

        if (!rpn_process(_rpn_ctxt, ctx.argv[1].c_str())) {
            rpn_stack_clear(_rpn_ctxt);
            char buffer[64] = {0};
            snprintf_P(buffer, sizeof(buffer), PSTR("position=%u category=%d code=%d"),
                _rpn_ctxt.error.position, static_cast<int>(_rpn_ctxt.error.category), _rpn_ctxt.error.code);
            terminalError(ctx, buffer);
            return;
        }

        _rpnShowStack(ctx.output);
        rpn_stack_clear(_rpn_ctxt);

        terminalOK(ctx);
    });

}
#endif

// enables us to use rules without any events firing
// notice: requires rpnRun to trigger at least once so that we can install runners
void _rpnRunnersCheck() {
    auto ts = millis();
    for (auto& runner : _rpn_runners) {
        if (ts - runner.last >= runner.period) {
            runner.expired = true;
            runner.last = ts;
            _rpn_run = true;
        }
    }
}

void _rpnRunnersReset() {
    auto old = std::remove_if(_rpn_runners.begin(), _rpn_runners.end(), [](RpnRunner& runner) {
        return (RpnRunner::Policy::OneShot == runner.policy) && runner.expired;
    });

    if (old != _rpn_runners.end()) {
        _rpn_runners.erase(old, _rpn_runners.end());
    }

    for (auto& runner : _rpn_runners) {
        runner.expired = false;
    }
}

void _rpnRun() {

    if (!_rpn_run) {
        return;
    }

    if (millis() - _rpn_last <= _rpn_delay) {
        return;
    }

    _rpn_last = millis();
    _rpn_run = false;

    String rule;
    unsigned char i = 0;
    while ((rule = getSetting({"rpnRule", i++})).length()) {
        rpn_process(_rpn_ctxt, rule.c_str());
        rpn_stack_clear(_rpn_ctxt);
    }

    if (!getSetting("rpnSticky", 1 == RPN_STICKY)) {
        rpn_variables_clear(_rpn_ctxt);
    }

}

void _rpnLoop() {

    _rpnRunnersCheck();
    _rpnRun();
    _rpnRunnersReset();

}

void rpnSetup() {

    // Init context
    _rpnInit();

    // Load & cache settings
    _rpnConfigure();

    // Terminal commands
    #if TERMINAL_SUPPORT
        _rpnInitCommands();
    #endif

    // Websockets
    #if WEB_SUPPORT
        wsRegister()
            .onVisible([](JsonObject& root) { root["rpnVisible"] = 1; })
            .onConnected(_rpnWebSocketOnConnected)
            .onKeyCheck(_rpnWebSocketOnKeyCheck);
    #endif

    // MQTT
    #if MQTT_SUPPORT
        mqttRegister(_rpnMQTTCallback);
    #endif

    #if NTP_SUPPORT
        NtpBroker::Register([](const NtpTick tick, time_t timestamp, const String& datetime) {
            static const String tick_every_hour(F("tick1h"));
            static const String tick_every_minute(F("tick1m"));

            const char* ptr =
                (tick == NtpTick::EveryMinute) ? tick_every_minute.c_str() :
                (tick == NtpTick::EveryHour) ? tick_every_hour.c_str() : nullptr;

            if (ptr != nullptr) {
                rpn_value value { static_cast<rpn_int>(timestamp) };
                rpn_variable_set(_rpn_ctxt, ptr, value);
                _rpn_run = true;
            }
        });
    #endif

    StatusBroker::Register(_rpnBrokerStatus);

    #if SENSOR_SUPPORT
        SensorReadBroker::Register(_rpnBrokerCallback);
    #endif

    espurnaRegisterReload(_rpnConfigure);
    espurnaRegisterLoop(_rpnLoop);

    _rpn_last = millis();
    _rpn_run = true;

}

#endif // RPN_RULES_SUPPORT
