/**
 * @file myevent.cpp
 * @brief Event handlers for accept/read/write in the web server.
 */
#include "myevent.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <iomanip>
#include <random>
#include <sstream>
#include <unordered_map>

// Shared per-connection state maps.
std::unordered_map<int, Request> EventBase::requestStatus;
std::unordered_map<int, Response> EventBase::responseStatus;
std::mutex EventBase::requestMutex;
std::mutex EventBase::responseMutex;

namespace {

const std::string kFileRoot = "filedir";
const std::string kDataDir = "data";
const std::string kUserDbPath = "data/users.db";
const std::string kSessionCookieName = "WFS_SESSION";

std::mutex g_authMutex;
std::unordered_map<std::string, std::string> g_sessions;

struct UserRecord {
    std::string salt;
    std::string digest;
};

enum class UploadParseResult {
    NeedMore,
    Complete,
    Failed
};

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

std::string stripQuery(const std::string& path) {
    std::string::size_type pos = path.find('?');
    if (pos == std::string::npos) {
        return path;
    }
    return path.substr(0, pos);
}

std::string trim(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }

    size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }

    return value.substr(begin, end - begin);
}

void replaceAll(std::string& value, const std::string& from, const std::string& to) {
    if (from.empty()) {
        return;
    }

    size_t pos = 0;
    while ((pos = value.find(from, pos)) != std::string::npos) {
        value.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string urlDecode(const std::string& encoded) {
    std::string decoded;
    for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.size()) {
            int value = 0;
            std::istringstream iss(encoded.substr(i + 1, 2));
            if (iss >> std::hex >> value) {
                decoded += static_cast<char>(value);
                i += 2;
            } else {
                decoded += encoded[i];
            }
        } else if (encoded[i] == '+') {
            decoded += ' ';
        } else {
            decoded += encoded[i];
        }
    }
    return decoded;
}

std::string urlEncode(const std::string& raw) {
    std::ostringstream oss;
    oss << std::uppercase << std::hex;
    for (unsigned char ch : raw) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            oss << static_cast<char>(ch);
        } else {
            oss << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
        }
    }
    return oss.str();
}

std::string htmlEscape(const std::string& raw) {
    std::string escaped;
    escaped.reserve(raw.size());

    for (char ch : raw) {
        switch (ch) {
        case '&':
            escaped += "&amp;";
            break;
        case '<':
            escaped += "&lt;";
            break;
        case '>':
            escaped += "&gt;";
            break;
        case '"':
            escaped += "&quot;";
            break;
        case '\'':
            escaped += "&#39;";
            break;
        default:
            escaped += ch;
            break;
        }
    }

    return escaped;
}

std::string headerFileName(const std::string& raw) {
    std::string value;
    value.reserve(raw.size());
    for (unsigned char ch : raw) {
        if (ch == '"' || ch == '\\' || ch == '\r' || ch == '\n' || ch < 32) {
            value += '_';
        } else {
            value += static_cast<char>(ch);
        }
    }
    return value;
}

