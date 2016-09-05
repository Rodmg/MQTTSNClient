/*******************************************************************************
 * Copyright (c) 2014, 2015 IBM Corp.
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *    http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at
 *   http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *    Ian Craggs - initial API and implementation and/or initial documentation
 *    Rodrigo Mendez - Refactor for 8bit microcontrollers, easier API, bug fixes and new implementations
 *******************************************************************************/

#ifndef MQTTSNCLIENT_H
#define MQTTSNCLIENT_H

#include "FP.h"
#include "MQTTSNPacket.h"
#include "stdio.h"
#include <VirtualTimer.h>
#include <WSNetwork.h>

//#define DEBUG
#define DEBUG_PORT Serial

#define NETWORK WSNetwork
#define TIMER VirtualTimer
#define MAX_MESSAGE_HANDLERS 6
#define MAX_PACKET_SIZE 60

// Data limits
#if !defined(MAX_REGISTRATIONS)
  #define MAX_REGISTRATIONS 5
#endif
#if !defined(MAX_REGISTRATION_TOPIC_NAME_LENGTH)
  #define MAX_REGISTRATION_TOPIC_NAME_LENGTH 20
#endif
#if !defined(MAX_INCOMING_QOS2_MESSAGES)
  #define MAX_INCOMING_QOS2_MESSAGES 10
#endif

#if !defined(MAX_WILL_TOPIC_LENGTH)
  #define MAX_WILL_TOPIC_LENGTH 20
#endif
#if !defined(MAX_WILL_MSG_LENGTH)
  #define MAX_WILL_MSG_LENGTH 40
#endif

#if !defined(MQTTSNCLIENT_QOS1)
  #define MQTTSNCLIENT_QOS1 1
#endif
#if !defined(MQTTSNCLIENT_QOS2)
  #define MQTTSNCLIENT_QOS2 0
#endif

#if !defined(MAX_PING_RETRIES)
  #define MAX_PING_RETRIES 3
#endif

namespace MQTTSN
{


enum QoS { QOS0, QOS1, QOS2 };

// all failure return codes must be negative
enum returnCode { MAX_SUBSCRIPTIONS_EXCEEDED = -3, BUFFER_OVERFLOW = -2, FAILURE = -1, SUCCESS = 0 };


struct Message
{
  enum QoS qos;
  bool retained;
  bool dup;
  unsigned short id;
  void *payload;
  size_t payloadlen;
};


struct MessageData
{
  MessageData(MQTTSN_topicid &aTopic, struct Message &aMessage)  : message(aMessage), topic(aTopic)
  { }

  struct Message &message;
  MQTTSN_topicid &topic;
};


class PacketId
{
public:
  PacketId()
  {
    next = 0;
  }

  int getNext()
  {
    return next = (next == MAX_PACKET_ID) ? 1 : ++next;
  }

private:
  static const int MAX_PACKET_ID = 65535;
  int next;
};


/**
 * @class MQTTSNClient
 * @brief blocking, non-threaded MQTTSN client API
 *
 * This version of the API blocks on all method calls, until they are complete.  This means that only one
 * MQTT request can be in process at any one time.
 * @param NETWORK a network class which supports send, receive
 * @param TIMER a timer class with the methods:
 */
class Client
{

public:

  typedef void (*messageHandler)(MessageData&);

  /** Construct the client
   *  @param network - pointer to an instance of the NETWORK class - must be connected to the endpoint
   *      before calling MQTT connect
   *  @param limits an instance of the Limit class - to alter limits as required
   */
  Client(NETWORK& network, unsigned int command_timeout_ms = 30000);

  /** Set the default message handling callback - used for any message which does not match a subscription message handler
   *  @param mh - pointer to the callback function
   */
  void setDefaultMessageHandler(messageHandler mh)
  {
      defaultMessageHandler.attach(mh);
  }

  /** MQTT setWill - Setup will data
   *  Must be set before connect()
   *  @param topic - cstr
   *  @param msg - message
   *  @param msgLen - msg length
   *  @param qos - QOS
   *  @param retain - retain flag
   */
  void setWill(const char* topic, char* msg, size_t msgLen, enum QoS qos = QOS0, bool retain = false);

  /** MQTT Connect - send an MQTT connect packet down the network and wait for a Connack
   *  The nework object must be connected to the network endpoint before calling this
   *  Default connect options are used
   *  @return success code -
   */
  int connect();

  /** MQTT Connect - send an MQTT connect packet down the network and wait for a Connack
   *  The nework object must be connected to the network endpoint before calling this
   *  @param options - connect options
   *  @return success code -
   */
  int connect(MQTTSNPacket_connectData& options);

  /** MQTT Publish - send an MQTT register packet and wait for regack
   *  @param topic - the topic to register
   *  @param topiclen - the lenght of the topic
   *  @return success code -
   */
  int registerTopic(const char *topic, size_t topiclen);

  /** MQTT Publish - send an MQTT publish packet and wait for all acks to complete for all QoSs
   *  @param topic - the topic to publish to
   *  @param payload - the message to send
   *  @param payloadlen - lenght of the payload
   *  @return success code -
   */
  int publish(const char* topic, void* payload, size_t payloadlen, enum QoS qos = QOS0, bool retained = false);

  /** MQTT Publish - send an MQTT publish packet and wait for all acks to complete for all QoSs
   *  @param topic - the topic to publish to
   *  @param message - the message to send
   *  @return success code -
   */
  int publish(MQTTSN_topicid& topic, Message& message);

