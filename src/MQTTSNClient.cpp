#include "MQTTSNClient.h"

MQTTSN::Client::Client(NETWORK& network, unsigned int command_timeout_ms)  : ipstack(network), packetid()
{
  last_sent = TIMER();
  last_received = TIMER();
  last_ping = TIMER();
  ping_outstanding = false;
  clearSubscriptions();
  clearRegistrations();
  this->command_timeout_ms = command_timeout_ms;
  isconnected = false;
  isStateAwake = false;

  // Zero will settings
  willTopic[0] = 0;
  willMsg[0] = 0;
  willMsgLen = 0;
  willQoS = QOS0;
  willRetain = false;

#if MQTTCLIENT_QOS1 || MQTTCLIENT_QOS2
  inflightMsgid = 0;
  inflightQoS = QOS0;
#endif


#if MQTTCLIENT_QOS2
  pubrel = false;
  for (int i = 0; i < MAX_INCOMING_QOS2_MESSAGES; ++i)
    incomingQoS2messages[i] = 0;
#endif
}

void MQTTSN::Client::clearSubscriptions()
{
  for (int i = 0; i < MAX_MESSAGE_HANDLERS; ++i)
  {
    free(messageHandlers[i].topicFilter);
    messageHandlers[i].topicFilter = 0;
  }
}

void MQTTSN::Client::clearRegistrations()
{
  for (int i = 0; i < MAX_REGISTRATIONS; ++i)
    registrations[i].id = 0;
}

#if MQTTCLIENT_QOS2
bool MQTTSN::Client::isQoS2msgidFree(unsigned short id)
{
  for (int i = 0; i < MAX_INCOMING_QOS2_MESSAGES; ++i)
  {
    if (incomingQoS2messages[i] == id)
      return false;
  }
  return true;
}

bool MQTTSN::Client::useQoS2msgid(unsigned short id)
{
  for (int i = 0; i < MAX_INCOMING_QOS2_MESSAGES; ++i)
  {
    if (incomingQoS2messages[i] == 0)
    {
      incomingQoS2messages[i] = id;
      return true;
    }
  }
  return false;
}
#endif


int MQTTSN::Client::sendPacket(int length, TIMER& timer)
{
  int rc = FAILURE,
    sent = 0;

  while (sent < length && !timer.expired())
  {
    rc = ipstack.write(&sendbuf[sent], length, timer.left_ms());
    if (rc < 0)  // there was an error writing the data
      break;
    sent += rc;
  }
  if (sent == length)
  {
    if (this->duration > 0)
      last_sent.countdown(this->duration); // record the fact that we have successfully sent the packet
    rc = SUCCESS;
  }
  else
    rc = FAILURE;

#if defined(MQTT_DEBUG)
  char printbuf[50];
  DEBUG("Rc %d from sending packet %s\n", rc, MQTTPacket_toString(printbuf, sizeof(printbuf), sendbuf, length));
#endif
  return rc;
}


int MQTTSN::Client::decodePacket(int* value, int timeout)
{
  unsigned char c;
  int multiplier = 1;
  int len = 0;
  const int MAX_NO_OF_REMAINING_LENGTH_BYTES = 4;

  *value = 0;
  do
  {
    int rc = MQTTSNPACKET_READ_ERROR;

    if (++len > MAX_NO_OF_REMAINING_LENGTH_BYTES)
    {
      rc = MQTTSNPACKET_READ_ERROR; /* bad data */
      goto exit;
    }
    rc = ipstack.read(&c, 1, timeout);
    if (rc != 1)
      goto exit;
    *value += (c & 127) * multiplier;
    multiplier *= 128;
  } while ((c & 128) != 0);
exit:
  return len;
}


/**
 * If any read fails in this method, then we should disconnect from the network, as on reconnect
 * the packets can be retried.
 * @param timeout the max time to wait for the packet read to complete, in milliseconds
 * @return the MQTT packet type, or -1 if none
 */
