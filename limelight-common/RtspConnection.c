#include "Limelight-internal.h"
#include "Rtsp.h"

#include <enet/enet.h>

#define RTSP_MAX_RESP_SIZE 32768
#define RTSP_TIMEOUT_SEC 10

static int currentSeqNumber;
static char rtspTargetUrl[256];
static char sessionIdString[16];
static int hasSessionId;
static char responseBuffer[RTSP_MAX_RESP_SIZE];
static int rtspClientVersion;

static SOCKET sock = INVALID_SOCKET;
static ENetHost* client;
static ENetPeer* peer;

// Create RTSP Option
static POPTION_ITEM createOptionItem(char* option, char* content)
{
    POPTION_ITEM item = malloc(sizeof(*item));
    if (item == NULL) {
        return NULL;
    }

    item->option = malloc(strlen(option) + 1);
    if (item->option == NULL) {
        free(item);
        return NULL;
    }

    strcpy(item->option, option);

    item->content = malloc(strlen(content) + 1);
    if (item->content == NULL) {
        free(item->option);
        free(item);
        return NULL;
    }

    strcpy(item->content, content);

    item->next = NULL;
    item->flags = FLAG_ALLOCATED_OPTION_FIELDS;

    return item;
}

// Add an option to the RTSP Message
static int addOption(PRTSP_MESSAGE msg, char* option, char* content)
{
    POPTION_ITEM item = createOptionItem(option, content);
    if (item == NULL) {
        return 0;
    }

    insertOption(&msg->options, item);
    msg->flags |= FLAG_ALLOCATED_OPTION_ITEMS;

    return 1;
}

// Create an RTSP Request
static int initializeRtspRequest(PRTSP_MESSAGE msg, char* command, char* target)
{
    char sequenceNumberStr[16];
    char clientVersionStr[16];

    // FIXME: Hacked CSeq attribute due to RTSP parser bug
    createRtspRequest(msg, NULL, 0, command, target, "RTSP/1.0",
        0, NULL, NULL, 0);

    sprintf(sequenceNumberStr, "%d", currentSeqNumber++);
    sprintf(clientVersionStr, "%d", rtspClientVersion);
    if (!addOption(msg, "CSeq", sequenceNumberStr) ||
        !addOption(msg, "X-GS-ClientVersion", clientVersionStr)) {
        freeMessage(msg);
        return 0;
    }

    return 1;
}

// Send RTSP message and get response over ENet
static int transactRtspMessageEnet(PRTSP_MESSAGE request, PRTSP_MESSAGE response, int expectingPayload, int* error) {
    ENetEvent event;
    char* serializedMessage;
    int messageLen;
    int offset;
    ENetPacket* packet;
    char* payload;
    int payloadLength;
    int ret;

    // We're going to handle the payload separately, so temporarily set the payload to NULL
    payload = request->payload;
    payloadLength = request->payloadLength;
    request->payload = NULL;
    request->payloadLength = 0;
    
    // Serialize the RTSP message into a message buffer
    serializedMessage = serializeRtspMessage(request, &messageLen);
    if (serializedMessage == NULL) {
        ret = 0;
        goto Exit;
    }
    
    // Create the reliable packet that describes our outgoing message
    packet = enet_packet_create(serializedMessage, messageLen, ENET_PACKET_FLAG_RELIABLE);
    if (packet == NULL) {
        ret = 0;
        goto Exit;
    }
    
    // Send the message
    enet_peer_send(peer, 0, packet);
    enet_host_flush(client);

    // If we have a payload to send, we'll need to send that separately
    if (payload != NULL) {
        packet = enet_packet_create(payload, payloadLength, ENET_PACKET_FLAG_RELIABLE);
        if (packet == NULL) {
            ret = 0;
            goto Exit;
        }

        // Send the payload
        enet_peer_send(peer, 0, packet);
        enet_host_flush(client);
    }
    
    // Wait for a reply
    if (enet_host_service(client, &event, RTSP_TIMEOUT_SEC * 1000) <= 0 ||
        event.type != ENET_EVENT_TYPE_RECEIVE) {
        Limelog("Failed to receive RTSP reply\n");
        ret = 0;
        goto Exit;
    }

    if (event.packet->dataLength > RTSP_MAX_RESP_SIZE) {
        Limelog("RTSP message too long\n");
        ret = 0;
        goto Exit;
    }

    // Copy the data out and destroy the packet
    memcpy(responseBuffer, event.packet->data, event.packet->dataLength);
    offset = event.packet->dataLength;
    enet_packet_destroy(event.packet);

    // Wait for the payload if we're expecting some
    if (expectingPayload) {
        // Only wait 1 second since the packets should be here immediately
        // after the header.
        if (enet_host_service(client, &event, 1000) <= 0 ||
            event.type != ENET_EVENT_TYPE_RECEIVE) {
            Limelog("Failed to receive RTSP reply payload\n");
            ret = 0;
            goto Exit;
        }

        if (event.packet->dataLength + offset > RTSP_MAX_RESP_SIZE) {
            Limelog("RTSP message payload too long\n");
            ret = 0;
            goto Exit;
        }

        // Copy the payload out to the end of the response buffer and destroy the packet
        memcpy(&responseBuffer[offset], event.packet->data, event.packet->dataLength);
        offset += event.packet->dataLength;
        enet_packet_destroy(event.packet);
    }
        
    if (parseRtspMessage(response, responseBuffer, offset) == RTSP_ERROR_SUCCESS) {
        // Successfully parsed response
        ret = 1;
    }
    else {
        Limelog("Failed to parse RTSP response\n");
        ret = 0;
    }

Exit:
    // Swap back the payload pointer to avoid leaking memory later
    request->payload = payload;
    request->payloadLength = payloadLength;

    // Free the serialized buffer
    if (serializedMessage != NULL) {
        free(serializedMessage);
    }

    return ret;
}

