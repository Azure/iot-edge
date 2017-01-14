// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdlib.h>
#ifdef _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif
#include <ctype.h>
#include "azure_c_shared_utility/gballoc.h"

#include "iothubdeviceping.h"
#include "iothub_client.h"
#include "iothubtransport.h"
#include "iothubtransporthttp.h"
#include "iothubtransportamqp.h"
#include "iothubtransportmqtt.h"
#include "iothub_message.h"
#include "azure_c_shared_utility/threadapi.h"
#include "messageproperties.h"
#include "broker.h"

#include <parson.h>

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include "azure_c_shared_utility/vector.h"
#include "azure_c_shared_utility/xlogging.h"
#include "azure_c_shared_utility/strings.h"
#include "azure_c_shared_utility/platform.h"
#include "azure_c_shared_utility/tlsio.h"
#include "azure_uamqp_c/message_receiver.h"
#include "azure_uamqp_c/message.h"
#include "azure_uamqp_c/messaging.h"
#include "azure_uamqp_c/amqpalloc.h"
#include "azure_uamqp_c/saslclientio.h"
#include "azure_uamqp_c/sasl_plain.h"

#ifndef _ECHOREUQEST
#define ECHOREUQEST "echo request sent successfully"
#endif

#ifndef _ECHOREPLY
#define ECHOREPLY "echo response received successfully"
#endif

#ifndef _TIMEOUT
#define TIMEOUT "poll event hub time out"
#endif

typedef struct IOTHUBDEVICEPING_HANDLE_DATA_TAG
{
    THREAD_HANDLE threadHandle;
    IOTHUB_CLIENT_LL_HANDLE iotHubClientHandle;
    LOCK_HANDLE lockHandle;
    BROKER_HANDLE broker;
    IOTHUBDEVICEPING_CONFIG *config;
} IOTHUBDEVICEPING_HANDLE_DATA;

typedef struct MESSAGE_RECEIVER_CONTEXT_TAG
{
    THREAD_HANDLE threadHandle;
    IOTHUBDEVICEPING_HANDLE_DATA *moduleHandleData;
    const char *eventHubHost;
    const char *eventHubKeyName;
    const char *eventHubKey;
    const char *address;
    time_t begin_execution_time;
    int num_message_received;
    bool message_received;
} MESSAGE_RECEIVER_CONTEXT;

static const MESSAGE_COUNT = 1;
static const int MAX_DRAIN_TIME_IN_SECONDS = 10;
static bool g_echoreply = false;

static int strcmp_i(const char* lhs, const char* rhs)
{
    char lc, rc;
    int cmp;

    do
    {
        lc = *lhs++;
        rc = *rhs++;
        cmp = tolower(lc) - tolower(rc);
    } while (cmp == 0 && lc != 0 && rc != 0);

    return cmp;
}

static IOTHUBMESSAGE_DISPOSITION_RESULT create_and_publish_gateway_message(MESSAGE_CONFIG messageConfig, MODULE_HANDLE moduleHandle, MESSAGE_HANDLE *messageHandle, int count)
{
    IOTHUBMESSAGE_DISPOSITION_RESULT result;
    IOTHUBDEVICEPING_HANDLE_DATA *moduleHandleData = moduleHandle;
    // create gateway message
    *messageHandle = Message_Create(&messageConfig);
    if (*messageHandle == NULL)
    {
        LogError("failed to create gateway message");
        result = IOTHUBMESSAGE_REJECTED;
    }
    else
    {
        int i = count;
        while (i != 0)
        {
            i--;
            if (Lock(moduleHandleData->lockHandle) == LOCK_OK)
            {
                // publish gateway message to broker
                if (Broker_Publish(moduleHandleData->broker, moduleHandleData, *messageHandle) != BROKER_OK)
                {
                    LogError("failed to publish gateway message");
                    result = IOTHUBMESSAGE_REJECTED;
                }
                else
                {
                    result = IOTHUBMESSAGE_ACCEPTED;
                }
                (void)Unlock(moduleHandleData->lockHandle);
            }
        }
    }
    return result;
}

static char *concatenate(const char *s1, const char *s2)
{
    char *result = malloc(strlen(s1) + strlen(s2) + 1);
    if (result == NULL)
    {
        LogError("malloc failed");
    }
    else
    {
        strcpy(result, s1);
        strcat(result, s2);
    }
    return result;
}