int MQTTSN::Client::readPacket(TIMER& timer)
{
  int rc = FAILURE;
  int len = 0;  // the length of the whole packet including length field
  int lenlen = 0;
  int datalen = 0;

  #define MQTTSN_MIN_PACKET_LENGTH 2
  // 1. read the packet, datagram style
  if ((len = ipstack.read(readbuf, MAX_PACKET_SIZE, timer.left_ms())) < MQTTSN_MIN_PACKET_LENGTH)
    goto exit;

  // 2. read the length.  This is variable in itself
  lenlen = MQTTSNPacket_decode(readbuf, len, &datalen);
  if (datalen != len)
    goto exit; // there was an error

  rc = readbuf[lenlen];
  if (this->duration > 0)
    last_received.countdown(this->duration); // record the fact that we have successfully received a packet
exit:

#if defined(MQTT_DEBUG)
  char printbuf[50];
  DEBUG("Rc %d from receiving packet %s\n", rc, MQTTPacket_toString(printbuf, sizeof(printbuf), readbuf, len));
#endif
  return rc;
}


// assume topic filter and name is in correct format
// # can only be at end
// + and # can only be next to separator
bool MQTTSN::Client::isTopicMatched(MQTTSN_topicid& topicFilter, MQTTSN_topicid& topicName)
{
  char* curf = topicFilter.data.long_.name;
  char* curn = topicName.data.long_.name;
  char* curn_end = curn + topicName.data.long_.len;

  while (*curf && curn < curn_end)
  {
    if (*curn == '/' && *curf != '/')
      break;
    if (*curf != '+' && *curf != '#' && *curf != *curn)
      break;
    if (*curf == '+')
    {   // skip until we meet the next separator, or end of string
      char* nextpos = curn + 1;
      while (nextpos < curn_end && *nextpos != '/')
        nextpos = ++curn + 1;
    }
    else if (*curf == '#')
      curn = curn_end - 1;    // skip until end of string
    curf++;
    curn++;
  };

  return (curn == curn_end) && (*curf == '\0');
}

bool MQTTSNtopic_equals(MQTTSN_topicid& topic, MQTTSN_topicid& filter)
{
  #ifdef DEBUG
  DEBUG_PORT.println("Check equality");
  DEBUG_PORT.print(topic.type); DEBUG_PORT.print(", ");
  DEBUG_PORT.println(filter.type);

  DEBUG_PORT.print(topic.data.id); DEBUG_PORT.print(", ");
  DEBUG_PORT.println(filter.data.id);
  #endif

  if( (topic.type == filter.type) && (topic.data.id == filter.data.id) ) return true;
  return false;
}

int MQTTSN::Client::deliverMessage(MQTTSN_topicid& topic, Message& message)
{
  int rc = FAILURE;

  // we have to find the right message handler - indexed by topic
  for (int i = 0; i < MAX_MESSAGE_HANDLERS; i++)
  {
    #ifdef DEBUG
    DEBUG_PORT.println((int)messageHandlers[i].topicFilter);
    #endif

    if (messageHandlers[i].topicFilter != 0 && (MQTTSNtopic_equals(topic, *(messageHandlers[i].topicFilter)) ||
            isTopicMatched(*(messageHandlers[i].topicFilter), topic)))
    {
      #ifdef DEBUG
      DEBUG_PORT.println("GOT MATCH!");
      #endif
        if (messageHandlers[i].fp.attached())
        {
            MessageData md(*messageHandlers[i].topicFilter, message);
            messageHandlers[i].fp(md);
            rc = SUCCESS;
        }
    }

  }

  if (rc == FAILURE && defaultMessageHandler.attached())
  {
    MessageData md(topic, message);
    defaultMessageHandler(md);
    rc = SUCCESS;
  }

  return rc;
}

/*int MQTTSN::Client::yield(unsigned long timeout_ms)
{
    int rc = SUCCESS;
    TIMER timer = TIMER();

    timer.countdown_ms(timeout_ms);
    while (!timer.expired())
    {
        if (cycle(timer) == FAILURE)
        {
            rc = FAILURE;
            break;
        }
    }

    return rc;
}*/

int MQTTSN::Client::loop()
{
  TIMER timer = TIMER();
  timer.countdown_ms(1000);
  return cycle(timer);
}


