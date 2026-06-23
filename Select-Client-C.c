#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <signal.h>

#define SIZE_NAME 32U
#define SIZE_BUFFER 2048U
#define SIZE_FINAL_BUFFER 2144U

#define MIN_PORT 1L
#define MAX_PORT 65536L

#define SIZE_IP 16U

void checkSyscall(int32_t Status, char Message[]);
void handler(int32_t signalNumber);
uint16_t parsePort(const char *port, long minSize, long maxSize);
uint32_t parseIp(const char *Ip);

volatile sig_atomic_t socketStatus = -1;

bool validateBuffer(char *bufferText, ssize_t bytesRead, int32_t maxSize);
void clearStdin(void);
void safeExit(int32_t socketFd);

int main(int argc, char *argv[]){
    if(argc < 3U)
    {
        const char startMessage[] = "Error: Missing required arguments.\nUsage: ./Select-Client-C <Ip> <port>\n";
        (void)write(2, startMessage, sizeof(startMessage) - 1U);
        _exit(EXIT_FAILURE);
    }

    uint32_t cleanIp = parseIp(argv[1]);
    uint16_t port = parsePort(argv[2], MIN_PORT, MAX_PORT);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);

    sa.sa_flags = SA_RESTART;

    sa.sa_handler = handler;
    checkSyscall(sigaction(SIGINT, &sa, NULL), "Syscall: sigaction SIGINT");
    checkSyscall(sigaction(SIGTERM, &sa, NULL), "Syscall: sigaction SIGTERM");

    sa.sa_handler = SIG_IGN;
    checkSyscall(sigaction(SIGPIPE, &sa, NULL), "Sycall: sigaction SIGPIPE");
    checkSyscall(sigaction(SIGHUP, &sa, NULL), "Sycall: sigaction SIGHUP");

    const char EnterName[] = "Enter your name : ";
    (void)write(2, EnterName, sizeof(EnterName) - 1U);

    char bufferName[SIZE_NAME];
    memset(bufferName, 0 , sizeof(bufferName));

    ssize_t bytesRead = read(0, bufferName, sizeof(bufferName) - 1U);

    if(bytesRead <= 0)
    {
        safeExit(socketStatus);
    }

    if(!validateBuffer(bufferName, bytesRead, (int32_t)sizeof(bufferName)))
    {
        const char InvalidName[] = "Error: The name is too long or contains invalid characters.\n";
        (void)write(2, InvalidName, sizeof(InvalidName) - 1U);
        safeExit(socketStatus);
    }
    
    int32_t socketClient = socket(AF_INET, SOCK_STREAM, 0);
    checkSyscall(socketClient, "Syscall: socket client");

    socketStatus = socketClient;

    struct sockaddr_in address;
    
    address.sin_family = AF_INET;
    address.sin_port = (uint16_t)((uint32_t)(port >> 8U) | (uint32_t)(port << 8U));
    address.sin_addr.s_addr = cleanIp;

    int32_t connectClient = connect(socketClient, (struct sockaddr*)&address, sizeof(address));
    checkSyscall(connectClient, "Syscall: connect client");

    size_t nameLength = strlen(bufferName);
    (void)send(socketClient, bufferName, nameLength, MSG_NOSIGNAL);

    int32_t invalidInputCounter = 0;

    fd_set readFds;

    char finalText[SIZE_FINAL_BUFFER];
    int32_t totalBytes = 0;

    for(;;){
        FD_ZERO(&readFds);
        FD_SET(0, &readFds);
        FD_SET(socketClient, &readFds);

        int selectResult = select(socketClient + 1, &readFds, NULL, NULL, NULL);
        if(selectResult < 0)
        {
            if(errno == EINTR)
            {
                continue;
            }
            checkSyscall(selectResult, "Syscall: select");
        }

        if(FD_ISSET(0, &readFds)){
            char bufferText[SIZE_BUFFER];
            memset(bufferText, 0, sizeof(bufferText));

            ssize_t bytesRead = read(0, bufferText, sizeof(bufferText) - 1U);

            if(bytesRead <= 0)
            {
                const char connectClose[] = "Connection closed by user. Goodbye!";
                (void)write(2, connectClose, sizeof(connectClose) - 1U);
                safeExit(socketClient);
            }

            if((bytesRead == 1) && (bufferText[0] == '\n'))
            {
                continue;
            }

            if(!validateBuffer(bufferText, bytesRead, SIZE_BUFFER))
            {
                invalidInputCounter++;
                clearStdin();

                if(invalidInputCounter >= 3U)
                {
                    const char KickMessage[] = "Error: Too many invalid attempts. Disconnecting for safety.\n";
                    (void)write(2, KickMessage, sizeof(KickMessage) - 1U);
                    safeExit(socketClient);
                }
                else
                {
                    const char WarnMessage[] = "Warning: Invalid input or text too long! (Attempts left:";
                    (void)write(2, WarnMessage, sizeof(WarnMessage) - 1U);

                    char attemptsLeftChar = (char)((uint8_t)'3' - invalidInputCounter);
                    (void)write(2, &attemptsLeftChar, 1);

                    const char WarnEnd[] = ")\n";
                    (void)write(2, WarnEnd, sizeof(WarnEnd) - 1U);
                    continue;
                }
            }

            ssize_t messageLength = (ssize_t)strlen(bufferText);

            int32_t sendAttempts = 0;
            for(ssize_t totalSent = 0; totalSent < messageLength; ){
                ssize_t bytesSent = send(socketClient, bufferText + totalSent, (size_t)(messageLength - totalSent), MSG_NOSIGNAL); 

                if(bytesSent < 0)
                {
                    if(errno == EINTR)
                    {
                        continue;
                    }
                    if((errno == EAGAIN) || (errno == EWOULDBLOCK))
                    {
                        sendAttempts++;

                        if(sendAttempts > 100)
                        {
                            const char NetworkDead[] = "Error: Network buffer blocked for too long.\n";
                            (void)write(2, NetworkDead, sizeof(NetworkDead) - 1U);
                            safeExit(socketClient);
                        }
                        (void)usleep(500);
                        continue;
                    }

                    safeExit(socketClient);
                }
                else if(bytesSent == 0)
                {
                    safeExit(socketClient);
                }
                else
                {
                    totalSent += bytesSent;
                    sendAttempts = 0;
                }
            }

        }
    
    
        if(FD_ISSET(socketClient, &readFds))
        {
            int32_t spaceLeft = SIZE_FINAL_BUFFER - totalBytes;

            if(spaceLeft <= 0)
            {
                const char OverflowMsg[] = "Error: Local buffer overflow protection triggered.\n";
                (void)write(2, OverflowMsg, sizeof(OverflowMsg) - 1U);
                safeExit(socketClient);
            }

            ssize_t bytesReadNet = read(socketClient, &finalText[totalBytes], (size_t)spaceLeft);

            if(bytesReadNet < 0)
            {
                if(errno == EINTR) { continue; }
                safeExit(socketClient);
            }
            else if(bytesReadNet == 0)
            {
                const char ServerDeadMsg[] = "Error: Remote server closed the connection.\n";
                (void)write(2, ServerDeadMsg, sizeof(ServerDeadMsg) - 1U);
                safeExit(socketClient);
            }
            else
            {
                totalBytes += (int32_t)bytesReadNet;

                int32_t msgStart = 0;
                int32_t i = 0;

                for(i = 0; i < totalBytes; i++){
                    if(finalText[i] == '\n')
                    {
                    int32_t messageLength = (i - msgStart) + 1;
                    (void)write(2, &finalText[msgStart], (size_t)messageLength);
                    msgStart = i + 1;
                }
            }

                if(msgStart < totalBytes)
                {
                    int32_t leftoverBytes = totalBytes - msgStart;

                    for(i = 0; i < leftoverBytes; i++){
                        finalText[i] = finalText[msgStart + i];
                    }
                    totalBytes = leftoverBytes;
                }
                else
                {
                    totalBytes = 0;
                }
            }
        }
    }
    return 0;
}

