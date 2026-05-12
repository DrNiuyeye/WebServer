#ifndef MESSAGE_H
#define MESSAGE_H

#include <cctype>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>

enum MSGSTATUS {
    HANDLE_INIT,
    HANDLE_HEAD,
    HANDLE_BODY,
    HADNLE_COMPLATE,
    HANDLE_ERROR
};

enum MSGBODYTYPE {
    FILE_TYPE,
    HTML_TYPE,
    EMPTY_TYPE
};

enum FILEMSGBODYSTATUS {
    FILE_BEGIN_FLAG,
    FILE_HEAD,
    FILE_CONTENT,
    FILE_COMPLATE
};

class Message {
public:
    Message() : status(HANDLE_INIT) {}

    MSGSTATUS status;
    std::unordered_map<std::string, std::string> msgHeader;
};

class Request : public Message {
public:
    Request()
        : Message(),
          contentLength(0),
          msgBodyRecvLen(0),
          fileMsgStatus(FILE_BEGIN_FLAG),
          recvFileStarted(false) {}

    void setRequestLine(const std::string& requestLine) {
        std::istringstream lineStream(requestLine);
        lineStream >> requestMethod;
        lineStream >> requestResourse;
        lineStream >> httpVersion;
    }

    void addHeaderOpt(const std::string& headLine) {
        std::string::size_type colon = headLine.find(':');
        if (colon == std::string::npos) {
            return;
        }

        std::string key = headLine.substr(0, colon);
        std::string value = headLine.substr(colon + 1);
        while (!value.empty() && (value[0] == ' ' || value[0] == '\t')) {
            value.erase(0, 1);
        }
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
            value.pop_back();
        }

        std::string lowerKey = key;
        for (char& ch : lowerKey) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }

        if (lowerKey == "content-length") {
            key = "Content-Length";
            contentLength = std::stoll(value);
        } else if (lowerKey == "content-type") {
            key = "Content-Type";
            std::string::size_type semIndex = value.find(';');
            if (semIndex != std::string::npos) {
                msgHeader[key] = value.substr(0, semIndex);

                std::string rest = value.substr(semIndex + 1);
                while (!rest.empty() && (rest[0] == ' ' || rest[0] == '\t')) {
                    rest.erase(0, 1);
                }

                std::string::size_type eqIndex = rest.find('=');
                if (eqIndex != std::string::npos) {
                    std::string paramValue = rest.substr(eqIndex + 1);
                    if (paramValue.size() >= 2 && paramValue.front() == '"' && paramValue.back() == '"') {
                        paramValue = paramValue.substr(1, paramValue.size() - 2);
                    }
                    msgHeader[rest.substr(0, eqIndex)] = paramValue;
                }
            } else {
                msgHeader[key] = value;
            }
        } else {
            if (lowerKey == "cookie") {
                key = "Cookie";
            }
            msgHeader[key] = value;
        }
    }

    std::string recvMsg;
    std::string requestMethod;
    std::string requestResourse;
    std::string httpVersion;

    long long contentLength;
    long long msgBodyRecvLen;

    std::string recvFileName;
    FILEMSGBODYSTATUS fileMsgStatus;
    std::string userName;
    std::string uploadDir;
    bool recvFileStarted;
};

class Response : public Message {
public:
    Response()
        : Message(),
          bodyType(EMPTY_TYPE),
          beforeBodyMsgLen(0),
          msgBodyLen(0),
          fileMsgFd(-1),
          curStatusHasSendLen(0),
          closeConnection(false) {}

    std::string responseHttpVersion = "HTTP/1.1";
    std::string responseStatusCode;
    std::string responseStatusDes;

    MSGBODYTYPE bodyType;
    std::string bodyFileName;
    std::string userName;
    std::string sessionToken;
    std::string redirectLocation;
    std::string setCookie;
    std::string flashMessage;

    std::string beforeBodyMsg;
    int beforeBodyMsgLen;

    std::string msgBody;
    unsigned long msgBodyLen;

    int fileMsgFd;
    unsigned long curStatusHasSendLen;
    bool closeConnection;
};

#endif