// Send RTSP message and get response over TCP
static int transactRtspMessageTcp(PRTSP_MESSAGE request, PRTSP_MESSAGE response, int expectingPayload, int* error) {
    SOCK_RET err;
    int ret = 0;
    int offset;
    char* serializedMessage = NULL;
    int messageLen;

    *error = -1;

    sock = connectTcpSocket(&RemoteAddr, RemoteAddrLen, 48010, RTSP_TIMEOUT_SEC);
    if (sock == INVALID_SOCKET) {
        *error = LastSocketError();
        return ret;
    }
    enableNoDelay(sock);
    setRecvTimeout(sock, RTSP_TIMEOUT_SEC);

    serializedMessage = serializeRtspMessage(request, &messageLen);
    if (serializedMessage == NULL) {
        closeSocket(sock);
        sock = INVALID_SOCKET;
        return ret;
    }

    // Send our message
    err = send(sock, serializedMessage, messageLen, 0);
    if (err == SOCKET_ERROR) {
        *error = LastSocketError();
        Limelog("Failed to send RTSP message: %d\n", *error);
        goto Exit;
    }

    // Read the response until the server closes the connection
    offset = 0;
    for (;;) {
        err = recv(sock, &responseBuffer[offset], RTSP_MAX_RESP_SIZE - offset, 0);
        if (err <= 0) {
            // Done reading
            break;
        }
        offset += err;

        // Warn if the RTSP message is too big
        if (offset == RTSP_MAX_RESP_SIZE) {
            Limelog("RTSP message too long\n");
            goto Exit;
        }
    }

    if (parseRtspMessage(response, responseBuffer, offset) == RTSP_ERROR_SUCCESS) {
        // Successfully parsed response
        ret = 1;
    }
    else {
        Limelog("Failed to parse RTSP response\n");
    }

Exit:
    if (serializedMessage != NULL) {
        free(serializedMessage);
    }

    closeSocket(sock);
    sock = INVALID_SOCKET;
    return ret;
}

static int transactRtspMessage(PRTSP_MESSAGE request, PRTSP_MESSAGE response, int expectingPayload, int* error) {
    // Gen 5+ does RTSP over ENet not TCP
    if (ServerMajorVersion >= 5) {
        return transactRtspMessageEnet(request, response, expectingPayload, error);
    }
    else {
        return transactRtspMessageTcp(request, response, expectingPayload, error);
    }
}

// Terminate the RTSP Handshake process by shutting down the socket.
// The thread waiting on RTSP will close the socket.
void terminateRtspHandshake(void) {
    if (sock != INVALID_SOCKET) {
        shutdownTcpSocket(sock);
    }
    
    if (peer != NULL) {
        enet_peer_reset(peer);
        peer = NULL;
    }
    
    if (client != NULL) {
        enet_host_destroy(client);
        client = NULL;
    }
}

// Send RTSP OPTIONS request
static int requestOptions(PRTSP_MESSAGE response, int* error) {
    RTSP_MESSAGE request;
    int ret;

    *error = -1;

    ret = initializeRtspRequest(&request, "OPTIONS", rtspTargetUrl);
    if (ret != 0) {
        ret = transactRtspMessage(&request, response, 0, error);
        freeMessage(&request);
    }

    return ret;
}

