#include <sys/time.h>
#include <sys/select.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/uio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>

#define BUF_SIZE_INPUT   2048U
#define BUF_SIZE_OUTBOUND 2144U
#define MAX_NAME_LEN 32U
#define MAX_WELCOME_LEN 128U
#define DATABASE_PATH_SIZE 512U

#define PORT_MAX 65535L
#define PORT_MIN 1024L

#define CLIENTS_MAX 1000L
#define CLIENTS_MIN 1L


typedef struct {
    int32_t fd;
    bool isActive;
    char name[32];
    char text[2048];
    char timestamp[20];
} Client;


bool isSocketError(int32_t status, const char *msg);
void checkSyscall(int32_t status, const char *msg);
uint16_t parseServerArgs(const char *argStr, long minSize, long maxSize, const char *argName);
void sigintHandler(int signalNumber);

volatile sig_atomic_t serverStatus = 1;
static int32_t duplicateHistoryFd = -1;
static volatile sig_atomic_t currentClientsCount = 0;

void handleClientAuth(size_t clientIndex, Client *const clientArray, size_t  maxClient, int32_t historyFd);
void handleClientMessage(size_t clientIndex, Client *const clientArray, size_t maxClient, int32_t historyFd);
void disconnectClient(size_t clientIndex, Client *const clientArray, size_t maxClient, int32_t historyFd);

bool validateBuffer(char *buffer, ssize_t bytesRead, int32_t maxSize);
void formatMessage(char *destBuffer, int32_t destSize, const char *senderName, const char *text);
void broadcastMessage(const char *readyMessage, Client *const clientArray, int32_t senderFd, uint16_t maxClient, int32_t historyFd);
void writeLog(int32_t logFd, const char *readyMessage);