int MQTTSN::Client::cycle(TIMER& timer)
{
  /* get one piece of work off the wire and one pass through */

  // read the socket, see what work is due
  unsigned short packet_type = readPacket(timer);

  int len = 0;
  unsigned char rc = SUCCESS;

  switch (packet_type)
  {
    case MQTTSN_CONNACK:
    case MQTTSN_PUBACK:
    case MQTTSN_SUBACK:
    case MQTTSN_REGACK:
      break;
    case MQTTSN_REGISTER:
    {
      unsigned short topicid, packetid;
      MQTTSNString topicName;
      rc = MQTTSN_RC_ACCEPTED;
      if (MQTTSNDeserialize_register(&topicid, &packetid, &topicName, readbuf, MAX_PACKET_SIZE) != 1)
        goto exit;
      len = MQTTSNSerialize_regack(sendbuf, MAX_PACKET_SIZE, topicid, packetid, rc);
      if (len <= 0)
        rc = FAILURE;
      else
        rc = sendPacket(len, timer);
      break;
    }
    case MQTTSN_PUBLISH:
      MQTTSN_topicid topicid;
      Message msg;
      if (MQTTSNDeserialize_publish((unsigned char*)&msg.dup, (int*)&msg.qos, (unsigned char*)&msg.retained, (unsigned short*)&msg.id, &topicid,
                           (unsigned char**)&msg.payload, (int*)&msg.payloadlen, readbuf, MAX_PACKET_SIZE) != 1)
        goto exit;
    #if MQTTCLIENT_QOS2
      if (msg.qos != QOS2)
    #endif
        deliverMessage(topicid, msg);
    #if MQTTCLIENT_QOS2
      else if (isQoS2msgidFree(msg.id))
      {
        if (useQoS2msgid(msg.id))
          deliverMessage(topicName, msg);
        else
          WARN("Maximum number of incoming QoS2 messages exceeded");
      }
    #endif
    #if MQTTCLIENT_QOS1 || MQTTCLIENT_QOS2
      if (msg.qos != QOS0)
      {
        if (msg.qos == QOS1)
          len = MQTTSNSerialize_puback(sendbuf, MAX_PACKET_SIZE, topicid.data.id, msg.id, 0);
        else if (msg.qos == QOS2)
          len = MQTTSNSerialize_pubrec(sendbuf, MAX_PACKET_SIZE, msg.id);
        if (len <= 0)
          rc = FAILURE;
        else
          rc = sendPacket(len, timer);
        if (rc == FAILURE)
          goto exit; // there was a problem
      }
    #endif
      break;
    #if MQTTCLIENT_QOS2
    case PUBREC:
      unsigned short mypacketid;
      unsigned char dup, type;
      if (MQTTDeserialize_ack(&type, &dup, &mypacketid, readbuf, MAX_PACKET_SIZE) != 1)
        rc = FAILURE;
      else if ((len = MQTTSerialize_ack(sendbuf, MAX_PACKET_SIZE, PUBREL, 0, mypacketid)) <= 0)
        rc = FAILURE;
      else if ((rc = sendPacket(len, timer)) != SUCCESS) // send the PUBREL packet
        rc = FAILURE; // there was a problem
      if (rc == FAILURE)
        goto exit; // there was a problem
      break;
    case PUBCOMP:
      break;
    #endif
    case MQTTSN_PINGRESP:
      ping_outstanding = false;
      break;
    case MQTTSN_PINGREQ:
      len = MQTTSNSerialize_pingresp(sendbuf, MAX_PACKET_SIZE);
      if(len <= 0)
        rc = FAILURE;
      else
        rc = sendPacket(len, timer);
      break;
  }
  keepalive();
exit:
  if (rc == SUCCESS)
    rc = packet_type;
  return rc;
}

int MQTTSN::Client::sendPing()
{
  int rc = FAILURE;

  MQTTSNString clientid = MQTTSNString_initializer;
  TIMER timer = TIMER(1000);
  int len = MQTTSNSerialize_pingreq(sendbuf, MAX_PACKET_SIZE, clientid);
  if (len > 0 && (rc = sendPacket(len, timer)) == SUCCESS) // send the ping packet
  {
    ping_outstanding = true;
    last_ping.countdown_ms(1000);
  }
  #ifdef DEBUG
  DEBUG_PORT.print("Sent Ping:");
  DEBUG_PORT.println(rc);
  #endif
  return rc;
}

