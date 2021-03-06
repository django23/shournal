#pragma once

#include <unordered_set>

#include "os.h"



namespace subprocess {

typedef std::vector<std::string> Args_t;

/// Call external programs via fork and exec
/// and wait for it to finish later
class Subprocess {
public:

    Subprocess();

    void call(char *const argv[],
              bool forwardStdin=true,
              bool forwardStdout=true,
              bool forwardStderr=true);

    void call(const Args_t &args, bool forwardStdin=true,
                                  bool forwardStdout=true,
                                  bool forwardStderr=true);

    void call(const char *filename, char * const argv[],
              bool forwardStdin=true,
              bool forwardStdout=true,
              bool forwardStderr=true);

    void callDetached(char *const argv[], bool forwardStdin=false,
                      bool forwardStdout=true,
                      bool forwardStderr=true);

    void callDetached(const char *filename, char *const argv[], bool forwardStdin=false,
                      bool forwardStdout=true,
                      bool forwardStderr=true);

    void callDetached(const Args_t &args, bool forwardStdin=false,
                      bool forwardStdout=true,
                      bool forwardStderr=true);

    int waitFinish();

    void setAsRealUser(bool val);
    void setForwardFdsOnExec(const std::unordered_set<int>& forwardFds);
    void setForwardAllFds(bool val);
    void setInNewSid(bool val);

    pid_t lastPid() const;
    void setEnviron(char **env);

private:
    void closeAllButForwardFds(os::Pipes_t &startPipe);
    [[noreturn]]
    void handleChild(const char *filename, char * const argv[], os::Pipes_t & startPipe, bool writePidToStartPipe,
                     bool forwardStdin, bool forwardStdout, bool forwardStderr);

    pid_t m_lastPid;
    bool m_asRealUser;
    std::unordered_set<int> m_forwardFds;
    bool m_forwardAllFds;
    bool m_lastCallWasDetached;
    char** m_environ;
    bool m_inNewSid;
};


} // namespace subprocess