// Send RTSP DESCRIBE request
static int requestDescribe(PRTSP_MESSAGE response, int* error) {
    RTSP_MESSAGE request;
    int ret;

    *error = -1;

    ret = initializeRtspRequest(&request, "DESCRIBE", rtspTargetUrl);
    if (ret != 0) {
        if (addOption(&request, "Accept",
            "application/sdp") &&
            addOption(&request, "If-Modified-Since",
                "Thu, 01 Jan 1970 00:00:00 GMT")) {
            ret = transactRtspMessage(&request, response, 1, error);
        }
        else {
            ret = 0;
        }
        freeMessage(&request);
    }

    return ret;
}

// Send RTSP SETUP request
static int setupStream(PRTSP_MESSAGE response, char* target, int* error) {
    RTSP_MESSAGE request;
    int ret;

    *error = -1;

    ret = initializeRtspRequest(&request, "SETUP", target);
    if (ret != 0) {
        if (hasSessionId) {
            if (!addOption(&request, "Session", sessionIdString)) {
                ret = 0;
                goto FreeMessage;
            }
        }

        if (addOption(&request, "Transport", " ") &&
            addOption(&request, "If-Modified-Since",
                "Thu, 01 Jan 1970 00:00:00 GMT")) {
            ret = transactRtspMessage(&request, response, 0, error);
        }
        else {
            ret = 0;
        }

    FreeMessage:
        freeMessage(&request);
    }

    return ret;
}

// Send RTSP PLAY request
static int playStream(PRTSP_MESSAGE response, char* target, int* error) {
    RTSP_MESSAGE request;
    int ret;

    *error = -1;

    ret = initializeRtspRequest(&request, "PLAY", target);
    if (ret != 0) {
        if (addOption(&request, "Session", sessionIdString)) {
            ret = transactRtspMessage(&request, response, 0, error);
        }
        else {
            ret = 0;
        }
        freeMessage(&request);
    }

    return ret;
}

// Send RTSP ANNOUNCE message
static int sendVideoAnnounce(PRTSP_MESSAGE response, int* error) {
    RTSP_MESSAGE request;
    int ret;
    int payloadLength;
    char payloadLengthStr[16];

    *error = -1;

    ret = initializeRtspRequest(&request, "ANNOUNCE", "streamid=video");
    if (ret != 0) {
        ret = 0;

        if (!addOption(&request, "Session", sessionIdString) ||
            !addOption(&request, "Content-type", "application/sdp")) {
            goto FreeMessage;
        }

        request.payload = getSdpPayloadForStreamConfig(rtspClientVersion, &payloadLength);
        if (request.payload == NULL) {
            goto FreeMessage;
        }
        request.flags |= FLAG_ALLOCATED_PAYLOAD;
        request.payloadLength = payloadLength;

        sprintf(payloadLengthStr, "%d", payloadLength);
        if (!addOption(&request, "Content-length", payloadLengthStr)) {
            goto FreeMessage;
        }

        ret = transactRtspMessage(&request, response, 0, error);

    FreeMessage:
        freeMessage(&request);
    }

    return ret;
}