int MQTTSN::Client::keepalive()
{
  int rc = FAILURE;

  if (!isconnected)
    goto exit;

  if (duration == 0)
  {
    rc = SUCCESS;
    goto exit;
  }

  if (last_sent.expired() || last_received.expired())
  {
    if (!ping_outstanding)
    {
      ping_retries = 0;
      rc = sendPing();
    }
  }

  // if(ping_outstanding)
  // {
  //   DEBUG_PORT.print(" - ");
  //   DEBUG_PORT.println(last_ping.left_ms());
  // }

  if(ping_outstanding && last_ping.expired())
  {
    // Check if we havent reached pingresp timeout, retry or mark as disconnected
    ping_retries++;

    if(ping_retries < MAX_PING_RETRIES)
    {
      rc = sendPing();
    }
    else
    {
      isconnected = false;
      ping_outstanding = false;
    }
  }

exit:
  return rc;
}


// only used in single-threaded mode where one command at a time is in process
int MQTTSN::Client::waitfor(int packet_type, TIMER& timer)
{
  int rc = FAILURE;

  do
  {
    if (timer.expired())
      break; // we timed out
  }
  while ((rc = cycle(timer)) != packet_type);

  return rc;
}

void MQTTSN::Client::setWill(const char* topic, char* msg, size_t msgLen, enum QoS qos, bool retain)
{
  strncpy(willTopic, topic, MAX_WILL_TOPIC_LENGTH);
  memcpy(willMsg, msg, msgLen);
  willMsgLen = msgLen;
  willQoS = qos;
  willRetain = retain;
}

int MQTTSN::Client::connect(MQTTSNPacket_connectData& options)
{
  TIMER connect_timer = TIMER(command_timeout_ms);
  int rc = FAILURE;
  int len = 0;

  if (isconnected) // don't send connect packet again if we are already connected
    goto exit;

  this->duration = options.duration;
  this->cleansession = options.cleansession;
  if ((len = MQTTSNSerialize_connect(sendbuf, MAX_PACKET_SIZE, &options)) <= 0)
    goto exit;
  if ((rc = sendPacket(len, connect_timer)) != SUCCESS)  // send the connect packet
    goto exit; // there was a problem

  if (this->duration > 0)
    last_received.countdown(this->duration);

  // If we set will flag in options, wait for MQTTSN_WILLTOPICREQ,
  // respond and wait for MQTTSN_WILLMSGREQ, respond and send MQTTSN_CONNACK
  if(options.willFlag)
  {
    if( waitfor(MQTTSN_WILLTOPICREQ, connect_timer) ==  MQTTSN_WILLTOPICREQ )
    {
      // Send will topic
      MQTTSNString willTopicStr;
      willTopicStr.cstring = willTopic;
      willTopicStr.lenstring.data = willTopic;
      willTopicStr.lenstring.len = strlen(willTopic);
      if ((len = MQTTSNSerialize_willtopic(sendbuf, MAX_PACKET_SIZE, willQoS, willRetain, willTopicStr)) <= 0)
        goto exit;
      if ((rc = sendPacket(len, connect_timer)) != SUCCESS)  // send the connect packet
        goto exit; // there was a problem
    }
    else
      goto exit;

    if (this->duration > 0)
      last_received.countdown(this->duration);

    if( waitfor(MQTTSN_WILLMSGREQ, connect_timer) ==  MQTTSN_WILLMSGREQ )
    {
      // Send will topic
      MQTTSNString willMsgStr;
      willMsgStr.cstring = NULL;
      willMsgStr.lenstring.data = willMsg;
      willMsgStr.lenstring.len = willMsgLen;
      if ((len = MQTTSNSerialize_willmsg(sendbuf, MAX_PACKET_SIZE, willMsgStr)) <= 0)
        goto exit;
      if ((rc = sendPacket(len, connect_timer)) != SUCCESS)  // send the connect packet
        goto exit; // there was a problem
    }
    else
      goto exit;
  }

  if (this->duration > 0)
      last_received.countdown(this->duration);

  // this will be a blocking call, wait for the connack
  if (waitfor(MQTTSN_CONNACK, connect_timer) == MQTTSN_CONNACK)
  {
      //unsigned char connack_rc = 255;
      int connack_rc = 255;
      if (MQTTSNDeserialize_connack(&connack_rc, readbuf, MAX_PACKET_SIZE) == 1)
          rc = connack_rc;
      else
          rc = FAILURE;
  }
  else
      rc = FAILURE;

#if MQTTCLIENT_QOS2
  // resend an inflight publish
  if (inflightMsgid >0 && inflightQoS == QOS2 && pubrel)
  {
      if ((len = MQTTSerialize_ack(sendbuf, MAX_PACKET_SIZE, PUBREL, 0, inflightMsgid)) <= 0)
          rc = FAILURE;
      else
          rc = publish(len, connect_timer, inflightQoS);
  }
  else
#endif
#if MQTTCLIENT_QOS1 || MQTTCLIENT_QOS2
  if (inflightMsgid > 0)
  {
      memcpy(sendbuf, pubbuf, MAX_PACKET_SIZE);
      rc = publish(inflightLen, connect_timer, inflightQoS);
  }
#endif

exit:
  if (rc == SUCCESS)
      isconnected = true;
  return rc;
}


