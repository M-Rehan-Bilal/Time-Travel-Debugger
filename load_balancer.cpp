// ======================= TIME-TRAVEL DEBUGGER - LOAD BALANCER TEMPLATE =======================

// What load balancer does:
//   1. Many clients can connect at the same time. Each one sends one string (the source code).
//   2. Clients wait in a queue (first come, first served).
//   3. For each client, in order: make sure the server is running (start it with system() if not),
//      pass the string to the server, pass the server's reply back to the client.
//
// It does not read or understand the messages. It only moves bytes.
// The only messages it writes itself are errors ("server busy", "server failed to start").
//
// Two threads:
//   acceptor thread (acceptLoop) -> takes new clients and puts them in the queue. It never waits for the server.
//   main thread (dispatcherLoop) -> takes ONE client out of the queue and serves it completely, then the next.
//
// How the server is started:
//   system("./server 5001 &");
//   The "&" is required. Without it, system() waits until the server exits and the load balancer freezes.
//
//
// What the server program must do (the load balancer depends on this):
//   - it takes its port as the first argument, for example: ./server 5001
//   - it reads the string until the connection's write side is closed (recv returns 0)
//   - it sends its reply as [status: 1 byte][payload], then closes the connection

#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include "helper.h"
#include <cerrno>
#include <chrono>
#include <ctime>
#include <fcntl.h>
using namespace std;

// ---- Constants ----
const char *SERVER_PATH = "./server";     // the program we start when the server is down
const int LISTEN_BACKLOG = 16;            // connections the OS holds until accept() takes them
const int MAX_PENDING_CLIENTS = 8;        // clients allowed to wait in our queue. More than this get "server busy".
const int STARTUP_MAX_ATTEMPTS = 50;      // after starting the server, how many times we try to connect
const int STARTUP_POLL_INTERVAL_MS = 100; // wait between those tries (50 x 100 ms = 5 seconds in total)
int lbPort = 5000;
string serverHost = "127.0.0.1";
int serverPort = 5001;

// ---- Queue: here it holds client sockets (int). ----
// Implement it yourself, do not use std::queue.
template <typename T>
class Queue
{
    struct Node
    {
        T data;
        Node *next;
        Node(const T &val) : data(val), next(nullptr) {}
    };
    Node *front, *rear;
    int count;

public:
    // Implement these functions:
    Queue()
    {
        front = rear = nullptr;
        count = 0;
    }
    ~Queue()
    {
        while (front != nullptr)
        {
            Node *temp = front;
            front = front->next;
            delete temp;
        }
    }
    void enqueue(const T &val)
    {
        Node *newNode = new Node(val);
        if (count == 0)
        {
            front = rear = newNode;
        }
        else
        {
            rear->next = newNode;
            rear = rear->next;
        }
        count++;
    }
    T dequeue()
    {
        if (count == 0)
            throw underflow_error("No Element to Dequeue");
        Node *temp = front;
        front = front->next;
        count--;
        if (count == 0)
        {
            front = rear = nullptr;
        }
        T value = temp->data;
        delete temp;
        return value;
    }
    bool isEmpty()
    {
        return count == 0;
    }
    int size()
    {
        return count;
    }
};

struct ClientInfo
{
    int sock;         // the connected socket
    string address;   // the client's IP as text, e.g. "127.0.0.1"
    int port;         // the client's port
    time_t arrivedAt; // when accept() returned
};

// The queue is used by two threads, so every use of it must be inside queueMutex.
Queue<ClientInfo> pendingClients;
mutex queueMutex;
volatile sig_atomic_t flag = 1;