std::unordered_map<std::string, std::string> parseUrlEncodedForm(const std::string& body) {
    std::unordered_map<std::string, std::string> result;
    size_t begin = 0;

    while (begin <= body.size()) {
        size_t amp = body.find('&', begin);
        std::string pair = body.substr(begin, amp == std::string::npos ? std::string::npos : amp - begin);
        if (!pair.empty()) {
            size_t eq = pair.find('=');
            if (eq == std::string::npos) {
                result[urlDecode(pair)] = "";
            } else {
                result[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
            }
        }

        if (amp == std::string::npos) {
            break;
        }
        begin = amp + 1;
    }

    return result;
}

bool ensureDirectory(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    if (errno != ENOENT) {
        return false;
    }

    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

bool isValidUsername(const std::string& userName) {
    if (userName.size() < 3 || userName.size() > 32) {
        return false;
    }

    for (unsigned char ch : userName) {
        if (!std::isalnum(ch) && ch != '_' && ch != '-') {
            return false;
        }
    }

    return true;
}

bool isSafeFileName(const std::string& fileName) {
    if (fileName.empty() || fileName.size() > 255 ||
        fileName == "." || fileName == "..") {
        return false;
    }

    for (unsigned char ch : fileName) {
        if (ch == '/' || ch == '\\' || ch < 32) {
            return false;
        }
    }

    return true;
}

std::string baseName(const std::string& raw) {
    size_t pos = raw.find_last_of("/\\");
    if (pos == std::string::npos) {
        return raw;
    }
    return raw.substr(pos + 1);
}

std::string cleanUploadFileName(const std::string& raw) {
    std::string fileName = baseName(raw);
    if (!isSafeFileName(fileName)) {
        return "";
    }
    return fileName;
}

std::string cleanUrlFileName(const std::string& raw) {
    if (!isSafeFileName(raw)) {
        return "";
    }
    return raw;
}

std::string userDir(const std::string& userName) {
    return kFileRoot + "/" + userName;
}

bool ensureUserStorage(const std::string& userName) {
    return isValidUsername(userName) &&
           ensureDirectory(kFileRoot) &&
           ensureDirectory(userDir(userName));
}

std::string userFilePath(const std::string& userName, const std::string& fileName) {
    if (!ensureUserStorage(userName) || !isSafeFileName(fileName)) {
        return "";
    }
    return userDir(userName) + "/" + fileName;
}

uint64_t fnv1a64(const std::string& value, uint64_t seed) {
    uint64_t hash = seed;
    for (unsigned char ch : value) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string passwordDigest(const std::string& userName,
                           const std::string& password,
                           const std::string& salt) {
    uint64_t h1 = 1469598103934665603ULL;
    uint64_t h2 = 1099511628211ULL;
    const std::string material = salt + ":" + userName + ":" + password;

    for (int i = 0; i < 4096; ++i) {
        h1 = fnv1a64(material + ":" + std::to_string(h2), h1);
        h2 = fnv1a64(std::to_string(h1) + ":" + material, h2);
    }

    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << h1
        << std::setw(16) << h2;
    return oss.str();
}

std::string randomHex(size_t bytes) {
    static thread_local std::mt19937_64 rng([]() {
        std::random_device rd;
        return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
               static_cast<uint64_t>(getpid()) ^
               static_cast<uint64_t>(rd());
    }());

    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');

    size_t produced = 0;
    while (produced < bytes) {
        uint64_t value = dist(rng);
        for (int i = 0; i < 8 && produced < bytes; ++i, ++produced) {
            unsigned int byte = static_cast<unsigned int>((value >> (i * 8)) & 0xffU);
            oss << std::setw(2) << byte;
        }
    }

    return oss.str();
}

bool readUserLocked(const std::string& userName, UserRecord& record) {
    std::ifstream in(kUserDbPath);
    if (!in) {
        return false;
    }

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }

        size_t first = line.find(':');
        size_t second = first == std::string::npos ? std::string::npos : line.find(':', first + 1);
        if (first == std::string::npos || second == std::string::npos) {
            continue;
        }

        if (line.substr(0, first) == userName) {
            record.salt = line.substr(first + 1, second - first - 1);
            record.digest = line.substr(second + 1);
            return true;
        }
    }

    return false;
}

bool registerUser(const std::string& userName,
                  const std::string& password,
                  std::string& errorMessage) {
    if (!isValidUsername(userName)) {
        errorMessage = "Username must be 3-32 characters: letters, numbers, underscore, or hyphen.";
        return false;
    }

    if (password.size() < 4 || password.size() > 128) {
        errorMessage = "Password must be 4-128 characters.";
        return false;
    }

    std::lock_guard<std::mutex> lock(g_authMutex);
    ensureDirectory(kDataDir);

    UserRecord existing;
    if (readUserLocked(userName, existing)) {
        errorMessage = "Username already exists.";
        return false;
    }

    const std::string salt = randomHex(16);
    const std::string digest = passwordDigest(userName, password, salt);

    if (!ensureUserStorage(userName)) {
        errorMessage = "Unable to create the user storage directory.";
        return false;
    }

    std::ofstream out(kUserDbPath, std::ios::out | std::ios::app);
    if (!out) {
        errorMessage = "Unable to open the user database.";
        return false;
    }

    out << userName << ":" << salt << ":" << digest << "\n";
    if (!out) {
        errorMessage = "Unable to write the user database.";
        return false;
    }

    return true;
}

bool validateUser(const std::string& userName,
                  const std::string& password,
                  std::string& errorMessage) {
    std::lock_guard<std::mutex> lock(g_authMutex);

    UserRecord record;
    if (!readUserLocked(userName, record)) {
        errorMessage = "Invalid username or password.";
        return false;
    }

    if (passwordDigest(userName, password, record.salt) != record.digest) {
        errorMessage = "Invalid username or password.";
        return false;
    }

    if (!ensureUserStorage(userName)) {
        errorMessage = "Unable to create the user storage directory.";
        return false;
    }

    return true;
}

std::string createSession(const std::string& userName) {
    std::lock_guard<std::mutex> lock(g_authMutex);

    for (int i = 0; i < 16; ++i) {
        std::string token = randomHex(32);
        if (g_sessions.find(token) == g_sessions.end()) {
            g_sessions[token] = userName;
            return token;
        }
    }

    return "";
}

void destroySession(const std::string& token) {
    if (token.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_authMutex);
    g_sessions.erase(token);
}

std::string cookieValue(const std::string& cookieHeader, const std::string& key) {
    size_t begin = 0;
    while (begin < cookieHeader.size()) {
        size_t semi = cookieHeader.find(';', begin);
        std::string part = trim(cookieHeader.substr(
            begin,
            semi == std::string::npos ? std::string::npos : semi - begin));
        size_t eq = part.find('=');
        if (eq != std::string::npos && part.substr(0, eq) == key) {
            return part.substr(eq + 1);
        }

        if (semi == std::string::npos) {
            break;
        }
        begin = semi + 1;
    }

    return "";
}

bool requestUser(const Request& request, std::string& userName, std::string& token) {
    auto it = request.msgHeader.find("Cookie");
    if (it == request.msgHeader.end()) {
        return false;
    }

    token = cookieValue(it->second, kSessionCookieName);
    if (token.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_authMutex);
    auto sessionIt = g_sessions.find(token);
    if (sessionIt == g_sessions.end()) {
        return false;
    }

    userName = sessionIt->second;
    return true;
}

std::string sessionCookie(const std::string& token) {
    return kSessionCookieName + "=" + token + "; Path=/; HttpOnly; SameSite=Lax";
}

std::string expiredSessionCookie() {
    return kSessionCookieName + "=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax";
}

void setRedirectResponse(Response& response,
                         const std::string& location,
                         const std::string& cookie = "",
                         bool closeConnection = false) {
    response = Response();
    response.bodyFileName = "/redirect";
    response.redirectLocation = location;
    response.setCookie = cookie;
    response.closeConnection = closeConnection;
}

void setPageResponse(Response& response,
                     const std::string& page,
                     const std::string& message = "") {
    response = Response();
    response.bodyFileName = page;
    response.flashMessage = message;
}

std::string buildAuthPage(bool isRegister, const std::string& message) {
    const std::string title = isRegister ? "Create Account" : "Sign In";
    const std::string action = isRegister ? "/register" : "/login";
    const std::string alternateHref = isRegister ? "/login" : "/register";
    const std::string alternateText = isRegister ? "Already have an account? Sign in"
                                                 : "New here? Create an account";

    std::string html;
    html += "<!DOCTYPE html>\n";
    html += "<html lang=\"en\">\n";
    html += "<head>\n";
    html += "    <meta charset=\"utf-8\">\n";
    html += "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
    html += "    <title>" + title + " - WebFileServer</title>\n";
    html += "    <style>\n";
    html += "        :root{--bg:#101418;--panel:#f7f9fb;--text:#17202a;--muted:#667085;--line:#d9e0e8;--accent:#1f7a5b;--danger:#b42318;}\n";
    html += "        *{box-sizing:border-box} body{margin:0;min-height:100vh;display:grid;place-items:center;background:#101418;color:var(--text);font-family:Segoe UI,Arial,sans-serif;padding:24px;}\n";
    html += "        main{width:min(420px,100%);background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:28px;box-shadow:0 16px 45px rgba(0,0,0,.22);} h1{font-size:24px;margin:0 0 6px;} p{color:var(--muted);margin:0 0 22px;}\n";
    html += "        label{display:block;font-size:13px;font-weight:600;margin:14px 0 6px;} input{width:100%;height:42px;border:1px solid var(--line);border-radius:6px;padding:0 12px;font-size:15px;background:white;} button{width:100%;height:42px;border:0;border-radius:6px;background:var(--accent);color:white;font-weight:700;margin-top:20px;cursor:pointer;} a{display:block;margin-top:16px;color:var(--accent);text-decoration:none;text-align:center;} .msg{border:1px solid #fecdca;background:#fffbfa;color:var(--danger);border-radius:6px;padding:10px 12px;font-size:13px;margin-bottom:14px;}\n";
    html += "    </style>\n";
    html += "</head>\n";
    html += "<body>\n";
    html += "    <main>\n";
    html += "        <h1>" + title + "</h1>\n";
    html += "        <p>Use your private file space on this server.</p>\n";
    if (!message.empty()) {
        html += "        <div class=\"msg\">" + htmlEscape(message) + "</div>\n";
    }
    html += "        <form action=\"" + action + "\" method=\"post\">\n";
    html += "            <label for=\"username\">Username</label>\n";
    html += "            <input id=\"username\" name=\"username\" autocomplete=\"username\" required>\n";
    html += "            <label for=\"password\">Password</label>\n";
    html += "            <input id=\"password\" name=\"password\" type=\"password\" autocomplete=\"current-password\" required>\n";
    html += "            <button type=\"submit\">" + title + "</button>\n";
    html += "        </form>\n";
    html += "        <a href=\"" + alternateHref + "\">" + alternateText + "</a>\n";
    html += "    </main>\n";
    html += "</body>\n";
    html += "</html>\n";
    return html;
}

UploadParseResult parseMultipartUpload(Request& request) {
    auto contentType = request.msgHeader.find("Content-Type");
    auto boundaryIt = request.msgHeader.find("boundary");
    if (contentType == request.msgHeader.end() ||
        boundaryIt == request.msgHeader.end() ||
        contentType->second != "multipart/form-data" ||
        boundaryIt->second.empty()) {
        return UploadParseResult::Failed;
    }

    const std::string& boundary = boundaryIt->second;

    if (request.fileMsgStatus == FILE_BEGIN_FLAG) {
        size_t endIndex = request.recvMsg.find("\r\n");
        if (endIndex == std::string::npos) {
            return UploadParseResult::NeedMore;
        }

        std::string flag = request.recvMsg.substr(0, endIndex);
        if (flag != "--" + boundary) {
            return UploadParseResult::Failed;
        }

        request.recvMsg.erase(0, endIndex + 2);
        request.fileMsgStatus = FILE_HEAD;
    }

    if (request.fileMsgStatus == FILE_HEAD) {
        while (true) {
            size_t endIndex = request.recvMsg.find("\r\n");
            if (endIndex == std::string::npos) {
                return UploadParseResult::NeedMore;
            }

            std::string line = request.recvMsg.substr(0, endIndex + 2);
            request.recvMsg.erase(0, endIndex + 2);

            if (line == "\r\n") {
                request.fileMsgStatus = FILE_CONTENT;
                break;
            }

            size_t filenamePos = line.find("filename=\"");
            if (filenamePos != std::string::npos) {
                filenamePos += std::string("filename=\"").size();
                size_t filenameEnd = line.find('"', filenamePos);
                if (filenameEnd == std::string::npos) {
                    return UploadParseResult::Failed;
                }

                request.recvFileName = cleanUploadFileName(line.substr(filenamePos, filenameEnd - filenamePos));
                if (request.recvFileName.empty()) {
                    return UploadParseResult::Failed;
                }
            }
        }
    }

    if (request.fileMsgStatus == FILE_CONTENT) {
        if (request.recvFileName.empty() || request.uploadDir.empty()) {
            return UploadParseResult::Failed;
        }

        std::string targetPath = request.uploadDir + "/" + request.recvFileName;
        std::ios_base::openmode mode = std::ios::out | std::ios::binary;
        mode |= request.recvFileStarted ? std::ios::app : std::ios::trunc;

        std::ofstream ofs(targetPath, mode);
        if (!ofs) {
            return UploadParseResult::Failed;
        }
        request.recvFileStarted = true;

        const std::string finalBoundary = "\r\n--" + boundary + "--";
        const size_t keepTail = finalBoundary.size() + 2;

        while (!request.recvMsg.empty()) {
            size_t boundaryPos = request.recvMsg.find(finalBoundary);
            if (boundaryPos != std::string::npos) {
                if (boundaryPos > 0) {
                    ofs.write(request.recvMsg.c_str(), static_cast<std::streamsize>(boundaryPos));
                }

                size_t eraseLen = boundaryPos + finalBoundary.size();
                if (request.recvMsg.size() >= eraseLen + 2 &&
                    request.recvMsg.substr(eraseLen, 2) == "\r\n") {
                    eraseLen += 2;
                }
                request.recvMsg.erase(0, eraseLen);
                request.fileMsgStatus = FILE_COMPLATE;
                return UploadParseResult::Complete;
            }

            if (request.recvMsg.size() <= keepTail) {
                break;
            }

            size_t saveLen = request.recvMsg.size() - keepTail;
            ofs.write(request.recvMsg.c_str(), static_cast<std::streamsize>(saveLen));
            request.recvMsg.erase(0, saveLen);
        }
    }

    return request.fileMsgStatus == FILE_COMPLATE ? UploadParseResult::Complete
                                                  : UploadParseResult::NeedMore;
}

void handleAuthPost(const std::string& path, Request& request, Response& response) {
    std::string body = request.recvMsg.substr(0, static_cast<size_t>(request.contentLength));
    std::unordered_map<std::string, std::string> form = parseUrlEncodedForm(body);

    std::string userName = form["username"];
    std::string password = form["password"];
    std::string errorMessage;
    bool ok = false;

    if (path == "/register") {
        ok = registerUser(userName, password, errorMessage);
    } else {
        ok = validateUser(userName, password, errorMessage);
    }

    if (!ok) {
        setPageResponse(response, path, errorMessage);
        return;
    }

    std::string token = createSession(userName);
    if (token.empty()) {
        setPageResponse(response, path, "Unable to create a login session.");
        return;
    }

    setRedirectResponse(response, "/", sessionCookie(token));
}

void prepareHtmlResponse(HandleSend* sender, Response& response, const std::string& body) {
    response.beforeBodyMsg = sender->getStatusLine("HTTP/1.1", "200", "OK");
    response.msgBody = body;
    response.msgBodyLen = response.msgBody.size();
    response.beforeBodyMsg += sender->getMessageHeader(
        std::to_string(response.msgBodyLen),
        "html",
        "",
        "",
        response.setCookie,
        !response.closeConnection);
    response.beforeBodyMsg += "\r\n";
    response.beforeBodyMsgLen = response.beforeBodyMsg.size();
    response.bodyType = HTML_TYPE;
    response.status = HANDLE_HEAD;
    response.curStatusHasSendLen = 0;
}

void prepareRedirect(HandleSend* sender, Response& response, const std::string& fallback) {
    std::string location = response.redirectLocation.empty() ? fallback : response.redirectLocation;
    response.beforeBodyMsg = sender->getStatusLine("HTTP/1.1", "302", "Moved Temporarily");
    response.beforeBodyMsg += sender->getMessageHeader(
        "0",
        "html",
        location,
        "",
        response.setCookie,
        !response.closeConnection);
    response.beforeBodyMsg += "\r\n";
    response.beforeBodyMsgLen = response.beforeBodyMsg.size();
    response.bodyType = EMPTY_TYPE;
    response.status = HANDLE_HEAD;
    response.curStatusHasSendLen = 0;
}

} // namespace