int32_t main(int argc, char *argv[]){
    if (argc < 3U)
    {
        const char MissingArgumentsError[] = "Error: Missing required arguments.\nUsage: ./server <port> <max_users>\n";
        checkSyscall(write(2, MissingArgumentsError, sizeof(MissingArgumentsError) - 1), "Syscall: write missing arguments error");
        exit(EXIT_FAILURE);
    }

    uint16_t finalPort = parseServerArgs(argv[1], PORT_MIN, PORT_MAX, "Port");
    uint16_t maxClient = parseServerArgs(argv[2], CLIENTS_MIN, CLIENTS_MAX, "Client");

    const char *homeDir = getenv("HOME");
    if(homeDir == NULL)
    {
        homeDir = ".";
    }

    char databasePath[DATABASE_PATH_SIZE];
    memset(databasePath, 0, sizeof(databasePath));

    const char *pathFragments[] = {homeDir, "/Chat_history.txt"};
    size_t pathCursor = 0;

    for(size_t f = 0U; f < 2U; f++){
        const char *src = pathFragments[f];
        for(size_t i = 0; (src[i] != '\0' && (pathCursor < sizeof(databasePath) - 1U) ); i++){
            databasePath[pathCursor++] = src[i];
        }
    }

    databasePath[pathCursor] = '\0';

    int32_t historyFd = open(databasePath, O_RDWR | O_CREAT | O_APPEND | O_SYNC, 0644);
    checkSyscall(historyFd, "Syscall: open chat history database failed");

    duplicateHistoryFd = historyFd;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    sa.sa_handler = sigintHandler;
    checkSyscall(sigaction(SIGINT, &sa, NULL), "Syscall: sigaction SIGINT");
    checkSyscall(sigaction(SIGTERM, &sa, NULL), "Syscall: sigaction SIGTERM");

    sa.sa_handler = SIG_IGN;
    checkSyscall(sigaction(SIGPIPE, &sa, NULL), "Syscall: sigaction SIGPIPRE");
    checkSyscall(sigaction(SIGHUP, &sa, NULL), "Syscall: sigaction SIGHUP");

    int32_t serverFd = socket(AF_INET, SOCK_STREAM, 0);
    checkSyscall(serverFd, "Syscall: socket creation failed");

    const char startMsgPrefix[] = "Server started on port ";
    const char startMsgSuffix[] = "\n";

    struct iovec iov[3];

    iov[0].iov_base = (void *)startMsgPrefix;
    iov[0].iov_len = sizeof(startMsgPrefix) - 1;
    iov[1].iov_base = (void *)argv[1];
    iov[1].iov_len = strlen(argv[1]);
    iov[2].iov_base = (void *)startMsgSuffix;
    iov[2].iov_len = sizeof(startMsgSuffix) - 1;

    if(writev(2, iov, 3) < 0)
    {
        perror("Syscal: writev start message");
        exit(EXIT_FAILURE);
    }
    
    char formattedStartMsg[BUF_SIZE_OUTBOUND];
    memset(formattedStartMsg, 0, sizeof(formattedStartMsg));

    formatMessage(formattedStartMsg, sizeof(formattedStartMsg), "Server started", argv[1]);
    writeLog(historyFd, formattedStartMsg);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(finalPort);
    addr.sin_addr.s_addr = INADDR_ANY;

    int32_t sockOpt = 1;
    int32_t optStatus = setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &sockOpt, sizeof(sockOpt));
    checkSyscall(optStatus, "Syscall: setsockopt SO_REUSEADDR failed");

    ssize_t bindStatus = bind(serverFd, (struct sockaddr *)&addr, sizeof(addr));
    checkSyscall(bindStatus, "Syscall: bind address failed");
    
    const char BindSuccessNotice[] = "Port bound successfully!\n";
    checkSyscall(write(1, BindSuccessNotice, sizeof(BindSuccessNotice) - 1), "Syscall: write bind success notice failed");

    int32_t listenStatus = listen(serverFd, maxClient);
    checkSyscall(listenStatus, "Syscall: listen passive mode failed");

    const char ListenSuccessNotice[] = "Server is listening...\n";
    checkSyscall(write(1, ListenSuccessNotice, sizeof(ListenSuccessNotice) - 1), "Syscall: write listen success notice failed");

    serverStatus = 1;

    static Client clientArray[CLIENTS_MAX];
    memset(clientArray, 0, sizeof(clientArray));

    for(size_t i = 0; i < (size_t)CLIENTS_MAX; i++){
        clientArray[i].fd       = -1;
        clientArray[i].isActive = false;
    }

    fd_set MasterFds;
    fd_set ReadFds;

    FD_ZERO(&MasterFds);
    FD_SET(serverFd, &MasterFds);

    for(;;){
    ReadFds = MasterFds;

    int32_t maxFd = serverFd;

    for(size_t i = 0; i < (size_t)CLIENTS_MAX; i++){
        if(clientArray[i].fd != -1)
        {    
            FD_SET(clientArray[i].fd, &ReadFds);

            if(clientArray[i].fd > maxFd)
            {
                maxFd = clientArray[i].fd;
            }
        }
    }

    ssize_t selectStatus = select(maxFd + 1, &ReadFds, NULL, NULL, NULL);

    if(selectStatus < 0)
    {
        if(errno == EINTR)
        {
            continue;
        }
        checkSyscall(selectStatus, "Syscall: select failed");
    }

    size_t activeSockets = (size_t)selectStatus;

    if(FD_ISSET(serverFd, &ReadFds))
    {
        activeSockets--;

        if((size_t)currentClientsCount < (size_t)maxClient)
        {
            int32_t client = accept(serverFd, NULL, NULL);

            if(client >= 0)
            {
                for(size_t i = 0U; i < (size_t)CLIENTS_MAX; i++){
                    if(clientArray[i].fd == -1)
                    {
                        clientArray[i].fd = client;
                        FD_SET(client, &MasterFds);
                        currentClientsCount++;
                        break;
                    }
                }
            }
        }
        else
        {
            int32_t rejectedClientFd = accept(serverFd, NULL, NULL);
            if(rejectedClientFd >= 0)
            {
                const char rejectionMessage[] = "Server is full. Try again later.\n";
                (void)write(rejectedClientFd, rejectionMessage, sizeof(rejectionMessage));
                (void)close(rejectedClientFd);
            }
        }
    }
     for(size_t i = 0; (i < (size_t)CLIENTS_MAX && (activeSockets > 0U)); i++){
         if(clientArray[i].fd == -1)
         {
            continue;
         }

         if(FD_ISSET(clientArray[i].fd, &ReadFds))
         {
            activeSockets--;

            if(clientArray[i].name[0] == '\0')
            {
                handleClientAuth(i, clientArray, (size_t)maxClient, historyFd);
            }
            else
            {
                 handleClientMessage(i, clientArray, (size_t)maxClient, historyFd);
            }
         }
     }
}
    return 0;
}

bool isSocketError(int32_t status, const char *msg){
    if(msg == NULL)
    {
        msg = "Warning: isSocketError called with NULL msg pointer";
    }

    if(status >= 0)
    {
        return false;
    }

    if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
    {
        return false;
    }

    perror(msg);
    return true;
}