// Perform RTSP Handshake with the streaming server machine as part of the connection process
int performRtspHandshake(void) {
    char urlAddr[URLSAFESTRING_LEN];

    // Initialize global state
    addrToUrlSafeString(&RemoteAddr, urlAddr);
    sprintf(rtspTargetUrl, "rtsp://%s", urlAddr);
    currentSeqNumber = 1;
    hasSessionId = 0;

    if (ServerMajorVersion == 3) {
        rtspClientVersion = 10;
    }
    else if (ServerMajorVersion == 4) {
        rtspClientVersion = 11;
    }
    else {
        rtspClientVersion = 12;
    }
    
    // Gen 5 servers use ENet to do the RTSP handshake
    if (ServerMajorVersion >= 5) {
        ENetAddress address;
        ENetEvent event;
        
        // Create a client that can use 1 outgoing connection and 1 channel
        client = enet_host_create(NULL, 1, 1, 0, 0);
        if (client == NULL) {
            return -1;
        }
    
        enet_address_set_host(&address, RemoteAddrString);
        address.port = 48010;
    
        // Connect to the host
        peer = enet_host_connect(client, &address, 1, 0);
        if (peer == NULL) {
            return -1;
        }
    
        // Wait for the connect to complete
        if (enet_host_service(client, &event, RTSP_TIMEOUT_SEC * 1000) <= 0 ||
            event.type != ENET_EVENT_TYPE_CONNECT) {
            Limelog("RTSP: Failed to connect to UDP port 48010\n");
            return -1;
        }

        // Ensure the connect verify ACK is sent immediately
        enet_host_flush(client);
    }

    {
        RTSP_MESSAGE response;
        int error = -1;

        if (!requestOptions(&response, &error)) {
            Limelog("RTSP OPTIONS request failed: %d\n", error);
            return error;
        }

        if (response.message.response.statusCode != 200) {
            Limelog("RTSP OPTIONS request failed: %d\n",
                response.message.response.statusCode);
            return response.message.response.statusCode;
        }

        freeMessage(&response);
    }

    {
        RTSP_MESSAGE response;
        int error = -1;

        if (!requestDescribe(&response, &error)) {
            Limelog("RTSP DESCRIBE request failed: %d\n", error);
            return error;
        }

        if (response.message.response.statusCode != 200) {
            Limelog("RTSP DESCRIBE request failed: %d\n",
                response.message.response.statusCode);
            return response.message.response.statusCode;
        }
        
        // The RTSP DESCRIBE reply will contain a collection of SDP media attributes that
        // describe the various supported video stream formats and include the SPS, PPS,
        // and VPS (if applicable). We will use this information to determine whether the
        // server can support HEVC. For some reason, they still set the MIME type of the HEVC
        // format to H264, so we can't just look for the HEVC MIME type. What we'll do instead is
        // look for the base 64 encoded VPS NALU prefix that is unique to the HEVC bitstream.
        if (StreamConfig.supportsHevc && strstr(response.payload, "sprop-parameter-sets=AAAAAU")) {
            NegotiatedVideoFormat = VIDEO_FORMAT_H265;
        }
        else {
            NegotiatedVideoFormat = VIDEO_FORMAT_H264;
        }

        freeMessage(&response);
    }

    {
        RTSP_MESSAGE response;
        char* sessionId;
        int error = -1;

        if (!setupStream(&response, "streamid=audio", &error)) {
            Limelog("RTSP SETUP streamid=audio request failed: %d\n", error);
            return error;
        }

        if (response.message.response.statusCode != 200) {
            Limelog("RTSP SETUP streamid=audio request failed: %d\n",
                response.message.response.statusCode);
            return response.message.response.statusCode;
        }

        sessionId = getOptionContent(response.options, "Session");
        if (sessionId == NULL) {
            Limelog("RTSP SETUP streamid=audio is missing session attribute");
            return -1;
        }

        strcpy(sessionIdString, sessionId);
        hasSessionId = 1;

        freeMessage(&response);
    }

    {
        RTSP_MESSAGE response;
        int error = -1;

        if (!setupStream(&response, "streamid=video", &error)) {
            Limelog("RTSP SETUP streamid=video request failed: %d\n", error);
            return error;
        }

        if (response.message.response.statusCode != 200) {
            Limelog("RTSP SETUP streamid=video request failed: %d\n",
                response.message.response.statusCode);
            return response.message.response.statusCode;
        }

        freeMessage(&response);
    }

    {
        RTSP_MESSAGE response;
        int error = -1;

        if (!sendVideoAnnounce(&response, &error)) {
            Limelog("RTSP ANNOUNCE request failed: %d\n", error);
            return error;
        }

        if (response.message.response.statusCode != 200) {
            Limelog("RTSP ANNOUNCE request failed: %d\n",
                response.message.response.statusCode);
            return response.message.response.statusCode;
        }

        freeMessage(&response);
    }

    {
        RTSP_MESSAGE response;
        int error = -1;

        if (!playStream(&response, "streamid=video", &error)) {
            Limelog("RTSP PLAY streamid=video request failed: %d\n", error);
            return error;
        }

        if (response.message.response.statusCode != 200) {
            Limelog("RTSP PLAY streamid=video failed: %d\n",
                response.message.response.statusCode);
            return response.message.response.statusCode;
        }

        freeMessage(&response);
    }

    {
        RTSP_MESSAGE response;
        int error = -1;

        if (!playStream(&response, "streamid=audio", &error)) {
            Limelog("RTSP PLAY streamid=audio request failed: %d\n", error);
            return error;
        }

        if (response.message.response.statusCode != 200) {
            Limelog("RTSP PLAY streamid=audio failed: %d\n",
                response.message.response.statusCode);
            return response.message.response.statusCode;
        }

        freeMessage(&response);
    }
    
    // Cleanup the ENet stuff
    if (ServerMajorVersion >= 5) {
        if (peer != NULL) {
            enet_peer_reset(peer);
            peer = NULL;
        }
        
        if (client != NULL) {
            enet_host_destroy(client);
            client = NULL;
        }
    }

    return 0;
}