void AcceptConn::process() {
    while (true) {
        clientAddrLen = sizeof(clientAddr);
        int accFd = accept(m_listenFd, (sockaddr*)&clientAddr, &clientAddrLen);
        if (accFd == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            logStream("error") << "accept failed (errno=" << errno << ")" << std::endl;
            break;
        }

        setNonBlocking(accFd);
        addWaitFd(m_epollFd, accFd, true, true);
        statIncAcceptOk(1);
        logStream("info") << "accepted connection fd=" << accFd << std::endl;
    }
}

void HandleRecv::process() {
    std::lock_guard<std::mutex> reqLock(requestMutex);
    Request& request = requestStatus[m_clientFd];
    logStream("info") << "start HandleRecv, fd=" << m_clientFd << std::endl;

    char buf[2048];
    int recvLen = 0;

    while (true) {
        recvLen = recv(m_clientFd, buf, sizeof(buf), 0);

        if (recvLen == 0) {
            logStream("info") << "peer closed connection, fd=" << m_clientFd << std::endl;
            request.status = HANDLE_ERROR;
            break;
        }

        if (recvLen == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                request.status = HANDLE_ERROR;
                logStream("error") << "recv failed (errno=" << errno << "), fd=" << m_clientFd << std::endl;
                break;
            }

            modifyWaitFd(m_epollFd, m_clientFd, true, true, false);
            break;
        }

        request.recvMsg.append(buf, recvLen);

        if (request.status == HANDLE_INIT) {
            std::string::size_type endIndex = request.recvMsg.find("\r\n");
            if (endIndex != std::string::npos) {
                request.setRequestLine(request.recvMsg.substr(0, endIndex + 2));
                request.recvMsg.erase(0, endIndex + 2);
                request.status = HANDLE_HEAD;
                logStream("info") << "request line parsed, fd=" << m_clientFd << std::endl;
            }
        }

        if (request.status == HANDLE_HEAD) {
            while (true) {
                std::string::size_type endIndex = request.recvMsg.find("\r\n");
                if (endIndex == std::string::npos) {
                    break;
                }

                std::string curLine = request.recvMsg.substr(0, endIndex + 2);
                request.recvMsg.erase(0, endIndex + 2);

                if (curLine == "\r\n") {
                    request.status = HANDLE_BODY;
                    if (request.msgHeader["Content-Type"] == "multipart/form-data") {
                        request.fileMsgStatus = FILE_BEGIN_FLAG;
                    }
                    logStream("info") << "headers parsed, fd=" << m_clientFd << std::endl;
                    break;
                }

                request.addHeaderOpt(curLine);
            }
        }

        if (request.status != HANDLE_BODY) {
            continue;
        }

        std::string path = stripQuery(request.requestResourse);

        if (request.requestMethod == "GET") {
            std::string userName;
            std::string sessionToken;
            requestUser(request, userName, sessionToken);

            {
                std::lock_guard<std::mutex> respLock(responseMutex);
                Response& response = responseStatus[m_clientFd];
                response = Response();
                response.bodyFileName = path;
                response.userName = userName;
                response.sessionToken = sessionToken;
            }

            modifyWaitFd(m_epollFd, m_clientFd, true, true, true);
            request.status = HADNLE_COMPLATE;
            logStream("info") << "GET request parsed, schedule write event, fd=" << m_clientFd << std::endl;
            break;
        }

        if (request.requestMethod != "POST") {
            std::lock_guard<std::mutex> respLock(responseMutex);
            setRedirectResponse(responseStatus[m_clientFd], "/login", "", true);
            modifyWaitFd(m_epollFd, m_clientFd, true, true, true);
            request.status = HADNLE_COMPLATE;
            break;
        }

        if (path == "/login" || path == "/register") {
            if (static_cast<long long>(request.recvMsg.size()) < request.contentLength) {
                continue;
            }

            {
                std::lock_guard<std::mutex> respLock(responseMutex);
                handleAuthPost(path, request, responseStatus[m_clientFd]);
            }

            modifyWaitFd(m_epollFd, m_clientFd, true, true, true);
            request.status = HADNLE_COMPLATE;
            logStream("info") << "auth POST handled, schedule write event, fd=" << m_clientFd << std::endl;
            break;
        }

        if (path != "/upload") {
            std::lock_guard<std::mutex> respLock(responseMutex);
            setRedirectResponse(responseStatus[m_clientFd], "/", "", true);
            modifyWaitFd(m_epollFd, m_clientFd, true, true, true);
            request.status = HADNLE_COMPLATE;
            break;
        }

        std::string userName;
        std::string sessionToken;
        if (!requestUser(request, userName, sessionToken)) {
            std::lock_guard<std::mutex> respLock(responseMutex);
            setRedirectResponse(responseStatus[m_clientFd], "/login", "", true);
            modifyWaitFd(m_epollFd, m_clientFd, true, true, true);
            request.status = HADNLE_COMPLATE;
            break;
        }

        request.userName = userName;
        request.uploadDir = userDir(userName);
        if (!ensureUserStorage(userName)) {
            std::lock_guard<std::mutex> respLock(responseMutex);
            setRedirectResponse(responseStatus[m_clientFd], "/", "", true);
            modifyWaitFd(m_epollFd, m_clientFd, true, true, true);
            request.status = HADNLE_COMPLATE;
            break;
        }

        UploadParseResult uploadResult = parseMultipartUpload(request);
        if (uploadResult == UploadParseResult::Complete) {
            std::lock_guard<std::mutex> respLock(responseMutex);
            setRedirectResponse(responseStatus[m_clientFd], "/");
            modifyWaitFd(m_epollFd, m_clientFd, true, true, true);
            request.status = HADNLE_COMPLATE;
            logStream("info") << "upload handled, schedule write event, fd=" << m_clientFd << std::endl;
            break;
        }

        if (uploadResult == UploadParseResult::Failed) {
            std::lock_guard<std::mutex> respLock(responseMutex);
            setRedirectResponse(responseStatus[m_clientFd], "/", "", true);
            modifyWaitFd(m_epollFd, m_clientFd, true, true, true);
            request.status = HADNLE_COMPLATE;
            logStream("error") << "upload parse failed, fd=" << m_clientFd << std::endl;
            break;
        }
    }

    if (request.status == HADNLE_COMPLATE) {
        logStream("info") << "HandleRecv complete, fd=" << m_clientFd << std::endl;
        requestStatus.erase(m_clientFd);
    } else if (request.status == HANDLE_ERROR) {
        logStream("error") << "HandleRecv error, close fd=" << m_clientFd << std::endl;
        deleteWaitFd(m_epollFd, m_clientFd);
        shutdown(m_clientFd, SHUT_RDWR);
        close(m_clientFd);
        requestStatus.erase(m_clientFd);
    }
}

