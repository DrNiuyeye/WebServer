

/**
 * @file myevent.cpp
 * @brief Event handlers for accept/read/write in the web server.
 */
#include <string>
#include <sstream>
#include <iomanip>
#include "myevent.h"



// Shared per-connection state maps.
std::unordered_map<int, Request> EventBase::requestStatus;
std::unordered_map<int, Response> EventBase::responseStatus;
std::mutex EventBase::requestMutex;
std::mutex EventBase::responseMutex;


    
// Decode URL-encoded strings in paths.
std::string urlDecode(const std::string& encoded) {
    std::string decoded;
    for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.size()) {

            int value;
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



void AcceptConn::process(){

    while(1){
        clientAddrLen = sizeof(clientAddr);
        int accFd = accept(m_listenFd, (sockaddr*)&clientAddr, &clientAddrLen);
        if(accFd == -1){
            if(errno == EINTR){
                continue;
            }
            if(errno == EAGAIN || errno == EWOULDBLOCK){
                break;
            }
            std::cout << outHead("error") << "accept failed (errno=" << errno << ")" << std::endl;
            break;
        }


        setNonBlocking(accFd);


        addWaitFd(m_epollFd, accFd, true, true);
        std::cout << outHead("info") << "accepted connection fd=" << accFd << std::endl;
    }
}



void HandleRecv::process(){
    std::lock_guard<std::mutex> reqLock(requestMutex);
    requestStatus[m_clientFd];
    std::cout << outHead("info") << "start HandleRecv, fd=" << m_clientFd << std::endl;




    char buf[2048];
    int recvLen = 0;


    while(1){

        recvLen = recv(m_clientFd, buf, 2048, 0);


        if(recvLen == 0){
            std::cout << outHead("info") << "peer closed connection, fd=" << m_clientFd << std::endl;
            requestStatus[m_clientFd].status = HANDLE_ERROR;
            break;
        }


        if(recvLen == -1){
            if(errno != EAGAIN){    
                requestStatus[m_clientFd].status = HANDLE_ERROR;
                std::cout << outHead("error") << "recv failed (errno=" << errno << "), fd=" << m_clientFd << std::endl;
                break;
            }

            modifyWaitFd(m_epollFd, m_clientFd, true, true, false);
            break;
        }


        requestStatus[m_clientFd].recvMsg.append(buf, recvLen);


       

        std::string::size_type endIndex = 0;
        


        if(requestStatus[m_clientFd].status == HANDLE_INIT){

            endIndex = requestStatus[m_clientFd].recvMsg.find("\r\n");       

            if(endIndex != std::string::npos){

                requestStatus[m_clientFd].setRequestLine(requestStatus[m_clientFd].recvMsg.substr(0, endIndex + 2) ); 
                requestStatus[m_clientFd].recvMsg.erase(0, endIndex + 2);    
                requestStatus[m_clientFd].status = HANDLE_HEAD;              
                std::cout << outHead("info") << "request line parsed, fd=" << m_clientFd << std::endl;
            }


        }
        

        if(requestStatus[m_clientFd].status == HANDLE_HEAD){
            
            std::string curLine;       

            while(1){
                
                endIndex = requestStatus[m_clientFd].recvMsg.find("\r\n");            
                if(endIndex == std::string::npos){                                    
                    break;
                }

                curLine = requestStatus[m_clientFd].recvMsg.substr(0, endIndex + 2);  
                requestStatus[m_clientFd].recvMsg.erase(0, endIndex + 2);             

                if(curLine == "\r\n"){
                    requestStatus[m_clientFd].status = HANDLE_BODY;                                       
                    if(requestStatus[m_clientFd].msgHeader["Content-Type"] == "multipart/form-data"){     
                        requestStatus[m_clientFd].fileMsgStatus = FILE_BEGIN_FLAG;
                    }
                    std::cout << outHead("info") << "headers parsed, fd=" << m_clientFd << std::endl;
                    if(requestStatus[m_clientFd].requestMethod == "POST"){
                        std::cout << outHead("info") << "POST body parsing started, fd=" << m_clientFd << std::endl;
                    }
                    break;                                                                              
                }

                requestStatus[m_clientFd].addHeaderOpt(curLine);                     




            }
        }


        if(requestStatus[m_clientFd].status == HANDLE_BODY){

            if(requestStatus[m_clientFd].requestMethod == "GET"){

                {
                    std::lock_guard<std::mutex> respLock(responseMutex);
                    responseStatus[m_clientFd].bodyFileName = requestStatus[m_clientFd].requestResourse;
                }


                modifyWaitFd(m_epollFd, m_clientFd, true, true, true);
                requestStatus[m_clientFd].status = HADNLE_COMPLATE;
                std::cout << outHead("info") << "GET request parsed, schedule write event, fd=" << m_clientFd << std::endl;
                break;
            }


            if(requestStatus[m_clientFd].requestMethod == "POST"){

                std::string::size_type beginSize = requestStatus[m_clientFd].recvMsg.size();
                (void)beginSize;

                if(requestStatus[m_clientFd].msgHeader["Content-Type"] == "multipart/form-data"){

                    if(requestStatus[m_clientFd].fileMsgStatus == FILE_BEGIN_FLAG){
                        std::cout << outHead("info") << "POST multipart: waiting boundary begin, fd=" << m_clientFd << std::endl;

                        endIndex = requestStatus[m_clientFd].recvMsg.find("\r\n");


                        if(endIndex != std::string::npos){
                            std::string flagStr = requestStatus[m_clientFd].recvMsg.substr(0, endIndex);

                            if(flagStr == "--" +requestStatus[m_clientFd].msgHeader["boundary"]){  
                                requestStatus[m_clientFd].fileMsgStatus = FILE_HEAD;               
                                requestStatus[m_clientFd].recvMsg.erase(0, endIndex + 2);          
                                std::cout << outHead("info") << "POST multipart: boundary begin matched, parsing part headers, fd=" << m_clientFd << std::endl;
                            }else{

                                {
                                    std::lock_guard<std::mutex> respLock(responseMutex);
                                    responseStatus[m_clientFd].bodyFileName = "/redirect";
                                }
                                modifyWaitFd(m_epollFd, m_clientFd, true, true, true);   
                                requestStatus[m_clientFd].status = HADNLE_COMPLATE;
                                std::cout << outHead("error") << "POST multipart: boundary begin mismatch, redirect, fd=" << m_clientFd << std::endl;
                                break;
                            }
                        }
                    }


                    if(requestStatus[m_clientFd].fileMsgStatus == FILE_HEAD){
                        std::string strLine;
                        while(1){

                            endIndex = requestStatus[m_clientFd].recvMsg.find("\r\n");
                            if(endIndex != std::string::npos){
                                strLine = requestStatus[m_clientFd].recvMsg.substr(0, endIndex + 2);  
                                requestStatus[m_clientFd].recvMsg.erase(0, endIndex + 2);             


                                if(strLine == "\r\n"){
                                    requestStatus[m_clientFd].fileMsgStatus = FILE_CONTENT;
                                    std::cout << outHead("info") << "POST multipart: part headers parsed, receiving file body, fd=" << m_clientFd << std::endl;
                                    break;
                                }

                                endIndex = strLine.find("filename");
                                if(endIndex != std::string::npos){
                                    strLine.erase(0, endIndex + std::string("filename=\"").size());          
                                    for(int i = 0; strLine[i] != '\"'; ++i){                                 
                                        requestStatus[m_clientFd].recvFileName += strLine[i];
                                    }
                                    std::cout << outHead("info") << "POST multipart: filename=" << requestStatus[m_clientFd].recvFileName << ", fd=" << m_clientFd << std::endl;
                                }
                            }else{   
                                break;
                            }
                        }
                    }


                    if(requestStatus[m_clientFd].fileMsgStatus == FILE_CONTENT){

                        std::ofstream ofs("filedir/" + requestStatus[m_clientFd].recvFileName, std::ios::out | std::ios::app | std::ios::binary);
                        if(!ofs){
                            std::cout << outHead("error") << "open upload target failed, fd=" << m_clientFd << std::endl;
                            break;
                        }

                        while(1){
                            int saveLen = requestStatus[m_clientFd].recvMsg.size();        
                            if(saveLen == 0){                                              
                                break;
                            }

                            endIndex = requestStatus[m_clientFd].recvMsg.find('\r');
                                        
                            if(endIndex != std::string::npos){   

                                int boundarySecLen = requestStatus[m_clientFd].msgHeader["boundary"].size() + 8;
                                if(requestStatus[m_clientFd].recvMsg.size() - endIndex >= boundarySecLen){

                                    if(requestStatus[m_clientFd].recvMsg.substr(endIndex, boundarySecLen) ==
                                                    "\r\n--" + requestStatus[m_clientFd].msgHeader["boundary"] + "--\r\n"){
                                        if(endIndex == 0){                  
                                            std::cout << outHead("info") << "POST multipart: upload complete, fd=" << m_clientFd << std::endl;
                                            requestStatus[m_clientFd].fileMsgStatus = FILE_COMPLATE;
                                            break;
                                        }


                                        saveLen = endIndex;
                                        
                                    }else{  

                                        endIndex = requestStatus[m_clientFd].recvMsg.find('\r', endIndex + 1);
                                        if(endIndex != std::string::npos){
                                            saveLen = endIndex;
                                        }
                                    }

                                }else{  



                                    if(endIndex == 0){   
                                        break;
                                    }


                                    saveLen = endIndex;
                                }
                            }

                            ofs.write(requestStatus[m_clientFd].recvMsg.c_str(), saveLen);
                            requestStatus[m_clientFd].recvMsg.erase(0, saveLen);
                        }
                        ofs.close();
                    }


                    if(requestStatus[m_clientFd].fileMsgStatus == FILE_COMPLATE){

                        {
                            std::lock_guard<std::mutex> respLock(responseMutex);
                            responseStatus[m_clientFd].bodyFileName = "/redirect";
                        }
                        modifyWaitFd(m_epollFd, m_clientFd, true, true, true);   
                        requestStatus[m_clientFd].status = HADNLE_COMPLATE;
                        std::cout << outHead("info") << "POST handled, schedule write event, fd=" << m_clientFd << std::endl;
                        break;
                    }
                }else{    

                    {
                        std::lock_guard<std::mutex> respLock(responseMutex);
                        responseStatus[m_clientFd].bodyFileName = "/redirect";
                    }
                    modifyWaitFd(m_epollFd, m_clientFd, true, true, true);
                    requestStatus[m_clientFd].status = HADNLE_COMPLATE;
                    std::cout << outHead("error") << "POST content-type unsupported, redirect, fd=" << m_clientFd << std::endl;
                    break;
                }
            }

        }

    }

    

    if(requestStatus[m_clientFd].status == HADNLE_COMPLATE){
        std::cout << outHead("info") << "HandleRecv complete, fd=" << m_clientFd << std::endl;
        requestStatus.erase(m_clientFd);
    }else if(requestStatus[m_clientFd].status == HANDLE_ERROR){        

        std::cout << outHead("error") << "HandleRecv error, close fd=" << m_clientFd << std::endl;

        deleteWaitFd(m_epollFd, m_clientFd);

        shutdown(m_clientFd, SHUT_RDWR);
        close(m_clientFd);
        requestStatus.erase(m_clientFd);

    
}
}



void HandleSend::process(){
    std::lock_guard<std::mutex> respLock(responseMutex);
    std::cout << outHead("info") << "start HandleSend, fd=" << m_clientFd << std::endl;
    if(responseStatus.find(m_clientFd) == responseStatus.end()){
        std::cout << outHead("info") << "response state not found, fd=" << m_clientFd << std::endl;
        return;
    }


    if(responseStatus[m_clientFd].status == HANDLE_INIT){
        std::string opera, filename;
        if(responseStatus[m_clientFd].bodyFileName == "/"){

            opera = "/";
        }else{



            int i = 1;
            while(i < responseStatus[m_clientFd].bodyFileName.size() && responseStatus[m_clientFd].bodyFileName[i] != '/'){
                ++i;
            }

            if(i < responseStatus[m_clientFd].bodyFileName.size() - 1){
                opera = responseStatus[m_clientFd].bodyFileName.substr(1, i - 1);
                filename = responseStatus[m_clientFd].bodyFileName.substr(i+1);
            }else{
                opera = "redirect";
            }

        }
        


        if(opera == "/"){                   

            responseStatus[m_clientFd].beforeBodyMsg = getStatusLine("HTTP/1.1", "200", "OK");


            getFileListPage(responseStatus[m_clientFd].msgBody);

            responseStatus[m_clientFd].msgBodyLen = responseStatus[m_clientFd].msgBody.size();



            responseStatus[m_clientFd].beforeBodyMsg += getMessageHeader(std::to_string(responseStatus[m_clientFd].msgBodyLen), "html");
            responseStatus[m_clientFd].beforeBodyMsg += "\r\n";
            responseStatus[m_clientFd].beforeBodyMsgLen = responseStatus[m_clientFd].beforeBodyMsg.size();



            responseStatus[m_clientFd].bodyType = HTML_TYPE;      
            responseStatus[m_clientFd].status = HANDLE_HEAD;      
            responseStatus[m_clientFd].curStatusHasSendLen = 0;   
            std::cout << outHead("info") << "prepared file list page response, fd=" << m_clientFd << std::endl;

        }else if(opera == "download"){      



            responseStatus[m_clientFd].beforeBodyMsg = getStatusLine("HTTP/1.1", "200", "OK");


            std::string decodedFilename = urlDecode(filename);  


            responseStatus[m_clientFd].fileMsgFd = open(("filedir/" + decodedFilename).c_str(), O_RDONLY);

            if(responseStatus[m_clientFd].fileMsgFd == -1){                  
                std::cout << outHead("error") << "open download file failed, file=" << filename << ", fd=" << m_clientFd << std::endl;
                responseStatus[m_clientFd] = Response();                     
                responseStatus[m_clientFd].bodyFileName = "/redirect";
                modifyWaitFd(m_epollFd, m_clientFd, true, true, true);       
                return;
            }else{    

                struct stat fileStat;
                fstat(responseStatus[m_clientFd].fileMsgFd, &fileStat);
                

                responseStatus[m_clientFd].msgBodyLen = fileStat.st_size;
                

                responseStatus[m_clientFd].beforeBodyMsg += getMessageHeader(std::to_string(responseStatus[m_clientFd].msgBodyLen), "file", std::to_string(responseStatus[m_clientFd].msgBodyLen - 1));

                responseStatus[m_clientFd].beforeBodyMsg += "\r\n";
                responseStatus[m_clientFd].beforeBodyMsgLen = responseStatus[m_clientFd].beforeBodyMsg.size();
                

                responseStatus[m_clientFd].bodyType = FILE_TYPE;      
                responseStatus[m_clientFd].status = HANDLE_HEAD;      
                responseStatus[m_clientFd].curStatusHasSendLen = 0;   

                std::cout << outHead("info") << "prepared download response, file=" << filename << ", fd=" << m_clientFd << std::endl;
                
            }

        }else if(opera == "delete"){        

            int ret = remove(("filedir/" + filename).c_str());
            if(ret != 0){
                std::cout << outHead("error") << "delete file failed, file=" << filename << ", fd=" << m_clientFd << std::endl;
            }else{
                std::cout << outHead("info") << "delete file success, file=" << filename << ", fd=" << m_clientFd << std::endl;
            }


            responseStatus[m_clientFd] = Response();                     
            responseStatus[m_clientFd].bodyFileName = "/redirect";       

            std::cout << outHead("info") << "delete done, schedule redirect response, fd=" << m_clientFd << std::endl;


            modifyWaitFd(m_epollFd, m_clientFd, true, true, true);
            return;
        }else{                              

            responseStatus[m_clientFd].beforeBodyMsg = getStatusLine("HTTP/1.1", "302", "Moved Temporarily");


            responseStatus[m_clientFd].beforeBodyMsg += getMessageHeader("0", "html", "/", "");


            responseStatus[m_clientFd].beforeBodyMsg += "\r\n";

            responseStatus[m_clientFd].beforeBodyMsgLen = responseStatus[m_clientFd].beforeBodyMsg.size();


            responseStatus[m_clientFd].bodyType = EMPTY_TYPE;    
            responseStatus[m_clientFd].status = HANDLE_HEAD;     
            responseStatus[m_clientFd].curStatusHasSendLen = 0;   
            std::cout << outHead("info") << "prepared redirect response, fd=" << m_clientFd << std::endl;
        }
    }


    while(1){
        long long sentLen = 0;
        if(responseStatus[m_clientFd].status == HANDLE_HEAD){

            sentLen = responseStatus[m_clientFd].curStatusHasSendLen;
            sentLen = send(m_clientFd, responseStatus[m_clientFd].beforeBodyMsg.c_str() + sentLen, responseStatus[m_clientFd].beforeBodyMsgLen - sentLen, 0);
            if(sentLen == -1) {
                if(errno != EAGAIN){

                    responseStatus[m_clientFd].status = HANDLE_ERROR;
                    std::cout << outHead("error") << "send headers failed (errno=" << errno << "), fd=" << m_clientFd << std::endl;
                    break;
                }

                break;
            }
            responseStatus[m_clientFd].curStatusHasSendLen += sentLen;

            if(responseStatus[m_clientFd].curStatusHasSendLen >= responseStatus[m_clientFd].beforeBodyMsgLen){
                responseStatus[m_clientFd].status = HANDLE_BODY;     
                responseStatus[m_clientFd].curStatusHasSendLen = 0;   
                std::cout << outHead("info") << "headers sent, continue body send, fd=" << m_clientFd << std::endl;
            }


            if(responseStatus[m_clientFd].bodyType == FILE_TYPE){
                std::cout << outHead("info") << "sending file body=" << responseStatus[m_clientFd].bodyFileName << ", fd=" << m_clientFd << std::endl;
            }
        }


        if(responseStatus[m_clientFd].status == HANDLE_BODY){

            if(responseStatus[m_clientFd].bodyType == HTML_TYPE){

                sentLen = responseStatus[m_clientFd].curStatusHasSendLen;
                sentLen = send(m_clientFd, responseStatus[m_clientFd].msgBody.c_str() + sentLen, responseStatus[m_clientFd].msgBodyLen - sentLen, 0);
                if(sentLen == -1){
                    if(errno != EAGAIN){

                        responseStatus[m_clientFd].status = HANDLE_ERROR;
                        std::cout << outHead("error") << "send HTML body failed (errno=" << errno << "), fd=" << m_clientFd << std::endl;
                        break;
                    }
                    

                    break;
                }
                responseStatus[m_clientFd].curStatusHasSendLen += sentLen;
                

                if(responseStatus[m_clientFd].curStatusHasSendLen >= responseStatus[m_clientFd].msgBodyLen){
                    responseStatus[m_clientFd].status = HADNLE_COMPLATE;     
                    responseStatus[m_clientFd].curStatusHasSendLen = 0;   
                    std::cout << outHead("info") << "HTML body sent, fd=" << m_clientFd << std::endl;
                    break;
                }

            }else if(responseStatus[m_clientFd].bodyType == FILE_TYPE){

                

                sentLen = responseStatus[m_clientFd].curStatusHasSendLen;
                

                sentLen = sendfile(m_clientFd, responseStatus[m_clientFd].fileMsgFd, (off_t *)&sentLen, responseStatus[m_clientFd].msgBodyLen - sentLen);
                if(sentLen == -1){
                    if(errno != EAGAIN){

                        responseStatus[m_clientFd].status = HANDLE_ERROR;
                        std::cout << outHead("error") << "sendfile failed (errno=" << errno << "), fd=" << m_clientFd << std::endl;
                        break;
                    }

                    break;
                }
                

                responseStatus[m_clientFd].curStatusHasSendLen += sentLen;


                if(responseStatus[m_clientFd].curStatusHasSendLen >= responseStatus[m_clientFd].msgBodyLen){
                    responseStatus[m_clientFd].status = HADNLE_COMPLATE;     
                    responseStatus[m_clientFd].curStatusHasSendLen = 0;       

                    std::cout << outHead("info") << "file body sent, fd=" << m_clientFd << std::endl;
                    break;
                }

            }else if(responseStatus[m_clientFd].bodyType == EMPTY_TYPE){

                responseStatus[m_clientFd].status = HADNLE_COMPLATE;       
                responseStatus[m_clientFd].curStatusHasSendLen = 0;         
                std::cout << outHead("info") << "empty body response sent, fd=" << m_clientFd << std::endl;
                break;
            }
        }

        if(responseStatus[m_clientFd].status == HANDLE_ERROR){    
            break;
        }
    }
    



    if(responseStatus[m_clientFd].status == HADNLE_COMPLATE){
        int fileFdToClose = -1;
        if(responseStatus[m_clientFd].bodyType == FILE_TYPE){
            fileFdToClose = responseStatus[m_clientFd].fileMsgFd;
        }
        responseStatus.erase(m_clientFd);
        modifyWaitFd(m_epollFd, m_clientFd, true, true, false);   
        std::cout << outHead("info") << "response complete, fd=" << m_clientFd << std::endl;
        if(fileFdToClose >= 0){
            close(fileFdToClose);
        }
    }else if(responseStatus[m_clientFd].status == HANDLE_ERROR){ 
        int fileFdToClose = -1;
        if(responseStatus[m_clientFd].bodyType == FILE_TYPE){
            fileFdToClose = responseStatus[m_clientFd].fileMsgFd;
        }
        responseStatus.erase(m_clientFd);
        modifyWaitFd(m_epollFd, m_clientFd, true, false, false);
        shutdown(m_clientFd, SHUT_WR);
        close(m_clientFd);
        std::cout << outHead("error") << "response failed, closed fd=" << m_clientFd << std::endl;
        if(fileFdToClose >= 0){
            close(fileFdToClose);
        }
    }else{ 

        modifyWaitFd(m_epollFd, m_clientFd, true, true, true);
        return;
    }

}



std::string HandleSend::getStatusLine(const std::string &httpVersion, const std::string &statusCode, const std::string &statusDes){
    std::string statusLine;
    statusLine = httpVersion + " ";
    statusLine += statusCode + " ";
    statusLine += statusDes + "\r\n";

    return statusLine;
}


void HandleSend::getFileListPage(std::string &fileListHtml){

    std::vector<std::string> fileVec;
    getFileVec("filedir", fileVec);
    

    std::ifstream fileListStream("html/filelist.html", std::ios::in);
    std::string tempLine;

    while(1){
        getline(fileListStream, tempLine);
        if(tempLine == "<!--filelist_label-->"){
            break;
        }
        fileListHtml += tempLine + "\n";
    }



    for(const auto &filename : fileVec){
        fileListHtml += "            <tr><td class=\"col1\">" + filename +
                    "</td> <td class=\"col2\"><a href=\"download/" + filename +
                    "\">??</a></td> <td class=\"col3\"><a href=\"delete/" + filename +
                    "\" onclick=\"return confirmDelete();\">??</a></td></tr>" + "\n";
    }


    while(getline(fileListStream, tempLine)){
        fileListHtml += tempLine + "\n";
    }
    
}

void HandleSend::getFileVec(const std::string dirName, std::vector<std::string> &resVec){
    DIR *dir = opendir(dirName.c_str());
    if(dir == nullptr){
        return;
    }
    struct dirent *stdinfo;
    while(1){
        stdinfo = readdir(dir);
        if(stdinfo == nullptr){
            break;
        }
        resVec.push_back(stdinfo->d_name);
        if(resVec.back() == "." || resVec.back() == ".."){
            resVec.pop_back();
        }
    }
    closedir(dir);
}







std::string HandleSend::getMessageHeader(const std::string contentLength, const std::string contentType, const std::string redirectLoction, const std::string contentRange){
    std::string headerOpt;


    if(contentLength != ""){
        headerOpt += "Content-Length: " + contentLength + "\r\n";
    }


    if(contentType != ""){
        if(contentType == "html"){
            headerOpt += "Content-Type: text/html;charset=UTF-8\r\n";     
        }else if(contentType == "file"){
            headerOpt += "Content-Type: application/octet-stream\r\n";    
        }
    }


    if(redirectLoction != ""){
        headerOpt += "Location: " + redirectLoction + "\r\n";
    }


    if(contentRange != ""){
        headerOpt += "Content-Range: 0-" + contentRange + "\r\n";
    }

    headerOpt += "Connection: keep-alive\r\n";

    return headerOpt;
}