// callback of AMQP message receiver
static AMQP_VALUE on_message_received(const void *context, MESSAGE_HANDLE message)
{
    MESSAGE_RECEIVER_CONTEXT *ctx = (MESSAGE_RECEIVER_CONTEXT *)context;
    IOTHUBDEVICEPING_HANDLE_DATA *moduleHandleData = (IOTHUBDEVICEPING_HANDLE_DATA *)ctx->moduleHandleData;
    AMQP_VALUE result;
    BINARY_DATA binary_data;
    MESSAGE_HANDLE messageHandle;
    MESSAGE_CONFIG messageConfig;

    MAP_HANDLE propertiesMap = Map_Create(NULL);
    if (propertiesMap == NULL)
    {
        LogError("unable to create a Map");
    }
    else
    {
        // if failed to get amqp data, sink "messaging_delivery_rejected" to logger
        if (message_get_body_amqp_data(message, 0, &binary_data) != 0)
        {
            LogError("Cannot get message data");
            result = messaging_delivery_rejected("Rejected due to failure reading AMQP message", "Failed reading message body");
            if (Map_AddOrUpdate(propertiesMap, "messaging_delivery_rejected", ECHOREPLY) != MAP_OK)
            {
                LogError("unable to Map_AddOrUpdate ECHOREPLY");
                return result;
            }
            messageConfig.size = strlen("messaging_delivery_rejected");
            messageConfig.source = "messaging_delivery_rejected";
            messageConfig.sourceProperties = propertiesMap;
            if (create_and_publish_gateway_message(messageConfig, moduleHandleData, &messageHandle, MESSAGE_COUNT) != IOTHUBMESSAGE_ACCEPTED)
            {
                LogError("iot hub message rejected");
            }
        }
        else
        {
            // otherwise, check if binary_data.bytes = ECHOREUQEST
            result = messaging_delivery_accepted();
            if (strstr((const char *)binary_data.bytes, ECHOREUQEST) != NULL)
            {
                // ECHOREUQEST has been received
                if (Map_AddOrUpdate(propertiesMap, "ECHOREPLY", ECHOREPLY) != MAP_OK)
                {
                    LogError("unable to Map_AddOrUpdate ECHOREPLY");
                    return result;
                }
                // set g_echoreply to true to stop the rest of threads
                g_echoreply = true;
                ctx->message_received = g_echoreply;
                // sink ECHOREPLY to logger
                messageConfig.size = strlen(ECHOREPLY);
                messageConfig.source = ECHOREPLY;
                messageConfig.sourceProperties = propertiesMap;
                if (create_and_publish_gateway_message(messageConfig, moduleHandleData, &messageHandle, MESSAGE_COUNT) != IOTHUBMESSAGE_ACCEPTED)
                {
                    LogError("iot hub message rejected");
                }
                LogInfo("%d number of message received by thread %d\r\n", ctx->num_message_received, ctx->threadHandle);
            }
            // no matter if ECHOREUQEST has been received or not, increment the counter
            ctx->num_message_received++;
            LogInfo("Message received: %s; Length: %u, Thread: %d\r\n", (const char *)binary_data.bytes, binary_data.length, ctx->threadHandle);
        }
        Message_Destroy(messageHandle);
    }
    return result;
}