void HandleSend::process() {
    std::lock_guard<std::mutex> respLock(responseMutex);
    logStream("info") << "start HandleSend, fd=" << m_clientFd << std::endl;

    auto responseIt = responseStatus.find(m_clientFd);
    if (responseIt == responseStatus.end()) {
        logStream("info") << "response state not found, fd=" << m_clientFd << std::endl;
        modifyWaitFd(m_epollFd, m_clientFd, true, true, false);
        return;
    }

    Response& response = responseIt->second;

    if (response.status == HANDLE_INIT) {
        std::string path = stripQuery(response.bodyFileName);

        if (path == "/redirect") {
            prepareRedirect(this, response, response.userName.empty() ? "/login" : "/");
        } else if (path == "/logout") {
            destroySession(response.sessionToken);
            setRedirectResponse(response, "/login", expiredSessionCookie());
            prepareRedirect(this, response, "/login");
        } else if (path == "/login" || path == "/register") {
            if (!response.userName.empty()) {
                setRedirectResponse(response, "/");
                prepareRedirect(this, response, "/");
            } else {
                prepareHtmlResponse(this, response, buildAuthPage(path == "/register", response.flashMessage));
            }
        } else if (path == "/") {
            if (response.userName.empty()) {
                setRedirectResponse(response, "/login");
                prepareRedirect(this, response, "/login");
            } else {
                getFileListPage(response.msgBody, response.userName);
                prepareHtmlResponse(this, response, response.msgBody);
            }
        } else if (startsWith(path, "/download/")) {
            if (response.userName.empty()) {
                setRedirectResponse(response, "/login");
                prepareRedirect(this, response, "/login");
            } else {
                std::string fileName = cleanUrlFileName(urlDecode(path.substr(std::string("/download/").size())));
                std::string pathOnDisk = userFilePath(response.userName, fileName);
                response.fileMsgFd = pathOnDisk.empty() ? -1 : open(pathOnDisk.c_str(), O_RDONLY);

                if (response.fileMsgFd == -1) {
                    logStream("error") << "open download file failed, file=" << fileName << ", fd=" << m_clientFd << std::endl;
                    setRedirectResponse(response, "/");
                    prepareRedirect(this, response, "/");
                } else {
                    struct stat fileStat;
                    fstat(response.fileMsgFd, &fileStat);

                    response.msgBodyLen = static_cast<unsigned long>(fileStat.st_size);
                    response.beforeBodyMsg = getStatusLine("HTTP/1.1", "200", "OK");
                    response.beforeBodyMsg += getMessageHeader(
                        std::to_string(response.msgBodyLen),
                        "file",
                        "",
                        "",
                        response.setCookie,
                        !response.closeConnection);
                    response.beforeBodyMsg += "Content-Disposition: attachment; filename=\"" +
                                              headerFileName(fileName) + "\"\r\n";
                    response.beforeBodyMsg += "\r\n";
                    response.beforeBodyMsgLen = response.beforeBodyMsg.size();
                    response.bodyType = FILE_TYPE;
                    response.status = HANDLE_HEAD;
                    response.curStatusHasSendLen = 0;
                    logStream("info") << "prepared download response, file=" << fileName << ", fd=" << m_clientFd << std::endl;
                }
            }
        } else if (startsWith(path, "/delete/")) {
            if (response.userName.empty()) {
                setRedirectResponse(response, "/login");
                prepareRedirect(this, response, "/login");
            } else {
                std::string fileName = cleanUrlFileName(urlDecode(path.substr(std::string("/delete/").size())));
                std::string pathOnDisk = userFilePath(response.userName, fileName);
                if (!pathOnDisk.empty()) {
                    int ret = remove(pathOnDisk.c_str());
                    if (ret != 0) {
                        logStream("error") << "delete file failed, file=" << fileName << ", fd=" << m_clientFd << std::endl;
                    } else {
                        logStream("info") << "delete file success, file=" << fileName << ", fd=" << m_clientFd << std::endl;
                    }
                }

                setRedirectResponse(response, "/");
                prepareRedirect(this, response, "/");
            }
        } else {
            setRedirectResponse(response, response.userName.empty() ? "/login" : "/");
            prepareRedirect(this, response, response.userName.empty() ? "/login" : "/");
        }
    }

    while (true) {
        long long sentLen = 0;

        if (response.status == HANDLE_HEAD) {
            sentLen = response.curStatusHasSendLen;
            sentLen = send(
                m_clientFd,
                response.beforeBodyMsg.c_str() + sentLen,
                response.beforeBodyMsgLen - sentLen,
                0);
            if (sentLen == -1) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    response.status = HANDLE_ERROR;
                    logStream("error") << "send headers failed (errno=" << errno << "), fd=" << m_clientFd << std::endl;
                    break;
                }
                break;
            }
            response.curStatusHasSendLen += sentLen;

            if (response.curStatusHasSendLen >= response.beforeBodyMsgLen) {
                response.status = HANDLE_BODY;
                response.curStatusHasSendLen = 0;
                logStream("info") << "headers sent, continue body send, fd=" << m_clientFd << std::endl;
            }
        }

        if (response.status == HANDLE_BODY) {
            if (response.bodyType == HTML_TYPE) {
                sentLen = response.curStatusHasSendLen;
                sentLen = send(
                    m_clientFd,
                    response.msgBody.c_str() + sentLen,
                    response.msgBodyLen - sentLen,
                    0);
                if (sentLen == -1) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        response.status = HANDLE_ERROR;
                        logStream("error") << "send HTML body failed (errno=" << errno << "), fd=" << m_clientFd << std::endl;
                        break;
                    }
                    break;
                }
                response.curStatusHasSendLen += sentLen;

                if (response.curStatusHasSendLen >= response.msgBodyLen) {
                    response.status = HADNLE_COMPLATE;
                    response.curStatusHasSendLen = 0;
                    logStream("info") << "HTML body sent, fd=" << m_clientFd << std::endl;
                    break;
                }
            } else if (response.bodyType == FILE_TYPE) {
                sentLen = response.curStatusHasSendLen;
                sentLen = sendfile(
                    m_clientFd,
                    response.fileMsgFd,
                    (off_t*)&sentLen,
                    response.msgBodyLen - sentLen);
                if (sentLen == -1) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        response.status = HANDLE_ERROR;
                        logStream("error") << "sendfile failed (errno=" << errno << "), fd=" << m_clientFd << std::endl;
                        break;
                    }
                    break;
                }
                response.curStatusHasSendLen += sentLen;

                if (response.curStatusHasSendLen >= response.msgBodyLen) {
                    response.status = HADNLE_COMPLATE;
                    response.curStatusHasSendLen = 0;
                    logStream("info") << "file body sent, fd=" << m_clientFd << std::endl;
                    break;
                }
            } else if (response.bodyType == EMPTY_TYPE) {
                response.status = HADNLE_COMPLATE;
                response.curStatusHasSendLen = 0;
                logStream("info") << "empty body response sent, fd=" << m_clientFd << std::endl;
                break;
            }
        }

        if (response.status == HANDLE_ERROR) {
            break;
        }
    }

    if (response.status == HADNLE_COMPLATE) {
        int fileFdToClose = -1;
        bool closeConnection = response.closeConnection;
        if (response.bodyType == FILE_TYPE) {
            fileFdToClose = response.fileMsgFd;
        }
        responseStatus.erase(m_clientFd);

        if (fileFdToClose >= 0) {
            close(fileFdToClose);
        }

        if (closeConnection) {
            deleteWaitFd(m_epollFd, m_clientFd);
            shutdown(m_clientFd, SHUT_RDWR);
            close(m_clientFd);
            logStream("info") << "response complete, closed fd=" << m_clientFd << std::endl;
        } else {
            modifyWaitFd(m_epollFd, m_clientFd, true, true, false);
            logStream("info") << "response complete, fd=" << m_clientFd << std::endl;
        }
    } else if (response.status == HANDLE_ERROR) {
        int fileFdToClose = -1;
        if (response.bodyType == FILE_TYPE) {
            fileFdToClose = response.fileMsgFd;
        }
        responseStatus.erase(m_clientFd);
        deleteWaitFd(m_epollFd, m_clientFd);
        shutdown(m_clientFd, SHUT_RDWR);
        close(m_clientFd);
        logStream("error") << "response failed, closed fd=" << m_clientFd << std::endl;
        if (fileFdToClose >= 0) {
            close(fileFdToClose);
        }
    } else {
        modifyWaitFd(m_epollFd, m_clientFd, true, true, true);
    }
}