void checkSyscall(int32_t status, const char *msg){
    if(status >= 0)
    {
        return;
    }

    if(msg == NULL)
    {
        msg = "Fatal: checkSyscall caught an unnamed system error";
    }

    perror(msg);
    exit(EXIT_FAILURE);
}

uint16_t parseServerArgs(const char *argStr, long minSize, long maxSize, const char *argName){
    char *argEndPtr;
    long parseVal = strtol(argStr, &argEndPtr, 10);

    if (argStr == argEndPtr)
    {
        const char errPrefix[] = "Error: ";
        const char errSuffix[] = " must begin with a valid digit!\n";

        struct iovec iov[3];

        iov[0].iov_base = (void *)errPrefix;
        iov[0].iov_len = sizeof(errPrefix) - 1;
        iov[1].iov_base = (void *)argName;
        iov[1].iov_len = strlen(argName);
        iov[2].iov_base = (void *)errSuffix;
        iov[2].iov_len = sizeof(errSuffix) - 1;
        
        if(writev(1, iov, 3) < 0)
        {
            perror("Syscall: writev format error failed");
        }

        exit(EXIT_FAILURE);
    }
    else if (*argEndPtr != '\0')
    {
        const char errPrefix[] = "Error: ";
        const char errSuffix[] = " contains invalid trailing characters!\n";

        struct iovec iov[3];

        iov[0].iov_base = (void *)errPrefix;
        iov[0].iov_len = sizeof(errPrefix) - 1;
        iov[1].iov_base = (void *)argName;
        iov[1].iov_len = strlen(argName);
        iov[2].iov_base = (void *)errSuffix;
        iov[2].iov_len = sizeof(errSuffix) - 1;

        if(writev(1, iov, 3) < 0)
        {
            perror("Syscall: writev trailing error failed");
        }

        exit(EXIT_FAILURE);
    }

    if ( (parseVal < minSize) || (parseVal > maxSize) )
    {
        const char errPrefix[] = "Error: ";
        const char errSuffix[] = " number is out of allowed range!\n";

        struct iovec iov[3];

        iov[0].iov_base = (void *)errPrefix;
        iov[0].iov_len = sizeof(errPrefix) - 1;
        iov[1].iov_base = (void *)argName;
        iov[1].iov_len = strlen(argName);
        iov[2].iov_base = (void *)errSuffix;
        iov[2].iov_len = sizeof(errSuffix) - 1;

        if(writev(1, iov, 3) < 0)
        {
            perror("Syscall: writev invalid limit error");
        }

        exit(EXIT_FAILURE);
    }

    return (uint16_t)parseVal;
}

void sigintHandler(int signalNumber){
    static const char MessageInt[] = "\n[SIGNAL] SIGINT received (Ctrl+C). Emergency termination...\n";
    static const char MessageTerm[] = "\n[SIGNAL] SIGTERM (Termination) received. Shutting down...\n";
    static const char MessageEarly[] = "\n[SIGNAL] The server has been stopped before the network was initialised.\n";

    const char *selectedMessage = NULL;
    size_t messageLength = 0U;

    if(serverStatus == 0)
    {
        selectedMessage = MessageEarly;
        messageLength = sizeof(MessageEarly) - 1U;
    }
    else if(signalNumber == SIGINT)
    {
        selectedMessage = MessageInt;
        messageLength = sizeof(MessageInt) - 1U;
    }
    else if(signalNumber == SIGTERM)
    {
        selectedMessage = MessageTerm;
        messageLength = sizeof(MessageTerm) - 1U;
    }
    else
    {
        _exit(EXIT_FAILURE);
    }

    (void)write(2, selectedMessage, messageLength);

    if(duplicateHistoryFd != -1)
    {
        (void)write(duplicateHistoryFd, selectedMessage, messageLength);
        (void)fsync(duplicateHistoryFd);
        (void)close(duplicateHistoryFd);
    }

    _exit(EXIT_FAILURE);
}