// ---- Basic socket helpers ----
bool sendAll(int sock, const char *buf, size_t n)
{
    // Reply format: [status: 1 byte][sending_material][checksum: 4 bytes].
    // send() may send fewer bytes than asked for, so keep calling it until all n bytes are sent.
    size_t sent = 0;
    while (sent < n)
    {
        ssize_t s = send(sock, buf + sent, n - sent, MSG_NOSIGNAL);
        if (s < 0 && errno == EINTR)
        {
            continue;
        }
        if (s <= 0)
        {
            return false;
        }
        sent += s;
    }
    return true;
}
bool sendError(int sock, const string &message)
{
    // Reply format: [status: 1 byte][payload until close][checksum: 4 bytes].
    // For an error: send one byte with value 1, then the message text.
    char status = 1;
    // 1. status
    if (!sendAll(sock, &status, 1))
    {
        return false;
    }
    Crc32 checksum;
    checksum.update(&status, 1);
    if (message.size() > 0)
    {
        // 2. message
        if (!sendAll(sock, message.data(), message.size()))
        {
            return false;
        }
        checksum.update(message.data(), message.size());
    }
    uint32_t cs = checksum.finish();
    // 3. checksum
    return sendAll(sock, (const char *)&cs, sizeof(cs));
}
int createListenSocket(int port)
{
    // Create a TCP socket with SOCK_CLOEXEC(flag).
    // set SO_REUSEADDR so we can restart the load balancer right away.
    // bind() to the port on any address, then listen(LISTEN_BACKLOG).
    // If bind or listen fails, print the error and exit, because without this socket we cannot work.
    int welcomeSocket = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (welcomeSocket == -1)
    {
        perror("socket");
        exit(1);
    }

    int allowed = 1;
    if (setsockopt(welcomeSocket, SOL_SOCKET, SO_REUSEADDR, &allowed, sizeof(allowed)) == -1)
    {
        perror("setsockopt");
        close(welcomeSocket);
        exit(1);
    }

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    if (bind(welcomeSocket, (sockaddr *)&address, sizeof(address)) == -1)
    {
        perror("bind");
        close(welcomeSocket);
        exit(1);
    }
    if (listen(welcomeSocket, LISTEN_BACKLOG) == -1)
    {
        perror("listen");
        close(welcomeSocket);
        exit(1);
    }
    return welcomeSocket;
}
int connectToServer(const char *host, int port)
{
    // Create a TCP socket (SOCK_CLOEXEC) and connect it to host:port.
    // Return the socket, or -1 if the connection fails
    int serverSocket = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (serverSocket == -1)
    {
        return -1;
    }

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &address.sin_addr) != 1)
    {
        close(serverSocket);
        return -1;
    }
    if (connect(serverSocket, (sockaddr *)&address, sizeof(address)) == -1)
    {
        close(serverSocket);
        return -1;
    }
    return serverSocket;
}

// =============================================================================================
// PART 1: WAKING UP THE SERVER IF IT IS DOWN
// =============================================================================================

bool startServer(int port)
{
    // Build the command text: SERVER_PATH + " " + port as text + "&"
    //   Example: "./server 5001 &"
    // Call system() with it.
    // Return true if system() returned 0, otherwise false.
    string command = string(SERVER_PATH) + " " + to_string(port) + " &";
    if (system(command.c_str()) == 0)
    {
        return true;
    }
    return false;
}
int connectToServerWithWakeup()
{
    // 1. Try connectToServer(). If it works, return that socket. The server is alive.
    // 2. Otherwise call startServer(). If it fails, return -1.
    // 3. Try connectToServer() up to STARTUP_MAX_ATTEMPTS times
    // 4. Return the first working socket, or -1 if the server never came up.
    int serverSocket = connectToServer(serverHost.c_str(), serverPort);
    if (serverSocket != -1) // server is already running
    {
        return serverSocket;
    }
    if (!startServer(serverPort))
    {
        return -1;
    }
    for (int i = 0; i < STARTUP_MAX_ATTEMPTS; i++)
    {
        this_thread::sleep_for(chrono::milliseconds(STARTUP_POLL_INTERVAL_MS));
        serverSocket = connectToServer(serverHost.c_str(), serverPort);
        if (serverSocket != -1)
        {
            return serverSocket;
        }
    }
    return -1;
}

// =============================================================================================
// PART 2: PASSING THE STRING AND THE REPLY (one client, called by the main thread only)
// =============================================================================================

