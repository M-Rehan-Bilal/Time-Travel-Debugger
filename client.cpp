// ======================= TIME-TRAVEL DEBUGGER - CLIENT TEMPLATE =======================

// The client never talks to the server directly. It only talks to the load balancer.
// The load balancer wakes the server up if it is down, so the client does not care.
// The first "start" may just take a bit longer.
//
// The network is used ONLY by "start". After the .tdbg file is downloaded,
// next / prv / jmp read that file on the local disk. They never contact the load balancer.
//
// How the pieces fit together:
//   main loop -> readCommand -> runCommand
//   start -> cmdStart()  -> requestDebugFile() (Part A) -> openTdbg() (Part B) -> shows step 1
//   next, prv, jmp -> moveBy() (Part D) -> readSnapshotAt() (Part B) -> printSnapshot() (Part E)

#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h> // 1
#include <arpa/inet.h>  // 2
#include <cerrno>
#include <fcntl.h>
#include "helper.h"
using namespace std;

// ---- Constants ----
const char *PROGRAM_NAME = "ttdb";
const char *PROMPT = "ttdb> ";
const char *TDBG_FILE = "ttdb_last.tdbg"; // where the downloaded .tdbg is saved
const int MAX_JUMP = 1000000000;          // biggest n accepted by jmp
string LB_HOST = "127.0.0.1";
int LB_PORT = 5000;

// ---- Structs only the client needs ----
struct TdbgReader
{
    ssize_t file_index;
    TTDBHeader header;
    vector<int64_t> index;
};
struct DebugSession
{
    int active_flag;   // false until a "start" has worked
    TdbgReader reader; // the .tdbg file stays open for the whole session
    int64_t currentStep;
};

// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================

bool recvAll(int sock, char *buf, size_t n)
{
    // recv() can return fewer bytes than asked for, so keep calling it until exactly n bytes are read.
    // Return false if the connection closes or fails before n bytes arrive.
    size_t received = 0;
    while (received < n)
    {
        ssize_t r = recv(sock, buf + received, n - received, 0);
        if (r < 0 && errno == EINTR)
        {
            continue;
        }
        if (r <= 0)
        {
            return false;
        }
        received += r;
    }
    return true;
}