int MQTTSN::Client::connect()
{
  MQTTSNPacket_connectData default_options = MQTTSNPacket_connectData_initializer;
  return connect(default_options);
}

int MQTTSN::Client::subscribe(const char* topic, enum QoS qos, messageHandler mh)
{
  MQTTSN_topicid* topicid = (MQTTSN_topicid*)malloc(sizeof(*topicid));
  topicid->type = MQTTSN_TOPIC_TYPE_NORMAL;
  topicid->data.long_.name = (char*)topic;
  topicid->data.long_.len = strnlen(topic, MAX_REGISTRATION_TOPIC_NAME_LENGTH);
  return subscribe(*topicid, qos, mh);
}

int MQTTSN::Client::subscribe(MQTTSN_topicid& topicFilter, enum QoS qos, messageHandler messageHandler)
{
  bool freeHandler;
  int rc = FAILURE;
  TIMER timer = TIMER(command_timeout_ms);
  int len = 0;

  if (!isconnected)
    goto exit;

  freeHandler = false;
  for (int i = 0; i < MAX_MESSAGE_HANDLERS; ++i)
  {
    if (messageHandlers[i].topicFilter == 0)
    {
      freeHandler = true;
      break;
    }
  }
  if (!freeHandler)
  {                                 // No message handler free
    rc = MAX_SUBSCRIPTIONS_EXCEEDED;
    #ifdef DEBUG
    DEBUG_PORT.println("No free handlers");
    #endif
    goto exit;
  }

  len = MQTTSNSerialize_subscribe(sendbuf, MAX_PACKET_SIZE, 0, qos, packetid.getNext(), &topicFilter);
  if (len <= 0)
    goto exit;
  if ((rc = sendPacket(len, timer)) != SUCCESS) // send the subscribe packet
    goto exit;             // there was a problem

  if (waitfor(MQTTSN_SUBACK, timer) == MQTTSN_SUBACK)      // wait for suback
  {
    #ifdef DEBUG
    DEBUG_PORT.println("Got suback");
    #endif
    int grantedQoS = -1;
    unsigned short mypacketid;
    unsigned char rc, returncode;
    if (MQTTSNDeserialize_suback(&grantedQoS, &topicFilter.data.id, &mypacketid, &returncode, readbuf, MAX_PACKET_SIZE) == 1)
      rc = grantedQoS;

    #ifdef DEBUG
    DEBUG_PORT.print("rc "); DEBUG_PORT.println(rc);
    DEBUG_PORT.print("returncode "); DEBUG_PORT.println(returncode);
    #endif

    if (returncode == MQTTSN_RC_ACCEPTED)
    {
      #ifdef DEBUG
      DEBUG_PORT.println("Suback accepted");
      #endif

      for (int i = 0; i < MAX_MESSAGE_HANDLERS; ++i)
      {
        if (messageHandlers[i].topicFilter == 0)
        {
          messageHandlers[i].topicFilter = &topicFilter;
          messageHandlers[i].fp.attach(messageHandler);
          rc = 0;

          #ifdef DEBUG
          DEBUG_PORT.println("Subscribed mh");
          DEBUG_PORT.println(messageHandlers[i].topicFilter->data.id);
          DEBUG_PORT.println(messageHandlers[i].topicFilter->type);
          #endif

          break;
        }
      }
    }
  }
  else
    rc = FAILURE;

exit:
  if (rc != SUCCESS){
    isconnected = false;
    #ifdef DEBUG
    DEBUG_PORT.println("Subscribe failure");
    #endif
  }
  return rc;
}