void handleClientAuth(size_t clientIndex, Client *const clientArray, size_t maxClient, int32_t historyFd){
    if(clientArray == NULL)
    {
        return;
    }

    char authBuffer[MAX_NAME_LEN];
    memset(authBuffer, 0, sizeof(authBuffer));

    int32_t senderFd = clientArray[clientIndex].fd;

    ssize_t bytesRead = read(clientArray[clientIndex].fd, authBuffer, sizeof(authBuffer) - 1);

    if(bytesRead == 0)
    {
        disconnectClient(clientIndex, clientArray, maxClient, historyFd);
        return;
    }

    if(isSocketError(bytesRead, "Syscall: read incoming client name failed"))
    {
        disconnectClient(clientIndex, clientArray, maxClient, historyFd);
        return;
    }

    if(!validateBuffer(authBuffer, bytesRead, MAX_NAME_LEN))
    {
        disconnectClient(clientIndex, clientArray, maxClient, historyFd);
        return;
    }

    for(ssize_t i = 0; i < bytesRead; i++){
        if(authBuffer[i] == '\n' || authBuffer[i] == '\r')
        {
            authBuffer[i] = '\0';
        }
    }

    if(authBuffer[0] == '\0')
    {
        const char WithoutName[] = "Error : You tried connect without name.";
        (void)write(clientArray[clientIndex].fd, WithoutName, sizeof(WithoutName) - 1U);
        disconnectClient(clientIndex, clientArray, maxClient, historyFd);
        return;
    }

    for(ssize_t i = 0; authBuffer[i] != '\0'; i++){
        if(authBuffer[i] == ' ')
        {
            disconnectClient(clientIndex, clientArray, maxClient, historyFd);
            return;
        }
    }

    (void)strncpy(clientArray[clientIndex].name, authBuffer, sizeof(clientArray[clientIndex].name) - 1U);
    clientArray[clientIndex].name[sizeof(clientArray[clientIndex].name) - 1U] = '\0';

    clientArray[clientIndex].isActive = true;

    char formattedMessage[MAX_WELCOME_LEN];
    memset(formattedMessage, 0, sizeof(formattedMessage));

    formatMessage(formattedMessage, sizeof(formattedMessage), clientArray[clientIndex].name, "Successfully connected to the server");

    broadcastMessage(formattedMessage, clientArray, senderFd, maxClient, historyFd);
    writeLog(historyFd, formattedMessage);

    const char welcomeNotice[] = "--- Welcome to the chat! Type your messages below ---\n";
    (void)write(clientArray[clientIndex].fd, welcomeNotice, sizeof(welcomeNotice) - 1U);
}

void handleClientMessage(size_t clientIndex, Client *const clientArray, size_t maxClient, int32_t historyFd){
    if(clientArray == NULL)
    {
        return;
    }

    ssize_t bytesRead = read(clientArray[clientIndex].fd, clientArray[clientIndex].text, sizeof(clientArray[clientIndex].text) - 1U);

    if(isSocketError(bytesRead, "Syscall: read incoming client message failed") || bytesRead == 0)
    {
        disconnectClient(clientIndex, clientArray, maxClient, historyFd);
        return;
    }

    if(!validateBuffer(clientArray[clientIndex].text, bytesRead, BUF_SIZE_INPUT))
    {
        disconnectClient(clientIndex, clientArray, maxClient, historyFd);
        return;
    }


    int32_t lastCharIndex = (int32_t)bytesRead - 1;
    for(; lastCharIndex >= 0; lastCharIndex--){
        char lastChar = clientArray[clientIndex].text[lastCharIndex];
        if(lastChar == '\n' || lastChar == '\r')
        {
            clientArray[clientIndex].text[lastCharIndex] = '\0';
        }
        else
        {
            break;
        }
    }
    
    if(clientArray[clientIndex].text[0] == '\0')
    {
        return;
    }

    char formattedMessage[BUF_SIZE_OUTBOUND];
    memset(formattedMessage, 0, sizeof(formattedMessage));

    formatMessage(formattedMessage, sizeof(formattedMessage), clientArray[clientIndex].name, clientArray[clientIndex].text);

    broadcastMessage(formattedMessage, clientArray, -1, maxClient, historyFd);
    writeLog(historyFd, formattedMessage);
}
void disconnectClient(size_t clientIndex, Client *const clientArray, size_t maxClient, int32_t historyFd){
    if(clientArray == NULL)
    {
        return;
    }

    if(clientArray[clientIndex].name[0] != '\0')
    {
    char formattedMessage[BUF_SIZE_OUTBOUND];
    memset(formattedMessage, 0, sizeof(formattedMessage));

    formatMessage(formattedMessage, sizeof(formattedMessage), clientArray[clientIndex].name, "Disconnected from the server");

    broadcastMessage(formattedMessage, clientArray, -1, maxClient, historyFd);
    writeLog(historyFd, formattedMessage);
    }

    if(clientArray[clientIndex].fd != -1)
    {
        close(clientArray[clientIndex].fd);
    }

    memset(&clientArray[clientIndex], 0, sizeof(clientArray[clientIndex]));

    clientArray[clientIndex].fd       = -1;
    clientArray[clientIndex].isActive = false;

    currentClientsCount--;
}