std::string HandleSend::getStatusLine(const std::string& httpVersion,
                                      const std::string& statusCode,
                                      const std::string& statusDes) {
    return httpVersion + " " + statusCode + " " + statusDes + "\r\n";
}

void HandleSend::getFileListPage(std::string& fileListHtml, const std::string& userName) {
    std::vector<std::string> fileVec;
    ensureUserStorage(userName);
    getFileVec(userDir(userName), fileVec);
    std::sort(fileVec.begin(), fileVec.end());

    std::ifstream fileListStream("html/filelist.html", std::ios::in);
    if (!fileListStream) {
        fileListHtml = "<!DOCTYPE html><html><body><h1>Private Cloud</h1></body></html>";
        return;
    }

    std::string tempLine;
    while (std::getline(fileListStream, tempLine)) {
        replaceAll(tempLine, "{{username}}", htmlEscape(userName));

        if (tempLine == "<!--filelist_label-->") {
            for (const auto& fileName : fileVec) {
                const std::string escapedName = htmlEscape(fileName);
                const std::string encodedName = urlEncode(fileName);
                fileListHtml += "                            <tr><td class=\"col-name\">" + escapedName +
                                "</td><td><a class=\"link\" href=\"/download/" + encodedName +
                                "\">Download</a></td><td><a class=\"link danger\" href=\"/delete/" + encodedName +
                                "\" onclick=\"return confirmDelete();\">Delete</a></td></tr>\n";
            }
        } else {
            fileListHtml += tempLine + "\n";
        }
    }
}