// TODO: Clear subscription from messageHandlers
int MQTTSN::Client::unsubscribe(MQTTSN_topicid& topicFilter)
{
  int rc = FAILURE;
  TIMER timer = TIMER(command_timeout_ms);
  int len = 0;

  if (!isconnected)
    goto exit;

  if ((len = MQTTSNSerialize_unsubscribe(sendbuf, MAX_PACKET_SIZE, packetid.getNext(), &topicFilter)) <= 0)
    goto exit;
  if ((rc = sendPacket(len, timer)) != SUCCESS) // send the unsubscribe packet
    goto exit; // there was a problem

  if (waitfor(MQTTSN_UNSUBACK, timer) == MQTTSN_UNSUBACK)
  {
    unsigned short mypacketid;  // should be the same as the packetid above
    if (MQTTSNDeserialize_unsuback(&mypacketid, readbuf, MAX_PACKET_SIZE) == 1)
      rc = 0;
  }
  else
    rc = FAILURE;

exit:
  if (rc != SUCCESS)
    isconnected = false;
  return rc;
}

int MQTTSN::Client::registerTopic(const char* topic, size_t topiclen)
{
  int rc = FAILURE;
  TIMER timer = TIMER(command_timeout_ms);
  int len = 0;

  if (!isconnected)
    goto exit;

  MQTTSNString topicStr;
  topicStr.cstring = (char*)topic;
  topicStr.lenstring.len = topiclen;
  if((len = MQTTSNSerialize_register(sendbuf, MAX_PACKET_SIZE, 0x000, packetid.getNext(), &topicStr)) <= 0)
    goto exit;
  if((rc = sendPacket(len, timer)) != SUCCESS)
    goto exit;

  if( waitfor(MQTTSN_REGACK, timer) == MQTTSN_REGACK )
  {
    unsigned short mypacketid, newTopicId;  // should be the same as the packetid above
    unsigned char return_code;
    int res = MQTTSNDeserialize_regack(&newTopicId, &mypacketid, &return_code, readbuf, MAX_PACKET_SIZE);

    // Check that return code is accepted, else do nothing
    if(return_code == MQTTSN_RC_ACCEPTED)
    {
      // Register topic in memory
      for(int i = 0; i < MAX_REGISTRATIONS; i++)
      {
        if(registrations[i].id == 0)
        {
          registrations[i].id = newTopicId;
          memcpy(registrations[i].name, topic, topiclen);
          // string security:
          registrations[i].name[topiclen] = 0;
          rc = 0;
          break;
        }
      }
    }

  }
  else
    rc = FAILURE;

exit:
  if (rc != SUCCESS)
    isconnected = false;
  return rc;

}

int MQTTSN::Client::publish(int len, TIMER& timer, enum QoS qos)
{
  int rc;

  if ((rc = sendPacket(len, timer)) != SUCCESS) // send the publish packet
    goto exit; // there was a problem

#if MQTTCLIENT_QOS1
  if (qos == QOS1)
  {
    if (waitfor(MQTTSN_PUBACK, timer) == MQTTSN_PUBACK)
    {
      unsigned short mypacketid;
      unsigned char type;
      if (MQTTSNDeserialize_ack(&type, &mypacketid, readbuf, MAX_PACKET_SIZE) != 1)
        rc = FAILURE;
      else if (inflightMsgid == mypacketid)
        inflightMsgid = 0;
    }
    else
      rc = FAILURE;
  }
#elif MQTTCLIENT_QOS2
  else if (qos == QOS2)
  {
    if (waitfor(PUBCOMP, timer) == PUBCOMP)
    {
      unsigned short mypacketid;
      unsigned char type;
      if (MQTTDeserialize_ack(&type, &mypacketid, readbuf, MAX_PACKET_SIZE) != 1)
        rc = FAILURE;
      else if (inflightMsgid == mypacketid)
        inflightMsgid = 0;
    }
    else
      rc = FAILURE;
  }
#endif

exit:
  if (rc != SUCCESS)
    isconnected = false;
  return rc;
}