static int poll_eventhub_thread(void *param)
{
    int result;
    XIO_HANDLE sasl_io = NULL;
    CONNECTION_HANDLE connection = NULL;
    SESSION_HANDLE session = NULL;
    LINK_HANDLE link = NULL;
    MESSAGE_RECEIVER_HANDLE message_receiver = NULL;

    MESSAGE_RECEIVER_CONTEXT *message_receiver_context = (MESSAGE_RECEIVER_CONTEXT *)param;

    // create SASL plain handler
    SASL_PLAIN_CONFIG sasl_plain_config = {message_receiver_context->eventHubKeyName, message_receiver_context->eventHubKey, NULL};
    SASL_MECHANISM_HANDLE sasl_mechanism_handle = saslmechanism_create(saslplain_get_interface(), &sasl_plain_config);
    XIO_HANDLE tls_io;

    // create the TLS IO
    TLSIO_CONFIG tls_io_config = {message_receiver_context->eventHubHost, 5671};
    const IO_INTERFACE_DESCRIPTION *tlsio_interface = platform_get_default_tlsio();
    tls_io = xio_create(tlsio_interface, &tls_io_config);

    // create the SASL client IO using the TLS IO
    SASLCLIENTIO_CONFIG sasl_io_config;
    sasl_io_config.underlying_io = tls_io;
    sasl_io_config.sasl_mechanism = sasl_mechanism_handle;
    sasl_io = xio_create(saslclientio_get_interface_description(), &sasl_io_config);

    // create the connection, session and link
    connection = connection_create(sasl_io, message_receiver_context->eventHubHost, "whatever", NULL, NULL);
    session = session_create(connection, NULL, NULL);

    // set incoming window to 1000 for the session
    session_set_incoming_window(session, 1000);

    char tempBuffer[256];
    const char filter_name[] = "apache.org:selector-filter:string";
    time_t receiveTimeRangeStart = time(NULL);
    int filter_string_length = sprintf(tempBuffer, "amqp.annotation.x-opt-enqueuedtimeutc > %llu", ((unsigned long long)receiveTimeRangeStart - 30) * 1000);
    if (filter_string_length < 0)
    {
        LogError("Failed creating filter set with enqueuedtimeutc filter.");
        result = -1;
    }
    else
    {
        // create the filter set to be used for the source of the link
        filter_set filter_set = amqpvalue_create_map();
        AMQP_VALUE filter_key = amqpvalue_create_symbol(filter_name);
        AMQP_VALUE descriptor = amqpvalue_create_symbol(filter_name);
        AMQP_VALUE filter_value = amqpvalue_create_string(tempBuffer);
        AMQP_VALUE described_filter_value = amqpvalue_create_described(descriptor, filter_value);
        amqpvalue_set_map_value(filter_set, filter_key, described_filter_value);
        amqpvalue_destroy(filter_key);
        if (filter_set == NULL)
        {
            LogError("Failed creating filter set with enqueuedtimeutc filter.");
            result = -1;
        }
        else
        {
            AMQP_VALUE target = NULL;
            AMQP_VALUE source = NULL;
            // create the source of the link
            SOURCE_HANDLE source_handle = source_create();
            AMQP_VALUE address_value = amqpvalue_create_string(message_receiver_context->address);
            source_set_address(source_handle, address_value);
            source_set_filter(source_handle, filter_set);
            amqpvalue_destroy(address_value);
            source = amqpvalue_create_source(source_handle);
            source_destroy(source_handle);

            if (source == NULL)
            {
                LogError("Failed creating source for link.");
                result = -1;
            }
            else
            {
                AMQP_VALUE target = messaging_create_target("messages/events");
                link = link_create(session, "receiver-link", role_receiver, source, target);
                link_set_rcv_settle_mode(link, receiver_settle_mode_first);
                amqpvalue_destroy(source);
                amqpvalue_destroy(target);

                // create a message receiver
                message_receiver = messagereceiver_create(link, NULL, NULL);
                if ((message_receiver == NULL) ||
                    (messagereceiver_open(message_receiver, on_message_received, message_receiver_context) != 0))
                {
                    result = -1;
                    LogError("messagereceiver_open failed");
                }
                else
                {
                    time_t nowExecutionTime;
                    time_t beginExecutionTime = message_receiver_context->begin_execution_time;
                    double timespan;
                    // poll event hub until time out or the message is received
                    while ((nowExecutionTime = time(NULL)), timespan = difftime(nowExecutionTime, beginExecutionTime), timespan < MAX_DRAIN_TIME_IN_SECONDS)
                    {
                        connection_dowork(connection);
                        ThreadAPI_Sleep(10);

                        // even when a thread find the ECHOREUQEST message e.g. message_received and g_echoreply both be true
                        // it will still finish receiving the remaining messages in the incomming window
                        // all other threads will break
                        if (g_echoreply)
                        {
                            break;
                        }
                    }
                    if (timespan >= MAX_DRAIN_TIME_IN_SECONDS)
                    {
                        // time out. message not received
                        result = -1;
                        MAP_HANDLE propertiesMap = Map_Create(NULL);
                        if (Map_AddOrUpdate(propertiesMap, "TIMEOUT", TIMEOUT) != MAP_OK)
                        {
                            LogError("unable to Map_AddOrUpdate TIMEOUT");
                        }
                        else
                        {
                            // sink TIMEOUT message
                            MESSAGE_CONFIG messageConfig;
                            messageConfig.size = strlen(TIMEOUT);
                            messageConfig.source = TIMEOUT;
                            messageConfig.sourceProperties = propertiesMap;

                            MESSAGE_HANDLE messageHandle = Message_Create(&messageConfig);
                            if (messageHandle == NULL)
                            {
                                LogError("unable to create ECHOREUQEST message");
                            }
                            else if (create_and_publish_gateway_message(messageConfig, message_receiver_context->moduleHandleData, &messageHandle, MESSAGE_COUNT) != IOTHUBMESSAGE_ACCEPTED)
                            {
                                LogError("iot hub message rejected");
                            }
                            else
                            {
                                Message_Destroy(messageHandle);
                            }
                        }
                    }
                    result = 0;
                }
            }
            messagereceiver_destroy(message_receiver);
        }
        amqpvalue_destroy(described_filter_value);
        amqpvalue_destroy(filter_set);
    }
    link_destroy(link);
    session_destroy(session);
    connection_destroy(connection);
    xio_destroy(sasl_io);
    xio_destroy(tls_io);
    saslmechanism_destroy(sasl_mechanism_handle);
    return result;
}