bool sendAll(int sock, const char *buf, size_t n)
{
    // send() can send fewer bytes than asked for, so keep calling it until all n bytes are sent.
    // Use the flag MSG_NOSIGNAL. Return false if the connection fails.
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
int connectToHost(const char *host, int port)
{
    // Create a TCP socket and connect it to host:port.
    // Return the socket, or -1 if the connection fails.
    int clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (clientSocket == -1)
        return -1;
    sockaddr_in serverAddress = {};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &serverAddress.sin_addr) != 1)
    {
        close(clientSocket);
        return -1;
    }
    if (connect(clientSocket, (sockaddr *)&serverAddress, sizeof(serverAddress)) == -1)
    {
        close(clientSocket);
        return -1;
    }
    return clientSocket;
}
bool uploadSource(int sock, const char *sourcePath)
{
    // have to do checksum and then send
    // When the file is finished, call shutdown(sock, SHUT_WR). This is how the server knows the string is complete.
    // Return false if the file cannot be opened or any send fails.
    FILE *file = fopen(sourcePath, "rb");
    if (file == nullptr)
        return false;

    char status = 0;
    int checksum = checksumInit();
    checksum = checksumUpdate(checksum, &status, 1);
    if (!sendAll(sock, &status, 1))
    {
        fclose(file);
        return false;
    }

    char buffer[IO_BUFFER_SIZE];
    size_t amount = fread(buffer, 1, IO_BUFFER_SIZE, file);
    while (amount > 0)
    {
        checksum = checksumUpdate(checksum, buffer, amount);
        if (!sendAll(sock, buffer, amount))
        {
            fclose(file);
            return false;
        }
        amount = fread(buffer, 1, IO_BUFFER_SIZE, file);
    }

    if (ferror(file))
    {
        fclose(file);
        return false;
    }

    fclose(file);

    if (!sendAll(sock, (const char *)&checksum, sizeof(checksum)))
    {
        return false;
    }

    if (shutdown(sock, SHUT_WR) == -1)
    {
        return false;
    }

    return true;
}
bool receiveResponse(int sock, const char *outTdbgPath, string &outError)
{
    // Reply on the wire: [status][payload][CRC-32]. The CRC covers the status byte too.
    // Feed every received byte (status included) to a TrailerChecker.
    // Status 1: collect the payload into outError. Status 0: write the payload to the file.
    // At the end call verify(). If it fails, delete the file, set outError to
    // "checksum mismatch, response corrupted" and return false.
    char status;
    if (!recvAll(sock, &status, 1))
    {
        outError = "Connection closed before reply";
        return false;
    }
    if (status != 0 && status != 1)
    {
        outError = "Unknown reply status";
        return false;
    }
    string tempPath = string(outTdbgPath) + ".part";
    FILE *file = nullptr;
    if (status == 0)
    {
        file = fopen(tempPath.c_str(), "wb");

        if (file == nullptr)
        {
            outError = "Could not create downloaded .tdbg file";
            return false;
        }
    }

    int checksum = checksumInit();
    checksum = checksumUpdate(checksum, &status, 1);
    char buffer[IO_BUFFER_SIZE];
    char lastFour[4];
    int count = 0;
    bool failed = false;

    while (true)
    {
        int bytes = recv(sock, buffer, IO_BUFFER_SIZE, 0);
        if (bytes == 0)
            break;
        if (bytes < 0)
        {
            failed = true;
            break;
        }
        for (int i = 0; i < bytes; i++)
        {
            if (count < 4)
            {
                lastFour[count] = buffer[i];
                count++;
            }
            else
            {
                checksum = checksumUpdate(checksum, lastFour, 1);
                if (status == 0)
                {
                    if (fwrite(lastFour, 1, 1, file) != 1)
                        failed = true;
                }
                else
                {
                    outError += lastFour[0];
                }
                for (int j = 0; j < 3; j++)
                    lastFour[j] = lastFour[j + 1];

                lastFour[3] = buffer[i];
            }
            if (failed)
                break;
        }
        if (failed)
            break;
    }

    if (file != nullptr)
        fclose(file);
    if (failed || count != 4)
    {
        remove(tempPath.c_str());
        outError = "Connection error or incomplete response";
        return false;
    }
    int receivedChecksum;
    memcpy(&receivedChecksum, lastFour, 4);
    if (receivedChecksum != checksum)
    {
        remove(tempPath.c_str());
        outError = "checksum mismatch, response corrupted";
        return false;
    }
    if (status == 1)
        return false;
    if (rename(tempPath.c_str(), outTdbgPath) != 0)
    {
        remove(tempPath.c_str());
        outError = "Could not save downloaded .tdbg file";
        return false;
    }
    return true;
}

// =============================================================================================
// PART B: READING THE .tdbg FILE
// File layout (written by Phase 1):
//   header | snapshot 0 | snapshot 1 | ... | index (one 8 byte offset per snapshot)
// The index lets us jump straight to any step without reading the steps before it.
// =============================================================================================
bool openTdbg(const char *path, TdbgReader &r)
{
    r.file_index = open(path, O_RDONLY);
    if (r.file_index == -1)
        return false;

    bool ok =
        pread(r.file_index, r.header.magic, 4, 0) == 4 &&
        pread(r.file_index, &r.header.version, 4, 4) == 4 &&
        pread(r.file_index, &r.header.stepCount, 4, 8) == 4 &&
        pread(r.file_index, &r.header.indexOffset, 8, 12) == 8;

    if (ok)
        ok = memcmp(r.header.magic, "TTDB", 4) == 0 && r.header.stepCount > 0;

    if (ok)
    {
        size_t indexBytes = (size_t)r.header.stepCount * 8;
        r.index.resize(r.header.stepCount);
        ok = pread(r.file_index, r.index.data(), indexBytes, r.header.indexOffset) == (ssize_t)indexBytes;
    }

    if (!ok)
    {
        close(r.file_index);
        r.file_index = -1;
        r.index.clear();
        return false;
    }
    return true;
}
void closeTdbg(TdbgReader &r)
{
    // Free r.index and close the file.
    if (r.file_index != -1)
    {
        close(r.file_index);
    }
    r.file_index = -1;
    r.index.clear();
    r.index.shrink_to_fit();
}