int MQTTSN::Client::publish(const char* topic, void* payload, size_t payloadlen, enum QoS qos, bool retained)
{
  MQTTSN_topicid topicObj;
  topicObj.type = MQTTSN_TOPIC_TYPE_NORMAL;
  topicObj.data.id = 0;

  // get registered topic id
  for(int i = 0; i < MAX_REGISTRATIONS; i++)
  {
    if( registrations[i].id != 0 ) if( strncmp(registrations[i].name, topic, MAX_REGISTRATION_TOPIC_NAME_LENGTH) == 0 )
    {
      topicObj.data.id = registrations[i].id;
      break;
    }
  }

  if(topicObj.data.id == 0) return FAILURE; // Couldn't get registered id

  return publish(topicObj, payload, payloadlen, qos, retained);
}

int MQTTSN::Client::publish(MQTTSN_topicid& topic, void* payload, size_t payloadlen, unsigned short& id, enum QoS qos, bool retained)
{
  int rc = FAILURE;
  TIMER timer = TIMER(command_timeout_ms);
  int len = 0;

  if (!isconnected)
    goto exit;

#if MQTTCLIENT_QOS1 || MQTTCLIENT_QOS2
  if (qos == QOS1 || qos == QOS2)
    id = packetid.getNext();
#endif

  len = MQTTSNSerialize_publish(sendbuf, MAX_PACKET_SIZE, 0, qos, retained, id,
          topic, (unsigned char*)payload, payloadlen);
  if (len <= 0)
    goto exit;

#if MQTTCLIENT_QOS1 || MQTTCLIENT_QOS2
  if (!cleansession)
  {
    memcpy(pubbuf, sendbuf, len);
    inflightMsgid = id;
    inflightLen = len;
    inflightQoS = qos;
#if MQTTCLIENT_QOS2
    pubrel = false;
#endif
  }
#endif

  rc = publish(len, timer, qos);
exit:
  return rc;
}


int MQTTSN::Client::publish(MQTTSN_topicid& topicName, void* payload, size_t payloadlen, enum QoS qos, bool retained)
{
  unsigned short id = 0;  // dummy - not used for anything
  return publish(topicName, payload, payloadlen, id, qos, retained);
}


int MQTTSN::Client::publish(MQTTSN_topicid& topicName, Message& message)
{
  return publish(topicName, message.payload, message.payloadlen, message.qos, message.retained);
}


int MQTTSN::Client::disconnect(unsigned short duration)
{
  int rc = FAILURE;
  TIMER timer = TIMER(command_timeout_ms);     // we might wait for incomplete incoming publishes to complete
  int int_duration = (duration == 0) ? -1 : (int)duration;
  int len = MQTTSNSerialize_disconnect(sendbuf, MAX_PACKET_SIZE, int_duration);
  if (len > 0)
    rc = sendPacket(len, timer);            // send the disconnect packet

  isconnected = false;
  return rc;
}

int MQTTSN::Client::awake()
{
  int rc = FAILURE;
  isStateAwake = true;

  MQTTSNString clientid = MQTTSNString_initializer;
  TIMER timer = TIMER(1000);
  int len = MQTTSNSerialize_pingreq(sendbuf, MAX_PACKET_SIZE, clientid);
  if (len > 0 && (rc = sendPacket(len, timer)) == SUCCESS) // send the ping packet
  {
    // wait for ping resp or publish
    while(rc != MQTTSN_PINGRESP)
    {
      timer.countdown_ms(1000);
      do
      {
        if (timer.expired())
        {
          // we timed out
          isStateAwake = false;
          return FAILURE;
        }
        rc = cycle(timer);
      }
      while (rc != MQTTSN_PINGRESP && rc != MQTTSN_PUBLISH);
    }
  }

  isStateAwake = false;
  return rc;
}
