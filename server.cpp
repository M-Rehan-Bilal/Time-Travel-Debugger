// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)


#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
//#include <unistd.h>
//#include <sys/socket.h>
//#include <cstdint>
#include <cstdio>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack {
    struct Node {
        T data;
        Node* next;
    };
    Node* top;
    int32_t count;
public:
    // Implement these functions:
    Stack() : top(nullptr), count(0) {}
    void push(const T& val) {
        // pushes the value on the stack if max limit is not reached yet.
        if (count == MAX_STACK_DEPTH)
            throw overflow_error("Max Stack Depth Reached");
        Node* temp = new Node{ val, nullptr };
        if (count == 0) {
            top = temp;
        }
        else {
            temp->next = top;
            top = temp;
        }
        count++;
    }
    T pop() {
        // pop the top value on the stack
        if (count == 0)
            throw underflow_error("Nothing to Pop from Stack");
        Node* temp = top;
        T val = top->data;
        top = top->next;
        delete temp;
        count--;
        return val;
    }
    T& peek() {
        // returns the top value on the stack
        if (count == 0)
            throw underflow_error("Nothing to Peek from Stack");
        return top->data;
    }
    bool isEmpty() {
        return count == 0;
    }
    int32_t depth() {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen) {
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
        Node* temp = top;
        int32_t i;
        for (i = 0; i < maxLen && temp != nullptr; i++) {
            out[i] = temp->data;
            temp = temp->next;
        }
        return i;
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode {
    Snapshot* data;
    TimelineNode* next;
    TimelineNode* prev;
};
class Timeline {
    TimelineNode* head, * tail;
    int32_t stepCount;
public:
    // Implement these functions
    Timeline() : head(nullptr), tail(nullptr), stepCount(0) {}
    void record(Snapshot* s) {
        // add record in the timeline
        TimelineNode* temp = new TimelineNode{ s, nullptr, nullptr };
        if (stepCount == 0) {
            head = tail = temp;
        }
        else {
            tail->next = temp;
            temp->prev = tail;
            tail = temp;
        }
        stepCount++;
    }
    TimelineNode* begin() {
        return head;
    }
    int32_t getStepCount() {
        return stepCount;
    }
};

// Core structs
struct Variable {
    string name;
    int32_t value;
};
struct Frame {
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot {
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader {
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
void writeHeader(FILE* f, const TTDBHeader& h) {
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    // placeholder for other two data members
    fwrite(&h.stepCount, sizeof(int32_t), 1, f);
    fwrite(&h.indexOffset, sizeof(int64_t), 1, f);
}

// resolve.bin - bookkeeping
struct FuncEntry {
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch {
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};

// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream& in, string& out) {
    // reads the next nonblank line
    while (getline(in, out)) {
        if (!out.empty())
            return true;
    }
    return false;
}
string firstWord(const string& line) {
    // returns first word from the input string
    string word = "";
    for (int i = 0; i < line.size(); i++) {
        if (line[i] == ' ')
            break;
        word += line[i];
    }
    return word;
}
string secondWord(const string& line) {
    // returns the second word
    string word = "";
    bool second = false;
    for (int i = 0; i < line.size(); i++) {
        if (line[i] == ' ') {
            if (second)
                break;
            second = true;
            word = "";
        }
        else {
            word += line[i];
        }
    }
    return word;
}

const int INSTRUCTIONS_COUNT = 8;
const string instructionSet[INSTRUCTIONS_COUNT] = { "func", "func_end", "call", "set", "add", "mul", "sub", "div" };

bool validateProgram(const char* sourcePath) {
    // for each func defined there should be exactly one func_end and no nested funcs allowed - 
    ifstream rdr(sourcePath);
    if (!rdr)
        return false;
    string line;
    Stack<string> st;
    bool isValidKeyword = false;
    bool main = false;
    while (readSourceLine(rdr, line)) {
        string fWord = firstWord(line);
        string sWord = secondWord(line);
        if (fWord == "func") {
            if (sWord == "" || !st.isEmpty())
                return false;
            st.push(fWord);
            if (sWord == "main")
                main = true;
        }
        else if (fWord == "func_end") {
            if (st.isEmpty())
                return false;
            st.pop();
        }
        isValidKeyword = false;
        for (int i = 0; i < INSTRUCTIONS_COUNT; i++) {
            if (fWord == instructionSet[i]) {
                isValidKeyword = true;
                break;
            }
        }
        if (!isValidKeyword)
            return false;
    }
    return st.isEmpty() && main;
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text) {
    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
    int64_t startingBytePos = ftell(f);
    fwrite(&offsetField, sizeof(int64_t), 1, f);
    int32_t size = text.size();
    fwrite(&size, sizeof(int32_t), 1, f);
    fwrite(&text[0], sizeof(text), 1, f);
    return startingBytePos;
}

int64_t readResolveRecord(FILE* f, string& outText) {
    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
    int64_t startingBytePos;
    if (fread(&startingBytePos, sizeof(int64_t), 1, f) != 1) {
        return -1;
    }
    int32_t size;
    if (fread(&size, sizeof(int32_t), 1, f) != 1) {
        return -1;
    }
    if (size <= 0 || size > IO_BUFFER_SIZE) {
        return -1; // corrupt size
    }
    outText.resize(size);
    if (fread(&outText[0], size, 1, f) != 1) {
        return -1;
    }
    return startingBytePos;
}
int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath) {
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;
    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error
    ifstream rdr(sourcePath);
    if (!rdr)
        return -1;
    //ofstream fout(resolveBinPath, ios::binary);
    FILE* fout = fopen(resolveBinPath, "wb");
    if (!fout)
        return -1;
    string line;
    int64_t offset = 0;
    while (readSourceLine(rdr, line)) {
        offset = writeResolveRecord(fout, offset, line);
        string fWord = firstWord(line);
        string sWord = secondWord(line);
        if (fWord == "func") {
            if (funcCount == MAX_FUNCS) {
                fclose(fout);
                rdr.close();
                return -1;
            }
            funcArray[funcCount] = { sWord, offset };
            funcCount++;
        }
        else if (fWord == "call") {
            if (patchCount == MAX_PATCHES) {
                fclose(fout);
                rdr.close();
                return -1;
            }
            patches[patchCount] = { offset, sWord };
            patchCount++;
        }
        offset += sizeof(int64_t) + sizeof(int32_t) + line.size();
    }
    for (int i = 0; i < patchCount; i++) {
        bool found = false;
        for (int j = 0; j < funcCount; j++) {
            if (patches[i].targetFuncName == funcArray[j].funcName) {
                int64_t target = funcArray[j].byteOffsetInResolveBin;
                if (fseek(fout, patches[i].byteOffsetOfOffsetField, SEEK_SET) != 0) {
                    fclose(fout);
                    rdr.close();
                    return -1;
                }
                if (fwrite(&target, sizeof(int64_t), 1, fout) != 1) {
                    fclose(fout);
                    rdr.close();
                    return -1;
                }
                found = true;
                break;
            }
        }
        if (!found) {
            fclose(fout);
            rdr.close();
            return -1;
        }
    }
    fclose(fout);
    rdr.close();
    for (int i = 0; i < funcCount; i++) {
        if (funcArray[i].funcName == "main") {
            return funcArray[i].byteOffsetInResolveBin;
        }
    }
    return -1;
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string &line, Token tokens[], int32_t maxTokens)
{
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
}
Snapshot *buildSnapshot(Stack<Frame> &callStack)
{
    // build the snapshot based on the callStack given
}
void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline &timeline, const char *tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}
// main section
int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}