void checkSyscall(int32_t Status, char Message[]){
    if(Status >= 0)
    {
        return;
    }

    if(Message == NULL)
    {
        Message = "Fatal: checkSyscall caught an unnamed system error";
    }

    perror(Message);
    _exit(EXIT_FAILURE);
}

void handler(int32_t signalNumber){
    static const char MessageInt[] = "\nSignal: SIGINT received (Ctrl+C). Emergency termination...\n";
    static const char MessageTerm[] = "\nSignal: SIGTERM (Termination) received. Shutting down...\n";
    static const char MessageEarly[] = "\nSignal: The client has been stopped before the network was initialised.\n";

    const char *selectedMessage = MessageEarly;
    size_t messageLength = sizeof(MessageEarly) - 1U;

    if(socketStatus >= 0)
    {
        if(signalNumber == SIGINT)
        {
            selectedMessage = MessageInt;
            messageLength = sizeof(MessageInt) - 1U;
        }
        else if(signalNumber == SIGTERM)
        {
            selectedMessage = MessageTerm;
            messageLength = sizeof(MessageTerm) - 1U;
        }
    }

    (void)write(2, selectedMessage, messageLength);
    _exit(EXIT_FAILURE);
}

uint16_t parsePort(const char *port, long minSize, long maxSize){
    char *EndPtrPort;
    long parseVal = strtol(port, &EndPtrPort, 10);

    if(port == EndPtrPort)
    {
        const char ErrorStart[] = "Error: Port must begin with a valid digit!\n";
        (void)write(2, ErrorStart, sizeof(ErrorStart) - 1U);
        _exit(EXIT_FAILURE);
    } 

    if(*EndPtrPort != '\0')
    {
        const char ErrorMiddle[] = "Error: Port contains invalid trailing characters!\n";
        (void)write(2, ErrorMiddle, sizeof(ErrorMiddle) - 1U);
        _exit(EXIT_FAILURE);
    }

    if((parseVal < minSize) || (parseVal > maxSize))
    {
        const char PortLimit[] = "Error: Port number is out of allowed range!\n";
        (void)write(2, PortLimit, sizeof(PortLimit) - 1U);
        _exit(EXIT_FAILURE);
    }

    return (uint16_t)parseVal;
}

