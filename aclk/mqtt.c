// SPDX-License-Identifier: GPL-3.0-or-later

#include <libnetdata/json/json.h>
#include "../daemon/common.h"
#include "mqtt.h"

void (*_on_connect)(void *ptr) = NULL;
void (*_on_disconnect)(void *ptr) = NULL;
extern int cmdpause;


#ifndef ENABLE_ACLK

inline const char *_link_strerror(int rc)
{
    (void) rc;
    return "no error";
}

int _link_event_loop(int timeout)
{
    (void) timeout;
    return 0;
}

int _link_send_message(char *topic, char *message)
{
    (void) topic;
    (void) message;
    return 0;
}

int _link_subscribe(char  *topic, int qos)
{
    (void) topic;
    (void) qos;
    return 0;
}

void _link_shutdown()
{
    return;
}

int _link_lib_init(char *aclk_hostname, int aclk_port, void (*on_connect)(void *), void (*on_disconnect)(void *))
{
    (void) aclk_hostname;
    (void) aclk_port;
    (void) on_connect;
    (void) on_disconnect;
    return 0;
}

#else
/*
 * Just report the library info in the logfile for reference when issues arise
 *
 */

struct mosquitto *mosq = NULL;

// Get a string description of the error

inline const char *_link_strerror(int rc)
{
    return mosquitto_strerror(rc);
}


void mqtt_message_callback(
    struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg)
{
    (void) mosq;
    (void) obj;

    // TODO: handle commands in a more efficient way, if we have many

    if (strcmp((char *)msg->payload, "pause") == 0) {
        cmdpause = 1;
        return;
    }

    if (strcmp((char *)msg->payload, "resume") == 0) {
        cmdpause = 0;
        return;
    }

    if (strcmp((char *)msg->payload, "reload") == 0) {
        error_log_limit_unlimited();
        info("Reloading health configuration");
        health_reload();
        error_log_limit_reset();
        return;
    }

    if (strcmp((char *)msg->payload, "info") == 0) {
        aclk_send_metadata();
        return;
    }

    aclk_handle_cloud_request(msg->payload);

    //info("Received type=[%s], msg-id=[%s], topic=[%s], url=[%s]",cloud_to_agent.type_id, cloud_to_agent.msg_id, cloud_to_agent.topic, cloud_to_agent.url);

}

void connect_callback(struct mosquitto *mosq, void *obj, int rc)
{
    (void) obj;
    (void) rc;

    info("Connection to cloud estabilished");

    aclk_connection_initialized = 1;
    _on_connect((void *) mosq);

    return;
}


void disconnect_callback(struct mosquitto *mosq, void *obj, int rc)
{
    (void) obj;
    (void) rc;

    info("Connection to cloud failed");
    // TODO: Keep the connection "alive" for now. The library will reconnect.

    //mqtt_connection_initialized = 0;
    _on_disconnect((void *) mosq);
    //sleep_usec(USEC_PER_SEC * 5);
    return;
}


void _show_mqtt_info()
{
    int libmosq_major, libmosq_minor, libmosq_revision, libmosq_version;
    libmosq_version =  mosquitto_lib_version(&libmosq_major, &libmosq_minor, &libmosq_revision);

    info("Detected libmosquitto library version %d, %d.%d.%d",libmosq_version, libmosq_major, libmosq_minor, libmosq_revision);
}