void relayOneClient(ClientInfo &c)
{
    // 1. serverSock = connectToServerWithWakeup().
    //    If it is -1: sendError "backend server failed to start", close clientSock, return.
    // 3. Client -> server: repeat recv() from clientSock into a buffer of IO_BUFFER_SIZE and sendAll() it to serverSock.
    //    Stop when recv() returns 0 (the client finished sending) or fails.
    // 4. Call shutdown(serverSock, SHUT_WR), so the server knows the string is complete.
    // 5. Server -> client: repeat recv() from serverSock and sendAll() to clientSock.
    //    Stop when recv() returns 0 (the server sent its whole reply and closed) or fails.
    // 6. Close both sockets.
    int serverSocket = connectToServerWithWakeup();
    if (serverSocket < 0)
    {
        sendError(c.sock, "backend server failed to start");
        close(c.sock);
        return;
    }

    char buffer[IO_BUFFER_SIZE];
    char lastFour[4];
    int lastCount = 0;
    Crc32 checksum;
    bool valid = true;

    // Forward client request to server
    while (true)
    {
        int bytes = recv(c.sock, buffer, IO_BUFFER_SIZE, 0);
        if (bytes <= 0)
            break;
        char combined[IO_BUFFER_SIZE + 4];
        int total = 0;
        for (int i = 0; i < lastCount; i++)
            combined[total++] = lastFour[i];
        for (int i = 0; i < bytes; i++)
            combined[total++] = buffer[i];
        if (total > 4)
        {
            int dataCount = total - 4;
            checksum.update(combined, dataCount);
            if (!sendAll(serverSocket, combined, dataCount))
            {
                valid = false;
                break;
            }
            lastCount = 4;
            for (int i = 0; i < 4; i++)
                lastFour[i] = combined[dataCount + i];
        }
        else
        {
            lastCount = total;
            for (int i = 0; i < total; i++)
                lastFour[i] = combined[i];
        }
    }
    uint32_t cs = checksum.finish();

    // Check the request checksum
    if (lastCount != 4)
        valid = false;
    else
    {
        uint32_t receivedChecksum;
        memcpy(&receivedChecksum, lastFour, 4);
        if (receivedChecksum != cs)
            valid = false;
    }

    if (valid)
        sendAll(serverSocket, lastFour, 4); // forward the trailer only when the upload checksum is good

    shutdown(serverSocket, SHUT_WR);

    if (!valid)
    {
        sendError(c.sock, "checksum mismatch");
        close(serverSocket);
        close(c.sock);
        return;
    }

    // Forward server response to client
    while (true)
    {
        int bytes = recv(serverSocket, buffer, IO_BUFFER_SIZE, 0);
        if (bytes <= 0)
            break;
        if (!sendAll(c.sock, buffer, bytes))
            break;
    }

    close(serverSocket);
    close(c.sock);
}

// =============================================================================================
// PART 3: THE TWO THREADS
// =============================================================================================

void acceptLoop(int listenSock)
{
    // Repeat: accept() a new client.
    //  If pendingClients.size() is already MAX_PENDING_CLIENTS: unlock, sendError "server busy, try again later",
    // else
    // Lock queueMutex.
    //   close the client, and go on.
    //   Otherwise enqueue the client socket.
    // Never wait for the server here, so new clients are always accepted quickly.
    // If accept() fails, print the error and keep going (do not end the thread).
    while (flag)
    {
        int clientSocket = accept(listenSock, nullptr, nullptr);

        if (clientSocket == -1)
        {
            if (errno == EINTR)
                continue;

            if (flag)
                perror("accept");

            continue;
        }

        int flags = fcntl(clientSocket, F_GETFD);

        if (flags == -1 ||
            fcntl(clientSocket, F_SETFD, flags | FD_CLOEXEC) == -1)
        {
            perror("fcntl");
            close(clientSocket);
            continue;
        }

        ClientInfo client = {};
        client.sock = clientSocket;
        client.arrivedAt = time(nullptr);

        queueMutex.lock();

        if (pendingClients.size() >= MAX_PENDING_CLIENTS)
        {
            queueMutex.unlock();

            sendError(clientSocket, "server busy, try again later");
            close(clientSocket);
        }
        else
        {
            pendingClients.enqueue(client);
            queueMutex.unlock();
        }
    }
}
void dispatcherLoop()
{
    // This runs in the main thread and never ends.
    // Repeat:
    //   Lock queueMutex, take ONE client out of the queue if there is one, unlock.
    //   If we got a client: call relayOneClient(). The next client is taken out of the queue only after this one is finished.
    while (flag)
    {
        queueMutex.lock();
        ClientInfo client = {};
        bool clientExists = false;
        if (!pendingClients.isEmpty())
        {
            client = pendingClients.dequeue();
            clientExists = true;
        }
        queueMutex.unlock();
        if (clientExists)
        {
            relayOneClient(client);
        }
        else
        {
            this_thread::sleep_for(chrono::milliseconds(10));
        }
    }
}

void handler(int sg)
{
    (void)sg;
    flag = 0;
}
int main(int argc, char *argv[])
{

    if (argc != 3)
    {
        cout << "Usage: ./load_balancer <server_ip> <server_port>" << endl;
        return 1;
    }

    serverHost = argv[1];
    serverPort = convertPortIntoInteger(argv[2]);

    if (serverPort == -1)
    {
        cout << "Invalid Server Port" << endl;
        return 1;
    }

    signal(SIGINT, handler); // whenever ctrl+c will be pressed to exit the program the flag wll be set so insead of using infinite loop you can loop on flag to safely close things and exit
    // 2. listenSock = createListenSocket(LB_PORT).
    // 3. Start a thread that runs acceptLoop(listenSock) and detach it.
    // 4. Call dispatcherLoop().

    signal(SIGPIPE, SIG_IGN);

    int listenSocket = createListenSocket(lbPort);
    thread acceptor(acceptLoop, listenSocket);
    acceptor.detach();

    dispatcherLoop();

    close(listenSocket);

    return 0;
}