static int poll_eventhub_entry(IOTHUBDEVICEPING_HANDLE_DATA *handleData)
{
    const char *eventHubHost = ((const IOTHUBDEVICEPING_CONFIG *)(handleData->config))->EH_HOST;
    const char *eventHubKeyName = ((const IOTHUBDEVICEPING_CONFIG *)(handleData->config))->EH_KEY_NAME;
    const char *eventHubKey = ((const IOTHUBDEVICEPING_CONFIG *)(handleData->config))->EH_KEY;
    const char *eventHubCompatibleName = ((const IOTHUBDEVICEPING_CONFIG *)(handleData->config))->EH_COMP_NAME;
    const char *eventHubPartitionNum = ((const IOTHUBDEVICEPING_CONFIG *)(handleData->config))->EH_PARTITION_NUM;

    int partitionNum = (int)atoi(((const IOTHUBDEVICEPING_CONFIG *)(handleData->config))->EH_PARTITION_NUM);

    if (platform_init() != 0)
    {
        LogError("platform_init failed");
        return -1;
    }
    else
    {
        MESSAGE_RECEIVER_CONTEXT *message_receiver_context_arr = (MESSAGE_RECEIVER_CONTEXT *)malloc(sizeof(MESSAGE_RECEIVER_CONTEXT) * partitionNum);
        if (message_receiver_context_arr == NULL)
        {
            LogError("malloc returned NULL\r\n");
            return -1;
        }
        else
        {
            char *result1 = concatenate("amqps://", eventHubHost);
            char *result2 = concatenate("/", eventHubCompatibleName);
            char *result3 = concatenate((const char *)result1, (const char *)result2);
            char *result4 = concatenate((const char *)result3, "/ConsumerGroups/$Default/Partitions/");

            // launch x num of threads for x num of partitions
            int i = 0;
            for (i = 0; i < partitionNum; i++)
            {
                int length = snprintf(NULL, 0, "%d", i);
                char *str = malloc(length + 1);
                snprintf(str, length + 1, "%d", i);
                message_receiver_context_arr[i].address = (char *)concatenate((const char *)result4, (const char *)str);
                message_receiver_context_arr[i].moduleHandleData = handleData;
                message_receiver_context_arr[i].eventHubHost = eventHubHost;
                message_receiver_context_arr[i].eventHubKeyName = eventHubKeyName;
                message_receiver_context_arr[i].eventHubKey = eventHubKey;
                message_receiver_context_arr[i].begin_execution_time = time(NULL);
                message_receiver_context_arr[i].num_message_received = 0;
                message_receiver_context_arr[i].message_received = false;
                free(str);

                if (ThreadAPI_Create(&message_receiver_context_arr[i].threadHandle, poll_eventhub_thread, &message_receiver_context_arr[i]) != THREADAPI_OK)
                {
                    LogError("failed to spawn a thread");
                    return -1;
                }
                else
                {
                    // all is fine apparently
                }
            }
            // wait for all threads to exit
            for (i = 0; i < partitionNum; i++)
            {
                int thread_result;
                if (ThreadAPI_Join(message_receiver_context_arr[i].threadHandle, &thread_result) != THREADAPI_OK)
                {
                    LogError("ThreadAPI_Join() for message receiver threads returned an error");
                    return -1;
                }
                else
                {
                    LogInfo("%d messages received by thread %d result %d\r\n", message_receiver_context_arr[i].num_message_received, message_receiver_context_arr[i].threadHandle, thread_result);
                }
            }
            //deinit the platform once
            platform_deinit();
        }
        free(message_receiver_context_arr);
    }
    return 0;
}