// <.......................................................................................>

// Reads n bytes at pos and moves pos forward.
static bool readAt(int fd, int64_t &pos, void *buf, size_t n)
{
    if (pread(fd, buf, n, pos) != (ssize_t)n)
        return false;
    pos += (int64_t)n;
    return true;
}

// A string is stored as [int32 length][bytes].
static bool readStr(int fd, int64_t &pos, string &s)
{
    int32_t len = 0;
    if (!readAt(fd, pos, &len, sizeof(len)) || len < 0 || len > IO_BUFFER_SIZE)
        return false;
    s.resize(len);
    return len == 0 || readAt(fd, pos, &s[0], len);
}

// Reads the snapshot for one step.
// Layout (written by the server's writeTdbg):
// [int32 stackDepth] then per frame: [string func_name][int32 argc]
//   argc x ([string name][int32 value]) [int32 returnLine][int32 localCount]
//   localCount x ([string name][int32 value])
Snapshot *readSnapshotAt(TdbgReader &r, int32_t step)
{
    if (r.file_index == -1 || step < 0 || step >= r.header.stepCount)
        return nullptr;

    int fd = r.file_index;
    int64_t pos = r.index[step];
    Snapshot *s = new Snapshot;

    bool ok = readAt(fd, pos, &s->stackDepth, sizeof(int32_t)) &&
              s->stackDepth >= 0 && s->stackDepth <= MAX_STACK_DEPTH;

    for (int i = 0; ok && i < s->stackDepth; i++)
    {
        Frame &f = s->callStack[i];
        ok = readStr(fd, pos, f.func_name) &&
             readAt(fd, pos, &f.argc, sizeof(int32_t)) &&
             f.argc >= 0 && f.argc <= MAX_VARS_PER_FRAME;

        for (int j = 0; ok && j < f.argc; j++)
            ok = readStr(fd, pos, f.argv[j].name) &&
                 readAt(fd, pos, &f.argv[j].value, sizeof(int32_t));

        ok = ok && readAt(fd, pos, &f.returnLine, sizeof(int32_t)) &&
             readAt(fd, pos, &f.localCount, sizeof(int32_t)) &&
             f.localCount >= 0 && f.localCount <= MAX_VARS_PER_FRAME;

        for (int j = 0; ok && j < f.localCount; j++)
            ok = readStr(fd, pos, f.locals[j].name) &&
                 readAt(fd, pos, &f.locals[j].value, sizeof(int32_t));
    }

    if (!ok)
    {
        delete s;
        return nullptr;
    }
    return s;
}

// <.......................................................................................>



// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================

void initSession(DebugSession &s)
{
    s.active_flag = 0;        // no debug file yet, so no session is active
    s.reader.file_index = -1; // -1 means no file is open
    s.reader.header = {};     // zero the header so no garbage is left in it
    s.currentStep = 0;
}
void closeSession(DebugSession &s)
{
    // If active, call closeTdbg(s.reader). Then set active = false.
    // Called when the user types quit and when the input ends.
    if (s.active_flag != 0)
    {
        closeTdbg(s.reader);
    }
    s.active_flag = 0;
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s); // defined in Part E

