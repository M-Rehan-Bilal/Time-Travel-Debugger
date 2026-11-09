#pragma once
#include <cstdint>
#include <string>

using namespace std;

// Helper.h should contain the structs, functions, constants, etc. that are used more than one time in the project. For example, the structs that are used in both server and client, the constants that are used in all three files.

const int32_t IO_BUFFER_SIZE = 64 * 1024; // fixed buffer for streaming to/from disk
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;

// Reply and upload status bytes
const char STATUS_OK = 0; // payload is the .tdbg file
const char STATUS_ERROR = 1; // payload is an error message


struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};

struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

// Checksum starts at 0
int checksumInit()
{
    return 0;
}

// Add the bytes to the running total
int checksumUpdate(int total, const char *data, int n)
{
    for (int i = 0; i < n; i++)
    {
        total += data[i];
    }
    return total;
}

string firstWord(const string &line)
{
    // returns first word from the input string
    string word = "";
    for (int i = 0; i < line.size(); i++)
    {
        if (line[i] == ' ')
            break;
        word += line[i];
    }
    return word;
}
string secondWord(const string &line)
{
    // returns the second word
    string word = "";
    bool second = false;
    for (int i = 0; i < line.size(); i++)
    {
        if (line[i] == ' ')
        {
            if (second)
                break;
            second = true;
            word = "";
        }
        else
        {
            word += line[i];
        }
    }
    if (!second) // second word not exists
        return "";
    return word;
}
string thirdWord(const string &line)
{
    // returns the third word
    string word = "";
    int spaces = 0;
    for (int i = 0; i < line.size(); i++)
    {
        if (line[i] == ' ')
        {
            if (spaces >= 2)
            {
                break;
            }
            spaces++;
        }
        else if (spaces == 2)
        {
            word += line[i];
        }
    }
    return word;
}
int countWords(const string& line) {
    if (line.empty())
        return 0;
    int n = 1;
    for (int i = 0; i < line.size(); i++) {
        if (line[i] == ' ')
            n++;
    }
    return n;
}

int convertPortIntoInteger(const char *text)
{
    if (text[0] == '\0')
        return -1;
    int value = 0;
    for (int i = 0; text[i] != '\0'; i++)
    {
        if (text[i] < '0' || text[i] > '9')
            return -1;
        value = value * 10 + (text[i] - '0');
        if (value > 65535)
            return -1;
    }
    if (value == 0)
        return -1;
    return value;
}