static void send_confirmation_callback(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void *userContextCallback)
{
    IOTHUBDEVICEPING_HANDLE_DATA *moduleHandleData = (IOTHUBDEVICEPING_HANDLE_DATA *)userContextCallback;
    if (result == IOTHUB_CLIENT_CONFIRMATION_OK)
    {
        // poll event hub
        if (poll_eventhub_entry(moduleHandleData) != 0)
        {
            LogError("poll_eventhub_entry failed.");
        }
    }
    LogInfo("Press any key to exit...\r\n");
}

static IOTHUB_MESSAGE_HANDLE IoTHubMessage_CreateFromGWMessage(MESSAGE_HANDLE message)
{
    IOTHUB_MESSAGE_HANDLE result;
    const CONSTBUFFER *content = Message_GetContent(message);

    result = IoTHubMessage_CreateFromByteArray(content->buffer, content->size);
    if (result == NULL)
    {
        LogError("IoTHubMessage_CreateFromByteArray failed\r\n");
        /*return as is*/
    }
    else
    {
        MAP_HANDLE iothubMessageProperties = IoTHubMessage_Properties(result);
        CONSTMAP_HANDLE gwMessageProperties = Message_GetProperties(message);
        const char *const *keys;
        const char *const *values;
        size_t nProperties;
        if (ConstMap_GetInternals(gwMessageProperties, &keys, &values, &nProperties) != CONSTMAP_OK)
        {
            LogError("unable to get properties of the GW message\r\n");
            IoTHubMessage_Destroy(result);
            result = NULL;
        }
        else
        {
            size_t i;
            for (i = 0; i < nProperties; i++)
            {
                if (
                    (strcmp(keys[i], "deviceName") != 0) &&
                    (strcmp(keys[i], "deviceKey") != 0))
                {

                    if (Map_AddOrUpdate(iothubMessageProperties, keys[i], values[i]) != MAP_OK)
                    {
                        LogError("unable to Map_AddOrUpdate\r\n");
                        break;
                    }
                }
            }

            if (i == nProperties)
            {
                /*all is fine, return as is*/
            }
            else
            {
                IoTHubMessage_Destroy(result);
                result = NULL;
            }
        }
        ConstMap_Destroy(gwMessageProperties);
    }
    return result;
}