bool showCurrent(DebugSession &s)
{
    // Read the snapshot at s.currentStep, call printSnapshot, delete the snapshot.
    // If readSnapshotAt returns nullptr, print "damaged .tdbg file" and return false.
    if (s.active_flag == 0)
    {
        return false;
    }
    Snapshot *snapshot = readSnapshotAt(s.reader, s.currentStep);
    if (snapshot == nullptr)
    {
        cout << "damaged .tdbg file" << endl;
        return false;
    }
    printSnapshot(s.currentStep, s.reader.header.stepCount, *snapshot);
    delete snapshot;
    return true;
}
bool moveBy(DebugSession &s, int32_t delta)
{
    if (s.active_flag == 0)
    {
        cout << "No active debugging session" << endl;
        return false;
    }
    int64_t target = s.currentStep + (int64_t)delta;
    if (target < 0)
        target = 0;
    if (target >= s.reader.header.stepCount)
        target = s.reader.header.stepCount - 1;
    s.currentStep = target;
    return showCurrent(s);
}
bool requestDebugFile(const string &sourcePath, string &outError)
{
    int sock = connectToHost(LB_HOST.c_str(), LB_PORT);
    if (sock == -1)
    {
        outError = "Cannot connect to the load balancer";
        return false;
    }
    if (!uploadSource(sock, sourcePath.c_str()))
    {
        close(sock);
        outError = "Could not upload source file";
        return false;
    }
    if (!receiveResponse(sock, TDBG_FILE, outError))
    {
        close(sock);
        return false;
    }
    close(sock);
    return true;
}
bool cmdStart(DebugSession &s, const string &sourcePath)
{
    // 1. requestDebugFile with host and port, saving to TDBG_FILE.
    // 2. If it fails, print the error text and return false. Keep the old session as it was.
    // 3. Open the new file with openTdbg into a NEW local TdbgReader.
    //    If that fails, print "server sent a damaged .tdbg file", keep the old session, return false.
    // 4. Only now close the old reader (if s.active), then put the new reader into s,
    //    set s.currentStep = 0 and s.active = true.
    // 5. Show step 1 with showCurrent(s).
    string error;
    if (!requestDebugFile(sourcePath, error))
    {
        cout << error << endl;
        return false;
    }
    TdbgReader newReader = {};
    newReader.file_index = -1;
    if (!openTdbg(TDBG_FILE, newReader))
    {
        cout << "Server sent a damaged .tdbg file" << endl;
        return false;
    }
    if (s.active_flag)
    {
        closeTdbg(s.reader);
    }
    s.reader = newReader;
    s.currentStep = 0;
    s.active_flag = 1;
    return showCurrent(s);
}
bool cmdNext(DebugSession &s)
{
    return moveBy(s, 1);
}
bool cmdPrv(DebugSession &s)
{
    return moveBy(s, -1);
}
bool cmdJmpNext(DebugSession &s, int32_t n)
{
    return moveBy(s, n);
}
bool cmdJmpPrv(DebugSession &s, int32_t n)
{
    return moveBy(s, -n);
}

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s)
{
    cout << "<---------- DEBUGGER ---------->" << endl;
    cout << "Step: " << step + 1 << " / " << totalSteps << endl;
    cout << "Stack Depth: " << s.stackDepth << endl;
    int depth = s.stackDepth;
    if (depth > MAX_STACK_DEPTH)
    {
        depth = MAX_STACK_DEPTH;
    }
    for (int i = depth - 1; i >= 0; i--)
    {
        const Frame &frame = s.callStack[i];
        cout << "Function: " << frame.func_name << endl;
        cout << "Arguments:" << endl;
        int argc = frame.argc;
        if (argc > MAX_VARS_PER_FRAME)
        {
            argc = MAX_VARS_PER_FRAME;
        }
        for (int j = 0; j < argc; j++)
        {
            cout << frame.argv[j].name << " = " << frame.argv[j].value << endl;
        }
        cout << "Local Variables:" << endl;
        int locals = frame.localCount;
        if (locals > MAX_VARS_PER_FRAME)
        {
            locals = MAX_VARS_PER_FRAME;
        }
        for (int j = 0; j < locals; j++)
        {
            cout << frame.locals[j].name << " = " << frame.locals[j].value << endl;
        }
        cout << "Return Line: " << frame.returnLine << endl;
    }
    cout << "<------------------------------>" << endl;
}
void printHelp()
{
    // Print the commands
    cout << "<---------- TTDB COMMANDS ---------->" << endl;
    cout << "start <source_file>\t- Start debugging a source file" << endl;
    cout << "next\t\t\t- Move one step forward" << endl;
    cout << "prv\t\t\t- Move one step backward" << endl;
    cout << "jmp next <n>\t\t- Jump forward n steps" << endl;
    cout << "jmp prv <n>\t\t- Jump backward n steps" << endl;
    cout << "help\t\t\t- Display available commands" << endl;
    cout << "quit\t\t\t- Exit the debugger" << endl;
    cout << "exit\t\t\t- Exit the debugger" << endl;
}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================