int _link_lib_init(char *aclk_hostname, int aclk_port, void (*on_connect)(void *), void (*on_disconnect)(void *))
{
    int rc;
    int libmosq_major, libmosq_minor, libmosq_revision, libmosq_version;
    char *ca_crt;
    char *server_crt;
    char *server_key;

    // show library info so can have in in the logfile
    libmosq_version = mosquitto_lib_version(&libmosq_major, &libmosq_minor, &libmosq_revision);
    ca_crt = config_get(CONFIG_SECTION_ACLK, "agent cloud link cert", "*");
    server_crt = config_get(CONFIG_SECTION_ACLK, "agent cloud link server cert", "*");
    server_key = config_get(CONFIG_SECTION_ACLK, "agent cloud link server key", "*");


    if (ca_crt[0] == '*') {
        freez(ca_crt);
        ca_crt = NULL;
    }

    if (server_crt[0] == '*') {
        freez(server_crt);
        server_crt = NULL;
    }

    if (server_key[0] == '*') {
        freez(server_key);
        server_key = NULL;
    }

    info(
        "Detected libmosquitto library version %d, %d.%d.%d", libmosq_version, libmosq_major, libmosq_minor,
        libmosq_revision);

    rc = mosquitto_lib_init();
    if (unlikely(rc != MOSQ_ERR_SUCCESS)) {
        error("Failed to initialize MQTT (libmosquitto library)");
        return 1;
    }

    mosq = mosquitto_new("anon", true, NULL);
    if (unlikely(!mosq)) {
        mosquitto_lib_cleanup();
        error("MQTT new structure  -- %s", mosquitto_strerror(errno));
        return 1;
    }

    _on_connect = on_connect;
    _on_disconnect = on_disconnect;

    mosquitto_connect_callback_set(mosq, connect_callback);
    mosquitto_disconnect_callback_set(mosq, disconnect_callback);

    mosquitto_username_pw_set(mosq, "anon", "anon");

    rc = mosquitto_threaded_set(mosq, 1);
    if (unlikely(rc != MOSQ_ERR_SUCCESS))
        error("Failed to tune the thread model for libmoquitto (%s)", mosquitto_strerror(rc));

#if defined(LIBMOSQUITTO_VERSION_NUMBER) >= 1006000
    rc = mosquitto_int_option(mosq, MQTT_PROTOCOL_V311, 0);
    if (unlikely(rc != MOSQ_ERR_SUCCESS))
        error("MQTT protocol specification rc = %d (%s)", rc, mosquitto_strerror(rc));

    rc = mosquitto_int_option(mosq, MOSQ_OPT_SEND_MAXIMUM, 1);
    info("MQTT in flight messages set to 1  -- %s", mosquitto_strerror(rc));
#endif

    rc = mosquitto_reconnect_delay_set(mosq, ACLK_RECONNECT_DELAY, ACLK_MAX_RECONNECT_DELAY, 1);

    if (unlikely(rc != MOSQ_ERR_SUCCESS))
        error("Failed to set the reconnect delay (%d) (%s)", rc, mosquitto_strerror(rc));

    mosquitto_tls_set(mosq, ca_crt, NULL, server_crt, server_key, NULL);

    rc = mosquitto_connect_async(mosq, aclk_hostname, aclk_port, ACLK_PING_INTERVAL);

    if (unlikely(rc != MOSQ_ERR_SUCCESS))
        error("Connect %s MQTT status = %d (%s)", aclk_hostname, rc, mosquitto_strerror(rc));
    else
        info("Establishing MQTT link to %s", aclk_hostname);

    return rc;
}

int _link_event_loop(int timeout)
{
    int rc;

    rc = mosquitto_loop(mosq, timeout, 1);

    if (unlikely(rc != MOSQ_ERR_SUCCESS)) {
        errno = 0;
        error("Loop error code %d (%s)", rc, mosquitto_strerror(rc));
        rc = mosquitto_reconnect(mosq);
        if (unlikely(rc != MOSQ_ERR_SUCCESS)) {
            error("Reconnect loop error code %d (%s)", rc, mosquitto_strerror(rc));
        }
        // TBD: Using delay
        sleep_usec(USEC_PER_SEC * 10);
    }
    return rc;
}

void _link_shutdown()
{
    int rc;

    rc = mosquitto_disconnect(mosq);
    switch (rc) {
        case MOSQ_ERR_SUCCESS:
            info("MQTT disconnected from broker");
            break;
        default:
            info("MQTT invalid structure");
            break;
    };

    mosquitto_destroy(mosq);
    mosq = NULL;
    return;
}


int _link_subscribe(char  *topic, int qos)
{
    int rc;

    if (unlikely(!mosq))
        return 1;

    mosquitto_message_callback_set(mosq, mqtt_message_callback);

    rc = mosquitto_subscribe(mosq, NULL, topic, qos);
    if (unlikely(rc)) {
        errno = 0;
        error("Failed to register subscription %d (%s)", rc, mosquitto_strerror(rc));
        return 1;
    }

    return 0;
}


/*
 * Send a message to the cloud to specific topic
 *
 */

int _link_send_message(char *topic, char *message)
{
    int rc;

    rc = mosquitto_pub_topic_check(topic);

    if (unlikely(rc != MOSQ_ERR_SUCCESS))
        return rc;

    int msg_len = strlen(message);

    // TODO: handle encoding validation -- the message should be UFT8 encoded by the sender
    //rc = mosquitto_validate_utf8(message, msg_len);
    //if (unlikely(rc != MOSQ_ERR_SUCCESS))
    //    return rc;

    rc = mosquitto_publish(mosq, NULL, topic, msg_len, message, ACLK_QOS, 0);

    // TODO: Add better handling -- error will flood the logfile here
    if (unlikely(rc != MOSQ_ERR_SUCCESS)) {
        error("MQTT message failed : %s", mosquitto_strerror(rc));
    }

    return rc;
}
#endif