static int device_ping_thread(void *param)
{
    // wait for gw to start
    (void)ThreadAPI_Sleep(2000);
    // send message to iot hub and publish it to broker
    IOTHUBDEVICEPING_HANDLE_DATA *moduleHandleData = (IOTHUBDEVICEPING_HANDLE_DATA *)param;
    MESSAGE_HANDLE messageHandle;
    MAP_HANDLE propertiesMap = Map_Create(NULL);
    if (propertiesMap == NULL)
    {
        LogError("unable to create a Map");
        return 1;
    }
    else
    {
        if (Map_AddOrUpdate(propertiesMap, "ECHOREUQEST", ECHOREUQEST) != MAP_OK)
        {
            LogError("unable to Map_AddOrUpdate ECHOREUQEST");
        }
        else if (Map_AddOrUpdate(propertiesMap, "REQUEST_PROTOCOL", moduleHandleData->config->DataProtocol) != MAP_OK)
        {
            LogError("unable to Map_AddOrUpdate REQUEST_PROTOCOL");
        }
        else
        {
            MESSAGE_CONFIG messageConfig;
            messageConfig.size = strlen(ECHOREUQEST);
            messageConfig.source = ECHOREUQEST;
            messageConfig.sourceProperties = propertiesMap;
            // create and publish gateway message
            if (create_and_publish_gateway_message(messageConfig, moduleHandleData, &messageHandle, MESSAGE_COUNT) != IOTHUBMESSAGE_ACCEPTED)
            {
                LogError("iot hub message rejected");
                return 1;
            }
            // create iot hub message from gateway message
            IOTHUB_MESSAGE_HANDLE iotHubMessage = IoTHubMessage_CreateFromGWMessage(messageHandle);
            if (iotHubMessage == NULL)
            {
                LogError("unable to IoTHubMessage_CreateFromGWMessage (internal)\r\n");
                Message_Destroy(messageHandle);
                return 1;
            }
            else
            {
                if (IoTHubClient_LL_SendEventAsync(moduleHandleData->iotHubClientHandle, iotHubMessage, send_confirmation_callback, (void *)moduleHandleData) != IOTHUB_CLIENT_OK)
                {
                    LogError("unable to IoTHubClient_SendEventAsync\r\n");
                    Message_Destroy(messageHandle);
                    return 1;
                }

                IOTHUB_CLIENT_STATUS status;
                while ((IoTHubClient_LL_GetSendStatus(moduleHandleData->iotHubClientHandle, &status) == IOTHUB_CLIENT_OK) && (status == IOTHUB_CLIENT_SEND_STATUS_BUSY))
                {
                    IoTHubClient_LL_DoWork(moduleHandleData->iotHubClientHandle);
                    ThreadAPI_Sleep(100);
                }

                IoTHubMessage_Destroy(iotHubMessage);
            }
            Message_Destroy(messageHandle);
        }

    }
    return 0;
}

static IOTHUBDEVICEPING_CONFIG *clone_iothub_device_ping_config(const IOTHUBDEVICEPING_CONFIG *config)
{
    IOTHUBDEVICEPING_CONFIG *clonedConfig = malloc(sizeof(IOTHUBDEVICEPING_CONFIG));
    if (clonedConfig == NULL)
    {
        LogError("malloc returned NULL\r\n");
    }
    else
    {
        if ((clonedConfig->DeviceConnectionString = (const char *)malloc(strlen(config->DeviceConnectionString) + 1)) != NULL)
        {
            memcpy((char *)clonedConfig->DeviceConnectionString, (char *)config->DeviceConnectionString, strlen(config->DeviceConnectionString) + 1);
        }
        else
        {
            LogError("config->DeviceConnectionString malloc returned NULL\r\n");
        }
        if ((clonedConfig->EH_HOST = (const char *)malloc(strlen(config->EH_HOST) + 1)) != NULL)
        {
            memcpy((char *)clonedConfig->EH_HOST, (char *)config->EH_HOST, strlen(config->EH_HOST) + 1);
        }
        else
        {
            LogError("config->EH_HOST malloc returned NULL\r\n");
        }
        if ((clonedConfig->EH_KEY_NAME = (const char *)malloc(strlen(config->EH_KEY_NAME) + 1)) != NULL)
        {
            memcpy((char *)clonedConfig->EH_KEY_NAME, (char *)config->EH_KEY_NAME, strlen(config->EH_KEY_NAME) + 1);
        }
        else
        {
            LogError("config->EH_KEY_NAME malloc returned NULL\r\n");
        }
        if ((clonedConfig->EH_KEY = (const char *)malloc(strlen(config->EH_KEY) + 1)) != NULL)
        {
            memcpy((char *)clonedConfig->EH_KEY, (char *)config->EH_KEY, strlen(config->EH_KEY) + 1);
        }
        else
        {
            LogError("config->EH_KEY malloc returned NULL\r\n");
        }
        if ((clonedConfig->EH_COMP_NAME = (const char *)malloc(strlen(config->EH_COMP_NAME) + 1)) != NULL)
        {
            memcpy((char *)clonedConfig->EH_COMP_NAME, (char *)config->EH_COMP_NAME, strlen(config->EH_COMP_NAME) + 1);
        }
        else
        {
            LogError("config->EH_COMP_NAME malloc returned NULL\r\n");
        }
        if ((clonedConfig->EH_PARTITION_NUM = (const char *)malloc(strlen(config->EH_PARTITION_NUM) + 1)) != NULL)
        {
            memcpy((char *)clonedConfig->EH_PARTITION_NUM, (char *)config->EH_PARTITION_NUM, strlen(config->EH_PARTITION_NUM) + 1);
        }
        else
        {
            LogError("config->EH_PARTITION_NUM malloc returned NULL\r\n");
        }
        if ((clonedConfig->DataProtocol = (const char *)malloc(strlen(config->DataProtocol) + 1)) != NULL)
        {
            memcpy((char *)clonedConfig->DataProtocol, (char *)config->DataProtocol, strlen(config->DataProtocol) + 1);
        }
        else
        {
            LogError("config->DataProtocol malloc returned NULL\r\n");
        }
    }
    return clonedConfig;
}

