/**
 * @file       BlynkProtocol.h
 * @author     Volodymyr Shymanskyy
 * @date       Jan 2015
 * @brief      Blynk protocol implementation
 *
 */

#ifndef BlynkProtocol_h
#define BlynkProtocol_h

#include <string.h>
#include <stdlib.h>
#include <Blynk/BlynkDebug.h>
#include <Blynk/BlynkProtocolDefs.h>
#include <Blynk/BlynkApi.h>

template <class Transp>
class BlynkProtocol
    : public BlynkApi< BlynkProtocol<Transp> >
{
public:
    BlynkProtocol(Transp& conn)
        : conn(conn), authkey(NULL)
        , lastActivityIn(0)
        , lastActivityOut(0)
        , lastHeartbeat(0)
        , currentMsgId(0)
    {}

    bool connect();

    void run(void);

    void sendCmd(uint8_t cmd, uint16_t id, const void* data, size_t length, const void* data2 = NULL, size_t length2 = 0);

private:
    bool readHeader(BlynkHeader& hdr);
    uint16_t getNextMsgId();

protected:
    void begin(const char* authkey) {
        this->authkey = authkey;
    }
    void processInput(void);

    Transp& conn;

private:
    const char* authkey;
    unsigned long lastActivityIn;
    unsigned long lastActivityOut;
    unsigned long lastHeartbeat;
    uint16_t currentMsgId;
};

template <class Transp>
bool BlynkProtocol<Transp>::connect()
{
    if (!conn.connect()) {
        return false;
    }

    uint16_t id = getNextMsgId();
    sendCmd(BLYNK_CMD_LOGIN, id, authkey, strlen(authkey), NULL, 0);

#ifdef BLYNK_DEBUG
    const unsigned long t = millis();
#endif

    BlynkHeader hdr;
    if (!readHeader(hdr)) {
        hdr.length = BLYNK_TIMEOUT;
    }

    if (BLYNK_CMD_RESPONSE != hdr.type ||
        id != hdr.msg_id ||
        (BLYNK_SUCCESS != hdr.length && BLYNK_ALREADY_LOGGED_IN != hdr.length))
    {
        if (BLYNK_TIMEOUT == hdr.length) {
            BLYNK_LOG("Timeout");
        } else if (BLYNK_INVALID_TOKEN == hdr.length) {
            BLYNK_LOG("Invalid auth token");
        } else {
            BLYNK_LOG("Connect failed (code: %d)", hdr.length);
        }
        conn.disconnect();
        delay(5000);
        return false;
    }

    lastHeartbeat = lastActivityIn = lastActivityOut = millis();
    BLYNK_LOG("Blynk v" BLYNK_VERSION " connected");
#ifdef BLYNK_DEBUG
    BLYNK_LOG("Roundtrip: %dms", lastActivityIn-t);
#endif

    return true;
}

template <class Transp>
void BlynkProtocol<Transp>::run(void)
{
    if (!conn.connected()) {
        if (!connect()) {
            return;
        }
    }

    if (conn.available() >= sizeof(BlynkHeader)) {
        //BLYNK_LOG("Available: %d", conn.available());
        //const unsigned long t = micros();
        processInput();
        //BLYNK_LOG("Proc time: %d", micros() - t);
    }

    const unsigned long t = millis();

    if (t - lastActivityIn > (1000UL * BLYNK_HEARTBEAT + BLYNK_TIMEOUT_MS*3)) {
#ifdef BLYNK_DEBUG
        BLYNK_LOG("Heartbeat timeout (last in: %lu)", lastActivityIn);
#else
        BLYNK_LOG("Heartbeat timeout");
#endif
        conn.disconnect();
    } else if ((t - lastActivityIn > 1000UL * BLYNK_HEARTBEAT ||
               t - lastActivityOut > 1000UL * BLYNK_HEARTBEAT) &&
               t - lastHeartbeat     > BLYNK_TIMEOUT_MS)
    {
        // Send ping if we didn't both send and receive something for BLYNK_HEARTBEAT seconds
#ifdef BLYNK_DEBUG
        BLYNK_LOG("Heartbeat");
#endif

        sendCmd(BLYNK_CMD_PING, 0, NULL, 0, NULL, 0);
        lastActivityOut = lastHeartbeat = t;
    }
}

