#include <glog/logging.h>
#include <gflags/gflags.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string>
#include <sstream>
#include <iomanip>
#include <map>
#include <vector>
#include <functional>

typedef std::vector<unsigned char> Buffer;
typedef std::function< void(const Buffer& message, bool& stop)> MessageReceived;

DEFINE_double(lat, 37.39, "latitude deg");
DEFINE_double(lon, -122.15, "longitude deg");
DEFINE_string(station_id, "", "station id");
DEFINE_string(station_token, "", "station token");

// Thank you: RTKLIB
static unsigned int getBitsUnsigned(
        const unsigned char *buff, 
        int pos, 
        int len)
{
    unsigned int bits = 0;
    int i;
    for (i=pos; i<pos+len; i++) {
        bits = (bits<<1) + ((buff[i/8]>>(7-i%8))&1u);
    }
    return bits;
}

static bool sendBuffer(
        int sockfd, 
        const unsigned char* buffer, 
        size_t bufferSize)
{
    bool isOK = true;

    size_t offset = 0;
    do {
        auto bytes = write(sockfd, buffer+offset, bufferSize-offset);
        CHECK_GT(bytes, 0) << "failed to sendBuffer: " << bytes;
        if (bytes < 0) {
            isOK = false;
            break;
        }
        else if (bytes == 0) {
            break;
        }
        offset += (size_t)bytes;
    } while (offset < bufferSize);

    return isOK;
}

static std::vector<unsigned char> pointOneFrame(
        const unsigned char frameType[2],
        const unsigned char* framePayload,
        size_t framePayloadSize)
{
    std::vector<unsigned char> result;
    result.reserve(256);
    result.push_back(0xB5);
    result.push_back(0x62);

    result.push_back(frameType[0]);
    result.push_back(frameType[1]);

    // TODO:: this has to be uint16_t here ?
    result.push_back((unsigned char)framePayloadSize);
    result.push_back((unsigned char)0x00);
    result.insert(result.end(), framePayload, framePayload+framePayloadSize);

    unsigned char checkA = 0;
    unsigned char checkB = 0;

    for (size_t i = 2; i < result.size(); i++) {
        checkA = checkA + result[i];
        checkB = checkB + checkA;
    }

    checkA = checkA & 0xFF;
    checkB = checkB & 0xFF;

    result.push_back(checkA);
    result.push_back(checkB);

    return result;
}