static MODULE_HANDLE IoTHubDevicePing_Create(BROKER_HANDLE brokerHandle, const void *configuration)
{
    // set g_echoreply false
    g_echoreply = false;
    IOTHUBDEVICEPING_HANDLE_DATA *result = NULL;
    if (
        (brokerHandle == NULL) ||
        (configuration == NULL) ||
        (((const IOTHUBDEVICEPING_CONFIG *)configuration)->DeviceConnectionString == "NULL") ||
        (((const IOTHUBDEVICEPING_CONFIG *)configuration)->EH_HOST == "NULL") ||
        (((const IOTHUBDEVICEPING_CONFIG *)configuration)->EH_KEY_NAME == "NULL") ||
        (((const IOTHUBDEVICEPING_CONFIG *)configuration)->EH_KEY == "NULL") ||
        (((const IOTHUBDEVICEPING_CONFIG *)configuration)->EH_COMP_NAME == "NULL") ||
        (((const IOTHUBDEVICEPING_CONFIG *)configuration)->DataProtocol == "NULL"))
    {
        LogError("invalid arg brokerHandle=%p, configuration=%p\r\n", brokerHandle, configuration);
        result = NULL;
    }
    else
    {
        result = malloc(sizeof(IOTHUBDEVICEPING_HANDLE_DATA));
        if (result == NULL)
        {
            LogError("malloc returned NULL\r\n");
        }
        else
        {
            result->config = (IOTHUBDEVICEPING_CONFIG *)clone_iothub_device_ping_config(configuration);
            IOTHUB_CLIENT_TRANSPORT_PROVIDER transportProvider;
            if (strcmp_i(((const IOTHUBDEVICEPING_CONFIG *)configuration)->DataProtocol, "MQTT") == 0)
            {
                transportProvider = MQTT_Protocol;
            }
            else if (strcmp_i(((const IOTHUBDEVICEPING_CONFIG *)configuration)->DataProtocol, "AMQP") == 0)
            {
                transportProvider = AMQP_Protocol;
            }
            else if (strcmp_i(((const IOTHUBDEVICEPING_CONFIG *)configuration)->DataProtocol, "HTTP") == 0)
            {
                transportProvider = HTTP_Protocol;
            }
            else
            {
                LogError("invalid data protocol\r\n");
                return result;
            }
            result->iotHubClientHandle = IoTHubClient_LL_CreateFromConnectionString(((const IOTHUBDEVICEPING_CONFIG *)configuration)->DeviceConnectionString, transportProvider);
            if (result->iotHubClientHandle == NULL)
            {
                free(result);
                result = NULL;
                LogError("iotHubClientHandle returned NULL\r\n");
            }
            else
            {
                result->broker = brokerHandle;
                result->lockHandle = Lock_Init();
                if (result->lockHandle == NULL)
                {
                    free(result);
                    result = NULL;
                    LogError("unable to Lock_Init");
                }
                else
                {
                    if (ThreadAPI_Create(&result->threadHandle, device_ping_thread, result) != THREADAPI_OK)
                    {
                        (void)Lock_Deinit(result->lockHandle);
                        free(result);
                        result = NULL;
                        LogError("failed to spawn a thread");
                    }
                    else
                    {
                        // all is fine apparently
                    }
                }
            }
        }
    }
    return result;
}