bool validateBuffer(char *buffer, ssize_t bytesRead, int32_t maxSize){
    if(buffer == NULL)
    {
        return false;
    }

    if(bytesRead >= (maxSize - 1))
    {
        const char errBufferOverflow[] = "Security alert: incoming buffer overflow detected\n";
        (void)write(1, errBufferOverflow, sizeof(errBufferOverflow) - 1U);
        return false;
    }

    for(ssize_t i = 0; i < bytesRead; i++){
        uint8_t symbol = (uint8_t)buffer[i];
        if(symbol == '\0')
        {
            const char errNullInjection[] = "Security alert: null byte injection attempt detected\n";
            (void)write(1, errNullInjection, sizeof(errNullInjection) - 1U);
            return false;
        }

        if((symbol < 32) && (symbol != (uint8_t)'\n') && (symbol != (uint8_t)'\r'))
        {
            const char errInvalidChar[] = "Security alert: invalid control characters detected\n";
            (void)write(1, errInvalidChar, sizeof(errInvalidChar) - 1U);
            return false;
        }

        if((symbol == '/') || (symbol == '\\'))
        {
            const char errPathInjection[] = "Security alert: path injection attempt detected\n";
            (void)write(1, errPathInjection, sizeof(errPathInjection) - 1U);
            return false;
        }
    }

    buffer[bytesRead] = '\0';
    return true;
}

void formatMessage(char *destBuffer, int32_t destSize, const char *senderName, const char *text){
    if((destBuffer == NULL) || (senderName == NULL) || (text == NULL))
    {
        return;
    }

    time_t now = time(NULL);
    struct tm timeInfo;

    if(localtime_r(&now, &timeInfo) != NULL)
    {
        char timeBuffer[32];
        memset(timeBuffer, 0, sizeof(timeBuffer));

        size_t len = strftime(timeBuffer, sizeof(timeBuffer), "[%Y:%m:%d:%H:%M:%S]", &timeInfo);

        if(len > 0)
        {
            const char *fragments[] = {timeBuffer, " [", senderName, "] : [", text, "].\n"};

            int32_t cursorPosition = 0;
            size_t numberFragments = sizeof(fragments) / sizeof(fragments[0]);

            for(size_t f = 0; f < numberFragments; f++){
                const char *src = fragments[f];

                for(size_t i = 0; src[i] != '\0' && cursorPosition < (destSize - 1); i++){
                    destBuffer[cursorPosition++] = src[i];
                }
            }
        }
    }
}

void broadcastMessage(const char *readyMessage, Client *const clientArray, int32_t senderFd, uint16_t maxClient, int32_t historyFd){
    size_t messageLength = strlen(readyMessage);

    for(uint16_t i = 0; i < CLIENTS_MAX; i++){
    
    if(clientArray[i].fd == -1 || clientArray[i].fd == senderFd)
    {
        continue;
    }

    for(ssize_t totalSent = 0; totalSent < messageLength; ){

    ssize_t sendResult = send(clientArray[i].fd, readyMessage + totalSent, messageLength - totalSent, MSG_NOSIGNAL);

        if(sendResult < 0)
        {
            if(errno == EPIPE || errno == ECONNRESET)
            {
                disconnectClient(i, clientArray, maxClient, historyFd);
                break;
            }
            else if(errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            else if(errno == EINTR)
            {
                continue;
            }
            else
            {
                disconnectClient(i, clientArray, maxClient, historyFd);
                break;
            }
        }
        
        totalSent += sendResult;

        }
    }
}

void writeLog(int32_t logFd, const char *readyMessage){
    size_t messageLength = strlen(readyMessage);

    for(int32_t attempts = 0; attempts < 3; attempts++){
        
        size_t totalWritten = 0;

        for(; totalWritten < messageLength; ){
            ssize_t writeResult = write(logFd, readyMessage + totalWritten, messageLength - totalWritten);

            if(writeResult == 0)
            {
                break;
            }
            if(writeResult < 0)
            {
                if(errno == EINTR)
                {
                    continue;
                }

                break;
            }

            totalWritten += writeResult;
        }
      
        if(totalWritten == messageLength)
        {
            fsync(logFd);
            return;
        }
   }
}