bool connectToStation(
        const std::string& stationId,
        const std::string& stationToken,
        const double latDeg,
        const double lonDeg,
        MessageReceived messageReceivedBlock)
{
    const std::string HOSTNAME = "polaris.pointonenav.com";
    const uint16_t PORT = 8088;
    bool isOK = true;

    const unsigned char PONAuthenticationFrame[2] = {0xE0, 0x01};
    std::vector<unsigned char> authBuffer =
        pointOneFrame(PONAuthenticationFrame,
                (const unsigned char*)stationToken.c_str(),
                stationToken.size());

    int32_t v[3];
    v[0] = (int32_t)(latDeg * 10000000);
    v[1] = (int32_t)(lonDeg * 10000000);
    v[2] = (int32_t)(0.0 * 10000000);

    const unsigned char PONLocationLLAFrame[2] = {0xE0, 0x04};
    std::vector<unsigned char> positionBuffer =
        pointOneFrame(PONLocationLLAFrame, (unsigned char*)&v, sizeof(v));

    std::stringstream ss;
    ss << "GET /" << stationId << " HTTP/1.0\r\n";
    ss << "\r\n";
    ss << "User-Agent: NTRIP\r\n";
    ss << "Content-Type: text/event-stream\r\n";
    ss << "Connection: keep-alive\r\n";
    ss << "\r\n";


    int sockfd;
    struct sockaddr_in serv_addr{0};
    struct hostent *server = nullptr;

    if (isOK) {
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        isOK = (sockfd >= 0);
        LOG_IF(ERROR, !isOK) << "failed to open socket";
    }

    if (isOK) {
        int ret = 0;
        struct timeval timeout;
        timeout.tv_sec = 10;
        timeout.tv_usec = 0;

        ret = setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout));
        isOK = (ret == 0);
        LOG_IF(ERROR, !isOK) << "failed to SO_SNDTIMEO";
    }

    if (isOK) {
        server = gethostbyname(HOSTNAME.c_str());
        isOK = (server != NULL);
        if (!isOK) {
            LOG(ERROR) << "POINTONE: failed: gethostbyname";
        }
    }

    if (isOK) {
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(PORT);
        memcpy(&serv_addr.sin_addr.s_addr,server->h_addr,server->h_length);
    }

    if (isOK) {
        int ret = connect(sockfd,(struct sockaddr *)&serv_addr,sizeof(serv_addr));
        isOK = (ret == 0);
        CHECK(isOK);

        if (!isOK) {
            LOG(ERROR) << "POINTONE: failed: connect: " << ret;
        }
    }

    if (isOK) {
        const std::string getRequest = ss.str();
        isOK = sendBuffer(sockfd, (const unsigned char*)getRequest.c_str(), getRequest.size());
        if (!isOK) {
            LOG(ERROR) << "POINTONE: failed: sendBuffer: getRequest";
        }
    }

    if (isOK) {
        isOK = sendBuffer(sockfd, authBuffer.data(), authBuffer.size());
        if (!isOK) {
            LOG(ERROR) << "POINTONE: failed: sendBuffer: authBuffer";
        }
    }

    if (isOK) {
        isOK = sendBuffer(sockfd, positionBuffer.data(), positionBuffer.size());
        if (!isOK) {
            LOG(ERROR) << "POINTONE: failed: sendBuffer: positionBuffer";
        }
    }

    bool isStop = false;
    const auto BUFFER_SIZE = 2048;
    const auto MAX_PAYLOAD_SIZE = 200*1024;

    std::vector<unsigned char> messageBuffer(BUFFER_SIZE, 0);
    messageBuffer.reserve(BUFFER_SIZE);

    while (isOK && !isStop) {
        uint8_t headerD3 = 0;
        uint16_t numBytesToRead = 0;
        size_t offset = 0;

        messageBuffer.resize(BUFFER_SIZE, 0);

        auto bytes = read(sockfd, (void*)(messageBuffer.data()+offset), 1);
        headerD3 = getBitsUnsigned(messageBuffer.data()+offset, 0, 8);
        isOK = (bytes == 1);
        if (!isOK) {
            LOG(ERROR) << "POINTONE: failed to read bytes";
            break;
        }
        isOK = (headerD3 == 0xd3);
        if (!isOK) {
            LOG(ERROR) << "POINTONE: bad headerD3" << std::hex << (int)headerD3;
            break;
        }

        offset++;

        bytes = read(sockfd, (void*)(messageBuffer.data()+offset), 2);
        numBytesToRead = ntohs(getBitsUnsigned(messageBuffer.data()+offset, 8, 16));
        offset += 2;

        isOK = (bytes == 2);
        if (!isOK) {
            LOG(ERROR) << "POINTONE: failed to read bytes";
            break;
        }
        isOK = (numBytesToRead > 0 && (int)numBytesToRead < MAX_PAYLOAD_SIZE);
        if (!isOK) {
            LOG(ERROR) << "POINTONE: invalid numBytesToRead: " << numBytesToRead;
            break;
        }

        messageBuffer.resize(3 + numBytesToRead + 3);

        bytes = read(sockfd, (void*)(messageBuffer.data()+offset), messageBuffer.size()-offset);
        offset += messageBuffer.size();

        isOK = ((size_t)bytes < messageBuffer.size());
        if (!isOK) {
            LOG(ERROR) << "POINTONE: invalid messageBuffer.size(): " << messageBuffer.size();
            break;
        }

        if (messageReceivedBlock) {
            messageReceivedBlock(messageBuffer, isStop);
        }
    }

    if (sockfd >= 0) {
        close(sockfd);
    }

    return isOK;
}

int main(int argc, char** argv) {
    google::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr=true;

    LOG(INFO) << "hello";

    const std::string stationId = FLAGS_station_id;
    const std::string stationToken = FLAGS_station_token;
    const double stationLatDeg = FLAGS_lat;
    const double stationLonDeg = FLAGS_lon;
    auto messagesCount = 0;

    if (stationToken == "") {
        LOG(FATAL) << "must specify: stationToken";
    }

    if (stationId == "") {
        LOG(FATAL) << "must specify: stationId";
    }

    auto onMessageReceived = [&](const Buffer& message, bool& isStop) {
        const auto headerBits = 24;  // jump over 0xd3 <size> <size>
        const auto messageId = getBitsUnsigned(message.data(), headerBits+0, 12);

        LOG(INFO) << " => MSG: " << std::fixed << " COUNT: " << std::dec << messagesCount
            << " HEADER: " << std::hex << (int)message[0]
            << " MESSAGE_ID: " << std::dec << messageId;

        if (messageId >= 1001 && messageId <= 1004) {
            LOG(INFO)   << "        STATION: " << getBitsUnsigned(message.data(), headerBits+12, 12)
                << " TOW: " << std::setw(10) << getBitsUnsigned(message.data(), headerBits+24, 30);
        }
        else if (messageId >= 1009 && messageId <= 1012) {
            LOG(INFO) << "        STATION: " << getBitsUnsigned(message.data(), headerBits+12, 12)
                << " TOW: " << std::setw(10) << getBitsUnsigned(message.data(), headerBits+24, 27);
        }
        else {
            LOG(INFO) << "        STATION: " << getBitsUnsigned(message.data(), headerBits+12, 12);
        }
    };

    LOG(INFO) << " => POINTONE: CONNECT: STATION:" << stationId
        << " LOCATION: "
        << std::fixed << std::setprecision(2) << stationLatDeg
        << ","
        << std::fixed << std::setprecision(2) << stationLonDeg;

    auto isOK = connectToStation(
            stationId,
            stationToken,
            stationLatDeg,
            stationLonDeg,
            onMessageReceived);
    LOG_IF(ERROR, !isOK) << " => POINTONE: DISCONNECT: STATION:" << stationId;

    return (isOK ? 0 : 1);
}