static MODULE_HANDLE IoTHubDevicePing_CreateFromJson(BROKER_HANDLE brokerHandle, const char* configuration)
{
    MODULE_HANDLE *result;
    if ((brokerHandle == NULL) || (configuration == NULL))
    {
        LogError("Invalid NULL parameter, brokerHandle=[%p] configuration=[%p]", brokerHandle, configuration);
        result = NULL;
    }
    else
    {
        JSON_Value *json = json_parse_string((const char *)configuration);
        if (json == NULL)
        {
            LogError("Unable to parse json string");
            result = NULL;
        }
        else
        {
            JSON_Object *obj = json_value_get_object(json);
            if (obj == NULL)
            {
                LogError("Expected a JSON Object in configuration");
                result = NULL;
            }
            else
            {
                const char *DeviceConnectionString;
                const char *eventHubHost;
                const char *eventHubKeyName;
                const char *eventHubKey;
                const char *eventHubCompatibleName;
                const char *eventHubPartitionNum;
                const char *dataProtocol;
                if ((DeviceConnectionString = json_object_get_string(obj, "DeviceConnectionString")) == "NULL" ||
                    (eventHubHost = json_object_get_string(obj, "EH_HOST")) == "NULL" ||
                    (eventHubKeyName = json_object_get_string(obj, "EH_KEY_NAME")) == "NULL" ||
                    (eventHubKey = json_object_get_string(obj, "EH_KEY")) == "NULL" ||
                    (eventHubCompatibleName = json_object_get_string(obj, "EH_COMP_NAME")) == "NULL" ||
                    (eventHubPartitionNum = json_object_get_string(obj, "EH_PARTITION_NUM")) == "NULL" ||
                    (dataProtocol = json_object_get_string(obj, "DataProtocol")) == "NULL") // NULL
                {
                    LogError("Did not find expected configuration");
                    result = NULL;
                }
                else
                {
                    MODULE_APIS apis;
                    Module_GetAPIS(&apis);
                    IOTHUBDEVICEPING_CONFIG llConfiguration;
                    llConfiguration.DeviceConnectionString = DeviceConnectionString;
                    llConfiguration.EH_HOST = eventHubHost;
                    llConfiguration.EH_KEY_NAME = eventHubKeyName;
                    llConfiguration.EH_KEY = eventHubKey;
                    llConfiguration.EH_COMP_NAME = eventHubCompatibleName;
                    llConfiguration.EH_PARTITION_NUM = eventHubPartitionNum;
                    llConfiguration.DataProtocol = dataProtocol;
                    result = apis.Module_Create(brokerHandle, &llConfiguration);
                }
            }
            json_value_free(json);
        }
    }
    return result;
}

static void IoTHubDevicePing_Destroy(MODULE_HANDLE moduleHandle)
{
    IOTHUBDEVICEPING_HANDLE_DATA *handleData = moduleHandle;
    int notUsed;
    if (handleData->iotHubClientHandle != NULL)
    {
        IoTHubClient_LL_Destroy(handleData->iotHubClientHandle);
    }
    if (Lock(handleData->lockHandle) != LOCK_OK)
    {
        LogError("not able to Lock, still setting the thread to finish");
    }
    else
    {
        Unlock(handleData->lockHandle);
    }
    if (ThreadAPI_Join(handleData->threadHandle, &notUsed) != THREADAPI_OK)
    {
        LogError("unable to ThreadAPI_Join, still proceeding in _Destroy");
    }
    (void)Lock_Deinit(handleData->lockHandle);
    free(handleData);
}

static void IoTHubDevicePing_Receive(MODULE_HANDLE moduleHandle, MESSAGE_HANDLE messageHandle)
{
    (void)moduleHandle;
    (void)messageHandle;
    // no action, IoTHubDevicePing is not interested in any messages
}

static const MODULE_APIS moduleInterface =
{
    IoTHubDevicePing_CreateFromJson,
    IoTHubDevicePing_Create,
    IoTHubDevicePing_Destroy,
    IoTHubDevicePing_Receive,
    NULL
};

MODULE_EXPORT void Module_GetAPIS(MODULE_APIS *apis)
{
    if (!apis)
    {
        LogError("NULL passed to Module_GetAPIS");
    }
    else
    {
        (*apis) = moduleInterface;
    }
}