uint32_t parseIp(const char *Ip){
    if(Ip == NULL)
    {
        const char NullPointerError[] = "Error: IP address string is NULL.\n";
        (void)write(2, NullPointerError, sizeof(NullPointerError) - 1U);
        _exit(EXIT_FAILURE);
    }

    uint32_t octets[4] = {0U, 0U, 0U, 0U};
    uint32_t currentOctet = 0U;
    uint32_t octetIndex = 0U;
    uint32_t numberDigits = 0U;

    for(size_t i = 0; Ip[i] != '\0'; i++){
        char symbol = Ip[i];

        if((symbol >= '0') && (symbol <= '9'))
        {
            numberDigits++;
            if(numberDigits > 3U)
            {
                const char ToolLongOctet[] = "Error: Invalid IP address format. Octet cannot exceed 3 digits.\n";
                (void)write(2, ToolLongOctet, sizeof(ToolLongOctet) - 1U);
                _exit(EXIT_FAILURE);
            }

            currentOctet = (currentOctet * 10U) + (uint32_t)(symbol - '0');

            if(currentOctet > 255U)
            {
                const char ValueTooBig[] = "Error: Invalid IP address value. Octet cannot be greater than 255.\n";
                (void)write(2, ValueTooBig, sizeof(ValueTooBig) - 1U);
                _exit(EXIT_FAILURE);
            }
        }
        else if(symbol == '.')
        {
            if(numberDigits == 0U)
            {
                const char EmptyOctet[] = "Error: Invalid IP address format. Empty octet.\n";
                (void)write(2, EmptyOctet, sizeof(EmptyOctet) - 1U);
                _exit(EXIT_FAILURE);
            }

            if(octetIndex >= 3U)
            {
                const char TooManyDots[] = "Error: Invalid IP address format. Too many dots.\n";
                (void)write(2, TooManyDots, sizeof(TooManyDots) - 1U);
                _exit(EXIT_FAILURE);
            }

            octets[octetIndex] = currentOctet;
            octetIndex++;
            currentOctet = 0U;
            numberDigits = 0U;
        }
        else
        {
            const char InvalidChar[] = "Error: Invalid character in IP address.\n";
            (void)write(2, InvalidChar, sizeof(InvalidChar) - 1U);
            _exit(EXIT_FAILURE);
        }
    }

    if(numberDigits == 0U)
    {
        const char TrailingDot[] = "Error: Invalid IP address format. Extra trailing dot.\n";
        (void)write(2, TrailingDot, sizeof(TrailingDot) - 1U);
        _exit(EXIT_FAILURE);
    }

    if(octetIndex != 3U)
    {
        const char WrongDotCount[] = "Error: Invalid IP address format. Must have exactly 3 dots.\n";
        (void)write(2, WrongDotCount, sizeof(WrongDotCount) - 1U);
        _exit(EXIT_FAILURE);
   }

    octets[octetIndex] = currentOctet;

    uint32_t finalIp = octets[0] | (octets[1] << 8U) | (octets[2] << 16U) | (octets[3] << 24U);

    return finalIp;
}

bool validateBuffer(char *bufferText, ssize_t bytesRead, int32_t maxSize){
    if(bufferText == NULL)
    {
        return false;
    }

    if(bytesRead >= maxSize)
    {
        return false;
    }

    for(size_t i = 0; i < (size_t)bytesRead; i++){
        uint8_t symbol = (uint8_t)bufferText[i];
        
        if(symbol == '\0')
        {
            return false;
        }

        if((symbol < 32) && (symbol != (uint8_t)'\n') && (symbol != (uint8_t)'\r'))
        {
            return false;
        }

        if((symbol == '/') || (symbol == '\\'))
        {
           return false; 
        }
    }

    bufferText[bytesRead] = '\0';
    return true;
}

void clearStdin(void){
    char dump = '\0';

    for(ssize_t result = read(0, &dump, 1); result > 0; result = read(0, &dump, 1)){
        if((dump == '\n') || (dump == '\r'))
        {
            break;
        }
    }
}

void safeExit(int32_t socketFd){
    if(socketFd >= 0)
    {
        (void)close(socketFd);
    }

    _exit(EXIT_FAILURE);
}