void HandleSend::getFileVec(const std::string dirName, std::vector<std::string>& resVec) {
    DIR* dir = opendir(dirName.c_str());
    if (dir == nullptr) {
        return;
    }

    struct dirent* stdinfo;
    while ((stdinfo = readdir(dir)) != nullptr) {
        std::string name = stdinfo->d_name;
        if (name == "." || name == "..") {
            continue;
        }

        std::string path = dirName + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            resVec.push_back(name);
        }
    }
    closedir(dir);
}

std::string HandleSend::getMessageHeader(const std::string contentLength,
                                         const std::string contentType,
                                         const std::string redirectLoction,
                                         const std::string contentRange,
                                         const std::string setCookie,
                                         bool keepAlive) {
    std::string headerOpt;

    if (contentLength != "") {
        headerOpt += "Content-Length: " + contentLength + "\r\n";
    }

    if (contentType != "") {
        if (contentType == "html") {
            headerOpt += "Content-Type: text/html;charset=UTF-8\r\n";
        } else if (contentType == "file") {
            headerOpt += "Content-Type: application/octet-stream\r\n";
        }
    }

    if (redirectLoction != "") {
        headerOpt += "Location: " + redirectLoction + "\r\n";
    }

    if (contentRange != "") {
        headerOpt += "Content-Range: 0-" + contentRange + "\r\n";
    }

    if (setCookie != "") {
        headerOpt += "Set-Cookie: " + setCookie + "\r\n";
    }

    headerOpt += std::string("Connection: ") + (keepAlive ? "keep-alive" : "close") + "\r\n";

    return headerOpt;
}