  /** MQTT Publish - send an MQTT publish packet and wait for all acks to complete for all QoSs
   *  @param topic - the topic to publish to
   *  @param payload - the data to send
   *  @param payloadlen - the length of the data
   *  @param qos - the QoS to send the publish at
   *  @param retained - whether the message should be retained
   *  @return success code -
   */
  int publish(MQTTSN_topicid &topic, void* payload, size_t payloadlen, enum QoS qos = QOS0, bool retained = false);

  /** MQTT Publish - send an MQTT publish packet and wait for all acks to complete for all QoSs
   *  @param topic - the topic to publish to
   *  @param payload - the data to send
   *  @param payloadlen - the length of the data
   *  @param id - the packet id used - returned
   *  @param qos - the QoS to send the publish at
   *  @param retained - whether the message should be retained
   *  @return success code -
   */
  int publish(MQTTSN_topicid& topic, void* payload, size_t payloadlen, unsigned short& id, enum QoS qos = QOS1, bool retained = false);

  /** Clear Subscriptions: Clear messageHandlers
   *
   */
  void clearSubscriptions();

  /** Clear Registrations
   */
  void clearRegistrations();

  /** MQTT Subscribe - send an MQTT subscribe packet and wait for the suback
   *  @param topic - a topic pattern which can include wildcards
   *  @param qos - the MQTT QoS to subscribe at
   *  @param mh - the callback function to be invoked when a message is received for this subscription
   *  @return success code -
   */
  int subscribe(const char* topic, enum QoS qos, messageHandler mh);

  /** MQTT Subscribe - send an MQTT subscribe packet and wait for the suback
   *  @param topicFilter - a topic pattern which can include wildcards
   *  @param qos - the MQTT QoS to subscribe at
   *  @param mh - the callback function to be invoked when a message is received for this subscription
   *  @return success code -
   */
  int subscribe(MQTTSN_topicid& topicFilter, enum QoS qos, messageHandler mh);

  /** MQTT Unsubscribe - send an MQTT unsubscribe packet and wait for the unsuback
   *  @param topicFilter - a topic pattern which can include wildcards
   *  @return success code -
   */
  int unsubscribe(MQTTSN_topicid& topicFilter);

  /** MQTT Disconnect - send an MQTT disconnect packet, and clean up any state
   *  @param duration - used for sleeping clients, 0 means no duration
   *  @return success code -
   */
  int disconnect(unsigned short duration = 0);

  /** A call to this API must be made within the keepAlive interval to keep the MQTT connection alive
   *  yield can be called if no other MQTT operation is needed.  This will also allow messages to be
   *  received.
   *  @param timeout_ms the time to wait, in milliseconds
   *  @return success code - on failure, this means the client has disconnected
   */
  //int yield(unsigned long timeout_ms = 1000L);

  int loop();

  /** Is the client connected?
   *  @return flag - is the client connected or not?
   */
  bool isConnected()
  {
    return isconnected;
  }

  int awake();

protected:

  int sendPing();
  int cycle(TIMER& timer);
  int waitfor(int packet_type, TIMER& timer);

private:

  char willTopic[MAX_WILL_TOPIC_LENGTH];
  char willMsg[MAX_WILL_MSG_LENGTH];
  size_t willMsgLen;
  enum QoS willQoS;
  bool willRetain;

  int keepalive();
  int publish(int len, TIMER& timer, enum QoS qos);

  int decodePacket(int* value, int timeout);
  int readPacket(TIMER& timer);
  int sendPacket(int length, TIMER& timer);
  int deliverMessage(MQTTSN_topicid& topic, Message& message);
  bool isTopicMatched(MQTTSN_topicid& topicFilter, MQTTSN_topicid& topicName);

  NETWORK& ipstack;
  unsigned long command_timeout_ms;

  unsigned char sendbuf[MAX_PACKET_SIZE];
  unsigned char readbuf[MAX_PACKET_SIZE];

  TIMER last_sent, last_received, last_ping;
  unsigned short duration;
  bool ping_outstanding;
  uint8_t ping_retries;
  bool cleansession;

  PacketId packetid;

  struct MessageHandlers
  {
    MQTTSN_topicid* topicFilter;
    FP<void, MessageData&> fp;
  } messageHandlers[MAX_MESSAGE_HANDLERS];      // Message handlers are indexed by subscription topic

  FP<void, MessageData&> defaultMessageHandler;

  bool isconnected;
  bool isStateAwake;

  struct Registrations
  {
    unsigned short id;
    char name[MAX_REGISTRATION_TOPIC_NAME_LENGTH];
  } registrations[MAX_REGISTRATIONS];

#if MQTTCLIENT_QOS1 || MQTTCLIENT_QOS2
  unsigned char pubbuf[MAX_PACKET_SIZE];  // store the last publish for sending on reconnect
  int inflightLen;
  unsigned short inflightMsgid;
  enum QoS inflightQoS;
#endif

#if MQTTCLIENT_QOS2
  bool pubrel;
  unsigned short incomingQoS2messages[MAX_INCOMING_QOS2_MESSAGES];
  bool isQoS2msgidFree(unsigned short id);
  bool useQoS2msgid(unsigned short id);
#endif

};

}

#endif //MQTTSNCLIENT_H