bool runCommand(DebugSession &s, const string &line, bool &quitRequested)
{
    // 1. Split the line into words.
    // 2. If the first word is "ttdb", drop it.
    // 3. Match the first word:
    //      start      -> needs a path after it, then cmdStart
    //      next       -> no more words, then cmdNext
    //      prv        -> no more words, then cmdPrv
    //      jmp        -> second word "next" or "prv", third word a positive number ,
    //                    then cmdJmpNext or cmdJmpPrv
    //      help       -> printHelp
    //      quit, exit -> set quitRequested = true
    // 4. For anything else, or a wrong number of words, print "unknown command" and the help text, and return false.
    string first = firstWord(line);
    string second = secondWord(line);
    string third = thirdWord(line);
    int words = countWords(line);

    if (first == "start")
    {
        if (words != 2)
        {
            cout << "Unknown Command. Usage: start <filename>" << endl;
            return false;
        }
        cmdStart(s, second);
    }
    else if (first == "next")
    {
        if (words != 1)
        {
            cout << "Unknown Command. Usage: next" << endl;
            return false;
        }
        cmdNext(s);
    }
    else if (first == "prv")
    {
        if (words != 1)
        {
            cout << "Unknown Command. Usage: prv" << endl;
            return false;
        }
        cmdPrv(s);
    }
    else if (first == "jmp")
    {
        if (words != 3)
        {
            cout << "Unknown Command. Usage: jmp <next/prv> <steps>" << endl;
            return false;
        }
        if (second != "next" && second != "prv")
        {
            cout << "Unknown Command. Usage: jmp <next/prv> <steps>" << endl;
            return false;
        }
        int steps = 0;
        for (int i = 0; i < third.length(); i++)
        {
            if (third[i] < '0' || third[i] > '9')
            {
                cout << "Invalid number of steps" << endl;
                return false;
            }
            steps = steps * 10 + (third[i] - '0');
        }
        if (steps <= 0)
        {
            cout << "Number of steps must be positive" << endl;
            return false;
        }
        if (second == "next")
        {
            cmdJmpNext(s, steps);
        }
        else
        {
            cmdJmpPrv(s, steps);
        }
    }
    else if (first == "help")
    {
        if (words != 1)
        {
            cout << "Unknown Command. Usage: help" << endl;
            return false;
        }
        printHelp();
    }
    else if (first == "quit" || first == "exit")
    {
        if (words != 1)
        {
            cout << "Unknown Command. Usage: <quit/exit>" << endl;
            return false;
        }
        quitRequested = true;
    }
    else
    {
        cout << "Unknown Command. Type 'help' for available commands." << endl;
        return false;
    }
    return true;
}

int main(int argc, char *argv[])
{
    // you get string LB_HOST and int LB_PORT = 0 in arguments of this main function.

    // DebugSession session; initSession(session);
    // Print a short welcome and the help text.
    //
    // Infinite loop:
    //   if quitRequested, break
    //
    // After the loop: closeSession(session) and return 0.
    // Never exit the loop because of an error. Only quit or the end of the input leaves it.

    if (argc != 3)
    {
        cout << "Usage: ./client <LB_HOST> <LB_PORT>" << endl;
        return 1;
    }
    LB_HOST = argv[1];
    int port = convertPortIntoInteger(argv[2]);
    if (port == -1)
    {
        cout << "Invalid Port Number" << endl;
        return 1;
    }
    LB_PORT = port;
    DebugSession session;
    initSession(session);
    cout << "Time-Travel-Debugger Client. Type 'help' for commands." << endl;
    printHelp();

    bool quitRequested = false;
    string line;
    while (!quitRequested)
    {
        cout << PROMPT;
        if (!getline(cin, line))
            break;
        runCommand(session, line, quitRequested);
    }

    closeSession(session);
    return 0;
}