template <class Transp>
BLYNK_FORCE_INLINE
void BlynkProtocol<Transp>::processInput(void)
{
    BlynkHeader hdr;
    if (!readHeader(hdr))
        return;

    //BLYNK_LOG("Message %d,%d,%d", hdr.type, hdr.msg_id, hdr.length);

    if (hdr.type == BLYNK_CMD_RESPONSE) {
#ifdef BLYNK_DEBUG
        BLYNK_LOG("Got response: %d", hdr.length);
#endif
        // TODO: return code may indicate App presence
        lastActivityIn = millis();
        return;
    }

    if (hdr.length > BLYNK_MAX_READBYTES) {
        BLYNK_LOG("Packet size (%d) > max allowed (%d)", hdr.length, BLYNK_MAX_READBYTES);
        conn.disconnect();
        return;
    }

    uint8_t inputBuffer[hdr.length+1]; // Add 1 to zero-terminate
    if (hdr.length != conn.read(inputBuffer, hdr.length)) {
        BLYNK_LOG("Can't read body");
        return;
    }
    inputBuffer[hdr.length] = 0;

#ifdef BLYNK_DEBUG
    BLYNK_DBG_DUMP(">", inputBuffer, hdr.length);
#endif

    lastActivityIn = millis();

    switch (hdr.type)
    {
    case BLYNK_CMD_PING:
        hdr.type = BLYNK_CMD_RESPONSE;
        hdr.msg_id = htons(hdr.msg_id);
        hdr.length = htons(BLYNK_SUCCESS);
        conn.write(&hdr, sizeof(hdr));
        lastActivityOut = lastActivityIn;
        break;
    case BLYNK_CMD_HARDWARE: {
        currentMsgId = hdr.msg_id;
        this->processCmd(inputBuffer, hdr.length);
        currentMsgId = 0;
    } break;
    default:
        BLYNK_LOG("Invalid header type: %d", hdr.type);
        conn.disconnect();
        break;
    }

}

template <class Transp>
bool BlynkProtocol<Transp>::readHeader(BlynkHeader& hdr)
{
    if (sizeof(hdr) != conn.read(&hdr, sizeof(hdr))) {
        return false;
    }
    hdr.msg_id = ntohs(hdr.msg_id);
    hdr.length = ntohs(hdr.length);
    return true;
}

template <class Transp>
void BlynkProtocol<Transp>::sendCmd(uint8_t cmd, uint16_t id, const void* data, size_t length, const void* data2, size_t length2)
{
    if (!conn.connected()) {
#ifdef BLYNK_DEBUG
        BLYNK_LOG("Cmd not sent");
#endif
        return;
    }
    BlynkHeader hdr;
    hdr.type = cmd;
    hdr.msg_id = htons((id == 0) ? getNextMsgId() : id);
    hdr.length = htons(length+length2);
    size_t wlen = 0;
    wlen += conn.write(&hdr, sizeof(hdr));
    if (length) {
        wlen += conn.write(data, length);
    }
    if (length2) {
        wlen += conn.write(data2, length2);
    }
    lastActivityOut = millis();

    if (wlen != sizeof(hdr)+length+length2) {
        BLYNK_LOG("Can't send cmd");
        conn.disconnect();
    }

#ifdef BLYNK_DEBUG
    BLYNK_DBG_DUMP("<", data, length);
    BLYNK_DBG_DUMP("<", data2, length2);
#endif
}

template <class Transp>
uint16_t BlynkProtocol<Transp>::getNextMsgId()
{
    static uint16_t last = 0;
    if (currentMsgId != 0)
        return currentMsgId;
    if (++last == 0)
        last = 1;
    return last;
}

#endif
